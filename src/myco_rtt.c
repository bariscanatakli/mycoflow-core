/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_rtt.c — Per-flow RTT engine
 *
 * Phase 5a: in-process stub table so the classifier + tests can exercise
 * the auto-correction logic. Phase 5b will swap the data source for a
 * BPF map populated by a kprobe on tcp_rcv_established reading
 * tcp_sock->srtt_us.
 *
 * The stub table is tiny (64 entries, linear probe). It's a test fixture,
 * not a production cache — real deployments reach past the stub into BPF.
 */
#include "myco_rtt.h"
#include "myco_log.h"

#include <stdlib.h>
#include <string.h>

#define RTT_STUB_SIZE 64

typedef struct {
    flow_key_t key;
    uint32_t   rtt_ms;
    int        used;
} rtt_stub_entry_t;

struct rtt_engine {
    rtt_stub_entry_t stub[RTT_STUB_SIZE];
    int              real_backed;   /* 1 when Phase 5b BPF map is wired up */
};

static int keys_equal(const flow_key_t *a, const flow_key_t *b) {
    return a->src_ip == b->src_ip && a->dst_ip == b->dst_ip &&
           a->src_port == b->src_port && a->dst_port == b->dst_port &&
           a->protocol == b->protocol;
}

rtt_engine_t *rtt_engine_open(void) {
    rtt_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) return NULL;
    /* Phase 5b will flip real_backed here when the BPF map attaches.
     * For now we log once so the operator knows RTT correction is idle. */
    log_msg(LOG_INFO, "rtt", "stub engine — Phase 5b BPF srtt not wired yet");
    return eng;
}

void rtt_engine_close(rtt_engine_t *eng) {
    free(eng);
}

uint32_t rtt_engine_lookup_ms(rtt_engine_t *eng, const flow_key_t *key) {
    if (!eng || !key) return 0;
    if (key->protocol != 6) return 0;   /* TCP only — UDP has no srtt */

    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        const rtt_stub_entry_t *e = &eng->stub[i];
        if (!e->used) continue;
        if (keys_equal(&e->key, key)) return e->rtt_ms;
    }
    return 0;
}

void rtt_engine_inject_stub(rtt_engine_t *eng, const flow_key_t *key,
                            uint32_t rtt_ms) {
    if (!eng || !key) return;

    /* Update-in-place if this flow is already stamped. */
    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        rtt_stub_entry_t *e = &eng->stub[i];
        if (e->used && keys_equal(&e->key, key)) {
            e->rtt_ms = rtt_ms;
            return;
        }
    }
    /* Else first free slot. Silently drops when full — test fixture. */
    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        rtt_stub_entry_t *e = &eng->stub[i];
        if (!e->used) {
            e->key    = *key;
            e->rtt_ms = rtt_ms;
            e->used   = 1;
            return;
        }
    }
}
