/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_persona.c — Persona inference heuristics
 */
#include "myco_persona.h"
#include "myco_log.h"

#include <string.h>

/*
 * Hierarchical 6-class decision tree.
 *
 * Uses per-device metrics from metrics_t:
 *   active_flows   — conntrack flow count for this device
 *   elephant_flow  — 1 if one flow carries >60% of device bytes
 *   tx_bps / rx_bps — per-device TX and RX bandwidth (set in device.c)
 *   avg_pkt_size   — average packet size across all device flows
 *
 * Key discriminators:
 *   TORRENT    : many simultaneous flows (>100) AND significant bandwidth (>500 kbps)
 *   GAMING     : high UDP ratio (>=25%) + moderate rx (<=30 Mbps)
 *   STREAMING  : high UDP ratio (>=25%) + heavy rx (>30 Mbps, QUIC video)
 *   BULK       : elephant flow (one flow >60% of cycle delta) OR high rx TCP download
 *   VOIP       : tiny packets (<120 B) + very low bandwidth (<200 kbps)
 *   GAMING     : small packets (<350 B) + few flows (<8)
 *   VIDEO      : mid-range bandwidth (200 kbps – 8 Mbps)
 *   UNKNOWN    : everything else
 *
 * Priority: first matching rule wins.
 */
static persona_t decide_persona(const metrics_t *metrics, persona_t hint) {
    if (!metrics) {
        return PERSONA_UNKNOWN;
    }

    double tx_bps = metrics->tx_bps;
    double rx_bps = metrics->rx_bps;
    double bw_bps = tx_bps + rx_bps;
    double tx_rx_ratio = tx_bps / (rx_bps + 1.0);
    int flows = metrics->active_flows;

    /* Rule 0 — Hint pre-check: if behavior would return UNKNOWN but we
     * have a port hint, use the hint. This catches low-traffic periods
     * where behavioral signals are too weak to classify. */

    /* Rule 1 — TORRENT: swarm of connections + significant bandwidth.
     * Strong behavioral signal — hint CANNOT override this.
     * Even if some flows hit gaming ports, 100+ flows is a swarm. */
    if (flows > 100 && bw_bps > 500000.0) {
        return PERSONA_TORRENT;
    }

    int udp_flows = metrics->udp_flows;
    double udp_ratio = (flows > 0) ? (double)udp_flows / (double)flows : 0.0;

    /* Rule 2a — STREAMING: heavy UDP download (QUIC video, rx > 30 Mbps).
     * But if hint says GAMING, trust the hint — could be a UDP game with
     * heavy download (large map, spectator mode). */
    if (udp_ratio >= 0.25 && rx_bps > 30000000.0 && tx_rx_ratio < 0.30) {
        if (hint == PERSONA_GAMING) {
            return PERSONA_GAMING;
        }
        return PERSONA_STREAMING;
    }

    /* Rule 2b — GAMING: high UDP ratio + moderate bandwidth.
     * Already correct for UDP games — hint reinforces.
     * Exception: if hint says VIDEO (e.g., Zoom on port 8801), trust the hint.
     * Video calls and games both produce high UDP ratios, but port resolves it. */
    if (udp_ratio >= 0.25 && bw_bps > 500000.0) {
        if (hint == PERSONA_VIDEO) {
            return PERSONA_VIDEO;
        }
        return PERSONA_GAMING;
    }

    /* Rule 3 — BULK: elephant flow or heavy TCP download.
     * KEY FIX: hint overrides elephant detection for interactive traffic.
     * LoL (TCP game) has one dominant connection → elephant_flow=1 → was BULK.
     * Now: if hint says GAMING/VOIP/VIDEO/STREAMING, trust the hint.
     * Sanity check: GAMING hint with >20 Mbps is likely a launcher download. */
    if (metrics->elephant_flow) {
        if (hint == PERSONA_GAMING && bw_bps <= 20000000.0) {
            return PERSONA_GAMING;
        }
        if (hint == PERSONA_VOIP && bw_bps <= 5000000.0) {
            return PERSONA_VOIP;
        }
        if (hint == PERSONA_VIDEO) {
            return PERSONA_VIDEO;
        }
        if (hint == PERSONA_STREAMING) {
            return PERSONA_STREAMING;
        }
        return PERSONA_BULK;
    }
    if (rx_bps > 5000000.0 && tx_rx_ratio < 0.30 && udp_ratio < 0.25) {
        if (hint == PERSONA_GAMING && bw_bps <= 20000000.0) {
            return PERSONA_GAMING;
        }
        if (hint == PERSONA_STREAMING) {
            return PERSONA_STREAMING;
        }
        return PERSONA_BULK;
    }

    /* Rule 4 — VOIP: tiny codec packets, very low rate.
     * Strong behavioral signal — no hint needed. */
    if (metrics->avg_pkt_size > 0.0 &&
        metrics->avg_pkt_size < 120.0 &&
        bw_bps < 200000.0) {
        return PERSONA_VOIP;
    }

    /* Rule 5 — GAMING: small packets, few concurrent flows, active traffic */
    if (metrics->avg_pkt_size > 0.0 &&
        metrics->avg_pkt_size < 350.0 &&
        flows > 0 && flows < 8 &&
        bw_bps > 100000.0) {
        return PERSONA_GAMING;
    }

    /* Rule 6 — VIDEO: mid-range bandwidth (200 kbps – 8 Mbps) */
    if (bw_bps >= 200000.0 && bw_bps <= 8000000.0) {
        return PERSONA_VIDEO;
    }

    /* Rule 7 — Hint fallback: behavior says UNKNOWN, but hint has a signal.
     * Trust the hint for active traffic. */
    if (hint != PERSONA_UNKNOWN && bw_bps > 50000.0) {
        return hint;
    }

    return PERSONA_UNKNOWN;
}

void persona_init(persona_state_t *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->current = PERSONA_UNKNOWN;
    state->history_len = 0;
}

const char *persona_name(persona_t persona) {
    switch (persona) {
        case PERSONA_VOIP:      return "voip";
        case PERSONA_GAMING:    return "gaming";
        case PERSONA_VIDEO:     return "video";
        case PERSONA_STREAMING: return "streaming";
        case PERSONA_BULK:      return "bulk";
        case PERSONA_TORRENT:   return "torrent";
        default:                return "unknown";
    }
}

/*
 * Sliding history window: keep last 3 candidates, switch persona when
 * the new candidate appears at least 2 times (2-of-3 majority).
 * This means persona stabilises after ~1 second at 2 Hz sampling.
 */
persona_t persona_update(persona_state_t *state, const metrics_t *metrics,
                         persona_t hint) {
    if (!state || !metrics) {
        return PERSONA_UNKNOWN;
    }

    persona_t candidate = decide_persona(metrics, hint);
    int window = (int)(sizeof(state->history) / sizeof(state->history[0]));

    if (state->history_len < window) {
        state->history[state->history_len++] = candidate;
    } else {
        memmove(&state->history[0], &state->history[1],
                sizeof(state->history) - sizeof(state->history[0]));
        state->history[state->history_len - 1] = candidate;
    }

    /* Count votes for each persona in the window */
    int counts[PERSONA_COUNT] = {0};
    for (int i = 0; i < state->history_len; i++) {
        int p = (int)state->history[i];
        if (p >= 0 && p < PERSONA_COUNT) {
            counts[p]++;
        }
    }

    /* Find persona with majority (>= 2 votes) — UNKNOWN is never promoted */
    persona_t next = state->current;
    for (int p = 1; p < PERSONA_COUNT; p++) {
        if (counts[p] >= 2) {
            next = (persona_t)p;
            break;
        }
    }
    /* Fall back to UNKNOWN only when the window is full and all votes are UNKNOWN */
    if (state->history_len >= window && counts[PERSONA_UNKNOWN] == window) {
        next = PERSONA_UNKNOWN;
    }

    if (next != state->current) {
        log_msg(LOG_INFO, "persona", "persona changed: %s -> %s",
                persona_name(state->current), persona_name(next));
        state->current = next;
    }

    return state->current;
}
