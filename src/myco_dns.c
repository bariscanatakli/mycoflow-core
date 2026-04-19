/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_dns.c — Passive DNS snooping + IP→persona cache
 *
 * Note: _GNU_SOURCE must be defined before any header is included so musl
 * exposes the Linux-style udphdr fields (source/dest) we rely on below. */
#define _GNU_SOURCE
/*
 *
 * Passively captures DNS responses on br-lan (UDP port 53) to build
 * an IP→domain→persona mapping cache. This allows MycoFlow to identify
 * traffic on port 443 (HTTPS/QUIC) by domain name — the key signal
 * that lifts classification accuracy from 78% (port-only) to 94%.
 *
 * Safety guarantees:
 *   - Parser rejects malformed packets silently (no crash, no log spam)
 *   - Sniffer thread checks g_stop every 1s via select() timeout
 *   - If DNS snooping fails, system degrades to port+behavior (78%)
 *   - Zero flash writes — all state in RAM
 */
#include "myco_dns.h"
#include "myco_log.h"

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <time.h>

/* ── Domain suffix → persona table ───────────────────────────── */

typedef struct {
    const char *suffix;
    persona_t   persona;
} domain_hint_t;

/*
 * Ordered longest-first so a specific suffix like "meet.google.com"
 * matches before a shorter ".google.com" (if we had one).
 * Only domains that resolve the 443 ambiguity are included.
 */
static const domain_hint_t domain_table[] = {
    /* STREAMING — video/audio content delivery */
    { "googlevideo.com",        PERSONA_STREAMING },
    { "nflxvideo.net",          PERSONA_STREAMING },
    { "netflix.com",            PERSONA_STREAMING },
    { "ttvnw.net",              PERSONA_STREAMING },  /* Twitch CDN */
    { "twitch.tv",              PERSONA_STREAMING },
    { "jtvnw.net",              PERSONA_STREAMING },  /* Twitch legacy CDN */
    { "fbcdn.net",              PERSONA_STREAMING },  /* Facebook/Instagram video */
    { "video.fbcdn.net",        PERSONA_STREAMING },
    { "spotify.com",            PERSONA_STREAMING },
    { "scdn.co",                PERSONA_STREAMING },  /* Spotify CDN */
    { "aaplimg.com",            PERSONA_STREAMING },  /* Apple TV+ CDN */
    { "hls.apple.com",          PERSONA_STREAMING },
    { "prd.media.h264.io",      PERSONA_STREAMING },  /* Disney+ CDN */
    { "disneyplus.com",         PERSONA_STREAMING },
    { "primevideo.com",         PERSONA_STREAMING },
    { "aiv-cdn.net",            PERSONA_STREAMING },  /* Amazon Prime Video CDN */
    { "dssott.com",             PERSONA_STREAMING },  /* Disney+ streaming */
    { "youtube.com",            PERSONA_STREAMING },
    { "ytimg.com",              PERSONA_STREAMING },
    { "ggpht.com",              PERSONA_STREAMING },  /* Google content CDN */

    /* VIDEO — real-time video calls */
    { "zoom.us",                PERSONA_VIDEO },
    { "zoomgov.com",            PERSONA_VIDEO },
    { "teams.microsoft.com",    PERSONA_VIDEO },
    { "teams.live.com",         PERSONA_VIDEO },
    { "skype.com",              PERSONA_VIDEO },
    { "meet.google.com",        PERSONA_VIDEO },
    { "discord.media",          PERSONA_VIDEO },
    { "discord.gg",             PERSONA_VIDEO },
    { "discordapp.com",         PERSONA_VIDEO },
    { "webex.com",              PERSONA_VIDEO },

    /* GAMING — game servers and platforms */
    { "riotgames.com",          PERSONA_GAMING },
    { "riotcdn.net",            PERSONA_GAMING },
    { "leagueoflegends.com",    PERSONA_GAMING },
    { "steampowered.com",       PERSONA_GAMING },
    { "steamcontent.com",       PERSONA_GAMING },
    { "steamserver.net",        PERSONA_GAMING },
    { "epicgames.com",          PERSONA_GAMING },
    { "unrealengine.com",       PERSONA_GAMING },
    { "battle.net",             PERSONA_GAMING },
    { "blizzard.com",           PERSONA_GAMING },
    { "xboxlive.com",           PERSONA_GAMING },
    { "gamepass.com",           PERSONA_GAMING },
    { "playstation.net",        PERSONA_GAMING },
    { "ea.com",                 PERSONA_GAMING },
    { "origin.com",             PERSONA_GAMING },
    { "ubisoft.com",            PERSONA_GAMING },
    { "valvesoftware.com",      PERSONA_GAMING },

    /* VOIP — voice communication */
    { "whatsapp.net",           PERSONA_VOIP },
    { "whatsapp.com",           PERSONA_VOIP },
    { "signal.org",             PERSONA_VOIP },
    { "telegram.org",           PERSONA_VOIP },
    { "viber.com",              PERSONA_VOIP },
    { "vonage.com",             PERSONA_VOIP },

    { NULL, PERSONA_UNKNOWN }  /* sentinel */
};

/* ── Domain suffix → service table (v3, finer-grained) ──────────
 * 90+ suffixes covering the service taxonomy (architecture §5).
 * Iterated in order, first matching suffix wins — put specific
 * subdomains before generic ones to avoid shadowing.
 */
typedef struct {
    const char *suffix;
    service_t   service;
} domain_service_t;

static const domain_service_t domain_service_table[] = {
    /* ═══ SVC_VIDEO_CONF — real-time conferencing (specific first) ═══ */
    { "meet.google.com",        SVC_VIDEO_CONF },
    { "teams.microsoft.com",    SVC_VIDEO_CONF },
    { "teams.live.com",         SVC_VIDEO_CONF },
    { "zoom.us",                SVC_VIDEO_CONF },
    { "zoomgov.com",            SVC_VIDEO_CONF },
    { "webex.com",              SVC_VIDEO_CONF },
    { "gotomeeting.com",        SVC_VIDEO_CONF },
    { "whereby.com",            SVC_VIDEO_CONF },
    { "bluejeans.com",          SVC_VIDEO_CONF },
    { "skype.com",              SVC_VIDEO_CONF },

    /* ═══ SVC_VOIP_CALL — voice-primary ═══ */
    { "discord.media",          SVC_VOIP_CALL },
    { "whatsapp.net",           SVC_VOIP_CALL },
    { "whatsapp.com",           SVC_VOIP_CALL },
    { "signal.org",             SVC_VOIP_CALL },
    { "telegram.org",           SVC_VOIP_CALL },
    { "viber.com",              SVC_VOIP_CALL },
    { "vonage.com",             SVC_VOIP_CALL },

    /* ═══ SVC_VIDEO_LIVE — live streams (specific before generic) ═══ */
    { "tiktokcdn-live.com",     SVC_VIDEO_LIVE },
    { "ttvnw.net",              SVC_VIDEO_LIVE },   /* Twitch CDN */
    { "jtvnw.net",              SVC_VIDEO_LIVE },   /* Twitch legacy */
    { "twitch.tv",              SVC_VIDEO_LIVE },

    /* ═══ SVC_VIDEO_VOD — buffered video/audio ═══ */
    { "googlevideo.com",        SVC_VIDEO_VOD },    /* YouTube */
    { "ytimg.com",              SVC_VIDEO_VOD },
    { "youtube.com",            SVC_VIDEO_VOD },
    { "ggpht.com",              SVC_VIDEO_VOD },
    { "nflxvideo.net",          SVC_VIDEO_VOD },    /* Netflix */
    { "netflix.com",            SVC_VIDEO_VOD },
    { "aiv-cdn.net",            SVC_VIDEO_VOD },    /* Amazon Prime */
    { "primevideo.com",         SVC_VIDEO_VOD },
    { "dssott.com",             SVC_VIDEO_VOD },    /* Disney+ */
    { "disneyplus.com",         SVC_VIDEO_VOD },
    { "hls.apple.com",          SVC_VIDEO_VOD },    /* Apple TV+ */
    { "aaplimg.com",            SVC_VIDEO_VOD },
    { "hbomax.com",             SVC_VIDEO_VOD },
    { "hulustream.com",         SVC_VIDEO_VOD },
    { "hulu.com",               SVC_VIDEO_VOD },
    { "video.fbcdn.net",        SVC_VIDEO_VOD },    /* Meta video (specific) */
    { "fbcdn.net",              SVC_VIDEO_VOD },    /* Meta CDN (generic) */
    { "cdninstagram.com",       SVC_VIDEO_VOD },
    { "tiktokcdn.com",          SVC_VIDEO_VOD },
    { "tiktokv.com",            SVC_VIDEO_VOD },
    { "spotify.com",            SVC_VIDEO_VOD },    /* Audio VOD */
    { "scdn.co",                SVC_VIDEO_VOD },

    /* ═══ SVC_GAME_LAUNCHER — bulk game content (before GAME_RT) ═══ */
    { "steamcontent.com",       SVC_GAME_LAUNCHER },
    { "steamserver.net",        SVC_GAME_LAUNCHER },

    /* ═══ SVC_GAME_RT — real-time gameplay ═══ */
    { "riotgames.com",          SVC_GAME_RT },
    { "riotcdn.net",            SVC_GAME_RT },
    { "leagueoflegends.com",    SVC_GAME_RT },
    { "steampowered.com",       SVC_GAME_RT },
    { "valvesoftware.com",      SVC_GAME_RT },
    { "epicgames.com",          SVC_GAME_RT },
    { "unrealengine.com",       SVC_GAME_RT },
    { "battle.net",             SVC_GAME_RT },
    { "blizzard.com",           SVC_GAME_RT },
    { "xboxlive.com",           SVC_GAME_RT },
    { "gamepass.com",           SVC_GAME_RT },
    { "playstation.net",        SVC_GAME_RT },
    { "playstation.com",        SVC_GAME_RT },
    { "ea.com",                 SVC_GAME_RT },
    { "easports.com",           SVC_GAME_RT },
    { "origin.com",             SVC_GAME_RT },
    { "ubisoft.com",            SVC_GAME_RT },
    { "ubi.com",                SVC_GAME_RT },
    { "mojang.com",             SVC_GAME_RT },
    { "minecraft.net",          SVC_GAME_RT },
    { "roblox.com",             SVC_GAME_RT },

    /* ═══ SVC_FILE_SYNC — cloud drives (specific subdomains first) ═══ */
    { "drive.google.com",       SVC_FILE_SYNC },
    { "docs.google.com",        SVC_FILE_SYNC },
    { "onedrive.live.com",      SVC_FILE_SYNC },
    { "files.live.com",         SVC_FILE_SYNC },
    { "icloud-content.com",     SVC_FILE_SYNC },
    { "icloud.com",             SVC_FILE_SYNC },
    { "dropboxusercontent.com", SVC_FILE_SYNC },
    { "dropbox.com",            SVC_FILE_SYNC },
    { "box.com",                SVC_FILE_SYNC },
    { "mega.nz",                SVC_FILE_SYNC },
    { "pcloud.com",             SVC_FILE_SYNC },

    /* ═══ SVC_BULK_DL — software distribution ═══ */
    { "githubusercontent.com",  SVC_BULK_DL },
    { "github.com",             SVC_BULK_DL },
    { "github.io",              SVC_BULK_DL },
    { "docker.io",              SVC_BULK_DL },
    { "docker.com",             SVC_BULK_DL },
    { "pypi.org",               SVC_BULK_DL },
    { "pythonhosted.org",       SVC_BULK_DL },
    { "npmjs.com",              SVC_BULK_DL },
    { "npmjs.org",              SVC_BULK_DL },
    { "archive.ubuntu.com",     SVC_BULK_DL },
    { "security.ubuntu.com",    SVC_BULK_DL },
    { "ubuntu.com",             SVC_BULK_DL },
    { "debian.org",             SVC_BULK_DL },
    { "kernel.org",             SVC_BULK_DL },

    /* ═══ SVC_WEB_INTERACTIVE — chat/web apps (non-voice Discord etc.) ═══ */
    { "discord.gg",             SVC_WEB_INTERACTIVE },
    { "discordapp.com",         SVC_WEB_INTERACTIVE },
    { "t.me",                   SVC_WEB_INTERACTIVE },

    { NULL, SVC_UNKNOWN }  /* sentinel */
};

/* ── Domain suffix → persona lookup ──────────────────────────── */

persona_t dns_domain_to_hint(const char *domain) {
    if (!domain || domain[0] == '\0') {
        return PERSONA_UNKNOWN;
    }

    size_t dlen = strlen(domain);

    for (const domain_hint_t *e = domain_table; e->suffix; e++) {
        size_t slen = strlen(e->suffix);
        if (slen > dlen) {
            continue;
        }
        /* Suffix match: domain ends with the suffix, and either the
         * domain IS the suffix or the char before the suffix is '.' */
        const char *tail = domain + (dlen - slen);
        if (strcasecmp(tail, e->suffix) == 0) {
            if (slen == dlen || *(tail - 1) == '.') {
                return e->persona;
            }
        }
    }

    return PERSONA_UNKNOWN;
}

service_t dns_domain_to_service(const char *domain) {
    if (!domain || domain[0] == '\0') {
        return SVC_UNKNOWN;
    }

    size_t dlen = strlen(domain);

    for (const domain_service_t *e = domain_service_table; e->suffix; e++) {
        size_t slen = strlen(e->suffix);
        if (slen > dlen) {
            continue;
        }
        const char *tail = domain + (dlen - slen);
        if (strcasecmp(tail, e->suffix) == 0) {
            if (slen == dlen || *(tail - 1) == '.') {
                return e->service;
            }
        }
    }

    return SVC_UNKNOWN;
}

/* ── Cache operations ────────────────────────────────────────── */

void dns_cache_init(dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    memset(cache->entries, 0, sizeof(cache->entries));
    pthread_mutex_init(&cache->lock, NULL);
}

void dns_cache_destroy(dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    pthread_mutex_destroy(&cache->lock);
}

persona_t dns_cache_lookup(dns_cache_t *cache, uint32_t ip) {
    if (!cache) {
        return PERSONA_UNKNOWN;
    }

    persona_t result = PERSONA_UNKNOWN;
    pthread_mutex_lock(&cache->lock);

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];
        if (e->active && e->ip == ip) {
            /* Check TTL expiry using monotonic clock */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            if (now < e->expire_time) {
                result = e->hint;
            } else {
                /* Expired — mark inactive */
                e->active = 0;
            }
            break;
        }
    }

    pthread_mutex_unlock(&cache->lock);
    return result;
}

void dns_cache_insert(dns_cache_t *cache, uint32_t ip,
                      const char *domain, uint32_t ttl_s) {
    if (!cache || !domain) {
        return;
    }

    persona_t hint = dns_domain_to_hint(domain);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    /* Clamp TTL: minimum 30s, maximum 3600s */
    if (ttl_s < 30)   ttl_s = 30;
    if (ttl_s > 3600)  ttl_s = 3600;

    pthread_mutex_lock(&cache->lock);

    /* Look for existing entry for this IP, or find LRU slot */
    int target = -1;
    double oldest_expire = 1e18;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];

        /* Exact IP match — update in place */
        if (e->active && e->ip == ip) {
            target = i;
            break;
        }

        /* Empty slot */
        if (!e->active) {
            target = i;
            oldest_expire = -1.0;  /* prefer empty over LRU */
            continue;
        }

        /* Track LRU (earliest expiry) for eviction */
        if (e->expire_time < oldest_expire && target < 0) {
            oldest_expire = e->expire_time;
            target = i;
        }
        /* If we already found an empty slot (oldest_expire == -1), skip LRU */
        if (oldest_expire < 0.0) {
            continue;
        }
        if (e->expire_time < oldest_expire) {
            oldest_expire = e->expire_time;
            target = i;
        }
    }

    if (target < 0) {
        target = 0;  /* fallback: should never happen with 64 slots */
    }

    dns_entry_t *slot = &cache->entries[target];
    slot->ip = ip;
    strncpy(slot->domain, domain, DNS_DOMAIN_MAXLEN - 1);
    slot->domain[DNS_DOMAIN_MAXLEN - 1] = '\0';
    slot->hint = hint;
    slot->service = dns_domain_to_service(domain);
    slot->expire_time = now + (double)ttl_s;
    slot->active = 1;

    pthread_mutex_unlock(&cache->lock);
}

service_t dns_cache_lookup_service(dns_cache_t *cache, uint32_t ip) {
    if (!cache) {
        return SVC_UNKNOWN;
    }

    service_t result = SVC_UNKNOWN;
    pthread_mutex_lock(&cache->lock);

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];
        if (e->active && e->ip == ip) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            if (now < e->expire_time) {
                result = e->service;
            } else {
                e->active = 0;
            }
            break;
        }
    }

    pthread_mutex_unlock(&cache->lock);
    return result;
}

/* ── DNS response parser ─────────────────────────────────────── */

/*
 * Paranoid DNS response parser. Extracts A records from a raw DNS
 * response (after UDP header). Inserts IP→domain pairs into the cache.
 *
 * DNS wire format (RFC 1035):
 *   Header: 12 bytes (ID, flags, QDCOUNT, ANCOUNT, NSCOUNT, ARCOUNT)
 *   Question section: variable (QNAME + QTYPE + QCLASS)
 *   Answer section: variable (NAME + TYPE + CLASS + TTL + RDLENGTH + RDATA)
 *
 * Safety: every offset is bounds-checked. Malformed packets are silently
 * rejected. No allocations, no recursion.
 */

/* Read a DNS name at offset, write it into buf (dot-separated).
 * Handles compression pointers (0xC0 prefix). Returns bytes consumed
 * from the packet at the original offset, or -1 on error.
 * max_jumps prevents infinite pointer loops. */
static int dns_read_name(const uint8_t *pkt, size_t pkt_len,
                         size_t offset, char *buf, size_t buf_len) {
    if (!pkt || !buf || buf_len == 0) {
        return -1;
    }

    size_t pos = offset;
    size_t out = 0;
    int jumped = 0;
    int bytes_consumed = 0;
    int max_jumps = 10;
    int jump_count = 0;

    buf[0] = '\0';

    while (pos < pkt_len) {
        uint8_t len = pkt[pos];

        if (len == 0) {
            /* End of name */
            if (!jumped) {
                bytes_consumed = (int)(pos - offset + 1);
            }
            break;
        }

        /* Compression pointer: 2 MSB = 11 */
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) {
                return -1;
            }
            if (!jumped) {
                bytes_consumed = (int)(pos - offset + 2);
            }
            uint16_t ptr = ((uint16_t)(len & 0x3F) << 8) | pkt[pos + 1];
            if (ptr >= pkt_len) {
                return -1;
            }
            pos = ptr;
            jumped = 1;
            if (++jump_count > max_jumps) {
                return -1;  /* infinite loop protection */
            }
            continue;
        }

        /* Label: len bytes follow */
        if ((len & 0xC0) != 0) {
            return -1;  /* reserved bits set — reject */
        }

        pos++;
        if (pos + len > pkt_len) {
            return -1;  /* label extends past packet */
        }

        /* Add dot separator between labels */
        if (out > 0 && out < buf_len - 1) {
            buf[out++] = '.';
        }

        for (uint8_t i = 0; i < len && out < buf_len - 1; i++) {
            buf[out++] = (char)pkt[pos + i];
        }
        pos += len;
    }

    buf[out] = '\0';

    return (bytes_consumed > 0) ? bytes_consumed : -1;
}

int dns_parse_response(dns_cache_t *cache, const uint8_t *pkt, size_t pkt_len) {
    if (!cache || !pkt) {
        return -1;
    }

    /* DNS header is 12 bytes minimum */
    if (pkt_len < 12) {
        return -1;
    }

    /* Check QR bit: must be a response (bit 15 of flags) */
    uint16_t flags = (uint16_t)(pkt[2] << 8 | pkt[3]);
    if (!(flags & 0x8000)) {
        return -1;  /* not a response */
    }

    /* Check RCODE: only accept NOERROR (0) */
    if ((flags & 0x000F) != 0) {
        return -1;
    }

    uint16_t qdcount = (uint16_t)(pkt[4] << 8 | pkt[5]);
    uint16_t ancount = (uint16_t)(pkt[6] << 8 | pkt[7]);

    if (ancount == 0) {
        return 0;  /* no answers — nothing to cache */
    }

    /* Sanity: reject unreasonable counts */
    if (qdcount > 10 || ancount > 50) {
        return -1;
    }

    /* Read the question name (we'll use it as the domain for A records
     * that use compression pointers back to the question) */
    char qname[DNS_DOMAIN_MAXLEN];
    qname[0] = '\0';

    size_t offset = 12;  /* past header */

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        int consumed = dns_read_name(pkt, pkt_len, offset, qname, sizeof(qname));
        if (consumed < 0) {
            return -1;
        }
        offset += (size_t)consumed;
        offset += 4;  /* QTYPE (2) + QCLASS (2) */
        if (offset > pkt_len) {
            return -1;
        }
    }

    /* Parse answer section */
    int a_records = 0;
    for (uint16_t a = 0; a < ancount; a++) {
        if (offset >= pkt_len) {
            break;
        }

        /* Read answer name */
        char aname[DNS_DOMAIN_MAXLEN];
        int consumed = dns_read_name(pkt, pkt_len, offset, aname, sizeof(aname));
        if (consumed < 0) {
            return a_records > 0 ? a_records : -1;
        }
        offset += (size_t)consumed;

        /* TYPE (2) + CLASS (2) + TTL (4) + RDLENGTH (2) = 10 bytes */
        if (offset + 10 > pkt_len) {
            return a_records > 0 ? a_records : -1;
        }

        uint16_t rtype   = (uint16_t)(pkt[offset] << 8 | pkt[offset + 1]);
        /* uint16_t rclass  = (uint16_t)(pkt[offset+2] << 8 | pkt[offset+3]); */
        uint32_t ttl     = (uint32_t)(pkt[offset + 4] << 24 | pkt[offset + 5] << 16 |
                                      pkt[offset + 6] << 8  | pkt[offset + 7]);
        uint16_t rdlen   = (uint16_t)(pkt[offset + 8] << 8 | pkt[offset + 9]);
        offset += 10;

        if (offset + rdlen > pkt_len) {
            return a_records > 0 ? a_records : -1;
        }

        /* A record: TYPE=1, RDLENGTH=4 (IPv4 address) */
        if (rtype == 1 && rdlen == 4) {
            uint32_t ip;
            memcpy(&ip, &pkt[offset], 4);  /* already in network byte order */

            /* Use the question name as the domain — it's the original
             * query that triggered this response. Answer names may be
             * CDN CNAMEs that don't match our suffix table. */
            const char *domain = (qname[0] != '\0') ? qname : aname;

            dns_cache_insert(cache, ip, domain, ttl);
            a_records++;
        }

        /* CNAME (TYPE=5): read the canonical name to use as domain
         * for subsequent A records in the same response */
        if (rtype == 5) {
            char cname[DNS_DOMAIN_MAXLEN];
            if (dns_read_name(pkt, pkt_len, offset, cname, sizeof(cname)) > 0) {
                /* We keep qname as the original query — CNAMEs are intermediate */
            }
        }

        offset += rdlen;
    }

    return a_records;
}

/* ── Sniffer thread ──────────────────────────────────────────── */

/*
 * Background thread: opens a raw socket filtered on UDP port 53,
 * captures DNS responses, and populates the cache.
 *
 * Uses select() with 1-second timeout to check g_stop periodically.
 * Requires CAP_NET_RAW or root.
 */
void *dns_sniff_thread(void *arg) {
    dns_cache_t *cache = (dns_cache_t *)arg;
    if (!cache) {
        return NULL;
    }

    /* Open raw socket for UDP packets */
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (sock < 0) {
        log_msg(LOG_WARN, "dns", "failed to open raw socket (need CAP_NET_RAW), DNS snooping disabled");
        return NULL;
    }

    log_msg(LOG_INFO, "dns", "DNS sniffer thread started");

    uint8_t buf[2048];  /* max DNS response over UDP is ~512-4096 bytes */

    while (!g_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) {
            continue;  /* timeout or error — check g_stop and retry */
        }

        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < (ssize_t)(sizeof(struct iphdr) + sizeof(struct udphdr))) {
            continue;  /* too short */
        }

        /* Parse IP header */
        struct iphdr *iph = (struct iphdr *)buf;
        if (iph->protocol != IPPROTO_UDP) {
            continue;
        }

        size_t ip_hlen = (size_t)(iph->ihl * 4);
        if (ip_hlen < 20 || ip_hlen + sizeof(struct udphdr) > (size_t)n) {
            continue;
        }

        /* Parse UDP header */
        struct udphdr *udph = (struct udphdr *)(buf + ip_hlen);
        uint16_t src_port = ntohs(udph->source);

        /* We only want DNS responses: source port 53 */
        if (src_port != 53) {
            continue;
        }

        /* DNS payload starts after IP + UDP headers */
        size_t dns_offset = ip_hlen + sizeof(struct udphdr);
        if (dns_offset >= (size_t)n) {
            continue;
        }

        const uint8_t *dns_payload = buf + dns_offset;
        size_t dns_len = (size_t)n - dns_offset;

        int count = dns_parse_response(cache, dns_payload, dns_len);
        if (count > 0) {
            log_msg(LOG_DEBUG, "dns", "parsed %d A record(s)", count);
        }
    }

    close(sock);
    log_msg(LOG_INFO, "dns", "DNS sniffer thread stopped");
    return NULL;
}
