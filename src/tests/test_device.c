/*
 * test_device.c - Unit tests for per-device persona tracking
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_device.h"
#include "../myco_flow.h"

int tests_run = 0;

/* Helper: create a flow entry with given src_ip, bytes, packets */
static void add_flow(flow_table_t *ft, const char *src_ip_str, const char *dst_ip_str,
                     uint16_t sport, uint16_t dport, uint8_t proto,
                     uint64_t packets, uint64_t bytes, double now) {
    flow_key_t key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, src_ip_str, &key.src_ip);
    inet_pton(AF_INET, dst_ip_str, &key.dst_ip);
    key.src_port = sport;
    key.dst_port = dport;
    key.protocol = proto;
    flow_table_update(ft, &key, packets, bytes, now);
}

static char *test_device_aggregation() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 1000.0;

    /* Device A: 192.168.1.10 — 3 small-packet flows (gaming-like) */
    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 500, 30000, now);  /* 60 bytes/pkt */
    add_flow(&ft, "192.168.1.10", "8.8.4.4", 40001, 443, 17, 300, 18000, now);  /* 60 bytes/pkt */
    add_flow(&ft, "192.168.1.10", "1.1.1.1", 40002, 53, 17, 100, 6000, now);    /* 60 bytes/pkt */

    /* Device B: 192.168.1.20 — 1 elephant flow (bulk download) */
    add_flow(&ft, "192.168.1.20", "10.0.0.1", 50000, 80, 6, 10000, 15000000, now); /* 1500 bytes/pkt */

    device_table_aggregate(&dt, &ft, now);

    /* Check Device A */
    uint32_t ip_a;
    inet_pton(AF_INET, "192.168.1.10", &ip_a);
    device_entry_t *dev_a = NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt.devices[i].active && dt.devices[i].ip == ip_a) {
            dev_a = &dt.devices[i];
            break;
        }
    }
    mu_assert("error, device A not found", dev_a != NULL);
    mu_assert("error, device A should have 3 flows", dev_a->flow_count == 3);
    mu_assert("error, device A avg_pkt_size should be ~60", dev_a->avg_pkt_size < 100.0);
    mu_assert("error, device A should NOT have elephant flow", dev_a->elephant_flow == 0);

    /* Check Device B */
    uint32_t ip_b;
    inet_pton(AF_INET, "192.168.1.20", &ip_b);
    device_entry_t *dev_b = NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt.devices[i].active && dt.devices[i].ip == ip_b) {
            dev_b = &dt.devices[i];
            break;
        }
    }
    mu_assert("error, device B not found", dev_b != NULL);
    mu_assert("error, device B should have 1 flow", dev_b->flow_count == 1);
    mu_assert("error, device B avg_pkt_size should be ~1500", dev_b->avg_pkt_size > 1000.0);
    mu_assert("error, device B should have elephant flow", dev_b->elephant_flow == 1);

    return 0;
}

static char *test_device_persona_inference() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    double now = 1000.0;

    /* Device A: gaming-like traffic (small packets, few flows, evenly distributed)
     * Need 3+ flows with similar byte counts to avoid false elephant detection
     * (one flow >60% of total would trigger elephant_flow = BULK +2) */
    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 500, 30000, now);
    add_flow(&ft, "192.168.1.10", "8.8.4.4", 40001, 443, 17, 400, 24000, now);
    add_flow(&ft, "192.168.1.10", "1.1.1.1", 40002, 53, 17, 300, 18000, now);

    /* Device B: bulk traffic (large packets, 1 elephant flow) */
    add_flow(&ft, "192.168.1.20", "10.0.0.1", 50000, 80, 6, 10000, 15000000, now);

    /* Run 4 cycles to build persona history (need 3+ votes) */
    for (int cycle = 0; cycle < 4; cycle++) {
        device_table_aggregate(&dt, &ft, now + cycle);
        device_table_update_personas(&dt);
    }

    /* Check personas */
    uint32_t ip_a, ip_b;
    inet_pton(AF_INET, "192.168.1.10", &ip_a);
    inet_pton(AF_INET, "192.168.1.20", &ip_b);

    device_entry_t *dev_a = NULL, *dev_b = NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt.devices[i].active && dt.devices[i].ip == ip_a) dev_a = &dt.devices[i];
        if (dt.devices[i].active && dt.devices[i].ip == ip_b) dev_b = &dt.devices[i];
    }

    mu_assert("error, device A not found", dev_a != NULL);
    mu_assert("error, device B not found", dev_b != NULL);
    mu_assert("error, device A should be INTERACTIVE",
              dev_a->persona == PERSONA_INTERACTIVE);
    mu_assert("error, device B should be BULK",
              dev_b->persona == PERSONA_BULK);

    return 0;
}

static char *test_device_eviction() {
    device_table_t dt;
    flow_table_t ft;
    device_table_init(&dt);
    flow_table_init(&ft);

    add_flow(&ft, "192.168.1.10", "8.8.8.8", 40000, 443, 17, 100, 6000, 1000.0);
    device_table_aggregate(&dt, &ft, 1000.0);

    mu_assert("error, should have 1 device", dt.count == 1);

    /* Evict after 120s */
    device_table_evict_stale(&dt, 1200.0, 120.0);
    mu_assert("error, device should be evicted", dt.count == 0);

    return 0;
}

static char *all_tests() {
    mu_run_test(test_device_aggregation);
    mu_run_test(test_device_persona_inference);
    mu_run_test(test_device_eviction);
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
