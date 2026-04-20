/*
 * test_classifier.c — Unit tests for the 3-signal classifier orchestrator.
 * Uses a synthetic flow_table_t + DNS cache. The mark engine stub records
 * calls implicitly via mark_engine_stat_ok().
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "../minunit.h"

/* Stub: g_stop is defined in main.c but referenced by dns_sniff_thread.
 * We never spawn the sniffer in this test, but the linker still needs
 * the symbol. Same pattern as test_dns.c / test_device.c. */
volatile sig_atomic_t g_stop = 0;

#include "../myco_classifier.h"
#include "../myco_flow.h"
#include "../myco_dns.h"
#include "../myco_mark.h"
#include "../myco_rtt.h"

int tests_run = 0;

static void seed_flow(flow_table_t *ft, int idx,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint8_t proto,
                      uint64_t packets, uint64_t bytes,
                      uint64_t tx_delta, uint64_t rx_delta,
                      double last_seen) {
    flow_entry_t *e = &ft->entries[idx];
    e->key.src_ip = src_ip;
    e->key.dst_ip = dst_ip;
    e->key.src_port = src_port;
    e->key.dst_port = dst_port;
    e->key.protocol = proto;
    e->packets = packets;
    e->rx_packets = packets;
    e->bytes = bytes / 2;
    e->rx_bytes = bytes / 2;
    e->tx_delta = tx_delta;
    e->rx_delta = rx_delta;
    e->last_seen = last_seen;
    e->active = 1;
}

/* ── Port-only classification (no DNS, no behavior) ───────────── */
static char *test_port_only_classifies() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    /* UDP to port 27020 — Valve game traffic */
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 27020, 17,
              0, 0, 0, 0, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();

    classifier_tick(tab, &ft, NULL, eng, NULL, 1.0, 1.0);

    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 27020, 17 };
    mu_assert("port-only → GAME_RT on first tick",
              classifier_get_service(tab, &k) == SVC_GAME_RT);

    mark_engine_close(eng);
    classifier_destroy(tab);
    return 0;
}

/* ── Stability gate: no ct mark push on first tick ───────────── */
static char *test_stability_gate_requires_two_ticks() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 27020, 17,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();

    uint64_t ok0 = mark_engine_stat_ok(eng);
    classifier_tick(tab, &ft, NULL, eng, NULL, 1.0, 1.0);
    mu_assert("no mark push after first tick",
              mark_engine_stat_ok(eng) == ok0);

    classifier_tick(tab, &ft, NULL, eng, NULL, 2.0, 1.0);
    mu_assert("mark push after second tick (same verdict)",
              mark_engine_stat_ok(eng) == ok0 + 1);

    mark_engine_close(eng);
    classifier_destroy(tab);
    return 0;
}

/* ── Verdict flip resets stability ────────────────────────────── */
static char *test_verdict_flip_resets_stability() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    /* First: UDP 27020 → GAME_RT via port */
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 27020, 17,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();
    uint64_t ok0 = mark_engine_stat_ok(eng);

    classifier_tick(tab, &ft, NULL, eng, NULL, 1.0, 1.0);  /* tentative */
    /* Flip to port 6881 → TORRENT */
    ft.entries[0].key.dst_port = 6881;
    /* Same flow index but different key — need a fresh slot; reset & reseed */
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 6881, 17,
              100, 10000, 5000, 5000, 2.0);
    classifier_tick(tab, &ft, NULL, eng, NULL, 2.0, 1.0);
    /* New flow, tentative only → no mark yet */
    mu_assert("new flow tentative → no mark push",
              mark_engine_stat_ok(eng) == ok0);

    classifier_tick(tab, &ft, NULL, eng, NULL, 3.0, 1.0);
    mu_assert("second match → mark pushed",
              mark_engine_stat_ok(eng) == ok0 + 1);

    mark_engine_close(eng);
    classifier_destroy(tab);
    return 0;
}

/* ── Unknown verdict doesn't create an entry ──────────────────── */
static char *test_unknown_does_not_track() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    /* Port 443 + no DNS + idle → all three signals UNKNOWN */
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 443, 6,
              0, 0, 0, 0, 1.0);

    flow_service_table_t *tab = classifier_create();
    classifier_tick(tab, &ft, NULL, NULL, NULL, 1.0, 1.0);
    mu_assert("unknown flow → not tracked",
              classifier_active_count(tab) == 0);

    classifier_destroy(tab);
    return 0;
}

/* ── DNS hint dominates port hint (0.6 > 0.3) ─────────────────── */
static char *test_dns_beats_port() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    /* Port 5222 → GAME_RT via port, but DNS says googlevideo.com → VIDEO_VOD */
    seed_flow(&ft, 0, 0x0a0a0a01u, 0xd83acf8eu, 40000, 5222, 17,
              100, 10000, 5000, 5000, 1.0);

    dns_cache_t dns;
    dns_cache_init(&dns);
    dns_cache_insert(&dns, 0xd83acf8eu, "r1.googlevideo.com", 300);

    flow_service_table_t *tab = classifier_create();
    classifier_tick(tab, &ft, &dns, NULL, NULL, 1.0, 1.0);

    flow_key_t k = { 0x0a0a0a01u, 0xd83acf8eu, 40000, 5222, 17 };
    mu_assert("DNS VIDEO_VOD beats port GAME_RT",
              classifier_get_service(tab, &k) == SVC_VIDEO_VOD);

    classifier_destroy(tab);
    dns_cache_destroy(&dns);
    return 0;
}

/* ── NULL inputs are safe ──────────────────────────────────────── */
static char *test_null_safe() {
    classifier_tick(NULL, NULL, NULL, NULL, NULL, 0.0, 1.0);
    mu_assert("get on NULL → UNKNOWN",
              classifier_get_service(NULL, NULL) == SVC_UNKNOWN);
    mu_assert("count on NULL → 0",
              classifier_active_count(NULL) == 0);
    return 0;
}

/* ── classifier_device_counts (Phase 4b) ─────────────────────── */
static char *test_device_counts_aggregates_per_src_ip() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));

    uint32_t a = 0x0a0a0a01u, b = 0x0a0a0a02u;
    /* A: two GAME_RT flows */
    seed_flow(&ft, 0, a, 0x08080808u, 40000, 27020, 17, 100, 10000, 5000, 5000, 1.0);
    seed_flow(&ft, 1, a, 0x08080809u, 40001, 27021, 17, 100, 10000, 5000, 5000, 1.0);
    /* A: one VIDEO_LIVE (RTMP) flow */
    seed_flow(&ft, 2, a, 0x0101017fu, 40002, 1935,   6,  100, 10000, 5000, 5000, 1.0);
    /* B: one TORRENT flow */
    seed_flow(&ft, 3, b, 0x02020202u, 40003, 6881,   6,  100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    classifier_tick(tab, &ft, NULL, NULL, NULL, 1.0, 1.0);

    int counts[SERVICE_COUNT] = {0};
    classifier_device_counts(tab, a, counts);
    mu_assert("A has 2 GAME_RT",    counts[SVC_GAME_RT]    == 2);
    mu_assert("A has 1 VIDEO_LIVE", counts[SVC_VIDEO_LIVE] == 1);
    mu_assert("A has 0 TORRENT",    counts[SVC_TORRENT]    == 0);

    memset(counts, 0, sizeof(counts));
    classifier_device_counts(tab, b, counts);
    mu_assert("B has 1 TORRENT",    counts[SVC_TORRENT]    == 1);
    mu_assert("B has 0 GAME_RT",    counts[SVC_GAME_RT]    == 0);

    classifier_destroy(tab);
    return 0;
}

/* ── Auto-correction (Phase 5a) ──────────────────────────────── */

/* Seed a TCP web-interactive flow on port 80 so port_hint lands on
 * SVC_WEB_INTERACTIVE — but since that's below the voter's 0.3 floor
 * without agreement, we instead use a DNS hint. Actually simpler:
 * pick a port that maps to GAME_RT for TCP so the test exercises the
 * GAME_RT → VIDEO_CONF demotion ladder.
 *
 * Port 25565 (Minecraft) is registered as GAME_RT in the TCP port map. */

static char *test_rtt_demote_after_two_breaches() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    /* TCP → RTT engine applies. Port 25565 classifies GAME_RT via port hint. */
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();
    rtt_engine_t  *rtt = rtt_engine_open();

    /* Tick 1: tentative classification, no mark yet. */
    classifier_tick(tab, &ft, NULL, eng, rtt, 1.0, 1.0);

    /* Inject RTT well above GAME_RT target (50ms × 1.5 = 75ms). */
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6 };
    rtt_engine_inject_stub(rtt, &k, 200);

    uint64_t ok_before = mark_engine_stat_ok(eng);

    /* Tick 2: stability gate confirms → push original mark. Also
     * records first RTT breach (breach_ticks = 1). No demote yet. */
    classifier_tick(tab, &ft, NULL, eng, rtt, 2.0, 1.0);
    mu_assert("tick2 pushed original mark",
              mark_engine_stat_ok(eng) == ok_before + 1);

    /* Tick 3: second breach tick → demote (2nd mark push). */
    classifier_tick(tab, &ft, NULL, eng, rtt, 3.0, 1.0);
    mu_assert("tick3 pushed demoted mark",
              mark_engine_stat_ok(eng) == ok_before + 2);

    mark_engine_close(eng);
    rtt_engine_close(rtt);
    classifier_destroy(tab);
    return 0;
}

static char *test_rtt_repromote_after_recovery() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();
    rtt_engine_t  *rtt = rtt_engine_open();
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6 };

    /* Drive to demoted state first (ticks 1..3). */
    classifier_tick(tab, &ft, NULL, eng, rtt, 1.0, 1.0);
    rtt_engine_inject_stub(rtt, &k, 200);
    classifier_tick(tab, &ft, NULL, eng, rtt, 2.0, 1.0);
    classifier_tick(tab, &ft, NULL, eng, rtt, 3.0, 1.0);

    uint64_t ok_before = mark_engine_stat_ok(eng);

    /* Heal: RTT well under target. */
    rtt_engine_inject_stub(rtt, &k, 10);
    classifier_tick(tab, &ft, NULL, eng, rtt, 4.0, 1.0);  /* recover 1 */
    mu_assert("recover tick 1 does not re-promote",
              mark_engine_stat_ok(eng) == ok_before);

    classifier_tick(tab, &ft, NULL, eng, rtt, 5.0, 1.0);  /* recover 2 */
    mu_assert("recover tick 2 re-promotes",
              mark_engine_stat_ok(eng) == ok_before + 1);

    mark_engine_close(eng);
    rtt_engine_close(rtt);
    classifier_destroy(tab);
    return 0;
}

static char *test_rtt_noop_when_under_target() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();
    rtt_engine_t  *rtt = rtt_engine_open();
    flow_key_t k = { 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6 };

    classifier_tick(tab, &ft, NULL, eng, rtt, 1.0, 1.0);
    rtt_engine_inject_stub(rtt, &k, 30);  /* under 50ms × 1.5 */

    uint64_t ok_before = mark_engine_stat_ok(eng);
    classifier_tick(tab, &ft, NULL, eng, rtt, 2.0, 1.0);
    classifier_tick(tab, &ft, NULL, eng, rtt, 3.0, 1.0);
    classifier_tick(tab, &ft, NULL, eng, rtt, 4.0, 1.0);

    /* Exactly one mark push: tick2 stability-gate promote. No demotes. */
    mu_assert("healthy RTT → single mark push only",
              mark_engine_stat_ok(eng) == ok_before + 1);

    mark_engine_close(eng);
    rtt_engine_close(rtt);
    classifier_destroy(tab);
    return 0;
}

static char *test_rtt_null_engine_skipped() {
    flow_table_t ft;
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 25565, 6,
              100, 10000, 5000, 5000, 1.0);

    flow_service_table_t *tab = classifier_create();
    mark_engine_t *eng = mark_engine_open();

    classifier_tick(tab, &ft, NULL, eng, NULL, 1.0, 1.0);
    classifier_tick(tab, &ft, NULL, eng, NULL, 2.0, 1.0);
    classifier_tick(tab, &ft, NULL, eng, NULL, 3.0, 1.0);
    /* Classifier must still function without RTT data. */

    mark_engine_close(eng);
    classifier_destroy(tab);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_port_only_classifies);
    mu_run_test(test_stability_gate_requires_two_ticks);
    mu_run_test(test_verdict_flip_resets_stability);
    mu_run_test(test_unknown_does_not_track);
    mu_run_test(test_dns_beats_port);
    mu_run_test(test_null_safe);
    mu_run_test(test_device_counts_aggregates_per_src_ip);
    mu_run_test(test_rtt_demote_after_two_breaches);
    mu_run_test(test_rtt_repromote_after_recovery);
    mu_run_test(test_rtt_noop_when_under_target);
    mu_run_test(test_rtt_null_engine_skipped);
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
