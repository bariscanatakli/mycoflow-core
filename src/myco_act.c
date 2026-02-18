/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_act.c — CAKE actuation & metric dumping
 */
#include "myco_act.h"
#include "myco_log.h"
#include "myco_persona.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int act_apply_policy(const char *iface, const policy_t *policy, int no_tc, int force_fail) {
    if (!iface || !policy) {
        return 0;
    }

    if (force_fail) {
        log_msg(LOG_WARN, "act", "forced actuation failure");
        return 0;
    }

    if (no_tc) {
        log_msg(LOG_INFO, "act", "tc disabled, would set %s to %d kbit", iface, policy->bandwidth_kbit);
        return 1;
    }

    char cmd[256];

    /* Try 'change' first to update bandwidth without resetting CAKE's internal
     * queue state, flow hash, and tin statistics. Fall back to 'replace' only
     * on first install (when no CAKE qdisc exists yet). */
    snprintf(cmd, sizeof(cmd), "tc qdisc change dev %s root cake bandwidth %dkbit", iface, policy->bandwidth_kbit);
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_DEBUG, "act", "tc change failed (rc=%d), trying replace (first install?)", rc);
        snprintf(cmd, sizeof(cmd), "tc qdisc replace dev %s root cake bandwidth %dkbit", iface, policy->bandwidth_kbit);
        rc = system(cmd);
        if (rc != 0) {
            log_msg(LOG_WARN, "act", "tc replace also failed (rc=%d)", rc);
            return 0;
        }
    }

    log_msg(LOG_INFO, "act", "applied cake bandwidth %d kbit on %s", policy->bandwidth_kbit, iface);
    return 1;
}

int act_apply_persona_tin(const char *iface, persona_t persona,
                          int bandwidth_kbit, int no_tc, int force_fail) {
    if (!iface) {
        return 0;
    }
    if (force_fail) {
        return 0;
    }

    /* Adapt CAKE AQM target latency to persona:
     *   INTERACTIVE → tight target (5ms) keeps queue short for gaming/VoIP
     *   BULK        → relaxed target (20ms) allows deeper queue for throughput
     *   UNKNOWN     → CAKE default (5ms with diffserv4)
     *
     * diffserv4 enables 4 CAKE tins so DSCP-marked traffic still gets
     * correct per-class treatment alongside the target adjustment. */
    int target_ms;
    int interval_ms;
    const char *persona_label;

    switch (persona) {
        case PERSONA_INTERACTIVE:
            target_ms    = 5;
            interval_ms  = 50;
            persona_label = "interactive";
            break;
        case PERSONA_BULK:
            target_ms    = 20;
            interval_ms  = 200;
            persona_label = "bulk";
            break;
        default:
            target_ms    = 5;
            interval_ms  = 100;
            persona_label = "unknown";
            break;
    }

    if (no_tc) {
        log_msg(LOG_INFO, "act", "tc disabled, would set diffserv4 target %dms persona=%s",
                target_ms, persona_label);
        return 1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc change dev %s root cake bandwidth %dkbit diffserv4 target %dms interval %dms",
             iface, bandwidth_kbit, target_ms, interval_ms);
    int rc = system(cmd);
    if (rc != 0) {
        /* On first call or if diffserv4 wasn't set, fall back to replace */
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc replace dev %s root cake bandwidth %dkbit diffserv4 target %dms interval %dms",
                 iface, bandwidth_kbit, target_ms, interval_ms);
        rc = system(cmd);
        if (rc != 0) {
            log_msg(LOG_WARN, "act", "cake tin setup failed (rc=%d) for persona=%s", rc, persona_label);
            return 0;
        }
    }

    log_msg(LOG_INFO, "act", "cake tin: persona=%s target=%dms interval=%dms bw=%dkbit on %s",
            persona_label, target_ms, interval_ms, bandwidth_kbit, iface);
    return 1;
}

void dump_metrics(const myco_config_t *cfg,
                  const metrics_t *metrics,
                  persona_t persona,
                  const char *reason) {
    if (!cfg || !metrics || !reason) {
        return;
    }
    if (cfg->metric_file[0] == '\0') {
        return;
    }
    FILE *fp = fopen(cfg->metric_file, "a");
    if (!fp) {
        log_msg(LOG_WARN, "metrics", "metric file open failed: %s", cfg->metric_file);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(fp,
            "{\"ts\":%ld.%03ld,\"rtt_ms\":%.2f,\"jitter_ms\":%.2f,\"tx_bps\":%.0f,\"rx_bps\":%.0f,\"cpu_pct\":%.1f,\"persona\":\"%s\",\"reason\":\"%s\"}\n",
            ts.tv_sec, ts.tv_nsec / 1000000L,
            metrics->rtt_ms, metrics->jitter_ms,
            metrics->tx_bps, metrics->rx_bps,
            metrics->cpu_pct, persona_name(persona), reason);
    fclose(fp);
}
