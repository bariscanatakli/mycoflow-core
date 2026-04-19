/*
 * test_dns.c - Unit tests for DNS cache, domain→persona lookup, and parser
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <time.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_dns.h"

int tests_run = 0;

/* Stub: g_stop is defined in main.c but needed by dns_sniff_thread.
 * Tests don't call the sniffer thread, so a stub suffices. */
volatile sig_atomic_t g_stop = 0;

/* ══════════════════════════════════════════════════════════════
 * DOMAIN SUFFIX → PERSONA TESTS
 * ══════════════════════════════════════════════════════════════ */

static char *test_domain_streaming_googlevideo() {
    mu_assert("googlevideo.com → STREAMING",
              dns_domain_to_hint("rr1---sn-abc.googlevideo.com") == PERSONA_STREAMING);
    return 0;
}

static char *test_domain_streaming_netflix() {
    mu_assert("nflxvideo.net → STREAMING",
              dns_domain_to_hint("ipv4-c001-ist001.nflxvideo.net") == PERSONA_STREAMING);
    return 0;
}

static char *test_domain_streaming_twitch() {
    mu_assert("ttvnw.net → STREAMING",
              dns_domain_to_hint("video-edge-123.ttvnw.net") == PERSONA_STREAMING);
    return 0;
}

static char *test_domain_streaming_youtube() {
    mu_assert("youtube.com → STREAMING",
              dns_domain_to_hint("www.youtube.com") == PERSONA_STREAMING);
    return 0;
}

static char *test_domain_streaming_spotify() {
    mu_assert("scdn.co → STREAMING",
              dns_domain_to_hint("audio-fa.scdn.co") == PERSONA_STREAMING);
    return 0;
}

static char *test_domain_video_zoom() {
    mu_assert("zoom.us → VIDEO",
              dns_domain_to_hint("us04web.zoom.us") == PERSONA_VIDEO);
    return 0;
}

static char *test_domain_video_teams() {
    mu_assert("teams.microsoft.com → VIDEO",
              dns_domain_to_hint("teams.microsoft.com") == PERSONA_VIDEO);
    return 0;
}

static char *test_domain_video_meet() {
    mu_assert("meet.google.com → VIDEO",
              dns_domain_to_hint("meet.google.com") == PERSONA_VIDEO);
    return 0;
}

static char *test_domain_video_discord() {
    mu_assert("discord.media → VIDEO",
              dns_domain_to_hint("us-west1.discord.media") == PERSONA_VIDEO);
    return 0;
}

static char *test_domain_gaming_riot() {
    mu_assert("riotgames.com → GAMING",
              dns_domain_to_hint("lol.sgp.riotgames.com") == PERSONA_GAMING);
    return 0;
}

static char *test_domain_gaming_steam() {
    mu_assert("steampowered.com → GAMING",
              dns_domain_to_hint("store.steampowered.com") == PERSONA_GAMING);
    return 0;
}

static char *test_domain_gaming_epic() {
    mu_assert("epicgames.com → GAMING",
              dns_domain_to_hint("launcher.epicgames.com") == PERSONA_GAMING);
    return 0;
}

static char *test_domain_gaming_battlenet() {
    mu_assert("battle.net → GAMING",
              dns_domain_to_hint("eu.actual.battle.net") == PERSONA_GAMING);
    return 0;
}

static char *test_domain_voip_whatsapp() {
    mu_assert("whatsapp.net → VOIP",
              dns_domain_to_hint("media-ist1-1.cdn.whatsapp.net") == PERSONA_VOIP);
    return 0;
}

static char *test_domain_voip_signal() {
    mu_assert("signal.org → VOIP",
              dns_domain_to_hint("chat.signal.org") == PERSONA_VOIP);
    return 0;
}

static char *test_domain_unknown_generic() {
    mu_assert("cloudflare.com → UNKNOWN",
              dns_domain_to_hint("1.1.1.1.cloudflare.com") == PERSONA_UNKNOWN);
    return 0;
}

static char *test_domain_unknown_null() {
    mu_assert("NULL → UNKNOWN",
              dns_domain_to_hint(NULL) == PERSONA_UNKNOWN);
    return 0;
}

static char *test_domain_unknown_empty() {
    mu_assert("empty → UNKNOWN",
              dns_domain_to_hint("") == PERSONA_UNKNOWN);
    return 0;
}

static char *test_domain_exact_match() {
    /* Domain IS the suffix exactly (no subdomain) */
    mu_assert("zoom.us exact → VIDEO",
              dns_domain_to_hint("zoom.us") == PERSONA_VIDEO);
    return 0;
}

static char *test_domain_no_partial_match() {
    /* "notzoom.us" should NOT match "zoom.us" — no dot before suffix */
    mu_assert("notzoom.us → UNKNOWN",
              dns_domain_to_hint("notzoom.us") == PERSONA_UNKNOWN);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * DNS CACHE TESTS
 * ══════════════════════════════════════════════════════════════ */

static char *test_cache_insert_lookup() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    uint32_t ip;
    inet_pton(AF_INET, "172.217.14.110", &ip);

    dns_cache_insert(&cache, ip, "rr1---sn-abc.googlevideo.com", 300);

    persona_t result = dns_cache_lookup(&cache, ip);
    mu_assert("cache: googlevideo.com → STREAMING", result == PERSONA_STREAMING);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_cache_miss() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    uint32_t ip;
    inet_pton(AF_INET, "10.0.0.1", &ip);

    persona_t result = dns_cache_lookup(&cache, ip);
    mu_assert("cache: miss → UNKNOWN", result == PERSONA_UNKNOWN);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_cache_update() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    uint32_t ip;
    inet_pton(AF_INET, "1.2.3.4", &ip);

    /* First insert: generic domain */
    dns_cache_insert(&cache, ip, "example.com", 300);
    mu_assert("cache: example.com → UNKNOWN",
              dns_cache_lookup(&cache, ip) == PERSONA_UNKNOWN);

    /* Update same IP with streaming domain */
    dns_cache_insert(&cache, ip, "rr1.googlevideo.com", 300);
    mu_assert("cache: updated → STREAMING",
              dns_cache_lookup(&cache, ip) == PERSONA_STREAMING);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_cache_fill_eviction() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    /* Fill all 64 slots */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        uint32_t ip = htonl(0x0A000001 + (uint32_t)i);  /* 10.0.0.1 .. 10.0.0.64 */
        dns_cache_insert(&cache, ip, "example.com", 300);
    }

    /* Insert one more — should evict LRU */
    uint32_t new_ip = htonl(0x0A000100);  /* 10.0.1.0 */
    dns_cache_insert(&cache, new_ip, "rr1.googlevideo.com", 300);

    mu_assert("cache: new entry after eviction → STREAMING",
              dns_cache_lookup(&cache, new_ip) == PERSONA_STREAMING);

    dns_cache_destroy(&cache);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * DNS PARSER TESTS
 * ══════════════════════════════════════════════════════════════ */

/*
 * Construct a minimal valid DNS response for "rr1.googlevideo.com"
 * with one A record pointing to 172.217.14.110.
 *
 * Wire format (hex):
 *   Header:  0x1234 8180 0001 0001 0000 0000
 *   Question: 03 72 72 31 0b 67 6f 6f 67 6c 65 76 69 64 65 6f 03 63 6f 6d 00
 *             0001 0001
 *   Answer:  c0 0c (pointer to question name)
 *            0001 0001 00000258 0004 ac d9 0e 6e
 */
static char *test_parser_valid_response() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    uint8_t pkt[] = {
        /* Header: ID=0x1234, QR=1 RD=1 RA=1, QDCOUNT=1, ANCOUNT=1 */
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,

        /* Question: rr1.googlevideo.com, A, IN */
        0x03, 'r', 'r', '1',
        0x0b, 'g', 'o', 'o', 'g', 'l', 'e', 'v', 'i', 'd', 'e', 'o',
        0x03, 'c', 'o', 'm',
        0x00,                           /* end of name */
        0x00, 0x01,                     /* QTYPE = A */
        0x00, 0x01,                     /* QCLASS = IN */

        /* Answer: pointer to offset 12 (question name) */
        0xc0, 0x0c,                     /* name = compressed pointer */
        0x00, 0x01,                     /* TYPE = A */
        0x00, 0x01,                     /* CLASS = IN */
        0x00, 0x00, 0x02, 0x58,         /* TTL = 600 */
        0x00, 0x04,                     /* RDLENGTH = 4 */
        0xac, 0xd9, 0x0e, 0x6e          /* RDATA = 172.217.14.110 */
    };

    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser: should parse 1 A record", count == 1);

    /* Check cache entry */
    uint32_t ip;
    inet_pton(AF_INET, "172.217.14.110", &ip);
    persona_t hint = dns_cache_lookup(&cache, ip);
    mu_assert("parser: googlevideo → STREAMING in cache", hint == PERSONA_STREAMING);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_query_rejected() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    /* DNS query (QR=0) — should be rejected */
    uint8_t pkt[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 'w', 'w', 'w', 0x06, 'g', 'o', 'o', 'g', 'l', 'e',
        0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01
    };

    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser: query should be rejected (-1)", count == -1);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_too_short() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    uint8_t pkt[] = { 0x12, 0x34, 0x81, 0x80 };  /* only 4 bytes */
    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser: short packet → -1", count == -1);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_null_inputs() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    mu_assert("parser: NULL cache → -1", dns_parse_response(NULL, (uint8_t *)"x", 1) == -1);
    mu_assert("parser: NULL pkt → -1", dns_parse_response(&cache, NULL, 0) == -1);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_nxdomain_rejected() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    /* NXDOMAIN response (RCODE=3) */
    uint8_t pkt[] = {
        0x12, 0x34, 0x81, 0x83, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 'b', 'a', 'd', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01
    };

    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser: NXDOMAIN → -1", count == -1);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_zoom_response() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    /* DNS response for us04web.zoom.us → 170.114.10.1 */
    uint8_t pkt[] = {
        /* Header */
        0xAB, 0xCD, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,

        /* Question: us04web.zoom.us */
        0x07, 'u', 's', '0', '4', 'w', 'e', 'b',
        0x04, 'z', 'o', 'o', 'm',
        0x02, 'u', 's',
        0x00,
        0x00, 0x01, 0x00, 0x01,

        /* Answer: compressed name, A record */
        0xc0, 0x0c,
        0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x01, 0x2c,  /* TTL = 300 */
        0x00, 0x04,
        0xaa, 0x72, 0x0a, 0x01   /* 170.114.10.1 */
    };

    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser_zoom: should parse 1 A record", count == 1);

    uint32_t ip;
    inet_pton(AF_INET, "170.114.10.1", &ip);
    mu_assert("parser_zoom: zoom.us → VIDEO in cache",
              dns_cache_lookup(&cache, ip) == PERSONA_VIDEO);

    dns_cache_destroy(&cache);
    return 0;
}

static char *test_parser_riot_response() {
    dns_cache_t cache;
    dns_cache_init(&cache);

    /* DNS response for lol.sgp.riotgames.com → 104.160.131.1 */
    uint8_t pkt[] = {
        /* Header */
        0x00, 0x01, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,

        /* Question: lol.sgp.riotgames.com */
        0x03, 'l', 'o', 'l',
        0x03, 's', 'g', 'p',
        0x09, 'r', 'i', 'o', 't', 'g', 'a', 'm', 'e', 's',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01, 0x00, 0x01,

        /* Answer: compressed name, A record */
        0xc0, 0x0c,
        0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x3c,  /* TTL = 60 */
        0x00, 0x04,
        0x68, 0xa0, 0x83, 0x01   /* 104.160.131.1 */
    };

    int count = dns_parse_response(&cache, pkt, sizeof(pkt));
    mu_assert("parser_riot: should parse 1 A record", count == 1);

    uint32_t ip;
    inet_pton(AF_INET, "104.160.131.1", &ip);
    mu_assert("parser_riot: riotgames.com → GAMING in cache",
              dns_cache_lookup(&cache, ip) == PERSONA_GAMING);

    dns_cache_destroy(&cache);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * Service (v3) finer-grained domain classification
 * ══════════════════════════════════════════════════════════════ */

static char *test_svc_vod_googlevideo() {
    mu_assert("youtube VOD CDN → VIDEO_VOD",
              dns_domain_to_service("rr1---sn-abc.googlevideo.com") == SVC_VIDEO_VOD);
    return 0;
}
static char *test_svc_vod_netflix() {
    mu_assert("netflix CDN → VIDEO_VOD",
              dns_domain_to_service("ipv4-c001.nflxvideo.net") == SVC_VIDEO_VOD);
    return 0;
}
static char *test_svc_vod_spotify() {
    mu_assert("spotify audio CDN → VIDEO_VOD",
              dns_domain_to_service("audio-fa.scdn.co") == SVC_VIDEO_VOD);
    return 0;
}
static char *test_svc_live_twitch() {
    mu_assert("twitch CDN → VIDEO_LIVE (not VOD — it's live content)",
              dns_domain_to_service("video-edge-1.ttvnw.net") == SVC_VIDEO_LIVE);
    return 0;
}
static char *test_svc_conf_zoom() {
    mu_assert("zoom → VIDEO_CONF",
              dns_domain_to_service("us04web.zoom.us") == SVC_VIDEO_CONF);
    return 0;
}
static char *test_svc_conf_meet() {
    mu_assert("google meet → VIDEO_CONF (not generic google.com)",
              dns_domain_to_service("meet.google.com") == SVC_VIDEO_CONF);
    return 0;
}
static char *test_svc_voip_discord_media() {
    mu_assert("discord.media → VOIP_CALL (voice-specific, finer than old VIDEO)",
              dns_domain_to_service("us-west1.discord.media") == SVC_VOIP_CALL);
    return 0;
}
static char *test_svc_voip_whatsapp() {
    mu_assert("whatsapp → VOIP_CALL",
              dns_domain_to_service("media-ist1.whatsapp.net") == SVC_VOIP_CALL);
    return 0;
}
static char *test_svc_game_rt_riot() {
    mu_assert("riot → GAME_RT",
              dns_domain_to_service("lol.sgp.riotgames.com") == SVC_GAME_RT);
    return 0;
}
static char *test_svc_game_rt_steampowered() {
    mu_assert("steampowered → GAME_RT (store/matchmaking)",
              dns_domain_to_service("store.steampowered.com") == SVC_GAME_RT);
    return 0;
}
static char *test_svc_game_launcher_steamcontent() {
    mu_assert("steamcontent → GAME_LAUNCHER (bulk, distinct from matchmaking)",
              dns_domain_to_service("cdn.steamcontent.com") == SVC_GAME_LAUNCHER);
    return 0;
}
static char *test_svc_sync_dropbox() {
    mu_assert("dropbox → FILE_SYNC",
              dns_domain_to_service("client.dropbox.com") == SVC_FILE_SYNC);
    return 0;
}
static char *test_svc_bulk_github() {
    mu_assert("github → BULK_DL",
              dns_domain_to_service("raw.githubusercontent.com") == SVC_BULK_DL);
    return 0;
}
static char *test_svc_web_discordapp() {
    mu_assert("discordapp.com → WEB_INTERACTIVE (chat, not voice)",
              dns_domain_to_service("cdn.discordapp.com") == SVC_WEB_INTERACTIVE);
    return 0;
}
static char *test_svc_null_safe() {
    mu_assert("NULL → SVC_UNKNOWN", dns_domain_to_service(NULL) == SVC_UNKNOWN);
    mu_assert("empty → SVC_UNKNOWN", dns_domain_to_service("") == SVC_UNKNOWN);
    mu_assert("generic → SVC_UNKNOWN",
              dns_domain_to_service("1.1.1.1.cloudflare.com") == SVC_UNKNOWN);
    return 0;
}
static char *test_svc_cache_lookup() {
    dns_cache_t cache;
    dns_cache_init(&cache);
    uint32_t ip = 0x08080808;
    dns_cache_insert(&cache, ip, "us-west1.discord.media", 300);
    mu_assert("cache service lookup → VOIP_CALL",
              dns_cache_lookup_service(&cache, ip) == SVC_VOIP_CALL);
    mu_assert("cache persona still resolves → VOIP (derived)",
              dns_cache_lookup(&cache, ip) == PERSONA_VIDEO);
    /* Note: persona lookup uses the OLD persona table for backward
     * compat with device.c; service lookup uses the new table.
     * Both can coexist until device.c migrates to service-based. */
    dns_cache_destroy(&cache);
    return 0;
}

/* ══════════════════════════════════════════════════════════════ */

static char *all_tests() {
    /* Domain suffix tests */
    mu_run_test(test_domain_streaming_googlevideo);
    mu_run_test(test_domain_streaming_netflix);
    mu_run_test(test_domain_streaming_twitch);
    mu_run_test(test_domain_streaming_youtube);
    mu_run_test(test_domain_streaming_spotify);
    mu_run_test(test_domain_video_zoom);
    mu_run_test(test_domain_video_teams);
    mu_run_test(test_domain_video_meet);
    mu_run_test(test_domain_video_discord);
    mu_run_test(test_domain_gaming_riot);
    mu_run_test(test_domain_gaming_steam);
    mu_run_test(test_domain_gaming_epic);
    mu_run_test(test_domain_gaming_battlenet);
    mu_run_test(test_domain_voip_whatsapp);
    mu_run_test(test_domain_voip_signal);
    mu_run_test(test_domain_unknown_generic);
    mu_run_test(test_domain_unknown_null);
    mu_run_test(test_domain_unknown_empty);
    mu_run_test(test_domain_exact_match);
    mu_run_test(test_domain_no_partial_match);

    /* Cache tests */
    mu_run_test(test_cache_insert_lookup);
    mu_run_test(test_cache_miss);
    mu_run_test(test_cache_update);
    mu_run_test(test_cache_fill_eviction);

    /* Service (v3 finer-grained) tests */
    mu_run_test(test_svc_vod_googlevideo);
    mu_run_test(test_svc_vod_netflix);
    mu_run_test(test_svc_vod_spotify);
    mu_run_test(test_svc_live_twitch);
    mu_run_test(test_svc_conf_zoom);
    mu_run_test(test_svc_conf_meet);
    mu_run_test(test_svc_voip_discord_media);
    mu_run_test(test_svc_voip_whatsapp);
    mu_run_test(test_svc_game_rt_riot);
    mu_run_test(test_svc_game_rt_steampowered);
    mu_run_test(test_svc_game_launcher_steamcontent);
    mu_run_test(test_svc_sync_dropbox);
    mu_run_test(test_svc_bulk_github);
    mu_run_test(test_svc_web_discordapp);
    mu_run_test(test_svc_null_safe);
    mu_run_test(test_svc_cache_lookup);

    /* Parser tests */
    mu_run_test(test_parser_valid_response);
    mu_run_test(test_parser_query_rejected);
    mu_run_test(test_parser_too_short);
    mu_run_test(test_parser_null_inputs);
    mu_run_test(test_parser_nxdomain_rejected);
    mu_run_test(test_parser_zoom_response);
    mu_run_test(test_parser_riot_response);

    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char *result = all_tests();
    if (result != 0) {
        printf("FAILED: %s\n", result);
    } else {
        printf("ALL TESTS PASSED\n");
    }
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
