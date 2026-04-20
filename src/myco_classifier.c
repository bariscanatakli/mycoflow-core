/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_classifier.c — Per-flow service classifier orchestrator
 *
 * Maintains a parallel table keyed by flow_key_t. Fixed size, linear
 * probe (same strategy as flow_table_t). A typical home network has
 * 50–150 active flows; 1024 is comfortable headroom.
 *
 * Stability gate: a flow_service_t.stable flag is set only when two
 * consecutive ticks agree on the same non-UNKNOWN service. Unstable
 * verdicts never push ct marks — this avoids whipsawing DSCP during
 * the first second of a flow when DNS may still be propagating.
 */
#include "myco_classifier.h"
#include "myco_hint.h"
#include "myco_log.h"

#include <stdlib.h>
#include <string.h>

#define FST_SIZE FLOW_TABLE_SIZE

struct flow_service_table {
    flow_service_t entries[FST_SIZE];
    int            count;
};

static int keys_equal(const flow_key_t *a, const flow_key_t *b) {
    return a->src_ip == b->src_ip && a->dst_ip == b->dst_ip &&
           a->src_port == b->src_port && a->dst_port == b->dst_port &&
           a->protocol == b->protocol;
}

static flow_service_t *fst_find(flow_service_table_t *tab, const flow_key_t *key) {
    for (int i = 0; i < FST_SIZE; i++) {
        flow_service_t *e = &tab->entries[i];
        if (e->detected_at == 0.0 && e->last_confirmed == 0.0) continue;
        flow_key_t k = {
            .src_ip = e->src_ip, .dst_ip = e->dst_ip,
            .src_port = e->src_port, .dst_port = e->dst_port,
            .protocol = e->proto,
        };
        if (keys_equal(&k, key)) return e;
    }
    return NULL;
}

/* Find a free slot, or evict the stalest entry (lowest last_confirmed). */
static flow_service_t *fst_slot(flow_service_table_t *tab) {
    flow_service_t *stalest = NULL;
    double oldest = 1e18;
    for (int i = 0; i < FST_SIZE; i++) {
        flow_service_t *e = &tab->entries[i];
        if (e->detected_at == 0.0 && e->last_confirmed == 0.0) {
            return e;
        }
        if (e->last_confirmed < oldest) {
            oldest = e->last_confirmed;
            stalest = e;
        }
    }
    if (stalest) {
        memset(stalest, 0, sizeof(*stalest));
        if (tab->count > 0) tab->count--;
    }
    return stalest;
}

flow_service_table_t *classifier_create(void) {
    flow_service_table_t *tab = calloc(1, sizeof(*tab));
    return tab;
}

void classifier_destroy(flow_service_table_t *tab) {
    free(tab);
}

static void compute_features(const flow_entry_t *fe, double window_s,
                             flow_features_t *out) {
    uint64_t pkts = fe->packets + fe->rx_packets;
    uint64_t bytes = fe->bytes + fe->rx_bytes;

    out->proto        = fe->key.protocol;
    out->pkts_total   = pkts;
    out->avg_pkt_size = pkts > 0 ? (double)bytes / (double)pkts : 0.0;
    out->bw_bps       = window_s > 0.0
        ? (double)(fe->tx_delta + fe->rx_delta) * 8.0 / window_s
        : 0.0;

    uint64_t total_delta = fe->tx_delta + fe->rx_delta;
    out->rx_ratio = total_delta > 0
        ? (double)fe->rx_delta / (double)total_delta
        : 0.5;
}

void classifier_tick(flow_service_table_t *tab,
                     const flow_table_t *ft,
                     dns_cache_t *dns,
                     mark_engine_t *eng,
                     double now,
                     double window_s) {
    if (!tab || !ft) return;

    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        const flow_entry_t *fe = &ft->entries[i];
        if (!fe->active) continue;

        /* ── Gather three signals ────────────────────────────── */
        service_signals_t sig = { SVC_UNKNOWN, SVC_UNKNOWN, SVC_UNKNOWN };

        if (dns) {
            sig.dns_hint = dns_cache_lookup_service(dns, fe->key.dst_ip);
        }
        sig.port_hint = service_from_port(fe->key.protocol, fe->key.dst_port);

        flow_features_t feat;
        compute_features(fe, window_s, &feat);
        sig.behavior_hint = service_infer_behavior(&feat);

        service_t verdict = service_classify(&sig);

        /* ── Upsert into fst ────────────────────────────────── */
        flow_service_t *fs = fst_find(tab, &fe->key);
        if (!fs) {
            if (verdict == SVC_UNKNOWN) continue;  /* don't track idle unknowns */
            fs = fst_slot(tab);
            if (!fs) continue;
            fs->src_ip = fe->key.src_ip;
            fs->dst_ip = fe->key.dst_ip;
            fs->src_port = fe->key.src_port;
            fs->dst_port = fe->key.dst_port;
            fs->proto = fe->key.protocol;
            fs->service = verdict;
            fs->ct_mark = 0;
            fs->detected_at = now;
            fs->last_confirmed = now;
            fs->stable = 0;
            tab->count++;
            continue;
        }

        /* ── Stability logic ────────────────────────────────── */
        fs->last_confirmed = now;
        if (verdict == SVC_UNKNOWN) {
            /* Keep last verdict but don't promote or push. */
            continue;
        }
        if (verdict == fs->service) {
            if (!fs->stable) {
                fs->stable = 1;   /* second consecutive match — promote */
                uint8_t new_mark = service_to_ct_mark(verdict);
                if (new_mark != fs->ct_mark) {
                    if (mark_engine_set(eng, &fe->key, new_mark) == 0) {
                        fs->ct_mark = new_mark;
                    }
                }
            }
        } else {
            /* Verdict flipped — restart stability window. */
            fs->service = verdict;
            fs->stable = 0;
        }
    }

    /* ── Evict entries whose underlying flow is gone ─────────── */
    for (int i = 0; i < FST_SIZE; i++) {
        flow_service_t *fs = &tab->entries[i];
        if (fs->detected_at == 0.0 && fs->last_confirmed == 0.0) continue;
        if (fs->last_confirmed < now - 30.0) {
            memset(fs, 0, sizeof(*fs));
            if (tab->count > 0) tab->count--;
        }
    }
}

service_t classifier_get_service(const flow_service_table_t *tab,
                                 const flow_key_t *key) {
    if (!tab || !key) return SVC_UNKNOWN;
    for (int i = 0; i < FST_SIZE; i++) {
        const flow_service_t *e = &tab->entries[i];
        if (e->detected_at == 0.0 && e->last_confirmed == 0.0) continue;
        flow_key_t k = {
            .src_ip = e->src_ip, .dst_ip = e->dst_ip,
            .src_port = e->src_port, .dst_port = e->dst_port,
            .protocol = e->proto,
        };
        if (keys_equal(&k, key)) return e->service;
    }
    return SVC_UNKNOWN;
}

int classifier_active_count(const flow_service_table_t *tab) {
    return tab ? tab->count : 0;
}

void classifier_device_counts(const flow_service_table_t *tab,
                              uint32_t device_ip,
                              int *out_counts) {
    if (!out_counts) return;
    memset(out_counts, 0, sizeof(int) * SERVICE_COUNT);
    if (!tab) return;
    for (int i = 0; i < FST_SIZE; i++) {
        const flow_service_t *e = &tab->entries[i];
        if (e->detected_at == 0.0 && e->last_confirmed == 0.0) continue;
        if (e->src_ip != device_ip) continue;
        if ((int)e->service > 0 && (int)e->service < SERVICE_COUNT) {
            out_counts[e->service]++;
        }
    }
}
