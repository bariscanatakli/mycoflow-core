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
 *   STREAMING  : elephant flow OR (many flows + high rx + asymmetric)
 *   BULK       : elephant flow (symmetric) OR high rx with few flows
 *   VOIP       : tiny packets (<120 B) + very low bandwidth (<200 kbps)
 *   GAMING     : small packets (<350 B) + few flows (<8)
 *   VIDEO      : mid-range bandwidth (200 kbps – 8 Mbps)
 *   UNKNOWN    : everything else
 *
 * Priority: first matching rule wins.
 */
static persona_t decide_persona(const metrics_t *metrics) {
    if (!metrics) {
        return PERSONA_UNKNOWN;
    }

    double tx_bps = metrics->tx_bps;
    double rx_bps = metrics->rx_bps;
    double bw_bps = tx_bps + rx_bps;
    double tx_rx_ratio = tx_bps / (rx_bps + 1.0);
    int flows = metrics->active_flows;

    /* Rule 1 — TORRENT: swarm of connections + significant bandwidth
     * Old threshold (30) was too low — modern browsers maintain 30-50+
     * concurrent connections (QUIC, prefetch, extensions, keep-alive).
     * Now requires both high flow count AND meaningful bandwidth to avoid
     * misclassifying idle browser sessions as torrent. */
    if (flows > 100 && bw_bps > 500000.0) {
        return PERSONA_TORRENT;
    }

    /* Rules 2 & 3 — Elephant-flow branch
     * Elephant = one flow dominates >60% of device bytes.
     * This indicates a single large transfer (download/upload), not distributed
     * streaming. Always classify as BULK regardless of direction.
     * Streaming (YouTube/Netflix) distributes across many QUIC flows. */
    if (metrics->elephant_flow) {
        return PERSONA_BULK;
    }

    /* Rule 2b — STREAMING: high download bandwidth + many flows but no elephant
     * Catches QUIC/HTTP3 video streaming (YouTube, Netflix) which distributes
     * traffic across many parallel UDP flows instead of one elephant flow. */
    if (flows > 15 && rx_bps > 2000000.0 && tx_rx_ratio < 0.25) {
        return PERSONA_STREAMING;
    }

    /* Rule 2c — BULK: high bandwidth download, fewer flows */
    if (rx_bps > 5000000.0 && tx_rx_ratio < 0.15 && flows <= 15) {
        return PERSONA_BULK;
    }

    /* Rule 4 — VOIP: tiny codec packets, very low rate */
    if (metrics->avg_pkt_size > 0.0 &&
        metrics->avg_pkt_size < 120.0 &&
        bw_bps < 200000.0) {
        return PERSONA_VOIP;
    }

    /* Rule 5 — GAMING: small packets, few concurrent flows */
    if (metrics->avg_pkt_size > 0.0 &&
        metrics->avg_pkt_size < 350.0 &&
        flows > 0 && flows < 8) {
        return PERSONA_GAMING;
    }

    /* Rule 6 — VIDEO: mid-range bandwidth (200 kbps – 8 Mbps) */
    if (bw_bps >= 200000.0 && bw_bps <= 8000000.0) {
        return PERSONA_VIDEO;
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
persona_t persona_update(persona_state_t *state, const metrics_t *metrics) {
    if (!state || !metrics) {
        return PERSONA_UNKNOWN;
    }

    persona_t candidate = decide_persona(metrics);
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
