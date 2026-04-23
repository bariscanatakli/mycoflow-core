/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_flow.c — Userspace LRU flow table
 *
 * Tracks per-flow statistics using a simple hash table with LRU eviction.
 * Can be populated from /proc/net/nf_conntrack or future eBPF per-flow maps.
 */
#include "myco_flow.h"
#include "myco_log.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* ── Hash ───────────────────────────────────────────────────── */

static uint32_t flow_hash(const flow_key_t *key) {
    /* FNV-1a inspired simple hash */
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof(*key); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h % FLOW_TABLE_SIZE;
}

static int key_equal(const flow_key_t *a, const flow_key_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* ── Init ───────────────────────────────────────────────────── */

void flow_table_init(flow_table_t *ft) {
    if (!ft) return;
    memset(ft, 0, sizeof(*ft));
}

/* ── Lookup ─────────────────────────────────────────────────── */

const flow_entry_t *flow_table_lookup(const flow_table_t *ft,
                                      const flow_key_t *key) {
    if (!ft || !key) return NULL;

    uint32_t idx = flow_hash(key);
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) % FLOW_TABLE_SIZE;
        if (!ft->entries[slot].active) {
            return NULL; /* empty slot = not found */
        }
        if (key_equal(&ft->entries[slot].key, key)) {
            return &ft->entries[slot];
        }
    }
    return NULL;
}

/* ── Update (insert or increment) ───────────────────────────── */

static int find_lru_slot(const flow_table_t *ft) {
    int oldest = 0;
    double oldest_time = ft->entries[0].last_seen;
    for (int i = 1; i < FLOW_TABLE_SIZE; i++) {
        if (!ft->entries[i].active) {
            return i; /* prefer empty slot */
        }
        if (ft->entries[i].last_seen < oldest_time) {
            oldest_time = ft->entries[i].last_seen;
            oldest = i;
        }
    }
    return oldest;
}

int flow_table_update(flow_table_t *ft, const flow_key_t *key,
                      uint64_t packets, uint64_t rx_packets,
                      uint64_t bytes, uint64_t rx_bytes,
                      double now) {
    if (!ft || !key) return -1;

    uint32_t idx = flow_hash(key);

    /* Linear probe for existing entry */
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) % FLOW_TABLE_SIZE;
        if (!ft->entries[slot].active) {
            /* Insert here */
            ft->entries[slot].key        = *key;
            ft->entries[slot].packets    = packets;
            ft->entries[slot].rx_packets = rx_packets;
            ft->entries[slot].bytes      = bytes;
            ft->entries[slot].rx_bytes   = rx_bytes;
            ft->entries[slot].tx_delta   = bytes;
            ft->entries[slot].rx_delta   = rx_bytes;
            ft->entries[slot].last_seen  = now;
            ft->entries[slot].active     = 1;
            ft->count++;
            return 0;
        }
        if (key_equal(&ft->entries[slot].key, key)) {
            /* Update existing */
            ft->entries[slot].tx_delta   = bytes >= ft->entries[slot].bytes ? bytes - ft->entries[slot].bytes : 0;
            ft->entries[slot].rx_delta   = rx_bytes >= ft->entries[slot].rx_bytes ? rx_bytes - ft->entries[slot].rx_bytes : 0;
            ft->entries[slot].packets    = packets;
            ft->entries[slot].rx_packets = rx_packets;
            ft->entries[slot].bytes      = bytes;
            ft->entries[slot].rx_bytes   = rx_bytes;
            ft->entries[slot].last_seen  = now;
            return 0;
        }
    }

    /* Table full — evict LRU */
    int victim = find_lru_slot(ft);
    ft->entries[victim].key        = *key;
    ft->entries[victim].packets    = packets;
    ft->entries[victim].rx_packets = rx_packets;
    ft->entries[victim].bytes      = bytes;
    ft->entries[victim].rx_bytes   = rx_bytes;
    ft->entries[victim].last_seen  = now;
    ft->entries[victim].active     = 1;
    return 0;
}

/* ── Eviction ───────────────────────────────────────────────── */

void flow_table_evict_stale(flow_table_t *ft, double now, double max_age_s) {
    if (!ft) return;
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        if (ft->entries[i].active &&
            (now - ft->entries[i].last_seen) > max_age_s) {
            ft->entries[i].active = 0;
            ft->count--;
        }
    }
}

/* ── Active count ───────────────────────────────────────────── */

int flow_table_active_count(const flow_table_t *ft) {
    if (!ft) return 0;
    return ft->count;
}

/* ── Conntrack population ───────────────────────────────────── */

#ifdef HAVE_LIBNFCT

#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

struct ct_dump_cb_data {
    flow_table_t *ft;
    double now;
    int parsed;
};

static int ct_dump_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data) {
    struct ct_dump_cb_data *cb_data = (struct ct_dump_cb_data *)data;
    
    uint8_t l4_proto = nfct_get_attr_u8(ct, ATTR_L4PROTO);
    if (l4_proto != IPPROTO_TCP && l4_proto != IPPROTO_UDP) {
        return NFCT_CB_CONTINUE;
    }

    uint8_t l3_proto = nfct_get_attr_u8(ct, ATTR_L3PROTO);
    if (l3_proto != AF_INET) {
        return NFCT_CB_CONTINUE;
    }

    flow_key_t key;
    memset(&key, 0, sizeof(key));
    key.src_ip = nfct_get_attr_u32(ct, ATTR_IPV4_SRC);
    key.dst_ip = nfct_get_attr_u32(ct, ATTR_IPV4_DST);
    key.src_port = ntohs(nfct_get_attr_u16(ct, ATTR_PORT_SRC));
    key.dst_port = ntohs(nfct_get_attr_u16(ct, ATTR_PORT_DST));
    key.protocol = l4_proto;

    uint64_t tx_bytes = nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_BYTES);
    uint64_t tx_pkts  = nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_PACKETS);
    uint64_t rx_bytes = nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_BYTES);
    uint64_t rx_pkts  = nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_PACKETS);

    flow_table_update(cb_data->ft, &key, tx_pkts, rx_pkts, tx_bytes, rx_bytes, cb_data->now);
    cb_data->parsed++;

    return NFCT_CB_CONTINUE;
}

int flow_table_populate_conntrack(flow_table_t *ft, double now) {
    if (!ft) return -1;

    struct nfct_handle *h = nfct_open(CONNTRACK, 0);
    if (!h) {
        return -1;
    }

    struct ct_dump_cb_data cb_data = {
        .ft = ft,
        .now = now,
        .parsed = 0
    };

    nfct_callback_register(h, NFCT_T_ALL, ct_dump_cb, &cb_data);
    
    int ret = nfct_query(h, NFCT_Q_DUMP, NULL);
    nfct_close(h);
    
    if (ret == -1) {
        return -1;
    }
    
    return cb_data.parsed;
}

#else /* ! HAVE_LIBNFCT */

/*
 * Parse /proc/net/nf_conntrack to populate flow table.
 * Format: ipv4  2 tcp  6 300 ESTABLISHED src=... dst=... sport=... dport=...
 *         packets=N bytes=N ...
 * Returns number of flows parsed, or -1 on error.
 */
int flow_table_populate_conntrack(flow_table_t *ft, double now) {
    if (!ft) return -1;

    FILE *fp = fopen("/proc/net/nf_conntrack", "r");
    if (!fp) {
        /* conntrack not available (not loaded or no permissions) */
        return -1;
    }

    char line[1024];
    int parsed = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Only handle IPv4 TCP/UDP */
        int proto_num = 0;
        char src_str[64] = {0}, dst_str[64] = {0};
        int sport = 0, dport = 0;
        uint64_t pkts = 0, rx_pkts = 0, tx_byts = 0, rx_byts = 0;

        /* Extract protocol number */
        char *p = strstr(line, "tcp");
        if (p) {
            proto_num = 6;
        } else {
            p = strstr(line, "udp");
            if (p) {
                proto_num = 17;
            } else {
                continue; /* skip non-tcp/udp */
            }
        }

        /* Extract src/dst/sport/dport */
        p = strstr(line, "src=");
        if (p) sscanf(p, "src=%63s", src_str);
        p = strstr(line, "dst=");
        if (p) sscanf(p, "dst=%63s", dst_str);
        p = strstr(line, "sport=");
        if (p) sscanf(p, "sport=%d", &sport);
        p = strstr(line, "dport=");
        if (p) sscanf(p, "dport=%d", &dport);
        /* nf_conntrack has TWO packets= fields per line (forward, then reverse) */
        p = strstr(line, "packets=");
        if (p) {
            sscanf(p, "packets=%llu", (unsigned long long *)&pkts);
            p = strstr(p + 8, "packets=");
            if (p) sscanf(p, "packets=%llu", (unsigned long long *)&rx_pkts);
        }

        /* nf_conntrack has TWO bytes= fields per line:
         *   first  = forward direction (client→server, TX)
         *   second = reverse direction (server→client, RX)
         * This distinction enables STREAMING vs BULK classification. */
        p = strstr(line, "bytes=");
        if (p) {
            sscanf(p, "bytes=%llu", (unsigned long long *)&tx_byts);
            p = strstr(p + 6, "bytes=");   /* skip past first occurrence */
            if (p) sscanf(p, "bytes=%llu", (unsigned long long *)&rx_byts);
        }

        if (!src_str[0] || !dst_str[0]) continue;

        flow_key_t key;
        memset(&key, 0, sizeof(key));
        inet_pton(AF_INET, src_str, &key.src_ip);
        inet_pton(AF_INET, dst_str, &key.dst_ip);
        key.src_port = (uint16_t)sport;
        key.dst_port = (uint16_t)dport;
        key.protocol = (uint8_t)proto_num;

        flow_table_update(ft, &key, pkts, rx_pkts, tx_byts, rx_byts, now);
        parsed++;
    }

    fclose(fp);
    return parsed;
}

#endif /* HAVE_LIBNFCT */

/* ── Elephant flow detection ────────────────────────────────── */

int flow_table_has_elephant(const flow_table_t *ft, double dominance_ratio) {
    if (!ft || ft->count == 0) {
        return 0;
    }
    uint64_t total_bytes = 0;
    uint64_t max_bytes   = 0;
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        if (!ft->entries[i].active) {
            continue;
        }
        total_bytes += ft->entries[i].bytes;
        if (ft->entries[i].bytes > max_bytes) {
            max_bytes = ft->entries[i].bytes;
        }
    }
    if (total_bytes == 0) {
        return 0;
    }
    return ((double)max_bytes / (double)total_bytes) >= dominance_ratio;
}
