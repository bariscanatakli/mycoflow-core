/*
 * test_persona.c - Unit tests for 6-class persona heuristics + hint integration
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_persona.h"

int tests_run = 0;

/* Feed the same metrics twice into a fresh state — should reach 2/3 majority */
#define FEED2(state, m, h) do { persona_update(&(state), &(m), (h)); persona_update(&(state), &(m), (h)); } while(0)

/* ══════════════════════════════════════════════════════════════
 * EXISTING BEHAVIORAL TESTS (hint = UNKNOWN, no regression)
 * ══════════════════════════════════════════════════════════════ */

/* ── VOIP ──────────────────────────────────────────────────── */
static char *test_voip() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 64.0,
        .tx_bps       = 32000.0,
        .rx_bps       = 32000.0,
        .active_flows = 1,
        .elephant_flow = 0,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("VOIP: should detect after 2/3 votes", state.current == PERSONA_VOIP);
    return 0;
}

/* ── GAMING ────────────────────────────────────────────────── */
/* CS2-like: moderate bandwidth, bidirectional UDP (tx_rx_ratio ≈ 0.5).
 * tx_rx_ratio=0.5 satisfies the >= 0.10 guard in Rule 2b. */
static char *test_gaming() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 470.0,
        .tx_bps       = 500000.0,
        .rx_bps       = 1000000.0,
        .active_flows = 6,
        .elephant_flow = 0,
        .udp_flows    = 4,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("GAMING: should detect after 2/3 votes", state.current == PERSONA_GAMING);
    return 0;
}

/* ── VIDEO ─────────────────────────────────────────────────── */
static char *test_video() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 800.0,
        .tx_bps       = 1500000.0,
        .rx_bps       = 1500000.0,
        .active_flows = 3,
        .elephant_flow = 0,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("VIDEO: should detect after 2/3 votes", state.current == PERSONA_VIDEO);
    return 0;
}

/* ── STREAMING ─────────────────────────────────────────────── */
static char *test_streaming() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 800.0,
        .tx_bps       = 2000000.0,
        .rx_bps       = 80000000.0,
        .active_flows = 60,
        .elephant_flow = 0,
        .udp_flows    = 20,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("STREAMING: should detect after 2/3 votes", state.current == PERSONA_STREAMING);
    return 0;
}

/* ── BULK ──────────────────────────────────────────────────── */
static char *test_bulk() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 1400.0,
        .tx_bps       = 15000000.0,
        .rx_bps       = 500000.0,
        .active_flows = 1,
        .elephant_flow = 1,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("BULK: should detect after 2/3 votes", state.current == PERSONA_BULK);
    return 0;
}

/* ── TORRENT ───────────────────────────────────────────────── */
static char *test_torrent() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 900.0,
        .tx_bps       = 1000000.0,
        .rx_bps       = 1000000.0,
        .active_flows = 150,
        .elephant_flow = 0,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("TORRENT: should detect after 2/3 votes", state.current == PERSONA_TORRENT);
    return 0;
}

/* ── BULK DOWNLOAD (curl/TCP) ─────────────────────────────── */
static char *test_bulk_download() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 1200.0,
        .tx_bps       = 1500000.0,
        .rx_bps       = 80000000.0,
        .active_flows = 80,
        .elephant_flow = 1,
        .udp_flows    = 14,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("BULK_DL: curl TCP download should be BULK", state.current == PERSONA_BULK);
    return 0;
}

/* ── 2/3 WINDOW BEHAVIOUR ──────────────────────────────────── */
static char *test_history_window() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m_gaming = {
        .avg_pkt_size = 150.0, .tx_bps = 150000.0, .rx_bps = 150000.0,
        .active_flows = 3, .elephant_flow = 0,
    };
    metrics_t m_bulk = {
        .avg_pkt_size = 1400.0, .tx_bps = 15000000.0, .rx_bps = 500000.0,
        .active_flows = 1, .elephant_flow = 1,
    };

    persona_update(&state, &m_gaming, PERSONA_UNKNOWN);
    mu_assert("history: 1 sample should stay UNKNOWN", state.current == PERSONA_UNKNOWN);

    persona_update(&state, &m_gaming, PERSONA_UNKNOWN);
    mu_assert("history: 2/3 gaming should switch to GAMING", state.current == PERSONA_GAMING);

    persona_update(&state, &m_bulk, PERSONA_UNKNOWN);
    mu_assert("history: 1 bulk interloper should not displace GAMING", state.current == PERSONA_GAMING);

    persona_update(&state, &m_bulk, PERSONA_UNKNOWN);
    mu_assert("history: 2/3 bulk should switch to BULK", state.current == PERSONA_BULK);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * HINT-AWARE TESTS — the KEY new tests
 * ══════════════════════════════════════════════════════════════ */

/* ── THE LOL FIX: hint=GAMING overrides elephant→BULK ──────── */
static char *test_hint_gaming_elephant() {
    persona_state_t state;
    persona_init(&state);

    /* LoL: TCP game, one dominant connection (elephant), moderate bandwidth */
    metrics_t m = {
        .avg_pkt_size = 500.0,
        .tx_bps       = 500000.0,
        .rx_bps       = 3000000.0,   /* 3 Mbps — typical LoL */
        .active_flows = 8,
        .elephant_flow = 1,           /* game server dominates */
        .udp_flows    = 0,            /* TCP game */
    };
    FEED2(state, m, PERSONA_GAMING);
    mu_assert("HINT: LoL (elephant+hint=GAMING) should be GAMING", state.current == PERSONA_GAMING);
    return 0;
}

/* ── hint=GAMING but high bandwidth → launcher download → BULK */
static char *test_hint_gaming_high_bw() {
    persona_state_t state;
    persona_init(&state);

    /* Steam download on a game port: huge bandwidth = not gameplay */
    metrics_t m = {
        .avg_pkt_size = 1400.0,
        .tx_bps       = 500000.0,
        .rx_bps       = 50000000.0,  /* 50 Mbps download */
        .active_flows = 3,
        .elephant_flow = 1,
        .udp_flows    = 0,
    };
    /* bw = 50.5 Mbps > 20 Mbps → sanity check overrides GAMING hint */
    FEED2(state, m, PERSONA_GAMING);
    mu_assert("HINT: launcher download (hint=GAMING, bw>20Mbps) should be BULK",
              state.current == PERSONA_BULK);
    return 0;
}

/* ── hint=VOIP overrides elephant (voice call via TURN relay on TCP) */
static char *test_hint_voip_elephant() {
    persona_state_t state;
    persona_init(&state);

    /* Voice call via TURN relay: TCP flows only, one dominant, low bw.
     * udp_flows=0 so we don't trigger the UDP GAMING rule (2b). */
    metrics_t m = {
        .avg_pkt_size = 300.0,
        .tx_bps       = 500000.0,
        .rx_bps       = 800000.0,
        .active_flows = 2,
        .elephant_flow = 1,
        .udp_flows    = 0,
    };
    FEED2(state, m, PERSONA_VOIP);
    mu_assert("HINT: Discord voice (elephant+hint=VOIP) should be VOIP",
              state.current == PERSONA_VOIP);
    return 0;
}

/* ── hint=STREAMING overrides TCP bulk download ────────────── */
static char *test_hint_streaming_tcp_bulk() {
    persona_state_t state;
    persona_init(&state);

    /* Netflix on TCP 443: high rx, low UDP ratio → behavior says BULK.
     * DNS hint (Phase 2) will provide this, but test the logic now. */
    metrics_t m = {
        .avg_pkt_size = 1200.0,
        .tx_bps       = 200000.0,
        .rx_bps       = 15000000.0,  /* 15 Mbps Netflix */
        .active_flows = 5,
        .elephant_flow = 1,
        .udp_flows    = 0,
    };
    FEED2(state, m, PERSONA_STREAMING);
    mu_assert("HINT: Netflix TCP (elephant+hint=STREAMING) should be STREAMING",
              state.current == PERSONA_STREAMING);
    return 0;
}

/* ── hint=VIDEO overrides elephant (Zoom with screen share) ── */
static char *test_hint_video_elephant() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 900.0,
        .tx_bps       = 2000000.0,
        .rx_bps       = 4000000.0,
        .active_flows = 3,
        .elephant_flow = 1,
        .udp_flows    = 1,
    };
    FEED2(state, m, PERSONA_VIDEO);
    mu_assert("HINT: Zoom (elephant+hint=VIDEO) should be VIDEO",
              state.current == PERSONA_VIDEO);
    return 0;
}

/* ── hint=UNKNOWN with elephant → BULK (no regression) ──────── */
static char *test_hint_unknown_elephant() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 1400.0,
        .tx_bps       = 500000.0,
        .rx_bps       = 20000000.0,
        .active_flows = 2,
        .elephant_flow = 1,
        .udp_flows    = 0,
    };
    FEED2(state, m, PERSONA_UNKNOWN);
    mu_assert("HINT: no hint + elephant should be BULK (no regression)",
              state.current == PERSONA_BULK);
    return 0;
}

/* ── Torrent is NOT overridden by hint ─────────────────────── */
static char *test_hint_cannot_override_torrent() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m = {
        .avg_pkt_size = 900.0,
        .tx_bps       = 1000000.0,
        .rx_bps       = 1000000.0,
        .active_flows = 150,
        .elephant_flow = 0,
    };
    FEED2(state, m, PERSONA_GAMING);
    mu_assert("HINT: torrent behavior (150 flows) overrides GAMING hint",
              state.current == PERSONA_TORRENT);
    return 0;
}

/* ── hint=GAMING overrides STREAMING when UDP + high rx ─────── */
static char *test_hint_gaming_overrides_streaming() {
    persona_state_t state;
    persona_init(&state);

    /* UDP game with heavy download (spectator mode / large map) */
    metrics_t m = {
        .avg_pkt_size = 600.0,
        .tx_bps       = 2000000.0,
        .rx_bps       = 40000000.0,   /* 40 Mbps */
        .active_flows = 20,
        .elephant_flow = 0,
        .udp_flows    = 8,            /* 8/20 = 40% UDP ratio */
    };
    /* Without hint: udp_ratio>=25% + rx>30Mbps → STREAMING.
     * With hint=GAMING: should override to GAMING. */
    FEED2(state, m, PERSONA_GAMING);
    mu_assert("HINT: GAMING hint overrides STREAMING behavior (UDP+high rx)",
              state.current == PERSONA_GAMING);
    return 0;
}

/* ── hint=STREAMING overrides high-rx TCP BULK ──────────────── */
static char *test_hint_streaming_tcp_high_rx() {
    persona_state_t state;
    persona_init(&state);

    /* YouTube TCP HLS: rx > 5 Mbps, low UDP ratio → behavior says BULK */
    metrics_t m = {
        .avg_pkt_size = 1200.0,
        .tx_bps       = 100000.0,
        .rx_bps       = 10000000.0,   /* 10 Mbps */
        .active_flows = 5,
        .elephant_flow = 0,
        .udp_flows    = 0,
    };
    /* rx>5Mbps, tx_rx<0.30, udp_ratio<0.25 → would be BULK.
     * hint=STREAMING → should override. */
    FEED2(state, m, PERSONA_STREAMING);
    mu_assert("HINT: STREAMING hint overrides TCP BULK (high rx)",
              state.current == PERSONA_STREAMING);
    return 0;
}

/* ── hint fallback: behavior=UNKNOWN but hint has signal ─────── */
static char *test_hint_fallback_unknown() {
    persona_state_t state;
    persona_init(&state);

    /* Low traffic that doesn't match any behavioral rule, but on a game port */
    metrics_t m = {
        .avg_pkt_size = 400.0,
        .tx_bps       = 50000.0,
        .rx_bps       = 50000.0,      /* 100 kbps total — too low for most rules */
        .active_flows = 2,
        .elephant_flow = 0,
        .udp_flows    = 0,
    };
    FEED2(state, m, PERSONA_GAMING);
    mu_assert("HINT: fallback — behavior=UNKNOWN + hint=GAMING → GAMING",
              state.current == PERSONA_GAMING);
    return 0;
}

static char *all_tests() {
    /* Original behavioral tests (no regression) */
    mu_run_test(test_voip);
    mu_run_test(test_gaming);
    mu_run_test(test_video);
    mu_run_test(test_streaming);
    mu_run_test(test_bulk);
    mu_run_test(test_torrent);
    mu_run_test(test_bulk_download);
    mu_run_test(test_history_window);

    /* Hint-aware tests */
    mu_run_test(test_hint_gaming_elephant);
    mu_run_test(test_hint_gaming_high_bw);
    mu_run_test(test_hint_voip_elephant);
    mu_run_test(test_hint_streaming_tcp_bulk);
    mu_run_test(test_hint_video_elephant);
    mu_run_test(test_hint_unknown_elephant);
    mu_run_test(test_hint_cannot_override_torrent);
    mu_run_test(test_hint_gaming_overrides_streaming);
    mu_run_test(test_hint_streaming_tcp_high_rx);
    mu_run_test(test_hint_fallback_unknown);
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
