/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_persona.c — Persona inference heuristics
 */
#include "myco_persona.h"
#include "myco_log.h"

#include <string.h>

static persona_t decide_persona(const metrics_t *metrics) {
    if (!metrics) {
        return PERSONA_UNKNOWN;
    }
    
    /* Signal 1: RTT/jitter — high values indicate interactive sensitivity */
    int sig_interactive = 0;
    int sig_bulk = 0;
    
    if (metrics->rtt_ms > 40.0 || metrics->jitter_ms > 15.0) {
        sig_interactive++;
    }
    
    /* Signal 2: TX/RX ratio — heavy upload suggests bulk */
    if (metrics->tx_bps > metrics->rx_bps * 1.5) {
        sig_bulk++;
    }
    
    /* Signal 3: Average packet size — small packets = interactive */
    if (metrics->avg_pkt_size > 0.0) {
        if (metrics->avg_pkt_size < 200.0) {
            sig_interactive++;  /* gaming, VoIP, DNS */
        } else if (metrics->avg_pkt_size > 1000.0) {
            sig_bulk++;         /* large transfers, streaming */
        }
    }

    /* Signal 4: Active flow count — few connections → gaming/interactive */
    if (metrics->active_flows > 0) {
        if (metrics->active_flows < 5) {
            sig_interactive++;  /* gaming: 1-2 UDP streams */
        } else if (metrics->active_flows > 50) {
            sig_bulk++;         /* torrent: hundreds of peers */
        }
    }

    /* Signal 5: Elephant flow — one flow dominates all bytes → bulk transfer */
    if (metrics->elephant_flow) {
        sig_bulk += 2;  /* weighted: elephant flow is a strong BULK indicator */
    }

    /* Signal 6: eBPF packet rate — high packet/s = interactive (gaming/VoIP) */
    if (metrics->ebpf_pkt_rate > 500.0) {
        sig_interactive++;
    } else if (metrics->ebpf_pkt_rate > 0.0 && metrics->ebpf_pkt_rate < 50.0) {
        sig_bulk++;  /* low packet rate = large bulk transfers */
    }

    /* Decision: majority vote across signals */
    if (sig_interactive > sig_bulk) {
        return PERSONA_INTERACTIVE;
    } else if (sig_bulk > sig_interactive) {
        return PERSONA_BULK;
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
        case PERSONA_INTERACTIVE: return "interactive";
        case PERSONA_BULK:        return "bulk";
        default:                  return "unknown";
    }
}

persona_t persona_update(persona_state_t *state, const metrics_t *metrics) {
    if (!state || !metrics) {
        return PERSONA_UNKNOWN;
    }

    persona_t candidate = decide_persona(metrics);
    if (state->history_len < (int)(sizeof(state->history) / sizeof(state->history[0]))) {
        state->history[state->history_len++] = candidate;
    } else {
        memmove(&state->history[0], &state->history[1], sizeof(state->history) - sizeof(state->history[0]));
        state->history[state->history_len - 1] = candidate;
    }

    int count_interactive = 0;
    int count_bulk = 0;
    for (int i = 0; i < state->history_len; i++) {
        if (state->history[i] == PERSONA_INTERACTIVE) {
            count_interactive++;
        } else if (state->history[i] == PERSONA_BULK) {
            count_bulk++;
        }
    }

    persona_t next = state->current;
    if (count_interactive >= 3) {
        next = PERSONA_INTERACTIVE;
    } else if (count_bulk >= 3) {
        next = PERSONA_BULK;
    } else if (state->history_len >= 5 && count_interactive == 0 && count_bulk == 0) {
        next = PERSONA_UNKNOWN;
    }

    if (next != state->current) {
        log_msg(LOG_INFO, "persona", "persona changed: %s -> %s", persona_name(state->current), persona_name(next));
        state->current = next;
    }

    return state->current;
}
