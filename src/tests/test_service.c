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

/* ══════════════════════════════════════════════════════════════
 * Phase 3d: behavior-based inference
 * ══════════════════════════════════════════════════════════════ */

static char *test_behavior_voip_udp() {
    /* Discord voice — 160B pkts, 60 kbps, symmetric */
    flow_features_t f = {
        .proto = 17, .avg_pkt_size = 160.0, .bw_bps = 60000.0,
        .rx_ratio = 0.5, .pkts_total = 500,
    };
    mu_assert("Discord voice → VOIP_CALL",
              service_infer_behavior(&f) == SVC_VOIP_CALL);
    return 0;
}

static char *test_behavior_video_conf_udp() {
    /* Zoom 720p — 900B pkts, 1.5 Mbps, mixed direction */
    flow_features_t f = {
        .proto = 17, .avg_pkt_size = 900.0, .bw_bps = 1500000.0,
        .rx_ratio = 0.55, .pkts_total = 1000,
    };
    mu_assert("Zoom → VIDEO_CONF",
              service_infer_behavior(&f) == SVC_VIDEO_CONF);
    return 0;
}

static char *test_behavior_game_rt_udp() {
    /* CS2 — 80B pkts, 200 kbps, asymmetric tx-heavy */
    flow_features_t f = {
        .proto = 17, .avg_pkt_size = 80.0, .bw_bps = 200000.0,
        .rx_ratio = 0.45, .pkts_total = 800,
    };
    mu_assert("CS2 → GAME_RT",
              service_infer_behavior(&f) == SVC_GAME_RT);
    return 0;
}

static char *test_behavior_vod_bulk_asymmetric() {
    /* YouTube 4K — MSS packets, 8 Mbps, heavily rx-dominant */
    flow_features_t f = {
        .proto = 6, .avg_pkt_size = 1400.0, .bw_bps = 8000000.0,
        .rx_ratio = 0.97, .pkts_total = 5000,
    };
    mu_assert("YouTube → VIDEO_VOD",
              service_infer_behavior(&f) == SVC_VIDEO_VOD);
    return 0;
}

static char *test_behavior_young_flow_unknown() {
    /* Only 5 packets — too young to classify */
    flow_features_t f = {
        .proto = 17, .avg_pkt_size = 80.0, .bw_bps = 200000.0,
        .rx_ratio = 0.5, .pkts_total = 5,
    };
    mu_assert("young flow → UNKNOWN",
              service_infer_behavior(&f) == SVC_UNKNOWN);
    return 0;
}

static char *test_behavior_idle_flow_unknown() {
    /* Established flow but 0 bw this window */
    flow_features_t f = {
        .proto = 17, .avg_pkt_size = 80.0, .bw_bps = 0.0,
        .rx_ratio = 0.5, .pkts_total = 500,
    };
    mu_assert("idle flow → UNKNOWN",
              service_infer_behavior(&f) == SVC_UNKNOWN);
    return 0;
}

static char *test_behavior_web_browsing_unknown() {
    /* HTTPS browsing — medium packets, moderate bw, TCP — ambiguous */
    flow_features_t f = {
        .proto = 6, .avg_pkt_size = 600.0, .bw_bps = 500000.0,
        .rx_ratio = 0.7, .pkts_total = 200,
    };
    mu_assert("web browsing → UNKNOWN",
              service_infer_behavior(&f) == SVC_UNKNOWN);
    return 0;
}

static char *test_behavior_null_safe() {
    mu_assert("NULL → UNKNOWN", service_infer_behavior(NULL) == SVC_UNKNOWN);
    return 0;
}

/* Integration: behavior hint strengthens port signal into a win */
static char *test_behavior_composes_with_voter() {
    /* Port says GAME_RT (0.3), behavior says GAME_RT (0.1) → 0.4, classify */
    service_signals_t s = {
        .dns_hint = SVC_UNKNOWN,
        .port_hint = SVC_GAME_RT,
        .behavior_hint = SVC_GAME_RT,
    };
    mu_assert("port+behavior agree → GAME_RT",
              service_classify(&s) == SVC_GAME_RT);
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
    mu_run_test(test_behavior_voip_udp);
    mu_run_test(test_behavior_video_conf_udp);
    mu_run_test(test_behavior_game_rt_udp);
    mu_run_test(test_behavior_vod_bulk_asymmetric);
    mu_run_test(test_behavior_young_flow_unknown);
    mu_run_test(test_behavior_idle_flow_unknown);
    mu_run_test(test_behavior_web_browsing_unknown);
    mu_run_test(test_behavior_null_safe);
    mu_run_test(test_behavior_composes_with_voter);
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
