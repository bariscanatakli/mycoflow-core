/*
 * myco_ubus.c
 * Implements ubus API for MycoFlow.
 * Includes JSON file dump fallback when compiled without native ubus support.
 */

#include "myco_ubus.h"
#include "myco_types.h"
#include "myco_persona.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

extern pthread_mutex_t g_state_mutex;
extern metrics_t g_last_metrics;
extern metrics_t g_last_baseline;
extern policy_t g_last_policy;
extern persona_t g_last_persona;
extern char g_last_reason[128];
extern int g_persona_override_active;
extern persona_t g_persona_override;
extern int g_last_safe_mode;

#ifdef HAVE_UBUS
#include <libubox/blobmsg_json.h>
#include <libubus.h>

static struct ubus_context *ctx = NULL;
static struct blob_buf b;

// Actual Ubus implementation (restored)
// Note: This block is currently inactive in static builds, but preserved for future dynamic linking.

static int ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
                      struct ubus_request_data *req, const char *method,
                      struct blob_attr *msg) {
    blob_buf_init(&b, 0);
    
    pthread_mutex_lock(&g_state_mutex);
    
    void *m = blobmsg_open_table(&b, "metrics");
    blobmsg_add_double(&b, "rtt_ms", g_last_metrics.rtt_ms);
    blobmsg_add_double(&b, "jitter_ms", g_last_metrics.jitter_ms);
    blobmsg_add_u64(&b, "tx_bps", (uint64_t)g_last_metrics.tx_bps);
    blobmsg_add_u64(&b, "rx_bps", (uint64_t)g_last_metrics.rx_bps);
    blobmsg_add_double(&b, "cpu_pct", g_last_metrics.cpu_pct);
    blobmsg_add_u32(&b, "qdisc_backlog", g_last_metrics.qdisc_backlog);
    blobmsg_add_u32(&b, "qdisc_drops", g_last_metrics.qdisc_drops);
    blobmsg_close_table(&b, m);

    void *base = blobmsg_open_table(&b, "baseline");
    blobmsg_add_double(&b, "rtt_ms", g_last_baseline.rtt_ms);
    blobmsg_add_double(&b, "jitter_ms", g_last_baseline.jitter_ms);
    blobmsg_close_table(&b, base);

    blobmsg_add_string(&b, "persona", persona_name(g_last_persona));
    blobmsg_add_string(&b, "reason", g_last_reason);
    
    void *pol = blobmsg_open_table(&b, "policy");
    blobmsg_add_u32(&b, "bandwidth_kbit", g_last_policy.bandwidth_kbit);
    blobmsg_close_table(&b, pol);

    blobmsg_add_u8(&b, "safe_mode", g_last_safe_mode);
    
    pthread_mutex_unlock(&g_state_mutex);
    
    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static const struct ubus_method myco_methods[] = {
    UBUS_METHOD_NOARG("status", ubus_status),
};

static struct ubus_object_type myco_obj_type =
    UBUS_OBJECT_TYPE("myco", myco_methods);

static struct ubus_object myco_obj = {
    .name = "myco",
    .type = &myco_obj_type,
    .methods = myco_methods,
    .n_methods = ARRAY_SIZE(myco_methods),
};

void ubus_init_service(void) {
    ctx = ubus_connect(NULL);
    if (!ctx) return;
    ubus_add_uloop(ctx);
    ubus_add_object(ctx, &myco_obj);
}

void ubus_tick(void) {
    // uloop handled in main or separate thread? 
    // Usually ubus needs uloop_run(), but for this hybrid approach we might rely on myco_dump_json for now.
}

void ubus_cleanup(void) {
    if (ctx) ubus_free(ctx);
}

#else

// Stubs for when ubus is not available (Static Build)
void ubus_init_service(void) {}
void ubus_tick(void) {}
void ubus_cleanup(void) {}

#endif

// Fallback: Dump state to JSON file for Lua Bridge
void myco_dump_json(void) {
    if (pthread_mutex_trylock(&g_state_mutex) != 0) {
        return;
    }

    FILE *f = fopen("/tmp/myco_state.json.tmp", "w");
    if (!f) {
        pthread_mutex_unlock(&g_state_mutex);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "\t\"metrics\": {\n");
    fprintf(f, "\t\t\"rtt_ms\": %.2f,\n", g_last_metrics.rtt_ms);
    fprintf(f, "\t\t\"jitter_ms\": %.2f,\n", g_last_metrics.jitter_ms);
    fprintf(f, "\t\t\"tx_bps\": %.0f,\n", g_last_metrics.tx_bps);
    fprintf(f, "\t\t\"rx_bps\": %.0f,\n", g_last_metrics.rx_bps);
    fprintf(f, "\t\t\"cpu_pct\": %.1f,\n", g_last_metrics.cpu_pct);
    fprintf(f, "\t\t\"qdisc_backlog\": %u,\n", g_last_metrics.qdisc_backlog);
    fprintf(f, "\t\t\"qdisc_drops\": %u,\n", g_last_metrics.qdisc_drops);
    fprintf(f, "\t\t\"avg_pkt_size\": %.1f\n", g_last_metrics.avg_pkt_size);
    fprintf(f, "\t},\n");

    fprintf(f, "\t\"baseline\": {\n");
    fprintf(f, "\t\t\"rtt_ms\": %.2f,\n", g_last_baseline.rtt_ms);
    fprintf(f, "\t\t\"jitter_ms\": %.2f\n", g_last_baseline.jitter_ms);
    fprintf(f, "\t},\n");

    fprintf(f, "\t\"policy\": {\n");
    fprintf(f, "\t\t\"bandwidth_kbit\": %d\n", g_last_policy.bandwidth_kbit);
    fprintf(f, "\t},\n");

    fprintf(f, "\t\"persona\": \"%s\",\n", persona_name(g_last_persona));
    fprintf(f, "\t\"reason\": \"%s\",\n", g_last_reason);
    
    fprintf(f, "\t\"persona_override\": %s,\n", g_persona_override_active ? "true" : "false");
    fprintf(f, "\t\"persona_override_value\": \"%s\",\n", persona_name(g_persona_override));
    fprintf(f, "\t\"safe_mode\": %s\n", g_last_safe_mode ? "true" : "false");

    fprintf(f, "}\n");
    
    fclose(f);
    rename("/tmp/myco_state.json.tmp", "/tmp/myco_state.json");
    
    pthread_mutex_unlock(&g_state_mutex);
}
