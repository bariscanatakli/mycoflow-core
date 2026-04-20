/*
 * test_mangle.c — Unit tests for mangle iface validator and mark engine
 * stub. Does not test mangle_apply() directly because it invokes iptables.
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_mangle.h"
#include "../myco_mark.h"

int tests_run = 0;

/* ── iface validator ───────────────────────────────────────────── */

static char *test_iface_valid() {
    mu_assert("eth0 valid",      mangle_iface_is_safe("eth0"));
    mu_assert("wan valid",       mangle_iface_is_safe("wan"));
    mu_assert("pppoe-wan valid", mangle_iface_is_safe("pppoe-wan"));
    mu_assert("br.lan valid",    mangle_iface_is_safe("br.lan"));
    mu_assert("eth0_1 valid",    mangle_iface_is_safe("eth0_1"));
    return 0;
}

static char *test_iface_invalid() {
    mu_assert("null rejected",    !mangle_iface_is_safe(NULL));
    mu_assert("empty rejected",   !mangle_iface_is_safe(""));
    mu_assert("shell meta rejected",
              !mangle_iface_is_safe("eth0; rm -rf /"));
    mu_assert("pipe rejected",    !mangle_iface_is_safe("eth0|ls"));
    mu_assert("space rejected",   !mangle_iface_is_safe("eth0 evil"));
    mu_assert("slash rejected",   !mangle_iface_is_safe("eth/0"));
    mu_assert("too long rejected",
              !mangle_iface_is_safe("abcdefghijklmnopq"));  /* 17 chars */
    return 0;
}

/* ── mark engine stub ──────────────────────────────────────────── */

static char *test_mark_engine_open_close() {
    mark_engine_t *eng = mark_engine_open();
    /* Real handle requires CAP_NET_ADMIN; stub always succeeds. Either
     * way the daemon must never crash on open. */
    mark_engine_close(eng);
    return 0;
}

static char *test_mark_engine_set_null_safe() {
    flow_key_t key = {
        .src_ip = 0x0100007f, .dst_ip = 0x08080808,
        .src_port = 12345, .dst_port = 443, .protocol = 6,
    };
    mu_assert("NULL engine → 0 (no-op)",
              mark_engine_set(NULL, &key, 1) == 0);
    return 0;
}

static char *test_mark_engine_stats_null_safe() {
    mu_assert("stat_ok(NULL) == 0",  mark_engine_stat_ok(NULL) == 0);
    mu_assert("stat_err(NULL) == 0", mark_engine_stat_err(NULL) == 0);
    return 0;
}

/* ── profile name validator (Phase 4c) ─────────────────────────── */

static char *test_profile_name_valid() {
    mu_assert("gaming valid",      mangle_profile_name_is_safe("gaming"));
    mu_assert("remote_work valid", mangle_profile_name_is_safe("remote_work"));
    mu_assert("family-media valid",mangle_profile_name_is_safe("family-media"));
    mu_assert("auto valid",        mangle_profile_name_is_safe("auto"));
    mu_assert("Mixed123 valid",    mangle_profile_name_is_safe("Mixed123"));
    return 0;
}

static char *test_profile_name_invalid() {
    mu_assert("null rejected",   !mangle_profile_name_is_safe(NULL));
    mu_assert("empty rejected",  !mangle_profile_name_is_safe(""));
    mu_assert("space rejected",  !mangle_profile_name_is_safe("my profile"));
    mu_assert("shell meta rejected",
              !mangle_profile_name_is_safe("gaming;ls"));
    mu_assert("dot rejected",    !mangle_profile_name_is_safe("a.b"));
    mu_assert("too long rejected",
              !mangle_profile_name_is_safe("abcdefghijklmnopqrstuvwxy"));  /* 25 */
    return 0;
}

/* ── IP validator (Phase 4c) ───────────────────────────────────── */

static char *test_ip_valid() {
    mu_assert("192.168.1.10 valid", mangle_ip_is_safe("192.168.1.10"));
    mu_assert("10.0.0.1 valid",     mangle_ip_is_safe("10.0.0.1"));
    mu_assert("8.8.8.8 valid",      mangle_ip_is_safe("8.8.8.8"));
    mu_assert("255.255.255.255 valid",
              mangle_ip_is_safe("255.255.255.255"));
    return 0;
}

static char *test_ip_invalid() {
    mu_assert("null rejected",     !mangle_ip_is_safe(NULL));
    mu_assert("empty rejected",    !mangle_ip_is_safe(""));
    mu_assert("3 dots needed",     !mangle_ip_is_safe("1.2.3"));
    mu_assert("letters rejected",  !mangle_ip_is_safe("a.b.c.d"));
    mu_assert("5 octets rejected", !mangle_ip_is_safe("1.2.3.4.5"));
    mu_assert("trailing dot rejected", !mangle_ip_is_safe("1.2.3."));
    mu_assert("too long rejected", !mangle_ip_is_safe("1234.5.6.7"));
    mu_assert("shell meta rejected",
              !mangle_ip_is_safe("1.2.3;x"));
    return 0;
}

static char *all_tests() {
    mu_run_test(test_iface_valid);
    mu_run_test(test_iface_invalid);
    mu_run_test(test_mark_engine_open_close);
    mu_run_test(test_mark_engine_set_null_safe);
    mu_run_test(test_mark_engine_stats_null_safe);
    mu_run_test(test_profile_name_valid);
    mu_run_test(test_profile_name_invalid);
    mu_run_test(test_ip_valid);
    mu_run_test(test_ip_invalid);
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
