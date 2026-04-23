/*
 * test_control.c - Unit tests for control logic
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_control.h"

int tests_run = 0;

/* ── Helpers ─────────────────────────────────────────────────── */

static void make_cfg(myco_config_t *cfg, int bw_kbit) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_cpu_pct           = 50.0;
    cfg->bandwidth_kbit        = bw_kbit;
    cfg->min_bandwidth_kbit    = 1000;
    cfg->max_bandwidth_kbit    = 100000;
    cfg->bandwidth_step_kbit   = 2000;
    cfg->rtt_margin_factor     = 0.30;
    cfg->action_cooldown_s     = 0.0;
    cfg->baseline_samples      = 1;
}

/* Metrics that signal CONGESTION via RTT spike */
static metrics_t congested_metrics(double baseline_rtt) {
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.rtt_ms    = baseline_rtt * 2.0;  /* 2× baseline → exceeds rtt_margin_factor */
    m.jitter_ms = 1.0;
    m.cpu_pct   = 5.0;
    return m;
}

/* Metrics that signal CLEAR (below threshold) */
static metrics_t clear_metrics(double baseline_rtt) {
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.rtt_ms    = baseline_rtt * 1.05;  /* 5% over baseline → below 30% margin */
    m.jitter_ms = 0.5;
    m.cpu_pct   = 5.0;
    return m;
}

/* Metrics that are explicit OUTLIER (CPU trigger) */
static metrics_t outlier_metrics(void) {
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.rtt_ms = 20.0;
    m.jitter_ms = 1.0;
    m.cpu_pct = 99.0;
    return m;
}

/* ── Original tests ──────────────────────────────────────────── */

static char *test_is_outlier() {
    myco_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_cpu_pct = 50.0;

    metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 10.0;
    baseline.jitter_ms = 2.0;

    metrics.cpu_pct = 10.0;
    metrics.rtt_ms = 12.0;
    metrics.jitter_ms = 3.0;
    mu_assert("error, normal metrics flagged as outlier", is_outlier(&metrics, &baseline, &cfg) == 0);

    metrics.cpu_pct = 60.0;
    mu_assert("error, high CPU not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);
    metrics.cpu_pct = 10.0;

    metrics.rtt_ms = 60.0; /* 6× baseline */
    mu_assert("error, high RTT not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);
    metrics.rtt_ms = 12.0;

    metrics.jitter_ms = 30.0; /* 15× baseline, satisfies > 12× threshold and > 15ms floor */
    mu_assert("error, high jitter not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);

    return 0;
}

static char *test_control_hysteresis() {
    control_state_t state;
    control_init(&state, 20000);
    state.stable_cycles = 5;

    control_on_action_result(&state, 0);
    mu_assert("error, stable_cycles not reset on failure", state.stable_cycles == 0);
    mu_assert("error, safe_mode not set on failure", state.safe_mode == 1);

    return 0;
}

static char *test_safe_mode_enters_on_outlier_streak() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;
    baseline.jitter_ms = 1.0;

    metrics_t m = outlier_metrics();
    policy_t desired;
    char reason[128];

    control_decide(&state, &cfg, &m, &baseline,
                   PERSONA_GAMING, 1.0,
                   &desired, reason, sizeof(reason));
    mu_assert("safe-mode should not enter on first outlier", state.safe_mode == 0);

    control_decide(&state, &cfg, &m, &baseline,
                   PERSONA_GAMING, 2.0,
                   &desired, reason, sizeof(reason));
    mu_assert("safe-mode should not enter on second outlier", state.safe_mode == 0);

    control_decide(&state, &cfg, &m, &baseline,
                   PERSONA_GAMING, 3.0,
                   &desired, reason, sizeof(reason));
    mu_assert("safe-mode should enter on third consecutive outlier", state.safe_mode == 1);

    return 0;
}

static char *test_safe_mode_clears_after_clean_streak() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);
    state.safe_mode = 1;

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;
    baseline.jitter_ms = 1.0;

    metrics_t m = clear_metrics(20.0);
    policy_t desired;
    char reason[128];

    for (int i = 0; i < 4; i++) {
        control_decide(&state, &cfg, &m, &baseline,
                       PERSONA_GAMING, 10.0 + i,
                       &desired, reason, sizeof(reason));
    }
    mu_assert("safe-mode should still be active before 5 clean cycles", state.safe_mode == 1);

    control_decide(&state, &cfg, &m, &baseline,
                   PERSONA_GAMING, 15.0,
                   &desired, reason, sizeof(reason));
    mu_assert("safe-mode should clear after 5 clean cycles", state.safe_mode == 0);

    return 0;
}

/* Regression: safe mode must exit even when baseline jitter is elevated
 * (simulates the contaminated-baseline bug where baseline drifted upward
 * via the sliding update and both baseline and EWMA were ~equal, causing
 * the 8× multiplier check to never fire → perpetual safe mode). */
static char *test_safe_mode_exits_with_elevated_baseline() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);
    state.safe_mode = 1;

    /* Elevated baseline jitter — simulates contaminated-baseline state */
    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms    = 24.0;
    baseline.jitter_ms = 3.0;   /* elevated, but still "normal" for this line */

    /* Metrics just slightly above baseline — not a real outlier (< 8×) */
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.rtt_ms    = 25.0;         /* ~1× baseline, clearly not an outlier */
    m.jitter_ms = 4.0;          /* ~1.3× baseline, well below 8× threshold */
    m.cpu_pct   = 5.0;

    policy_t desired;
    char reason[128];

    for (int i = 0; i < 5; i++) {
        control_decide(&state, &cfg, &m, &baseline,
                       PERSONA_GAMING, 100.0 + i,
                       &desired, reason, sizeof(reason));
    }
    mu_assert("safe-mode must exit with elevated-but-stable baseline", state.safe_mode == 0);

    return 0;
}

/* ── Persona tier tests ──────────────────────────────────────── */

/* Tier 1: VOIP congested → soften (-step/2) */
static char *test_control_voip_congested() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = congested_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_VOIP, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("VOIP congested: should trigger change", changed == 1);
    mu_assert("VOIP congested: bandwidth should decrease",
              desired.bandwidth_kbit < 20000);
    mu_assert("VOIP congested: should soften (not full step)",
              desired.bandwidth_kbit == 20000 - cfg.bandwidth_step_kbit / 2);
    return 0;
}

/* Tier 1: GAMING clear → boost (+step) */
static char *test_control_gaming_clear() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = clear_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_GAMING, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("GAMING clear: should trigger change", changed == 1);
    mu_assert("GAMING clear: bandwidth should increase",
              desired.bandwidth_kbit > 20000);
    mu_assert("GAMING clear: should boost full step",
              desired.bandwidth_kbit == 20000 + cfg.bandwidth_step_kbit);
    return 0;
}

/* Tier 2: VIDEO congested → soften (-step/2) */
static char *test_control_video_congested() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = congested_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_VIDEO, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("VIDEO congested: should trigger change", changed == 1);
    mu_assert("VIDEO congested: soften by half step",
              desired.bandwidth_kbit == 20000 - cfg.bandwidth_step_kbit / 2);
    return 0;
}

/* Tier 2: VIDEO clear → no-change */
static char *test_control_video_clear() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = clear_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_VIDEO, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("VIDEO clear: no-change expected", changed == 0);
    return 0;
}

/* Tier 3: STREAMING congested → full throttle (-step) */
static char *test_control_streaming_congested() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = congested_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_STREAMING, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("STREAMING congested: should trigger change", changed == 1);
    mu_assert("STREAMING congested: full throttle (-step)",
              desired.bandwidth_kbit == 20000 - cfg.bandwidth_step_kbit);
    return 0;
}

/* Tier 3: TORRENT congested → full throttle (-step) */
static char *test_control_torrent_congested() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = congested_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_TORRENT, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("TORRENT congested: should trigger change", changed == 1);
    mu_assert("TORRENT congested: full throttle (-step)",
              desired.bandwidth_kbit == 20000 - cfg.bandwidth_step_kbit);
    return 0;
}

/* Tier 3: BULK clear → no-change (don't reward bulk with more BW) */
static char *test_control_bulk_clear() {
    myco_config_t cfg;
    control_state_t state;
    make_cfg(&cfg, 20000);
    control_init(&state, 20000);

    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 20.0;

    metrics_t m = clear_metrics(20.0);
    policy_t desired;
    char reason[128];

    int changed = control_decide(&state, &cfg, &m, &baseline,
                                 PERSONA_BULK, 1.0,
                                 &desired, reason, sizeof(reason));

    mu_assert("BULK clear: no-change expected (hold)", changed == 0);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_is_outlier);
    mu_run_test(test_control_hysteresis);
    mu_run_test(test_safe_mode_enters_on_outlier_streak);
    mu_run_test(test_safe_mode_clears_after_clean_streak);
    mu_run_test(test_safe_mode_exits_with_elevated_baseline);
    mu_run_test(test_control_voip_congested);
    mu_run_test(test_control_gaming_clear);
    mu_run_test(test_control_video_congested);
    mu_run_test(test_control_video_clear);
    mu_run_test(test_control_streaming_congested);
    mu_run_test(test_control_torrent_congested);
    mu_run_test(test_control_bulk_clear);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char *result = all_tests();
    if (result != 0) {
        printf("FAILED: %s\n", result);
    } else {
        printf("ALL TESTS PASSED\n");
    }
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
