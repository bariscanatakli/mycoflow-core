/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ubus.c — ubus API surface (OpenWrt IPC)
 */
#ifdef HAVE_UBUS

#include "myco_ubus.h"
#include "myco_log.h"
#include "myco_act.h"
#include "myco_control.h"
#include "myco_persona.h"

#include <pthread.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

static struct ubus_context *g_ubus_ctx = NULL;
static struct ubus_object g_ubus_obj;
static pthread_t g_ubus_thread;
static int g_ubus_running = 0;
static int g_ubus_thread_started = 0;
static myco_config_t *g_ubus_cfg = NULL;
static control_state_t *g_ubus_control = NULL;

/* ── Policy constants ───────────────────────────────────────── */

enum {
    POLICY_BW,
    POLICY_STEP,
    __POLICY_MAX
};

enum {
    PERSONA_NAME,
    __PERSONA_MAX
};

static const struct blobmsg_policy policy_policy[__POLICY_MAX] = {
    [POLICY_BW]   = { .name = "bandwidth_kbit", .type = BLOBMSG_TYPE_INT32 },
    [POLICY_STEP] = { .name = "step_kbit",      .type = BLOBMSG_TYPE_INT32 }
};

static const struct blobmsg_policy persona_policy[__PERSONA_MAX] = {
    [PERSONA_NAME] = { .name = "persona", .type = BLOBMSG_TYPE_STRING }
};

/* ── Handlers ───────────────────────────────────────────────── */

static void ubus_fill_status(struct blob_buf *buf) {
    pthread_mutex_lock(&g_state_mutex);
    void *metrics = blobmsg_open_table(buf, "metrics");
    blobmsg_add_double(buf, "rtt_ms", g_last_metrics.rtt_ms);
    blobmsg_add_double(buf, "jitter_ms", g_last_metrics.jitter_ms);
    blobmsg_add_double(buf, "tx_bps", g_last_metrics.tx_bps);
    blobmsg_add_double(buf, "rx_bps", g_last_metrics.rx_bps);
    blobmsg_add_double(buf, "cpu_pct", g_last_metrics.cpu_pct);
    blobmsg_close_table(buf, metrics);

    void *baseline = blobmsg_open_table(buf, "baseline");
    blobmsg_add_double(buf, "rtt_ms", g_last_baseline.rtt_ms);
    blobmsg_add_double(buf, "jitter_ms", g_last_baseline.jitter_ms);
    blobmsg_close_table(buf, baseline);

    void *policy = blobmsg_open_table(buf, "policy");
    blobmsg_add_u32(buf, "bandwidth_kbit", g_last_policy.bandwidth_kbit);
    blobmsg_add_u32(buf, "boosted", g_last_policy.boosted);
    blobmsg_close_table(buf, policy);

    blobmsg_add_string(buf, "persona", persona_name(g_last_persona));
    blobmsg_add_string(buf, "reason", g_last_reason);
    blobmsg_add_u32(buf, "safe_mode", g_ubus_control ? g_ubus_control->safe_mode : 0);
    blobmsg_add_u32(buf, "persona_override", g_persona_override_active);
    if (g_persona_override_active) {
        blobmsg_add_string(buf, "persona_override_value", persona_name(g_persona_override));
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static int ubus_status(struct ubus_context *ctx,
                       struct ubus_object *obj,
                       struct ubus_request_data *req,
                       const char *method,
                       struct blob_attr *msg) {
    (void)obj; (void)method; (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    ubus_fill_status(&buf);
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_policy_get(struct ubus_context *ctx,
                           struct ubus_object *obj,
                           struct ubus_request_data *req,
                           const char *method,
                           struct blob_attr *msg) {
    (void)obj; (void)method; (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    blobmsg_add_u32(&buf, "bandwidth_kbit", g_last_policy.bandwidth_kbit);
    blobmsg_add_u32(&buf, "boosted", g_last_policy.boosted);
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_policy_set(struct ubus_context *ctx,
                           struct ubus_object *obj,
                           struct ubus_request_data *req,
                           const char *method,
                           struct blob_attr *msg) {
    (void)obj; (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!tb[POLICY_BW] || !g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int bw = blobmsg_get_u32(tb[POLICY_BW]);
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);

    pthread_mutex_lock(&g_state_mutex);
    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 0;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-set");
    }
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

static int ubus_policy_boost(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg) {
    (void)obj; (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int step = g_ubus_cfg->bandwidth_step_kbit;
    if (tb[POLICY_STEP]) {
        step = blobmsg_get_u32(tb[POLICY_STEP]);
    }
    pthread_mutex_lock(&g_state_mutex);
    int bw = g_ubus_control->current.bandwidth_kbit + step;
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);
    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 1;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-boost");
    }
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

static int ubus_policy_throttle(struct ubus_context *ctx,
                                struct ubus_object *obj,
                                struct ubus_request_data *req,
                                const char *method,
                                struct blob_attr *msg) {
    (void)obj; (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int step = g_ubus_cfg->bandwidth_step_kbit;
    if (tb[POLICY_STEP]) {
        step = blobmsg_get_u32(tb[POLICY_STEP]);
    }
    pthread_mutex_lock(&g_state_mutex);
    int bw = g_ubus_control->current.bandwidth_kbit - step;
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);
    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 0;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-throttle");
    }
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

static int ubus_persona_list(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg) {
    (void)obj; (void)method; (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    pthread_mutex_lock(&g_state_mutex);
    blobmsg_add_string(&buf, "current", persona_name(g_last_persona));
    blobmsg_add_u32(&buf, "override_active", g_persona_override_active);
    if (g_persona_override_active) {
        blobmsg_add_string(&buf, "override", persona_name(g_persona_override));
    }
    pthread_mutex_unlock(&g_state_mutex);
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_persona_add(struct ubus_context *ctx,
                            struct ubus_object *obj,
                            struct ubus_request_data *req,
                            const char *method,
                            struct blob_attr *msg) {
    (void)obj; (void)method;
    if (!msg) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    struct blob_attr *tb[__PERSONA_MAX];
    blobmsg_parse(persona_policy, __PERSONA_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!tb[PERSONA_NAME]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    const char *val = blobmsg_get_string(tb[PERSONA_NAME]);
    if (strcmp(val, "interactive") == 0) {
        g_persona_override = PERSONA_INTERACTIVE;
    } else if (strcmp(val, "bulk") == 0) {
        g_persona_override = PERSONA_BULK;
    } else {
        g_persona_override = PERSONA_UNKNOWN;
    }
    pthread_mutex_lock(&g_state_mutex);
    g_persona_override_active = 1;
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

static int ubus_persona_delete(struct ubus_context *ctx,
                               struct ubus_object *obj,
                               struct ubus_request_data *req,
                               const char *method,
                               struct blob_attr *msg) {
    (void)obj; (void)method; (void)msg;
    pthread_mutex_lock(&g_state_mutex);
    g_persona_override_active = 0;
    g_persona_override = PERSONA_UNKNOWN;
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

/* ── Method table & object ──────────────────────────────────── */

static const struct ubus_method myco_methods[] = {
    UBUS_METHOD_NOARG("status", ubus_status),
    UBUS_METHOD_NOARG("policy_get", ubus_policy_get),
    UBUS_METHOD("policy_set", ubus_policy_set, policy_policy),
    UBUS_METHOD("policy_boost", ubus_policy_boost, policy_policy),
    UBUS_METHOD("policy_throttle", ubus_policy_throttle, policy_policy),
    UBUS_METHOD_NOARG("persona_list", ubus_persona_list),
    UBUS_METHOD("persona_add", ubus_persona_add, policy_policy),
    UBUS_METHOD_NOARG("persona_delete", ubus_persona_delete)
};

static struct ubus_object_type myco_obj_type =
    UBUS_OBJECT_TYPE("mycoflow", myco_methods);

/* ── Thread ─────────────────────────────────────────────────── */

static void *ubus_loop(void *arg) {
    (void)arg;
    uloop_init();
    g_ubus_ctx = ubus_connect(NULL);
    if (!g_ubus_ctx) {
        log_msg(LOG_WARN, "ubus", "ubus connect failed");
        uloop_done();
        return NULL;
    }
    ubus_add_uloop(g_ubus_ctx);
    g_ubus_obj.name = "myco";
    g_ubus_obj.type = &myco_obj_type;
    g_ubus_obj.methods = myco_methods;
    g_ubus_obj.n_methods = sizeof(myco_methods) / sizeof(myco_methods[0]);

    if (ubus_add_object(g_ubus_ctx, &g_ubus_obj) != 0) {
        log_msg(LOG_WARN, "ubus", "failed to add ubus object");
        ubus_free(g_ubus_ctx);
        g_ubus_ctx = NULL;
        uloop_done();
        return NULL;
    }

    g_ubus_running = 1;
    uloop_run();
    g_ubus_running = 0;

    if (g_ubus_ctx) {
        ubus_free(g_ubus_ctx);
        g_ubus_ctx = NULL;
    }
    uloop_done();
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────── */

void ubus_start(myco_config_t *cfg, control_state_t *control) {
    g_ubus_cfg = cfg;
    g_ubus_control = control;
    if (pthread_create(&g_ubus_thread, NULL, ubus_loop, NULL) == 0) {
        g_ubus_thread_started = 1;
        log_msg(LOG_INFO, "ubus", "ubus thread started");
    } else {
        log_msg(LOG_WARN, "ubus", "ubus thread start failed");
    }
}

void ubus_stop(void) {
    if (g_ubus_running) {
        uloop_end();
    }
    if (g_ubus_thread_started) {
        pthread_join(g_ubus_thread, NULL);
    }
    g_ubus_thread_started = 0;
}

#endif /* HAVE_UBUS */
