/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_control.c — Reflexive control loop (hysteresis + policy)
 */
#include "myco_control.h"
#include "myco_log.h"

#include <math.h>
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

/* ── Action feedback ring helpers ───────────────────────────── */

static void ring_record_action(control_state_t *state, double now,
                               int bw_before, int bw_after, double rtt_before) {
    int idx = state->ring_head % ACTION_RING_SIZE;
    state->ring[idx].ts         = now;
    state->ring[idx].bw_before  = bw_before;
    state->ring[idx].bw_after   = bw_after;
    state->ring[idx].rtt_before = rtt_before;
    state->ring[idx].rtt_after  = -1.0;
    state->ring[idx].filled     = 0;
    state->ring_head++;
}

/* Called every cycle to fill in pending rtt_after values (3 cycles later).
 * Returns 1 if the step size was adapted downward. */
static int ring_fill_and_evaluate(control_state_t *state, myco_config_t *cfg,
                                  double now, double rtt_now) {
    int adapted = 0;
    for (int i = 0; i < ACTION_RING_SIZE; i++) {
        action_record_t *r = &state->ring[i];
        if (!r->filled && r->rtt_after < 0.0 && r->ts > 0.0 &&
            (now - r->ts) >= 3.0) {
            r->rtt_after = rtt_now;
            r->filled    = 1;
        }
    }

    /* Evaluate: if 4+ filled records exist and >50% showed no RTT improvement,
     * halve the bandwidth step to avoid thrashing. */
    int filled_count   = 0;
    int no_improve     = 0;
    for (int i = 0; i < ACTION_RING_SIZE; i++) {
        if (!state->ring[i].filled) {
            continue;
        }
        filled_count++;
        /* An action "improved" if RTT dropped by at least 2ms */
        if (state->ring[i].rtt_after >= state->ring[i].rtt_before - 2.0) {
            no_improve++;
        }
    }

    if (filled_count >= 4 && no_improve > filled_count / 2 && !state->step_adapted) {
        int new_step = cfg->bandwidth_step_kbit / 2;
        if (new_step < 500) {
            new_step = 500;
        }
        log_msg(LOG_INFO, "control",
                "action feedback: %d/%d actions ineffective, step %d->%d kbit",
                no_improve, filled_count, cfg->bandwidth_step_kbit, new_step);
        cfg->bandwidth_step_kbit = new_step;
        state->step_adapted = 1;
        adapted = 1;
    }
    return adapted;
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
    state->ring_head = 0;
    state->step_adapted = 0;
    for (int i = 0; i < ACTION_RING_SIZE; i++) {
        state->ring[i].rtt_after = -1.0;
    }
}

int control_decide(control_state_t *state,
                   myco_config_t *cfg,
                   const metrics_t *metrics,
                   const metrics_t *baseline,
                   persona_t persona,
                   double now,
                   policy_t *desired,
                   char *reason,
                   size_t reason_len) {
    if (!state || !cfg || !metrics || !baseline || !desired || !reason) {
        return 0;
    }

    *desired = state->current;
    snprintf(reason, reason_len, "no-change");

    /* Fill pending action feedback records and adapt step if needed */
    ring_fill_and_evaluate(state, cfg, now, metrics->rtt_ms);

    if (is_outlier(metrics, baseline, cfg)) {
        state->safe_mode = 1;
        *desired = state->last_stable;
        snprintf(reason, reason_len, "safe-mode: outlier");
        return (state->current.bandwidth_kbit != desired->bandwidth_kbit);
    }

    double rtt_delta     = metrics->rtt_ms    - baseline->rtt_ms;
    double jitter_delta  = metrics->jitter_ms - baseline->jitter_ms;

    /* Adaptive thresholds: scale with the observed baseline so a 5ms-RTT
     * fiber line and a 40ms-RTT ADSL line each use appropriate sensitivity.
     * Floor values prevent spurious triggers on near-zero baselines. */
    double thresh_rtt    = clamp_double(baseline->rtt_ms    * cfg->rtt_margin_factor, 8.0, 60.0);
    double thresh_jitter = clamp_double(baseline->jitter_ms * cfg->rtt_margin_factor, 4.0, 30.0);

    /* qdisc_backlog > 0 is a direct kernel-side bufferbloat indicator —
     * faster and more reliable than the RTT probe alone.
     * probe_loss_pct > 2% means CAKE is already dropping packets. */
    int backlog_congested = (metrics->qdisc_backlog > 0);
    int loss_congested    = (metrics->probe_loss_pct > 2.0);
    int congested = (rtt_delta > thresh_rtt) || (jitter_delta > thresh_jitter) ||
                    backlog_congested || loss_congested;

    log_msg(LOG_DEBUG, "control",
            "thresh_rtt=%.1fms thresh_jitter=%.1fms rtt_delta=%.1f jitter_delta=%.1f backlog=%u loss=%.1f%% congested=%d",
            thresh_rtt, thresh_jitter, rtt_delta, jitter_delta,
            metrics->qdisc_backlog, metrics->probe_loss_pct, congested);

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

    /* Propagate the same bandwidth delta to the ingress policy so the IFB
     * CAKE cap tracks egress adaptation rather than staying frozen at startup. */
    if (desired->ingress_bw_kbit > 0) {
        int delta = desired->bandwidth_kbit - state->current.bandwidth_kbit;
        desired->ingress_bw_kbit = (int)clamp_double(
            (double)(desired->ingress_bw_kbit + delta),
            (double)cfg->min_bandwidth_kbit,
            (double)cfg->max_bandwidth_kbit);
    }

    if (desired->bandwidth_kbit == state->current.bandwidth_kbit) {
        state->stable_cycles++;
        if (state->stable_cycles >= 3) {
            state->last_stable = state->current;
            state->stable_cycles = 0;
        }
        return 0;
    }

    state->stable_cycles = 0;

    /* Record this actuation in the feedback ring for later evaluation */
    ring_record_action(state, now,
                       state->current.bandwidth_kbit,
                       desired->bandwidth_kbit,
                       metrics->rtt_ms);
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
