/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * test_act.c — Unit tests for CAKE actuation and ingress IFB plumbing
 *
 * All tests use no_tc=1 (dry-run) so no actual shell commands are executed.
 * Tests cover: NULL guards, interface name validation, force_fail, and the
 * no_tc dry-run paths for act_setup_ingress_ifb / act_apply_ingress_policy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../minunit.h"
#include "../myco_act.h"
#include "../myco_types.h"

int tests_run = 0;

/* ── act_setup_ingress_ifb ───────────────────────────────────── */

static char *test_ingress_setup_no_tc_returns_success() {
    int rc = act_setup_ingress_ifb("eth0", "ifb0", 10000, /*no_tc=*/1, /*force_fail=*/0);
    mu_assert("no_tc path should return 1", rc == 1);
    return 0;
}

static char *test_ingress_setup_null_wan_returns_failure() {
    int rc = act_setup_ingress_ifb(NULL, "ifb0", 10000, 1, 0);
    mu_assert("NULL wan_iface should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_null_ifb_returns_failure() {
    int rc = act_setup_ingress_ifb("eth0", NULL, 10000, 1, 0);
    mu_assert("NULL ifb_iface should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_invalid_wan_rejected() {
    /* Shell metacharacter in wan_iface — must be rejected by is_valid_iface */
    int rc = act_setup_ingress_ifb("eth0; reboot", "ifb0", 10000, 1, 0);
    mu_assert("invalid wan_iface with ';' should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_invalid_ifb_rejected() {
    int rc = act_setup_ingress_ifb("eth0", "ifb0$(id)", 10000, 1, 0);
    mu_assert("invalid ifb_iface with '$(' should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_empty_iface_rejected() {
    int rc = act_setup_ingress_ifb("", "ifb0", 10000, 1, 0);
    mu_assert("empty wan_iface should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_force_fail_returns_zero() {
    int rc = act_setup_ingress_ifb("eth0", "ifb0", 10000, /*no_tc=*/0, /*force_fail=*/1);
    mu_assert("force_fail should return 0", rc == 0);
    return 0;
}

static char *test_ingress_setup_dotted_iface_valid() {
    /* eth0.1 (VLAN sub-interface) is a legitimate name */
    int rc = act_setup_ingress_ifb("eth0.1", "ifb0", 10000, 1, 0);
    mu_assert("dotted iface like eth0.1 should be accepted", rc == 1);
    return 0;
}

/* ── act_apply_ingress_policy ────────────────────────────────── */

static char *test_ingress_policy_interactive_no_tc() {
    int rc = act_apply_ingress_policy("ifb0", PERSONA_INTERACTIVE, 10000, 1, 0);
    mu_assert("INTERACTIVE no_tc should return 1", rc == 1);
    return 0;
}

static char *test_ingress_policy_bulk_no_tc() {
    int rc = act_apply_ingress_policy("ifb0", PERSONA_BULK, 10000, 1, 0);
    mu_assert("BULK no_tc should return 1", rc == 1);
    return 0;
}

static char *test_ingress_policy_unknown_no_tc() {
    int rc = act_apply_ingress_policy("ifb0", PERSONA_UNKNOWN, 10000, 1, 0);
    mu_assert("UNKNOWN persona no_tc should return 1", rc == 1);
    return 0;
}

static char *test_ingress_policy_null_iface_returns_failure() {
    int rc = act_apply_ingress_policy(NULL, PERSONA_INTERACTIVE, 10000, 1, 0);
    mu_assert("NULL ifb_iface should return 0", rc == 0);
    return 0;
}

static char *test_ingress_policy_invalid_iface_rejected() {
    int rc = act_apply_ingress_policy("ifb0 && evil", PERSONA_INTERACTIVE, 10000, 1, 0);
    mu_assert("invalid ifb_iface with '&&' should return 0", rc == 0);
    return 0;
}

static char *test_ingress_policy_force_fail_returns_zero() {
    int rc = act_apply_ingress_policy("ifb0", PERSONA_INTERACTIVE, 10000, 0, /*force_fail=*/1);
    mu_assert("force_fail should return 0", rc == 0);
    return 0;
}

/* ── Suite ───────────────────────────────────────────────────── */

static char *all_tests() {
    mu_run_test(test_ingress_setup_no_tc_returns_success);
    mu_run_test(test_ingress_setup_null_wan_returns_failure);
    mu_run_test(test_ingress_setup_null_ifb_returns_failure);
    mu_run_test(test_ingress_setup_invalid_wan_rejected);
    mu_run_test(test_ingress_setup_invalid_ifb_rejected);
    mu_run_test(test_ingress_setup_empty_iface_rejected);
    mu_run_test(test_ingress_setup_force_fail_returns_zero);
    mu_run_test(test_ingress_setup_dotted_iface_valid);
    mu_run_test(test_ingress_policy_interactive_no_tc);
    mu_run_test(test_ingress_policy_bulk_no_tc);
    mu_run_test(test_ingress_policy_unknown_no_tc);
    mu_run_test(test_ingress_policy_null_iface_returns_failure);
    mu_run_test(test_ingress_policy_invalid_iface_rejected);
    mu_run_test(test_ingress_policy_force_fail_returns_zero);
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
