/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_rtt.h — Per-flow RTT engine (Phase 5)
 *
 * Feeds smoothed RTT (srtt) samples to the classifier's auto-corrector.
 * Real implementation in Phase 5b uses a BPF kprobe on tcp_rcv_established
 * that writes srtt_us into a BPF_MAP_TYPE_HASH keyed by the 5-tuple.
 *
 * Phase 5a ships a stub: open/close succeed, lookups return 0 ("unknown").
 * A test-injection hook (rtt_engine_inject_stub) lets unit tests seed
 * specific RTT values for flows without needing a live kernel.
 *
 * UDP flows have no protocol RTT; the engine returns 0 for them.
 * classifier_tick() treats 0 as "skip auto-correction this tick".
 */
#ifndef MYCO_RTT_H
#define MYCO_RTT_H

#include "myco_flow.h"

#include <stdint.h>

typedef struct rtt_engine rtt_engine_t;

/* Open an RTT engine. Returns NULL on fatal error — callers should
 * continue without RTT-based auto-correction in that case. */
rtt_engine_t *rtt_engine_open(void);

void rtt_engine_close(rtt_engine_t *eng);

/* Fetch the smoothed RTT for a flow in milliseconds. Returns 0 if the
 * flow is UDP, not yet measured, or unknown. */
uint32_t rtt_engine_lookup_ms(rtt_engine_t *eng, const flow_key_t *key);

/* Test hook — stamp an RTT sample into the stub engine. No-op on the
 * real (eBPF) impl; exposed so unit tests can seed RTT values without
 * spinning up a kernel probe. */
void rtt_engine_inject_stub(rtt_engine_t *eng, const flow_key_t *key,
                            uint32_t rtt_ms);

#endif /* MYCO_RTT_H */
