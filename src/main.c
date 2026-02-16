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
char      g_last_reason[128];

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

    double interval_s = 1.0 / cfg.sample_hz;
    log_msg(LOG_INFO, "main", "baseline capture: %d samples", cfg.baseline_samples);
    sense_get_idle_baseline(cfg.egress_iface, cfg.probe_host, cfg.baseline_samples, interval_s, cfg.dummy_metrics, &baseline);
    log_msg(LOG_INFO, "main", "baseline rtt=%.2fms jitter=%.2fms", baseline.rtt_ms, baseline.jitter_ms);

    double last_action_ts = 0.0;
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

        ebpf_tick(&cfg);

        /* EWMA smoothing */
        double raw_rtt = metrics.rtt_ms;
        double raw_jitter = metrics.jitter_ms;
        metrics.rtt_ms = ewma_update(&ewma_rtt, metrics.rtt_ms, cfg.ewma_alpha);
        metrics.jitter_ms = ewma_update(&ewma_jitter, metrics.jitter_ms, cfg.ewma_alpha);

        /* Infer */
        persona_t persona = persona_update(&persona_state, &metrics);
        if (g_persona_override_active) {
            persona = g_persona_override;
        }
        policy_t desired;
        char reason[128];
        int change = control_decide(&control_state, &cfg, &metrics, &baseline, persona, &desired, reason, sizeof(reason));

        /* Update shared state */
        g_last_metrics = metrics;
        g_last_baseline = baseline;
        g_last_persona = persona;
        g_last_policy = control_state.current;
        strncpy(g_last_reason, reason, sizeof(g_last_reason) - 1);
        g_last_reason[sizeof(g_last_reason) - 1] = '\0';

        log_msg(LOG_INFO, "loop",
                "rtt=%.2f(raw=%.2f)ms jitter=%.2f(raw=%.2f)ms tx=%.0fbps rx=%.0fbps cpu=%.1f%% persona=%s bw=%dkbit reason=%s",
                metrics.rtt_ms, raw_rtt, metrics.jitter_ms, raw_jitter, metrics.tx_bps, metrics.rx_bps, metrics.cpu_pct,
                persona_name(persona), control_state.current.bandwidth_kbit, reason);

        dump_metrics(&cfg, &metrics, persona, reason);

        /* Act */
        if (control_state.safe_mode) {
            log_msg(LOG_WARN, "loop", "safe-mode active, skipping actuation");
        } else if (change) {
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

        /* Stabilize */
        sleep_interval(interval_s);
    }

    log_msg(LOG_INFO, "main", "shutdown complete");
    ubus_stop();
    ebpf_shutdown();
    return 0;
}
