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

    classifier_tick(tab, &ft, NULL, eng, 1.0, 1.0);

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
    classifier_tick(tab, &ft, NULL, eng, 1.0, 1.0);
    mu_assert("no mark push after first tick",
              mark_engine_stat_ok(eng) == ok0);

    classifier_tick(tab, &ft, NULL, eng, 2.0, 1.0);
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

    classifier_tick(tab, &ft, NULL, eng, 1.0, 1.0);  /* tentative */
    /* Flip to port 6881 → TORRENT */
    ft.entries[0].key.dst_port = 6881;
    /* Same flow index but different key — need a fresh slot; reset & reseed */
    memset(&ft, 0, sizeof(ft));
    seed_flow(&ft, 0, 0x0a0a0a01u, 0x08080808u, 40000, 6881, 17,
              100, 10000, 5000, 5000, 2.0);
    classifier_tick(tab, &ft, NULL, eng, 2.0, 1.0);
    /* New flow, tentative only → no mark yet */
    mu_assert("new flow tentative → no mark push",
              mark_engine_stat_ok(eng) == ok0);

    classifier_tick(tab, &ft, NULL, eng, 3.0, 1.0);
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
    classifier_tick(tab, &ft, NULL, NULL, 1.0, 1.0);
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
    classifier_tick(tab, &ft, &dns, NULL, 1.0, 1.0);

    flow_key_t k = { 0x0a0a0a01u, 0xd83acf8eu, 40000, 5222, 17 };
    mu_assert("DNS VIDEO_VOD beats port GAME_RT",
              classifier_get_service(tab, &k) == SVC_VIDEO_VOD);

    classifier_destroy(tab);
    dns_cache_destroy(&dns);
    return 0;
}

/* ── NULL inputs are safe ──────────────────────────────────────── */
static char *test_null_safe() {
    classifier_tick(NULL, NULL, NULL, NULL, 0.0, 1.0);
    mu_assert("get on NULL → UNKNOWN",
              classifier_get_service(NULL, NULL) == SVC_UNKNOWN);
    mu_assert("count on NULL → 0",
              classifier_active_count(NULL) == 0);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_port_only_classifies);
    mu_run_test(test_stability_gate_requires_two_ticks);
    mu_run_test(test_verdict_flip_resets_stability);
    mu_run_test(test_unknown_does_not_track);
    mu_run_test(test_dns_beats_port);
    mu_run_test(test_null_safe);
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
