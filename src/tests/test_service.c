/*
 * test_service.c — Unit tests for service taxonomy + 3-signal voter.
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_service.h"

int tests_run = 0;

/* ── Names ──────────────────────────────────────────────────── */
static char *test_service_names_stable() {
    mu_assert("SVC_UNKNOWN → unknown",
              strcmp(service_name(SVC_UNKNOWN), "unknown") == 0);
    mu_assert("SVC_GAME_RT → game_rt",
              strcmp(service_name(SVC_GAME_RT), "game_rt") == 0);
    mu_assert("SVC_VIDEO_VOD → video_vod",
              strcmp(service_name(SVC_VIDEO_VOD), "video_vod") == 0);
    mu_assert("SVC_TORRENT → torrent",
              strcmp(service_name(SVC_TORRENT), "torrent") == 0);
    mu_assert("out-of-range → unknown (no NULL)",
              strcmp(service_name((service_t)99), "unknown") == 0);
    return 0;
}

/* ── service_t → persona_t ─────────────────────────────────── */
static char *test_service_to_persona() {
    mu_assert("GAME_RT → GAMING",
              service_to_persona(SVC_GAME_RT) == PERSONA_GAMING);
    mu_assert("VOIP_CALL → VOIP",
              service_to_persona(SVC_VOIP_CALL) == PERSONA_VOIP);
    mu_assert("VIDEO_CONF → VIDEO",
              service_to_persona(SVC_VIDEO_CONF) == PERSONA_VIDEO);
    mu_assert("VIDEO_LIVE → VIDEO",
              service_to_persona(SVC_VIDEO_LIVE) == PERSONA_VIDEO);
    mu_assert("VIDEO_VOD → STREAMING",
              service_to_persona(SVC_VIDEO_VOD) == PERSONA_STREAMING);
    mu_assert("BULK_DL → BULK",
              service_to_persona(SVC_BULK_DL) == PERSONA_BULK);
    mu_assert("FILE_SYNC → BULK",
              service_to_persona(SVC_FILE_SYNC) == PERSONA_BULK);
    mu_assert("GAME_LAUNCHER → BULK (contradicts name, matches intent)",
              service_to_persona(SVC_GAME_LAUNCHER) == PERSONA_BULK);
    mu_assert("TORRENT → TORRENT",
              service_to_persona(SVC_TORRENT) == PERSONA_TORRENT);
    mu_assert("WEB_INTERACTIVE → UNKNOWN (no device role)",
              service_to_persona(SVC_WEB_INTERACTIVE) == PERSONA_UNKNOWN);
    mu_assert("SYSTEM → UNKNOWN (no device role)",
              service_to_persona(SVC_SYSTEM) == PERSONA_UNKNOWN);
    return 0;
}

/* ── service_t → ct mark ───────────────────────────────────── */
static char *test_service_to_ct_mark() {
    mu_assert("UNKNOWN → mark 0",
              service_to_ct_mark(SVC_UNKNOWN) == 0);
    mu_assert("GAME_RT → mark 1",
              service_to_ct_mark(SVC_GAME_RT) == 1);
    mu_assert("SYSTEM → mark 11",
              service_to_ct_mark(SVC_SYSTEM) == 11);
    mu_assert("out-of-range → mark 0",
              service_to_ct_mark((service_t)99) == 0);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * 3-signal weighted voter
 * ══════════════════════════════════════════════════════════════ */

/* DNS alone should win (0.6 >= 0.3 threshold) */
static char *test_voter_dns_alone_wins() {
    service_signals_t s = {
        .dns_hint = SVC_GAME_RT,
        .port_hint = SVC_UNKNOWN,
        .behavior_hint = SVC_UNKNOWN,
    };
    mu_assert("DNS alone → classify", service_classify(&s) == SVC_GAME_RT);
    return 0;
}

/* Port alone should win (0.3 == threshold) */
static char *test_voter_port_alone_wins() {
    service_signals_t s = {
        .dns_hint = SVC_UNKNOWN,
        .port_hint = SVC_VOIP_CALL,
        .behavior_hint = SVC_UNKNOWN,
    };
    mu_assert("port alone → classify", service_classify(&s) == SVC_VOIP_CALL);
    return 0;
}

/* Behavior alone should NOT win (0.1 < 0.3 threshold) */
static char *test_voter_behavior_alone_fails() {
    service_signals_t s = {
        .dns_hint = SVC_UNKNOWN,
        .port_hint = SVC_UNKNOWN,
        .behavior_hint = SVC_BULK_DL,
    };
    mu_assert("behavior alone → UNKNOWN", service_classify(&s) == SVC_UNKNOWN);
    return 0;
}

/* DNS + behavior agreeing on same service → that service */
static char *test_voter_dns_plus_behavior_agree() {
    service_signals_t s = {
        .dns_hint = SVC_VIDEO_VOD,
        .port_hint = SVC_UNKNOWN,
        .behavior_hint = SVC_VIDEO_VOD,
    };
    mu_assert("DNS+behavior agree → classify", service_classify(&s) == SVC_VIDEO_VOD);
    return 0;
}

/* DNS and port disagree: DNS (0.6) wins over port (0.3) */
static char *test_voter_dns_beats_port() {
    service_signals_t s = {
        .dns_hint = SVC_VIDEO_CONF,     /* Zoom domain */
        .port_hint = SVC_VOIP_CALL,     /* 3478 STUN port could be either */
        .behavior_hint = SVC_UNKNOWN,
    };
    mu_assert("DNS > port", service_classify(&s) == SVC_VIDEO_CONF);
    return 0;
}

/* Port + behavior agree (0.4 total) beats DNS alone of different svc (0.6) — no, 0.6 > 0.4 */
static char *test_voter_dns_beats_port_plus_behavior() {
    service_signals_t s = {
        .dns_hint = SVC_VIDEO_VOD,      /* 0.6 */
        .port_hint = SVC_BULK_DL,       /* 0.3 */
        .behavior_hint = SVC_BULK_DL,   /* 0.1 */
    };
    /* DNS=0.6 vs BULK_DL=0.4 → DNS wins */
    mu_assert("DNS (0.6) > port+behavior (0.4)",
              service_classify(&s) == SVC_VIDEO_VOD);
    return 0;
}

/* All three agree: no surprise */
static char *test_voter_all_agree() {
    service_signals_t s = {
        .dns_hint = SVC_GAME_RT,
        .port_hint = SVC_GAME_RT,
        .behavior_hint = SVC_GAME_RT,
    };
    mu_assert("all agree → that svc", service_classify(&s) == SVC_GAME_RT);
    return 0;
}

/* All UNKNOWN → UNKNOWN */
static char *test_voter_all_unknown() {
    service_signals_t s = {
        .dns_hint = SVC_UNKNOWN,
        .port_hint = SVC_UNKNOWN,
        .behavior_hint = SVC_UNKNOWN,
    };
    mu_assert("all unknown → UNKNOWN", service_classify(&s) == SVC_UNKNOWN);
    return 0;
}

/* NULL input → UNKNOWN (defensive) */
static char *test_voter_null_safe() {
    mu_assert("NULL → UNKNOWN", service_classify(NULL) == SVC_UNKNOWN);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_service_names_stable);
    mu_run_test(test_service_to_persona);
    mu_run_test(test_service_to_ct_mark);
    mu_run_test(test_voter_dns_alone_wins);
    mu_run_test(test_voter_port_alone_wins);
    mu_run_test(test_voter_behavior_alone_fails);
    mu_run_test(test_voter_dns_plus_behavior_agree);
    mu_run_test(test_voter_dns_beats_port);
    mu_run_test(test_voter_dns_beats_port_plus_behavior);
    mu_run_test(test_voter_all_agree);
    mu_run_test(test_voter_all_unknown);
    mu_run_test(test_voter_null_safe);
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
