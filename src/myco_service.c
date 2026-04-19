/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_service.c — Per-flow service taxonomy (L1 of v3 flow-aware arch)
 *
 * Phase 3a: enum plumbing, mappings, and the 3-signal weighted voter.
 * Signal producers (DNS, port, behavior) live in their own modules and
 * feed service_signals_t into service_classify(). This file is I/O-free.
 */
#include "myco_service.h"

#include <stddef.h>

/* ── Canonical names ─────────────────────────────────────────────
 * Indexed by service_t value. Keep in sync with the enum.
 */
static const char *const SERVICE_NAMES[SERVICE_COUNT] = {
    [SVC_UNKNOWN]         = "unknown",
    [SVC_GAME_RT]         = "game_rt",
    [SVC_VOIP_CALL]       = "voip_call",
    [SVC_VIDEO_CONF]      = "video_conf",
    [SVC_VIDEO_LIVE]      = "video_live",
    [SVC_VIDEO_VOD]       = "video_vod",
    [SVC_WEB_INTERACTIVE] = "web_interactive",
    [SVC_BULK_DL]         = "bulk_dl",
    [SVC_FILE_SYNC]       = "file_sync",
    [SVC_TORRENT]         = "torrent",
    [SVC_GAME_LAUNCHER]   = "game_launcher",
    [SVC_SYSTEM]          = "system",
};

const char *service_name(service_t svc) {
    if ((int)svc < 0 || (int)svc >= SERVICE_COUNT) {
        return "unknown";
    }
    return SERVICE_NAMES[svc];
}

/* ── service_t → persona_t ──────────────────────────────────────
 * Device-level role derivation. Each service maps to exactly one persona;
 * the device's winning persona is chosen from the set of active services
 * by myco_profile.c using the profile's winner_priority list.
 */
persona_t service_to_persona(service_t svc) {
    switch (svc) {
        case SVC_GAME_RT:         return PERSONA_GAMING;
        case SVC_VOIP_CALL:       return PERSONA_VOIP;
        case SVC_VIDEO_CONF:      return PERSONA_VIDEO;
        case SVC_VIDEO_LIVE:      return PERSONA_VIDEO;
        case SVC_VIDEO_VOD:       return PERSONA_STREAMING;
        case SVC_WEB_INTERACTIVE: return PERSONA_UNKNOWN;  /* no role */
        case SVC_BULK_DL:         return PERSONA_BULK;
        case SVC_FILE_SYNC:       return PERSONA_BULK;
        case SVC_TORRENT:         return PERSONA_TORRENT;
        case SVC_GAME_LAUNCHER:   return PERSONA_BULK;
        case SVC_SYSTEM:          return PERSONA_UNKNOWN;
        case SVC_UNKNOWN:
        default:                  return PERSONA_UNKNOWN;
    }
}

/* ── service_t → default ct mark ────────────────────────────────
 * The mark value is compact and profile-independent. The mark → DSCP
 * mangle table is rebuilt by myco_act.c whenever the active profile
 * changes (see §6 of the architecture doc).
 */
uint8_t service_to_ct_mark(service_t svc) {
    if ((int)svc < 0 || (int)svc >= SERVICE_COUNT) {
        return 0;
    }
    return (uint8_t)svc;  /* 1:1 until we need to decouple */
}

/* ── 3-signal weighted voter ────────────────────────────────────
 * Architecture §5:
 *   score[svc] = 0.6*(dns==svc) + 0.3*(port==svc) + 0.1*(behavior==svc)
 * Return argmax if max >= 0.3, else SVC_UNKNOWN.
 *
 * The 0.3 floor means: a lone port hint is enough to classify, but a lone
 * behavioral hint is not. This matches the intent: DNS is strongest,
 * behavior is a tie-breaker.
 */
service_t service_classify(const service_signals_t *signals) {
    if (!signals) {
        return SVC_UNKNOWN;
    }

    double score[SERVICE_COUNT] = {0.0};

    if (signals->dns_hint > SVC_UNKNOWN && signals->dns_hint < SERVICE_COUNT) {
        score[signals->dns_hint] += 0.6;
    }
    if (signals->port_hint > SVC_UNKNOWN && signals->port_hint < SERVICE_COUNT) {
        score[signals->port_hint] += 0.3;
    }
    if (signals->behavior_hint > SVC_UNKNOWN && signals->behavior_hint < SERVICE_COUNT) {
        score[signals->behavior_hint] += 0.1;
    }

    int best = SVC_UNKNOWN;
    double best_score = 0.0;
    for (int i = 1; i < SERVICE_COUNT; i++) {
        if (score[i] > best_score) {
            best_score = score[i];
            best = i;
        }
    }

    if (best_score < 0.3) {
        return SVC_UNKNOWN;
    }
    return (service_t)best;
}

/* ── Behavior-based inference ───────────────────────────────────
 * Thresholds tuned from live captures (see docs/architecture-v3):
 *
 *   VOIP_CALL   : UDP, avg pkt 60..220B, bw 10..150 kbps, ~symmetric.
 *                 Opus/SILK codecs send ~50 pkt/s of 80-160B; WhatsApp
 *                 typically 30-80 kbps.
 *   GAME_RT     : UDP, avg pkt 40..300B, bw < 600 kbps. Tighter than VOIP
 *                 on packet size floor (DTLS handshakes) but higher bw
 *                 ceiling (Valorant tick 128Hz ≈ 300 kbps).
 *   VIDEO_CONF  : UDP, avg pkt 400..1100B, bw 0.3..4 Mbps, mixed rx/tx.
 *                 Zoom/Meet in 720p call mode.
 *   VIDEO_LIVE /
 *   VIDEO_VOD   : avg pkt > 900B, rx_ratio > 0.85, bw > 1 Mbps.
 *                 Can't separate live vs VOD behaviorally — DNS decides.
 *                 We return SVC_VIDEO_VOD because VOD (YouTube) is the
 *                 overwhelmingly more common case; if DNS/port says LIVE,
 *                 their 0.6/0.3 weights win over our 0.1.
 *   BULK_DL     : TCP, rx_ratio > 0.9, bw > 2 Mbps, avg pkt ~MSS (>1300B).
 *                 Harder to separate from VOD on behavior; we don't try.
 *
 * All numbers require pkts_total >= 20 so we don't speculate on
 * flows that just opened.
 */
service_t service_infer_behavior(const flow_features_t *feat) {
    if (!feat)                    return SVC_UNKNOWN;
    if (feat->pkts_total < 20)    return SVC_UNKNOWN;   /* too young to tell */
    if (feat->bw_bps <= 0.0)      return SVC_UNKNOWN;   /* idle this window */

    const uint8_t  proto  = feat->proto;
    const double   apkt   = feat->avg_pkt_size;
    const double   bw     = feat->bw_bps;
    const double   rxr    = feat->rx_ratio;

    /* VOIP_CALL — tiny packets, very low bw, roughly symmetric, UDP */
    if (proto == 17 &&
        apkt >= 60.0 && apkt <= 220.0 &&
        bw   >= 10000.0 && bw <= 150000.0 &&
        rxr  >= 0.30 && rxr <= 0.70) {
        return SVC_VOIP_CALL;
    }

    /* VIDEO_CONF — medium packets, moderate bw, mixed direction, UDP */
    if (proto == 17 &&
        apkt >= 400.0 && apkt <= 1100.0 &&
        bw   >= 300000.0 && bw <= 4000000.0 &&
        rxr  >= 0.25 && rxr <= 0.80) {
        return SVC_VIDEO_CONF;
    }

    /* GAME_RT — small packets, low bw, UDP (broader catch-all after
     * the VOIP/CONF checks above) */
    if (proto == 17 &&
        apkt >= 40.0 && apkt <= 300.0 &&
        bw   <= 600000.0) {
        return SVC_GAME_RT;
    }

    /* VOD-ish bulk media — large packets, heavily asymmetric rx>>tx, high bw */
    if (apkt >= 900.0 && rxr >= 0.85 && bw >= 1000000.0) {
        return SVC_VIDEO_VOD;
    }

    return SVC_UNKNOWN;
}
