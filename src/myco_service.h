/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_service.h — Per-flow service taxonomy (L1 of v3 flow-aware arch)
 *
 * A flow belongs to exactly one service_t at any moment. Unlike persona_t
 * (which describes a whole device's role), service_t describes a single
 * 5-tuple flow. The DSCP mark applied to a flow's packets is derived from
 * its service_t plus the owning device's priority profile (see myco_profile.h).
 *
 * See docs/architecture-v3-flow-aware.md §3 for the full taxonomy.
 */
#ifndef MYCO_SERVICE_H
#define MYCO_SERVICE_H

#include "myco_types.h"

#include <stdint.h>

/* ── Service enum ────────────────────────────────────────────────
 * Ordered roughly from most to least latency-sensitive; stable integer
 * values are part of the on-wire contract (ct mark, LuCI JSON).
 */
typedef enum {
    SVC_UNKNOWN          = 0,
    SVC_GAME_RT          = 1,   /* Real-time gameplay (CS2, Valorant, LoL) */
    SVC_VOIP_CALL        = 2,   /* Discord/WhatsApp voice */
    SVC_VIDEO_CONF       = 3,   /* Zoom/Meet/Teams */
    SVC_VIDEO_LIVE       = 4,   /* Twitch/YouTube Live */
    SVC_VIDEO_VOD        = 5,   /* YouTube/Netflix buffered playback */
    SVC_WEB_INTERACTIVE  = 6,   /* Normal browsing, API calls, SSH control */
    SVC_BULK_DL          = 7,   /* apt, git clone, generic downloads */
    SVC_FILE_SYNC        = 8,   /* Dropbox/iCloud/OneDrive/Drive */
    SVC_TORRENT          = 9,   /* BitTorrent */
    SVC_GAME_LAUNCHER    = 10,  /* Steam/Epic content delivery (bulk, not RT) */
    SVC_SYSTEM           = 11,  /* DNS/NTP/DHCP/mDNS */
} service_t;

#define SERVICE_COUNT 12

/* ── Names & mappings ──────────────────────────────────────────── */

/* Short canonical name, suitable for logs and JSON ("game_rt", "voip_call"…).
 * Returns "unknown" for out-of-range values — never NULL. */
const char *service_name(service_t svc);

/* service_t → persona_t (device-level role). Used when deriving the winning
 * device persona from the set of active services. */
persona_t service_to_persona(service_t svc);

/* service_t → default conntrack mark value (profile-independent).
 * The mark → DSCP mapping in myco_act.c depends on the active profile;
 * the mark value itself is compact and stable. Returns 0 (MARK_UNKNOWN)
 * for SVC_UNKNOWN or out-of-range. */
uint8_t service_to_ct_mark(service_t svc);

/* ── Flow service state ──────────────────────────────────────────
 * One entry per tracked flow (keyed by 5-tuple). Lives in the
 * flow_service_table_t (myco_flow.h addition, Phase 3f).
 */
typedef struct {
    uint32_t  src_ip;         /* network byte order */
    uint32_t  dst_ip;
    uint16_t  src_port;       /* host byte order */
    uint16_t  dst_port;
    uint8_t   proto;          /* IPPROTO_TCP / IPPROTO_UDP */
    service_t service;        /* last classified service */
    uint8_t   ct_mark;        /* last mark pushed to kernel (0 = none yet) */
    double    detected_at;    /* monotonic timestamp of first classification */
    double    last_confirmed; /* last cycle the classification was re-asserted */
    uint8_t   stable;         /* 1 = survived 2 consecutive classifications */
} flow_service_t;

/* ── Classification (Phase 3b–3d wire-up) ──────────────────────── */

/* Per-signal evidence passed into the 3-signal weighted voter.
 * Each hint is either SVC_UNKNOWN (no signal) or a concrete service_t. */
typedef struct {
    service_t dns_hint;       /* from myco_dns: dst_ip → domain → service */
    service_t port_hint;      /* from myco_hint: dst_port → service */
    service_t behavior_hint;  /* from flow metrics: pkt size / bw / udp_ratio */
} service_signals_t;

/* Weighted voter (see architecture §5):
 *   score[svc] = 0.6*(dns==svc) + 0.3*(port==svc) + 0.1*(behavior==svc)
 *   return argmax if max >= 0.3, else SVC_UNKNOWN
 *
 * Pure function, no I/O, no globals. Unit-testable.
 */
service_t service_classify(const service_signals_t *signals);

/* ── Behavior-based signal producer (Phase 3d) ──────────────────
 * Observable per-flow features, pre-computed by the caller.
 * proto: IPPROTO_TCP (6) or IPPROTO_UDP (17).
 * avg_pkt_size: (tx_bytes + rx_bytes) / (tx_pkts + rx_pkts).
 * bw_bps: 8 * (tx_delta + rx_delta) / window_s (bidirectional).
 * rx_ratio: rx_delta / (tx_delta + rx_delta) in the current window (0..1).
 * pkts_total: cumulative tx+rx packet count (used to gate decisions on
 *             flows too young to characterise).
 */
typedef struct {
    uint8_t  proto;
    double   avg_pkt_size;
    double   bw_bps;
    double   rx_ratio;
    uint64_t pkts_total;
} flow_features_t;

/* Infer a service_t from behavioral fingerprints. Intentionally conservative:
 * returns SVC_UNKNOWN when the pattern is ambiguous (e.g. idle flow, or a
 * bulk-ish download that behavior alone can't separate from a VOD).
 *
 * Weighted at 0.1 in service_classify(), so this is a tiebreaker —
 * getting it wrong never flips a DNS/port verdict on its own. */
service_t service_infer_behavior(const flow_features_t *feat);

#endif /* MYCO_SERVICE_H */
