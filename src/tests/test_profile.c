/*
 * test_profile.c — Unit tests for priority profile parsing + lookup.
 * Does not exercise UCI (no uci binary in dev env); only defaults +
 * the pure parser helpers.
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_profile.h"

int tests_run = 0;

static char *test_defaults_shape() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    mu_assert("4 default profiles", ps.num_profiles == 4);
    mu_assert("gaming present",      profile_find(&ps, "gaming")       != NULL);
    mu_assert("remote_work present", profile_find(&ps, "remote_work")  != NULL);
    mu_assert("family_media present",profile_find(&ps, "family_media") != NULL);
    mu_assert("auto present",        profile_find(&ps, "auto")         != NULL);
    mu_assert("auto is default",
              strcmp(ps.profiles[ps.default_idx].name, "auto") == 0);
    mu_assert("0 bindings by default", ps.num_bindings == 0);
    return 0;
}

static char *test_gaming_priority() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    const profile_t *g = profile_find(&ps, "gaming");
    mu_assert("gaming first = GAME_RT",   g->winner_priority[0] == SVC_GAME_RT);
    mu_assert("gaming second = VOIP_CALL", g->winner_priority[1] == SVC_VOIP_CALL);
    return 0;
}

static char *test_remote_work_bumps_conf_dscp() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    const profile_t *rw = profile_find(&ps, "remote_work");
    mu_assert("remote_work VIDEO_CONF = EF(46)",
              rw->service_dscp[SVC_VIDEO_CONF] == 46);
    const profile_t *g = profile_find(&ps, "gaming");
    mu_assert("gaming VIDEO_CONF = CS4(32)",
              g->service_dscp[SVC_VIDEO_CONF] == 32);
    return 0;
}

static char *test_parse_service() {
    mu_assert("game_rt",    profile_parse_service("game_rt")    == SVC_GAME_RT);
    mu_assert("Game_RT",    profile_parse_service("Game_RT")    == SVC_GAME_RT);
    mu_assert("voip_call",  profile_parse_service("voip_call")  == SVC_VOIP_CALL);
    mu_assert("video_vod",  profile_parse_service("video_vod")  == SVC_VIDEO_VOD);
    mu_assert("bogus",      profile_parse_service("bogus")      == SVC_UNKNOWN);
    mu_assert("null safe",  profile_parse_service(NULL)         == SVC_UNKNOWN);
    return 0;
}

static char *test_parse_dscp() {
    mu_assert("EF = 46",    profile_parse_dscp("EF")    == 46);
    mu_assert("CS0 = 0",    profile_parse_dscp("CS0")   == 0);
    mu_assert("CS4 = 32",   profile_parse_dscp("CS4")   == 32);
    mu_assert("AF41 = 34",  profile_parse_dscp("AF41")  == 34);
    mu_assert("numeric 46", profile_parse_dscp("46")    == 46);
    mu_assert("numeric 0",  profile_parse_dscp("0")     == 0);
    mu_assert("numeric 63", profile_parse_dscp("63")    == 63);
    mu_assert("out of range", profile_parse_dscp("64")  == -1);
    mu_assert("garbage",    profile_parse_dscp("moo")   == -1);
    mu_assert("null safe",  profile_parse_dscp(NULL)    == -1);
    mu_assert("empty",      profile_parse_dscp("")      == -1);
    return 0;
}

static char *test_profile_for_ip_default() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    const profile_t *p = profile_for_ip(&ps, "192.168.1.50");
    mu_assert("unbound IP → default (auto)",
              p != NULL && strcmp(p->name, "auto") == 0);
    return 0;
}

static char *test_profile_for_ip_bound() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    /* Manually inject a binding (simulating UCI) */
    strncpy(ps.bindings[0].ip, "192.168.1.10", sizeof(ps.bindings[0].ip) - 1);
    strncpy(ps.bindings[0].profile_name, "gaming",
            sizeof(ps.bindings[0].profile_name) - 1);
    ps.num_bindings = 1;
    profile_resolve_bindings(&ps);

    mu_assert("binding resolved",
              ps.bindings[0].profile_idx >= 0);
    const profile_t *p = profile_for_ip(&ps, "192.168.1.10");
    mu_assert("bound IP → gaming profile",
              p != NULL && strcmp(p->name, "gaming") == 0);
    return 0;
}

static char *test_unresolved_binding_falls_back() {
    profile_set_t ps;
    profile_load_defaults(&ps);
    strncpy(ps.bindings[0].ip, "10.0.0.1", sizeof(ps.bindings[0].ip) - 1);
    strncpy(ps.bindings[0].profile_name, "nonexistent",
            sizeof(ps.bindings[0].profile_name) - 1);
    ps.num_bindings = 1;
    profile_resolve_bindings(&ps);
    mu_assert("unresolved → -1", ps.bindings[0].profile_idx == -1);
    const profile_t *p = profile_for_ip(&ps, "10.0.0.1");
    mu_assert("unresolved → falls through to default",
              p != NULL && strcmp(p->name, "auto") == 0);
    return 0;
}

static char *test_null_safety() {
    mu_assert("find NULL ps",   profile_find(NULL, "x") == NULL);
    mu_assert("find NULL name", profile_find(NULL, NULL) == NULL);
    mu_assert("for_ip NULL ps", profile_for_ip(NULL, "x") == NULL);
    /* These must not crash */
    profile_load_defaults(NULL);
    profile_resolve_bindings(NULL);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_defaults_shape);
    mu_run_test(test_gaming_priority);
    mu_run_test(test_remote_work_bumps_conf_dscp);
    mu_run_test(test_parse_service);
    mu_run_test(test_parse_dscp);
    mu_run_test(test_profile_for_ip_default);
    mu_run_test(test_profile_for_ip_bound);
    mu_run_test(test_unresolved_binding_falls_back);
    mu_run_test(test_null_safety);
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
