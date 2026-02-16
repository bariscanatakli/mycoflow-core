/*
 * test_control.c - Unit tests for control logic
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_control.h"

int tests_run = 0;

static char *test_is_outlier() {
    myco_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_cpu_pct = 50.0; // 50% max CPU

    metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.rtt_ms = 10.0;
    baseline.jitter_ms = 2.0;

    // Normal case
    metrics.cpu_pct = 10.0;
    metrics.rtt_ms = 12.0;
    metrics.jitter_ms = 3.0;
    mu_assert("error, normal metrics flagged as outlier", is_outlier(&metrics, &baseline, &cfg) == 0);

    // CPU outlier
    metrics.cpu_pct = 60.0;
    mu_assert("error, high CPU not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);
    metrics.cpu_pct = 10.0;

    // RTT outlier (> 5x baseline)
    metrics.rtt_ms = 60.0; // 6x
    mu_assert("error, high RTT not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);
    metrics.rtt_ms = 12.0;

    // Jitter outlier (> 5x baseline)
    metrics.jitter_ms = 12.0; // 6x
    mu_assert("error, high jitter not flagged", is_outlier(&metrics, &baseline, &cfg) == 1);

    return 0;
}

static char *test_control_hysteresis() {
    control_state_t state;
    control_init(&state, 20000); // Initial BW 20000
    // Force some stable cycles
    state.stable_cycles = 5;

    // Simulate action failure -> stable_cycles reset
    control_on_action_result(&state, 0); // 0 = fail
    mu_assert("error, stable_cycles not reset on failure", state.stable_cycles == 0);
    mu_assert("error, safe_mode not set on failure", state.safe_mode == 1);

    return 0;
}

static char *all_tests() {
    mu_run_test(test_is_outlier);
    mu_run_test(test_control_hysteresis);
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
