/*
 * test_persona.c - Unit tests for 6-class persona heuristics
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_persona.h"

int tests_run = 0;

/* Feed the same metrics twice into a fresh state — should reach 2/3 majority */
#define FEED2(state, m) do { persona_update(&(state), &(m)); persona_update(&(state), &(m)); } while(0)

/* ── VOIP ──────────────────────────────────────────────────── */
static char *test_voip() {
    persona_state_t state;
    persona_init(&state);

    /* G.711: 64-byte packets, 64 kbps total, 1 flow */
    metrics_t m = {
        .avg_pkt_size = 64.0,
        .tx_bps       = 32000.0,   /* 32 kbps TX */
        .rx_bps       = 32000.0,   /* 32 kbps RX */
        .active_flows = 1,
        .elephant_flow = 0,
    };
    /* bw = 64kbps < 200kbps, avg_pkt < 120 → VOIP */
    FEED2(state, m);
    mu_assert("VOIP: should detect after 2/3 votes", state.current == PERSONA_VOIP);
    return 0;
}

/* ── GAMING ────────────────────────────────────────────────── */
static char *test_gaming() {
    persona_state_t state;
    persona_init(&state);

    /* FPS game: 150-byte packets, 300 kbps, 3 concurrent flows */
    metrics_t m = {
        .avg_pkt_size = 150.0,
        .tx_bps       = 150000.0,
        .rx_bps       = 150000.0,
        .active_flows = 3,
        .elephant_flow = 0,
    };
    /* avg_pkt=150 < 350, flows=3 < 8 → GAMING (VOIP ruled out: pkt ≥ 120) */
    FEED2(state, m);
    mu_assert("GAMING: should detect after 2/3 votes", state.current == PERSONA_GAMING);
    return 0;
}

/* ── VIDEO ─────────────────────────────────────────────────── */
static char *test_video() {
    persona_state_t state;
    persona_init(&state);

    /* Zoom call: 800-byte packets, 3 Mbps bidirectional, 3 flows */
    metrics_t m = {
        .avg_pkt_size = 800.0,
        .tx_bps       = 1500000.0,
        .rx_bps       = 1500000.0,
        .active_flows = 3,
        .elephant_flow = 0,
    };
    /* avg_pkt=800 ≥ 350 → not GAMING; bw=3Mbps ∈ [200kbps, 8Mbps] → VIDEO */
    FEED2(state, m);
    mu_assert("VIDEO: should detect after 2/3 votes", state.current == PERSONA_VIDEO);
    return 0;
}

/* ── STREAMING ─────────────────────────────────────────────── */
static char *test_streaming() {
    persona_state_t state;
    persona_init(&state);

    /* Streaming: many flows (QUIC), high download, no elephant flow
     * YouTube/Netflix distribute across 15+ parallel UDP connections */
    metrics_t m = {
        .avg_pkt_size = 800.0,
        .tx_bps       = 200000.0,      /* small TX (ACKs + requests) */
        .rx_bps       = 10000000.0,    /* 10 Mbps download */
        .active_flows = 25,
        .elephant_flow = 0,            /* no single dominant flow */
    };
    /* flows>15 + rx>2Mbps + ratio<0.25 → STREAMING (Rule 2b) */
    FEED2(state, m);
    mu_assert("STREAMING: should detect after 2/3 votes", state.current == PERSONA_STREAMING);
    return 0;
}

/* ── BULK ──────────────────────────────────────────────────── */
static char *test_bulk() {
    persona_state_t state;
    persona_init(&state);

    /* Large file upload: elephant flow, client→server heavy (TX >> RX) */
    metrics_t m = {
        .avg_pkt_size = 1400.0,
        .tx_bps       = 15000000.0,    /* 15 Mbps upload */
        .rx_bps       = 500000.0,      /* ACKs */
        .active_flows = 1,
        .elephant_flow = 1,
    };
    /* tx_rx_ratio = 15M/500001 ≈ 30 ≥ 0.25 → BULK */
    FEED2(state, m);
    mu_assert("BULK: should detect after 2/3 votes", state.current == PERSONA_BULK);
    return 0;
}

/* ── TORRENT ───────────────────────────────────────────────── */
static char *test_torrent() {
    persona_state_t state;
    persona_init(&state);

    /* BitTorrent: 150 simultaneous peer connections + significant bandwidth */
    metrics_t m = {
        .avg_pkt_size = 900.0,
        .tx_bps       = 1000000.0,
        .rx_bps       = 1000000.0,
        .active_flows = 150,
        .elephant_flow = 0,
    };
    /* flows=150 > 100 AND bw=2Mbps > 500kbps → TORRENT */
    FEED2(state, m);
    mu_assert("TORRENT: should detect after 2/3 votes", state.current == PERSONA_TORRENT);
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

    /* 1 gaming sample — not enough */
    persona_update(&state, &m_gaming);
    mu_assert("history: 1 sample should stay UNKNOWN", state.current == PERSONA_UNKNOWN);

    /* 2nd gaming sample — 2/3 window filled, persona switches */
    persona_update(&state, &m_gaming);
    mu_assert("history: 2/3 gaming should switch to GAMING", state.current == PERSONA_GAMING);

    /* 1 bulk sample — window = [gaming, gaming, bulk], gaming still 2/3 */
    persona_update(&state, &m_bulk);
    mu_assert("history: 1 bulk interloper should not displace GAMING", state.current == PERSONA_GAMING);

    /* 2 bulk samples — window = [gaming, bulk, bulk], bulk gets 2/3 */
    persona_update(&state, &m_bulk);
    mu_assert("history: 2/3 bulk should switch to BULK", state.current == PERSONA_BULK);

    return 0;
}

static char *all_tests() {
    mu_run_test(test_voip);
    mu_run_test(test_gaming);
    mu_run_test(test_video);
    mu_run_test(test_streaming);
    mu_run_test(test_bulk);
    mu_run_test(test_torrent);
    mu_run_test(test_history_window);
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
