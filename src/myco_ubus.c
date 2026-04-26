/*
 * myco_ubus.c
 * Implements ubus API for MycoFlow.
 * Includes JSON file dump fallback when compiled without native ubus support.
 */

#include "myco_ubus.h"
#include "myco_types.h"
#include "myco_persona.h"
#include "myco_device.h"
#include "myco_classifier.h"
#include "myco_service.h"
#include "myco_log.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Per-device table pointer — set by ubus_start() when per_device is enabled */
static const device_table_t *g_device_table = NULL;
static int g_per_device_enabled = 0;

/* Per-flow service table pointer — set by main when flow_aware is enabled. */
static const flow_service_table_t *g_flow_table = NULL;
static int g_flow_aware_enabled = 0;

/* Control-channel mutation targets — registered by main once at startup. */
static control_state_t      *g_control_state = NULL;
static const myco_config_t  *g_control_cfg   = NULL;

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

void myco_set_device_table(const void *dt, int enabled) {
    g_device_table = (const device_table_t *)dt;
    g_per_device_enabled = enabled;
}

void myco_set_flow_table(const void *fst, int enabled) {
    g_flow_table = (const flow_service_table_t *)fst;
    g_flow_aware_enabled = enabled;
}

void myco_set_control_handles(void *control_state, const void *cfg) {
    g_control_state = (control_state_t *)control_state;
    g_control_cfg   = (const myco_config_t *)cfg;
}

/* ── Control-file fallback for static (no-ubus) builds ────────────────────
 *
 * LuCI writes a small JSON to /tmp/myco_control.json; we read it once per
 * loop cycle, apply the requested mutations, and unlink the file so each
 * command is consumed exactly once.
 *
 * Parser is deliberately minimal — string scan with strstr() — to avoid
 * pulling in a JSON library on the constrained target. The fields are
 * single-line shallow keys; no nesting, no escaping. */

static int parse_str_field(const char *buf, const char *key, char *out, size_t outlen) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t n = (size_t)(end - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int parse_int_field(const char *buf, const char *key, long *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static persona_t persona_from_control_str(const char *s) {
    if (!s) return PERSONA_UNKNOWN;
    if (strcasecmp(s, "voip")      == 0) return PERSONA_VOIP;
    if (strcasecmp(s, "gaming")    == 0 ||
        strcasecmp(s, "interactive") == 0) return PERSONA_GAMING;
    if (strcasecmp(s, "video")     == 0) return PERSONA_VIDEO;
    if (strcasecmp(s, "streaming") == 0) return PERSONA_STREAMING;
    if (strcasecmp(s, "bulk")      == 0) return PERSONA_BULK;
    if (strcasecmp(s, "torrent")   == 0) return PERSONA_TORRENT;
    return PERSONA_UNKNOWN;
}

void myco_apply_control_file(void) {
    const char *path = "/tmp/myco_control.json";
    FILE *f = fopen(path, "r");
    if (!f) return;  /* common case — no pending command */

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    /* Always unlink so a malformed file is not retried forever. */
    unlink(path);
    if (n == 0) return;
    buf[n] = '\0';

    /* persona_override: "voip" / "gaming" / ... / "clear" */
    char persona_str[32] = {0};
    if (parse_str_field(buf, "persona_override", persona_str, sizeof(persona_str))) {
        pthread_mutex_lock(&g_state_mutex);
        if (strcasecmp(persona_str, "clear") == 0 ||
            strcasecmp(persona_str, "none")  == 0 ||
            persona_str[0] == '\0') {
            g_persona_override_active = 0;
            g_persona_override = PERSONA_UNKNOWN;
            log_msg(LOG_INFO, "ctl", "persona override cleared");
        } else {
            persona_t p = persona_from_control_str(persona_str);
            if (p != PERSONA_UNKNOWN) {
                g_persona_override = p;
                g_persona_override_active = 1;
                log_msg(LOG_INFO, "ctl", "persona override set: %s", persona_name(p));
            } else {
                log_msg(LOG_WARN, "ctl", "ignoring unknown persona '%s'", persona_str);
            }
        }
        pthread_mutex_unlock(&g_state_mutex);
    }

    /* Bandwidth mutations require control_state + cfg registered. */
    if (!g_control_state || !g_control_cfg) {
        return;
    }
    long v = 0;
    int  bw_min = g_control_cfg->min_bandwidth_kbit;
    int  bw_max = g_control_cfg->max_bandwidth_kbit;
    if (bw_min <= 0) bw_min = 1000;
    if (bw_max <= 0) bw_max = 1000000;

    int new_bw = -1;
    if (parse_int_field(buf, "policy_set_kbit", &v)) {
        new_bw = (int)v;
    } else if (parse_int_field(buf, "policy_boost_kbit", &v)) {
        new_bw = g_control_state->current.bandwidth_kbit + (int)v;
    } else if (parse_int_field(buf, "policy_throttle_kbit", &v)) {
        new_bw = g_control_state->current.bandwidth_kbit - (int)v;
    }
    if (new_bw > 0) {
        if (new_bw < bw_min) new_bw = bw_min;
        if (new_bw > bw_max) new_bw = bw_max;
        pthread_mutex_lock(&g_state_mutex);
        g_control_state->current.bandwidth_kbit = new_bw;
        pthread_mutex_unlock(&g_state_mutex);
        log_msg(LOG_INFO, "ctl", "bandwidth manually set: %d kbit", new_bw);
    }
}

/* Visitor state for the flow-array emitter. */
typedef struct {
    FILE *f;
    int   first;
} flow_emit_ctx_t;

static int emit_flow_entry(const flow_service_t *fs, void *user) {
    flow_emit_ctx_t *ctx = (flow_emit_ctx_t *)user;
    char src_str[INET_ADDRSTRLEN];
    char dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &fs->src_ip, src_str, sizeof(src_str));
    inet_ntop(AF_INET, &fs->dst_ip, dst_str, sizeof(dst_str));

    fprintf(ctx->f,
            "%s\n\t\t{\"src\":\"%s\",\"dst\":\"%s\",\"sport\":%u,\"dport\":%u,"
            "\"proto\":%u,\"service\":\"%s\",\"mark\":%u,\"stable\":%u,"
            "\"rtt_ms\":%u,\"demoted\":%u}",
            ctx->first ? "" : ",",
            src_str, dst_str,
            (unsigned)fs->src_port, (unsigned)fs->dst_port,
            (unsigned)fs->proto,
            service_name(fs->service),
            (unsigned)fs->ct_mark,
            (unsigned)fs->stable,
            (unsigned)fs->rtt_ms,
            (unsigned)fs->demoted);
    ctx->first = 0;
    return 0;
}

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
    fprintf(f, "\t\"safe_mode\": %s,\n", g_last_safe_mode ? "true" : "false");

    /* Per-device persona table */
    fprintf(f, "\t\"devices\": [");
    if (g_per_device_enabled && g_device_table) {
        int first = 1;
        for (int i = 0; i < MAX_DEVICES; i++) {
            const device_entry_t *dev = &g_device_table->devices[i];
            if (!dev->active) {
                continue;
            }
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dev->ip, ip_str, sizeof(ip_str));
            /* Bandwidth: bytes accumulate over the sample interval (0.5s at
             * sample_hz=2). Multiply by 8 for bits, then by 1/interval for
             * per-second rate. Previously used /1.0 which under-reported by 2x. */
            const double SAMPLE_INTERVAL_S = 0.5;
            fprintf(f, "%s\n\t\t{\"ip\":\"%s\",\"persona\":\"%s\",\"flows\":%d,\"udp\":%d,\"tcp\":%d,\"udp_avg_pkt\":%d,\"bytes\":%llu,\"avg_pkt\":%d,\"elephant\":%d,\"rx_bps\":%.0f,\"tx_bps\":%.0f,\"override\":%s}",
                    first ? "" : ",",
                    ip_str, persona_name(dev->persona),
                    dev->flow_count,
                    dev->udp_flows,
                    dev->tcp_flows,
                    (int)dev->udp_avg_pkt,
                    (unsigned long long)dev->total_bytes,
                    (int)dev->avg_pkt_size,
                    dev->elephant_flow,
                    dev->rx_bytes * 8.0 / SAMPLE_INTERVAL_S,
                    dev->tx_bytes * 8.0 / SAMPLE_INTERVAL_S,
                    dev->override_active ? "true" : "false");
            first = 0;
        }
    }
    fprintf(f, "]");

    /* Per-flow service classification + RTT state */
    if (g_flow_aware_enabled && g_flow_table) {
        fprintf(f, ",\n\t\"flows\": [");
        flow_emit_ctx_t ctx = { .f = f, .first = 1 };
        classifier_for_each(g_flow_table, emit_flow_entry, &ctx);
        fprintf(f, "]");
    }

    fprintf(f, "\n}\n");
    
    fclose(f);
    rename("/tmp/myco_state.json.tmp", "/tmp/myco_state.json");
    
    pthread_mutex_unlock(&g_state_mutex);
}
