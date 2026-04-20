/*
 * test_rtt.c — Unit tests for the stub RTT engine (Phase 5a).
 * Phase 5b will add kernel-sourced tests when the BPF map is wired.
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "../minunit.h"

/* main.c owns g_stop; tests link the symbol stub because myco_log
 * indirectly references it via other modules some day. Pattern matches
 * test_dns / test_device / test_classifier. */
volatile sig_atomic_t g_stop = 0;

#include "../myco_rtt.h"

int tests_run = 0;

static char *test_open_close_null_safe() {
    rtt_engine_t *eng = rtt_engine_open(NULL, NULL);
    mu_assert("open returns non-null", eng != NULL);
    rtt_engine_close(eng);
    rtt_engine_close(NULL);   /* must not crash */
    return 0;
}

static char *test_lookup_unknown_returns_zero() {
    rtt_engine_t *eng = rtt_engine_open(NULL, NULL);
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 443, 6 };
    mu_assert("unknown flow → 0", rtt_engine_lookup_ms(eng, &k) == 0);
    rtt_engine_close(eng);
    return 0;
}

static char *test_inject_then_lookup() {
    rtt_engine_t *eng = rtt_engine_open(NULL, NULL);
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 443, 6 };
    rtt_engine_inject_stub(eng, &k, 42);
    mu_assert("lookup after inject → 42", rtt_engine_lookup_ms(eng, &k) == 42);
    rtt_engine_inject_stub(eng, &k, 130);
    mu_assert("update in place → 130", rtt_engine_lookup_ms(eng, &k) == 130);
    rtt_engine_close(eng);
    return 0;
}

static char *test_udp_flow_always_zero() {
    rtt_engine_t *eng = rtt_engine_open(NULL, NULL);
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 443, 17 };  /* UDP */
    rtt_engine_inject_stub(eng, &k, 25);   /* injection succeeds but... */
    mu_assert("UDP lookup skipped → 0", rtt_engine_lookup_ms(eng, &k) == 0);
    rtt_engine_close(eng);
    return 0;
}

static char *test_null_lookup_safe() {
    mu_assert("NULL engine → 0", rtt_engine_lookup_ms(NULL, NULL) == 0);
    rtt_engine_t *eng = rtt_engine_open(NULL, NULL);
    mu_assert("NULL key → 0", rtt_engine_lookup_ms(eng, NULL) == 0);
    rtt_engine_inject_stub(NULL, NULL, 1);   /* must not crash */
    rtt_engine_close(eng);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_open_close_null_safe);
    mu_run_test(test_lookup_unknown_returns_zero);
    mu_run_test(test_inject_then_lookup);
    mu_run_test(test_udp_flow_always_zero);
    mu_run_test(test_null_lookup_safe);
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
