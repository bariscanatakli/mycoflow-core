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
                      uint64_t packets, uint64_t bytes, double now) {
    if (!ft || !key) return -1;

    uint32_t idx = flow_hash(key);

    /* Linear probe for existing entry */
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) % FLOW_TABLE_SIZE;
        if (!ft->entries[slot].active) {
            /* Insert here */
            ft->entries[slot].key       = *key;
            ft->entries[slot].packets   = packets;
            ft->entries[slot].bytes     = bytes;
            ft->entries[slot].last_seen = now;
            ft->entries[slot].active    = 1;
            ft->count++;
            return 0;
        }
        if (key_equal(&ft->entries[slot].key, key)) {
            /* Update existing */
            ft->entries[slot].packets   = packets;
            ft->entries[slot].bytes     = bytes;
            ft->entries[slot].last_seen = now;
            return 0;
        }
    }

    /* Table full — evict LRU */
    int victim = find_lru_slot(ft);
    ft->entries[victim].key       = *key;
    ft->entries[victim].packets   = packets;
    ft->entries[victim].bytes     = bytes;
    ft->entries[victim].last_seen = now;
    ft->entries[victim].active    = 1;
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
        uint64_t pkts = 0, byts = 0;

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
        p = strstr(line, "packets=");
        if (p) sscanf(p, "packets=%llu", (unsigned long long *)&pkts);
        p = strstr(line, "bytes=");
        if (p) sscanf(p, "bytes=%llu", (unsigned long long *)&byts);

        if (!src_str[0] || !dst_str[0]) continue;

        flow_key_t key;
        memset(&key, 0, sizeof(key));
        inet_pton(AF_INET, src_str, &key.src_ip);
        inet_pton(AF_INET, dst_str, &key.dst_ip);
        key.src_port = (uint16_t)sport;
        key.dst_port = (uint16_t)dport;
        key.protocol = (uint8_t)proto_num;

        flow_table_update(ft, &key, pkts, byts, now);
        parsed++;
    }

    fclose(fp);
    return parsed;
}

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
