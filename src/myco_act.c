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
    snprintf(cmd, sizeof(cmd), "tc qdisc replace dev %s root cake bandwidth %dkbit", iface, policy->bandwidth_kbit);
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_WARN, "act", "tc call failed (rc=%d)", rc);
        return 0;
    }

    log_msg(LOG_INFO, "act", "applied cake bandwidth %d kbit on %s", policy->bandwidth_kbit, iface);
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
