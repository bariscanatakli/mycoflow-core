/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * main.c — Daemon entry point and main control loop
 *
 * All modules are included via headers; this file owns:
 *   - Global shared state definitions (extern'd in myco_types.h)
 *   - Signal handling
 *   - Main reflexive loop: Sense → Infer → Act → Stabilize
 */
#include "myco_types.h"
#include "myco_log.h"
#include "myco_config.h"
#include "myco_sense.h"
#include "myco_persona.h"
#include "myco_control.h"
#include "myco_act.h"
#include "myco_ebpf.h"
#include "myco_ewma.h"
#include "myco_flow.h"
#include "myco_ubus.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ── Global shared state (declared extern in myco_types.h) ──── */

volatile sig_atomic_t g_stop   = 0;
volatile sig_atomic_t g_reload = 0;

persona_t g_persona_override        = PERSONA_UNKNOWN;
int       g_persona_override_active = 0;
metrics_t g_last_metrics;
metrics_t g_last_baseline;
policy_t  g_last_policy;
persona_t g_last_persona = PERSONA_UNKNOWN;
int       g_last_safe_mode = 0;
char      g_last_reason[128];
pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Signal handler ─────────────────────────────────────────── */

static void handle_signal(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_stop = 1;
    } else if (signo == SIGHUP) {
        g_reload = 1;
    }
}

static void sleep_interval(double seconds) {
    if (seconds <= 0.0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000.0);
    nanosleep(&ts, NULL);
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    struct utsname buffer;
    myco_config_t cfg;
    metrics_t baseline;
    metrics_t metrics;
    persona_state_t persona_state;
    control_state_t control_state;
    ewma_filter_t ewma_rtt;
    ewma_filter_t ewma_jitter;
    flow_table_t  flow_table;

    if (config_load(&cfg) != 0) {
        fprintf(stderr, "MycoFlow config load failed\n");
        return 1;
    }

    log_init(cfg.log_level);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    log_msg(LOG_INFO, "main", "MycoFlow daemon starting");
    if (uname(&buffer) == 0) {
        log_msg(LOG_INFO, "main", "system: %s %s", buffer.sysname, buffer.machine);
    }

    sense_init(cfg.egress_iface, cfg.dummy_metrics);
    persona_init(&persona_state);
    control_init(&control_state, cfg.bandwidth_kbit);
    g_last_policy = control_state.current;
    snprintf(g_last_reason, sizeof(g_last_reason), "startup");

    ebpf_init(&cfg);
    ubus_start(&cfg, &control_state);

    ewma_init(&ewma_rtt);
    ewma_init(&ewma_jitter);
    flow_table_init(&flow_table);

    double interval_s = 1.0 / cfg.sample_hz;
    log_msg(LOG_INFO, "main", "baseline capture: %d samples", cfg.baseline_samples);
    sense_get_idle_baseline(cfg.egress_iface, cfg.probe_host, cfg.baseline_samples, interval_s, cfg.dummy_metrics, &baseline);
    log_msg(LOG_INFO, "main", "baseline rtt=%.2fms jitter=%.2fms", baseline.rtt_ms, baseline.jitter_ms);

    double last_action_ts = 0.0;
    int    loop_cycle     = 0;
    double min_action_interval = cfg.action_cooldown_s;
    if (cfg.action_rate_limit > 0.0) {
        double rate_interval = 1.0 / cfg.action_rate_limit;
        if (rate_interval > min_action_interval) {
            min_action_interval = rate_interval;
        }
    }

    /* ── Reflexive loop: Sense → Infer → Act → Stabilize ────── */

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            if (config_reload(&cfg) == 0) {
                log_set_level(cfg.log_level);
                interval_s = 1.0 / cfg.sample_hz;
                min_action_interval = cfg.action_cooldown_s;
                if (cfg.action_rate_limit > 0.0) {
                    double rate_interval = 1.0 / cfg.action_rate_limit;
                    if (rate_interval > min_action_interval) {
                        min_action_interval = rate_interval;
                    }
                }
                log_msg(LOG_INFO, "main", "baseline capture: %d samples", cfg.baseline_samples);
                sense_get_idle_baseline(cfg.egress_iface, cfg.probe_host, cfg.baseline_samples, interval_s, cfg.dummy_metrics, &baseline);
                log_msg(LOG_INFO, "main", "config reloaded");
            }
        }

        if (!cfg.enabled) {
            log_msg(LOG_INFO, "main", "disabled, sleeping");
            sleep_interval(interval_s);
            continue;
        }

        /* Sense */
        if (sense_sample(cfg.egress_iface, cfg.probe_host, interval_s, cfg.dummy_metrics, &metrics) != 0) {
            log_msg(LOG_WARN, "main", "sense sample failed");
        }

        /* Populate eBPF counters into metrics (no-op if libbpf unavailable) */
        if (ebpf_read_stats(&metrics.ebpf_rx_pkts, &metrics.ebpf_rx_bytes) != 0) {
            metrics.ebpf_rx_pkts  = 0;
            metrics.ebpf_rx_bytes = 0;
        }

        ebpf_tick(&cfg);

        /* Flow table: populate from conntrack, evict stale (>60s) */
        double ft_now = now_monotonic_s();
        flow_table_populate_conntrack(&flow_table, ft_now);
        flow_table_evict_stale(&flow_table, ft_now, 60.0);

        /* Flow-derived persona signals — populate into metrics */
        metrics.active_flows  = flow_table_active_count(&flow_table);
        metrics.elephant_flow = flow_table_has_elephant(&flow_table, 0.60);

        /* eBPF packet rate: delta from previous cumulative counter (pkt/s) */
        static uint64_t prev_ebpf_pkts = 0;
        if (prev_ebpf_pkts > 0 && metrics.ebpf_rx_pkts >= prev_ebpf_pkts && interval_s > 0.0) {
            metrics.ebpf_pkt_rate = (double)(metrics.ebpf_rx_pkts - prev_ebpf_pkts) / interval_s;
        } else {
            metrics.ebpf_pkt_rate = 0.0;
        }
        prev_ebpf_pkts = metrics.ebpf_rx_pkts;

        /* EWMA smoothing */
        double raw_rtt = metrics.rtt_ms;
        double raw_jitter = metrics.jitter_ms;
        metrics.rtt_ms = ewma_update(&ewma_rtt, metrics.rtt_ms, cfg.ewma_alpha);
        metrics.jitter_ms = ewma_update(&ewma_jitter, metrics.jitter_ms, cfg.ewma_alpha);

        /* Infer */
        pthread_mutex_lock(&g_state_mutex);
        int persona_override = g_persona_override_active;
        persona_t override_val = g_persona_override;
        pthread_mutex_unlock(&g_state_mutex);

        persona_t prev_persona = persona_state.current;
        persona_t persona = persona_update(&persona_state, &metrics);
        if (persona_override) {
            persona = override_val;
        }
        int persona_changed = (persona != prev_persona);
        policy_t desired;
        char reason[128];
        double now_ts = now_monotonic_s();
        int change = control_decide(&control_state, &cfg, &metrics, &baseline, persona, now_ts, &desired, reason, sizeof(reason));

        /* Update shared state */
        pthread_mutex_lock(&g_state_mutex);
        g_last_metrics = metrics;
        g_last_baseline = baseline;
        g_last_persona = persona;
        g_last_policy = control_state.current;
        g_last_safe_mode = control_state.safe_mode;
        strncpy(g_last_reason, reason, sizeof(g_last_reason) - 1);
        g_last_reason[sizeof(g_last_reason) - 1] = '\0';
        pthread_mutex_unlock(&g_state_mutex);

        /* Dump state to JSON for Lua bridge */
        myco_dump_json();

        log_msg(LOG_INFO, "loop",
                "rtt=%.2f(raw=%.2f)ms jitter=%.2f(raw=%.2f)ms tx=%.0fbps rx=%.0fbps cpu=%.1f%% qbl=%u qdr=%u flows=%d persona=%s bw=%dkbit reason=%s ebpf_pkts=%llu ebpf_bytes=%llu",
                metrics.rtt_ms, raw_rtt, metrics.jitter_ms, raw_jitter, metrics.tx_bps, metrics.rx_bps, metrics.cpu_pct,
                metrics.qdisc_backlog, metrics.qdisc_drops,
                flow_table_active_count(&flow_table),
                persona_name(persona), control_state.current.bandwidth_kbit, reason,
                (unsigned long long)metrics.ebpf_rx_pkts,
                (unsigned long long)metrics.ebpf_rx_bytes);

        dump_metrics(&cfg, &metrics, persona, reason);

        /* Act */
        if (control_state.safe_mode) {
            log_msg(LOG_WARN, "loop", "safe-mode active, skipping actuation");
        } else {
            /* Persona tin update: apply CAKE target latency when persona changes.
             * Not rate-limited — persona changes are infrequent and tin
             * reconfiguration does not disrupt existing flows. */
            if (persona_changed) {
                act_apply_persona_tin(cfg.egress_iface, persona,
                                      control_state.current.bandwidth_kbit,
                                      cfg.no_tc, cfg.force_act_fail);
            }

            if (change) {
                double now = now_monotonic_s();
                if ((now - last_action_ts) >= min_action_interval) {
                    int ok = act_apply_policy(cfg.egress_iface, &desired, cfg.no_tc, cfg.force_act_fail);
                    control_on_action_result(&control_state, ok);
                    if (ok) {
                        control_state.current = desired;
                        last_action_ts = now;
                    }
                } else {
                    log_msg(LOG_DEBUG, "loop", "action skipped (cooldown)");
                }
            }
        }

        /* Stabilize */
        loop_cycle++;

        /* Sliding baseline: drift toward current conditions every N cycles.
         * Keeps the reference point fresh without full recalibration.
         * Only updates rtt_ms and jitter_ms (probe-based fields). */
        if (cfg.baseline_update_interval > 0 &&
            (loop_cycle % cfg.baseline_update_interval) == 0) {
            sense_update_baseline_sliding(&baseline, &metrics, cfg.baseline_decay);
            log_msg(LOG_DEBUG, "main", "baseline updated: rtt=%.2fms jitter=%.2fms",
                    baseline.rtt_ms, baseline.jitter_ms);
        }

        sleep_interval(interval_s);
    }

    log_msg(LOG_INFO, "main", "shutdown complete");
    ubus_stop();
    ebpf_shutdown();
    return 0;
}
