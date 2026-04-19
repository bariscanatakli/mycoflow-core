/*
 * test_device.c - Unit tests for per-device persona tracking + hint aggregation
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_device.h"
#include "../myco_flow.h"

int tests_run = 0;

/* Stub: g_stop is defined in main.c but needed by dns_sniff_thread.
 * Tests don't call the sniffer thread, so a stub suffices. */
volatile sig_atomic_t g_stop = 0;

/* Helper: find a device entry by IP string */
static device_entry_t *find_dev(device_table_t *dt, const char *ip_str) {
    uint32_t ip;
    inet_pton(AF_INET, ip_str, &ip);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt->devices[i].active && dt->devices[i].ip == ip)
            return &dt->devices[i];
    }
    return NULL;
}

/* Helper: create a flow entry */
static void add_flow(flow_table_t *ft, const char *src_ip_str, const char *dst_ip_str,
                     uint16_t sport, uint16_t dport, uint8_t proto,
                     uint64_t packets, uint64_t bytes, uint64_t rx_bytes, double now) {
    flow_key_t key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, src_ip_str, &key.src_ip);
    inet_pton(AF_INET, dst_ip_str, &key.dst_ip);
    key.src_port = sport;
    key.dst_port = dport;
    key.protocol = proto;
    flow_table_update(ft, &key, packets, 0, bytes, rx_bytes, now);
}

/* ── Aggregation test ──────────────────────────────────────── */
static char *test_device_aggregation() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 1000.0;

    /* Device A: 192.168.1.10 — 3 small-packet UDP flows */
    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 500, 30000, 0, now);
    add_flow(&ft, "192.168.1.10", "8.8.4.4", 40001, 443, 17, 300, 18000, 0, now);
    add_flow(&ft, "192.168.1.10", "1.1.1.1", 40002,  53, 17, 100,  6000, 0, now);

    /* Device B: 192.168.1.20 — 1 elephant flow (bulk download) */
    add_flow(&ft, "192.168.1.20", "10.0.0.1", 50000, 80, 6, 10000, 15000000, 0, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev_a = find_dev(&dt, "192.168.1.10");
    device_entry_t *dev_b = find_dev(&dt, "192.168.1.20");

    mu_assert("error, device A not found", dev_a != NULL);
    mu_assert("error, device A should have 3 flows", dev_a->flow_count == 3);
    mu_assert("error, device A avg_pkt_size should be ~60", dev_a->avg_pkt_size < 100.0);
    mu_assert("error, device A should NOT have elephant flow", dev_a->elephant_flow == 0);

    mu_assert("error, device B not found", dev_b != NULL);
    mu_assert("error, device B should have 1 flow", dev_b->flow_count == 1);
    mu_assert("error, device B avg_pkt_size should be ~1500", dev_b->avg_pkt_size > 1000.0);
    mu_assert("error, device B should have elephant flow", dev_b->elephant_flow == 1);

    return 0;
}

/* ── TX/RX aggregation test ────────────────────────────────── */
static char *test_device_tx_rx_aggregation() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 2000.0;

    add_flow(&ft, "192.168.1.30", "1.2.3.4", 60000, 443, 6,
             5000, 50000, 5000000, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev_c = find_dev(&dt, "192.168.1.30");
    mu_assert("error, device C not found", dev_c != NULL);
    mu_assert("error, device C tx_bytes should be 50000",  dev_c->tx_bytes == 50000);
    mu_assert("error, device C rx_bytes should be 5000000", dev_c->rx_bytes == 5000000);
    mu_assert("error, device C tx_rx_ratio should be < 0.25", dev_c->tx_rx_ratio < 0.25);

    return 0;
}

/* ── avg_pkt_size bidirectional regression test ────────────── */
/* Regression: device downloading at 13 Mbps was classified as GAMING
 * because avg_pkt_size used TX-only bytes (small ACKs ~48B).
 * Fix: avg_pkt_size = (tx_bytes + rx_bytes) / (tx_packets + rx_packets).
 * With tx=200 pkts × 60B ACKs + rx=5000 pkts × 1400B data → bidir avg ~1333B. */
static char *test_device_avg_pkt_bidir() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 3000.0;

    /* Simulate a download flow: 200 small TX ACKs (12000 B) + 5000 large RX data (7000000 B)
     * rx_packets via second conntrack packets= field */
    flow_key_t key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, "192.168.1.40", &key.src_ip);
    inet_pton(AF_INET, "52.1.2.3",     &key.dst_ip);
    key.src_port = 60000; key.dst_port = 443; key.protocol = 6;
    flow_table_update(&ft, &key, 200, 5000, 12000, 7000000, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev = find_dev(&dt, "192.168.1.40");
    mu_assert("error, download device not found", dev != NULL);
    /* (12000 + 7000000) / (200 + 5000) = 7012000 / 5200 ≈ 1348B, well above 350B gaming threshold */
    mu_assert("error, download avg_pkt should be > 350B (not gaming-sized)", dev->avg_pkt_size > 350.0);
    /* Sanity: should be roughly 1350B */
    mu_assert("error, download avg_pkt should be < 2000B", dev->avg_pkt_size < 2000.0);

    return 0;
}

/* ── Persona inference test ────────────────────────────────── */
static char *test_device_persona_inference() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 1000.0;

    /* Device A: gaming-like traffic (small packets, few flows, evenly distributed) */
    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 500, 30000, 0, now);
    add_flow(&ft, "192.168.1.10", "8.8.4.4", 40001, 443, 17, 400, 24000, 0, now);
    add_flow(&ft, "192.168.1.10", "1.1.1.1", 40002,  53, 17, 300, 18000, 0, now);

    /* Device B: bulk traffic (large packets, 1 elephant flow) */
    add_flow(&ft, "192.168.1.20", "10.0.0.1", 50000, 80, 6, 10000, 15000000, 0, now);

    for (int cycle = 0; cycle < 4; cycle++) {
        device_table_aggregate(&dt, &ft, now + cycle, NULL);
        device_table_update_personas(&dt, NULL);
    }

    device_entry_t *dev_a = find_dev(&dt, "192.168.1.10");
    device_entry_t *dev_b = find_dev(&dt, "192.168.1.20");

    mu_assert("error, device A not found", dev_a != NULL);
    mu_assert("error, device B not found", dev_b != NULL);
    mu_assert("error, device A should be GAMING", dev_a->persona == PERSONA_GAMING);
    mu_assert("error, device B should be BULK",   dev_b->persona == PERSONA_BULK);

    return 0;
}

/* ── Eviction test ─────────────────────────────────────────── */
static char *test_device_eviction() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 100, 6000, 0, 1000.0);
    device_table_aggregate(&dt, &ft, 1000.0, NULL);

    mu_assert("error, should have 1 device", dt.count == 1);

    device_table_evict_stale(&dt, 1200.0, 120.0);
    mu_assert("error, device should be evicted", dt.count == 0);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * HINT AGGREGATION TESTS
 * ══════════════════════════════════════════════════════════════ */

/* ── Gaming hint from Riot ports ───────────────────────────── */
static char *test_hint_gaming_riot_ports() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 3000.0;

    /* LoL player: 3 flows on Riot TCP game ports + 2 on unknown ports */
    add_flow(&ft, "192.168.1.50", "104.160.131.1", 50000, 5000, 6, 200, 100000, 300000, now);
    add_flow(&ft, "192.168.1.50", "104.160.131.2", 50001, 5100, 6, 150, 80000,  200000, now);
    add_flow(&ft, "192.168.1.50", "104.160.131.3", 50002, 5200, 6, 100, 50000,  150000, now);
    add_flow(&ft, "192.168.1.50", "1.1.1.1",       50003,  443, 6,  50, 20000,   50000, now);
    add_flow(&ft, "192.168.1.50", "8.8.8.8",       50004,   53, 17,  10,  1000,    500, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev = find_dev(&dt, "192.168.1.50");
    mu_assert("hint_riot: device found", dev != NULL);
    mu_assert("hint_riot: dominant_hint should be GAMING",
              dev->dominant_hint == PERSONA_GAMING);
    mu_assert("hint_riot: has_hint should be 1", dev->has_hint == 1);
    mu_assert("hint_riot: GAMING votes should be 3",
              dev->hint_votes[PERSONA_GAMING] == 3);

    return 0;
}

/* ── Mixed hints: gaming + voip, voip wins by priority ──────── */
static char *test_hint_priority_tiebreak() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 4000.0;

    /* 2 gaming flows (Valve UDP) + 2 VOIP flows (STUN) */
    add_flow(&ft, "192.168.1.60", "1.2.3.4", 40000, 27015, 17, 100, 10000, 5000, now);
    add_flow(&ft, "192.168.1.60", "1.2.3.5", 40001, 27020, 17, 100, 10000, 5000, now);
    add_flow(&ft, "192.168.1.60", "5.6.7.8", 40002,  3478, 17,  50,  3000, 2000, now);
    add_flow(&ft, "192.168.1.60", "5.6.7.9", 40003,  3479, 17,  50,  3000, 2000, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev = find_dev(&dt, "192.168.1.60");
    mu_assert("hint_tie: device found", dev != NULL);
    /* 2 GAMING + 2 VOIP → tie → VOIP wins (lower enum = higher priority) */
    mu_assert("hint_tie: dominant_hint should be VOIP (priority tiebreak)",
              dev->dominant_hint == PERSONA_VOIP);

    return 0;
}

/* ── No hint: all flows on port 443 ────────────────────────── */
static char *test_hint_no_hint_port443() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 5000.0;

    add_flow(&ft, "192.168.1.70", "1.2.3.4", 40000, 443, 6, 500, 500000, 2000000, now);
    add_flow(&ft, "192.168.1.70", "1.2.3.5", 40001, 443, 6, 300, 300000, 1000000, now);

    device_table_aggregate(&dt, &ft, now, NULL);

    device_entry_t *dev = find_dev(&dt, "192.168.1.70");
    mu_assert("hint_443: device found", dev != NULL);
    mu_assert("hint_443: dominant_hint should be UNKNOWN",
              dev->dominant_hint == PERSONA_UNKNOWN);
    mu_assert("hint_443: has_hint should be 0", dev->has_hint == 0);

    return 0;
}

/* ── LoL persona integration: hint rescues from BULK ──────── */
static char *test_hint_lol_persona_integration() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 6000.0;

    /* LoL: one dominant TCP game flow (elephant) + browser/chat flows.
     * Without hint: elephant → BULK. With hint: GAMING.
     * Byte counts tuned so bandwidth ≈ 3.5 Mbps (realistic for LoL).
     * bandwidth_bps = (tx_bytes + rx_bytes) * 8.0 / 0.5 */
    add_flow(&ft, "192.168.1.80", "104.160.131.1", 50000, 5015, 6,
             500, 30000, 180000, now);   /* game server — dominant (~3.4 Mbps) */
    add_flow(&ft, "192.168.1.80", "1.2.3.4", 50001, 443, 6,
             10, 1000, 3000, now);       /* browser (tiny) */
    add_flow(&ft, "192.168.1.80", "1.2.3.5", 50002, 443, 6,
             8, 800, 2000, now);         /* chat (tiny) */

    /* Run 4 cycles to build persona history */
    for (int cycle = 0; cycle < 4; cycle++) {
        device_table_aggregate(&dt, &ft, now + cycle, NULL);
        device_table_update_personas(&dt, NULL);
    }

    device_entry_t *dev = find_dev(&dt, "192.168.1.80");
    mu_assert("lol_fix: device found", dev != NULL);
    mu_assert("lol_fix: has_hint should be 1", dev->has_hint == 1);
    mu_assert("lol_fix: dominant_hint should be GAMING",
              dev->dominant_hint == PERSONA_GAMING);
    mu_assert("lol_fix: persona should be GAMING (not BULK!)",
              dev->persona == PERSONA_GAMING);

    return 0;
}

static char *all_tests() {
    /* Original tests */
    mu_run_test(test_device_aggregation);
    mu_run_test(test_device_tx_rx_aggregation);
    mu_run_test(test_device_avg_pkt_bidir);
    mu_run_test(test_device_persona_inference);
    mu_run_test(test_device_eviction);

    /* Hint aggregation tests */
    mu_run_test(test_hint_gaming_riot_ports);
    mu_run_test(test_hint_priority_tiebreak);
    mu_run_test(test_hint_no_hint_port443);
    mu_run_test(test_hint_lol_persona_integration);
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
