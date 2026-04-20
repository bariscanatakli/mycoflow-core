/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_classifier.h — Per-flow service classifier orchestrator
 *
 * Owns a flow_service_table_t parallel to the main flow_table_t.
 * Each tick walks the active flows, runs the 3-signal voter, and
 * pushes a conntrack mark only after two consecutive identical
 * verdicts (stability gate — protects against DNS/port mid-flight
 * flapping).
 *
 *   flow stats ─┐
 *   DNS cache ──┼──► service_classify() ──► flow_service_t
 *   port table ─┘                              │
 *                                              ▼
 *                              stability gate ── ct mark push
 *
 * See docs/architecture-v3-flow-aware.md §5/§6.
 */
#ifndef MYCO_CLASSIFIER_H
#define MYCO_CLASSIFIER_H

#include "myco_dns.h"
#include "myco_flow.h"
#include "myco_mark.h"
#include "myco_rtt.h"
#include "myco_service.h"

typedef struct flow_service_table flow_service_table_t;

/* Allocate + zero the per-flow service table. Returns NULL on OOM. */
flow_service_table_t *classifier_create(void);

/* Free the table. Safe on NULL. */
void classifier_destroy(flow_service_table_t *tab);

/* One classification pass.
 *
 *   ft        : latest flow table snapshot (read-only)
 *   dns       : DNS cache for IP→service lookup (may be NULL)
 *   eng       : mark engine for ct mark push (may be NULL — stub/off)
 *   rtt       : RTT engine for auto-correction (may be NULL — skip)
 *   now       : monotonic time (seconds)
 *   window_s  : length of the delta window (flow_entry_t.tx_delta etc.
 *               accumulate over this interval). Used for bw computation.
 *
 * Side effects:
 *   - Upserts a flow_service_t entry per flow in ft.
 *   - Calls mark_engine_set() for flows whose verdict just became
 *     stable (and whose mark changed from whatever was there before).
 *   - When `rtt` is non-NULL, runs the auto-corrector: demotes a flow's
 *     ct_mark after two consecutive ticks with RTT > target×1.5; re-
 *     promotes after two consecutive recovered ticks.
 *   - Evicts entries for flows no longer in ft.
 */
void classifier_tick(flow_service_table_t *tab,
                     const flow_table_t *ft,
                     dns_cache_t *dns,
                     mark_engine_t *eng,
                     rtt_engine_t *rtt,
                     double now,
                     double window_s);

/* Observability: last verdict for a flow. SVC_UNKNOWN if not tracked. */
service_t classifier_get_service(const flow_service_table_t *tab,
                                 const flow_key_t *key);

/* Observability: number of active entries. */
int classifier_active_count(const flow_service_table_t *tab);

/* Populate `out_counts[service_t] = #flows of that service` for flows
 * whose src_ip matches `device_ip` (network byte order). out_counts must
 * have SERVICE_COUNT entries; we zero it before filling. */
void classifier_device_counts(const flow_service_table_t *tab,
                              uint32_t device_ip,
                              int *out_counts);

#endif /* MYCO_CLASSIFIER_H */
