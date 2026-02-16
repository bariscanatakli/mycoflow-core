/*
 * test_config.c - Unit tests for configuration logic
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_config.h"

int tests_run = 0;

static char *test_config_defaults() {
    myco_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg);

    mu_assert("error, default enabled should be 1", cfg.enabled == 1);
    mu_assert("error, default sample_hz should be 1.0", cfg.sample_hz == 1.0);
    mu_assert("error, default ewma_alpha should be 0.3", cfg.ewma_alpha == 0.3);
    mu_assert("error, default max_cpu_pct should be 40.0", cfg.max_cpu_pct == 40.0);
    return 0;
}

static char *test_config_validation() {
    myco_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    
    // Set invalid values
    cfg.ewma_alpha = -0.5;
    cfg.sample_hz = 0.0;
    
    // Manually trigger validation logic (simulating config_load behavior)
    // In a real integration test we would set env vars and call config_load,
    // but here we can test the specific validation logic if exposed, 
    // or just rely on config_load cleaning it up.
    // Let's rely on config_load to apply defaults and validation.
    
    // Reset and mock invalid env vars? 
    // Easier to just test the validation logic if we extract it, 
    // but config_load does it all. 
    // Let's setenv and test.
    setenv("MYCOFLOW_EWMA_ALPHA", "-0.5", 1);
    config_load(&cfg);
    mu_assert("error, ewma_alpha should be clamped to 0.01", cfg.ewma_alpha == 0.01);
    unsetenv("MYCOFLOW_EWMA_ALPHA");

    setenv("MYCOFLOW_EWMA_ALPHA", "1.5", 1);
    config_load(&cfg);
    mu_assert("error, ewma_alpha should be clamped to 1.0", cfg.ewma_alpha == 1.0);
    unsetenv("MYCOFLOW_EWMA_ALPHA");

    return 0;
}

static char *all_tests() {
    mu_run_test(test_config_defaults);
    mu_run_test(test_config_validation);
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
