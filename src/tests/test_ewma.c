/*
 * test_ewma.c - Unit tests for EWMA filter
 */
#include <stdio.h>
#include <math.h>
#include "../minunit.h"
#include "../myco_ewma.h"

int tests_run = 0;

static char *test_ewma_init() {
    ewma_filter_t f;
    ewma_init(&f);
    mu_assert("error, init value should be 0.0", f.value == 0.0);
    mu_assert("error, init flag should be 0", f.initialized == 0);
    return 0;
}

static char *test_ewma_smoothing() {
    ewma_filter_t f;
    ewma_init(&f);
    double alpha = 0.5;

    // First sample initializes
    double res = ewma_update(&f, 10.0, alpha);
    mu_assert("error, first sample should be used directly", res == 10.0);
    mu_assert("error, initialized flag should be set", f.initialized == 1);

    // Second sample: 0.5*20 + 0.5*10 = 15
    res = ewma_update(&f, 20.0, alpha);
    mu_assert("error, smoothing failed", fabs(res - 15.0) < 0.001);

    // Third sample: 0.5*20 + 0.5*15 = 17.5
    res = ewma_update(&f, 20.0, alpha);
    mu_assert("error, smoothing failed", fabs(res - 17.5) < 0.001);

    return 0;
}

static char *all_tests() {
    mu_run_test(test_ewma_init);
    mu_run_test(test_ewma_smoothing);
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
