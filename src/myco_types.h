/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_types.h — Shared type definitions
 */
#ifndef MYCO_TYPES_H
#define MYCO_TYPES_H

#include <signal.h>
#include <stdint.h>
#include <pthread.h>

/* ── Log Levels ─────────────────────────────────────────────── */
typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
} log_level_t;

/* ── Configuration ──────────────────────────────────────────── */
typedef struct {
    int    enabled;
    char   egress_iface[32];
    double sample_hz;
    double max_cpu_pct;
    int    log_level;
    int    dummy_metrics;
    int    baseline_samples;
    double action_cooldown_s;
    double action_rate_limit;
    int    bandwidth_kbit;
    int    bandwidth_step_kbit;
    int    min_bandwidth_kbit;
    int    max_bandwidth_kbit;
    int    no_tc;
    char   metric_file[128];
    char   probe_host[64];
    int    force_act_fail;
    int    ebpf_enabled;
    char   ebpf_obj[128];
    int    ebpf_attach;
    char   ebpf_tc_dir[16];
    double ewma_alpha;
} myco_config_t;

/* ── Metrics ────────────────────────────────────────────────── */
typedef struct {
    double rtt_ms;
    double jitter_ms;
    double rx_bps;
    double tx_bps;
    double cpu_pct;
    /* Qdisc stats (from netlink) */
    uint32_t qdisc_backlog;
    uint32_t qdisc_drops;
    uint32_t qdisc_overlimits;
    /* Packet-size signal (from /proc/net/dev) */
    double avg_pkt_size;
} metrics_t;

/* ── Persona ────────────────────────────────────────────────── */
typedef enum {
    PERSONA_UNKNOWN     = 0,
    PERSONA_INTERACTIVE = 1,
    PERSONA_BULK        = 2
} persona_t;

typedef struct {
    persona_t current;
    persona_t history[5];
    int       history_len;
} persona_state_t;

/* ── Policy / Control ───────────────────────────────────────── */
typedef struct {
    int bandwidth_kbit;
    int boosted;
} policy_t;

typedef struct {
    policy_t current;
    policy_t last_stable;
    int      safe_mode;
    int      stable_cycles;
} control_state_t;

/* ── Shared global state (defined in main.c) ────────────────── */
extern volatile sig_atomic_t g_stop;
extern volatile sig_atomic_t g_reload;

extern persona_t g_persona_override;
extern int        g_persona_override_active;
extern metrics_t  g_last_metrics;
extern metrics_t  g_last_baseline;
extern policy_t   g_last_policy;
extern persona_t  g_last_persona;
extern char        g_last_reason[128];
extern pthread_mutex_t g_state_mutex;

/* ── Utility ────────────────────────────────────────────────── */
double clamp_double(double value, double min_value, double max_value);
double now_monotonic_s(void);

#endif /* MYCO_TYPES_H */
