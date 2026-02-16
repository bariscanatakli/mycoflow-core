/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_control.c — Reflexive control loop (hysteresis + policy)
 */
#include "myco_control.h"
#include "myco_log.h"

#include <stdio.h>
#include <string.h>

int is_outlier(const metrics_t *metrics, const metrics_t *baseline, const myco_config_t *cfg) {
    if (!metrics || !baseline || !cfg) {
        return 0;
    }
    if (metrics->cpu_pct > cfg->max_cpu_pct) {
        return 1;
    }
    if (metrics->rtt_ms > baseline->rtt_ms * 5.0 && baseline->rtt_ms > 0.1) {
        return 1;
    }
    if (metrics->jitter_ms > baseline->jitter_ms * 5.0 && baseline->jitter_ms > 0.1) {
        return 1;
    }
    return 0;
}

void control_init(control_state_t *state, int initial_bw) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->current.bandwidth_kbit = initial_bw;
    state->last_stable = state->current;
    state->safe_mode = 0;
    state->stable_cycles = 0;
}

int control_decide(control_state_t *state,
                   const myco_config_t *cfg,
                   const metrics_t *metrics,
                   const metrics_t *baseline,
                   persona_t persona,
                   policy_t *desired,
                   char *reason,
                   size_t reason_len) {
    if (!state || !cfg || !metrics || !baseline || !desired || !reason) {
        return 0;
    }

    *desired = state->current;
    snprintf(reason, reason_len, "no-change");

    if (is_outlier(metrics, baseline, cfg)) {
        state->safe_mode = 1;
        *desired = state->last_stable;
        snprintf(reason, reason_len, "safe-mode: outlier");
        return (state->current.bandwidth_kbit != desired->bandwidth_kbit);
    }

    double rtt_delta = metrics->rtt_ms - baseline->rtt_ms;
    double jitter_delta = metrics->jitter_ms - baseline->jitter_ms;
    int congested = (rtt_delta > 20.0) || (jitter_delta > 10.0);

    if (congested && persona == PERSONA_BULK) {
        desired->bandwidth_kbit -= cfg->bandwidth_step_kbit;
        desired->boosted = 0;
        snprintf(reason, reason_len, "bulk-congested: throttle");
    } else if (!congested && persona == PERSONA_INTERACTIVE) {
        desired->bandwidth_kbit += cfg->bandwidth_step_kbit;
        desired->boosted = 1;
        snprintf(reason, reason_len, "interactive-clear: boost");
    } else if (congested && persona == PERSONA_INTERACTIVE) {
        desired->bandwidth_kbit -= cfg->bandwidth_step_kbit / 2;
        desired->boosted = 0;
        snprintf(reason, reason_len, "interactive-congested: soften");
    }

    desired->bandwidth_kbit = (int)clamp_double((double)desired->bandwidth_kbit,
                                                (double)cfg->min_bandwidth_kbit,
                                                (double)cfg->max_bandwidth_kbit);

    if (desired->bandwidth_kbit == state->current.bandwidth_kbit) {
        state->stable_cycles++;
        if (state->stable_cycles >= 3) {
            state->last_stable = state->current;
            state->stable_cycles = 0;
        }
        return 0;
    }

    state->stable_cycles = 0;
    return 1;
}

void control_on_action_result(control_state_t *state, int success) {
    if (!state) {
        return;
    }
    if (!success) {
        log_msg(LOG_WARN, "control", "actuation failed, entering safe mode");
        state->safe_mode = 1;
        state->current = state->last_stable;
        state->stable_cycles = 0;
    }
}
