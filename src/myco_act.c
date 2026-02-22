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

/* Validate that an interface name is safe to embed in a shell command.
 * Accepts alphanumeric, '.', '-', '_' up to IFNAMSIZ-1 (15) chars.
 * Returns 1 if valid, 0 if not. */
static int is_valid_iface(const char *name) {
    if (!name || name[0] == '\0') {
        return 0;
    }
    for (int i = 0; name[i] != '\0'; i++) {
        if (i >= 15) {
            return 0; /* IFNAMSIZ - 1 */
        }
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int act_setup_ingress_ifb(const char *wan_iface, const char *ifb_iface,
                          int bandwidth_kbit, int no_tc, int force_fail) {
    if (!wan_iface || !ifb_iface) {
        return 0;
    }
    if (!is_valid_iface(wan_iface) || !is_valid_iface(ifb_iface)) {
        log_msg(LOG_WARN, "act", "invalid interface name: wan='%s' ifb='%s'",
                wan_iface, ifb_iface);
        return 0;
    }
    if (force_fail) {
        log_msg(LOG_WARN, "act", "forced actuation failure (ingress setup)");
        return 0;
    }
    if (no_tc) {
        log_msg(LOG_INFO, "act", "tc disabled, would setup IFB %s <- %s @ %d kbit",
                ifb_iface, wan_iface, bandwidth_kbit);
        return 1;
    }

    char cmd[512];

    /* Create IFB device; EEXIST is normal on restart — ignore rc */
    snprintf(cmd, sizeof(cmd), "ip link add %s type ifb 2>/dev/null", ifb_iface);
    (void)system(cmd);

    snprintf(cmd, sizeof(cmd), "ip link set %s up", ifb_iface);
    if (system(cmd) != 0) {
        log_msg(LOG_WARN, "act", "ifb link set up failed for %s", ifb_iface);
        return 0;
    }

    /* Attach ingress qdisc to WAN; ignore EEXIST */
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s handle ffff: ingress 2>/dev/null", wan_iface);
    (void)system(cmd);

    /* Redirect all ingress WAN packets to IFB; ignore EEXIST */
    snprintf(cmd, sizeof(cmd),
             "tc filter add dev %s parent ffff: protocol all u32 match u32 0 0 "
             "action mirred egress redirect dev %s 2>/dev/null",
             wan_iface, ifb_iface);
    (void)system(cmd);

    /* Install CAKE on IFB root with diffserv4 — change first (idempotent if
     * CAKE already present), fall back to replace on first install.
     * Use diffserv4 upfront so act_apply_ingress_policy can later issue
     * 'change' without switching diffserv mode (which may reset state). */
    snprintf(cmd, sizeof(cmd),
             "tc qdisc change dev %s root cake bandwidth %dkbit diffserv4",
             ifb_iface, bandwidth_kbit);
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_DEBUG, "act",
                "ingress change failed (rc=%d), trying replace (first install?)", rc);
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc replace dev %s root cake bandwidth %dkbit diffserv4",
                 ifb_iface, bandwidth_kbit);
        rc = system(cmd);
        if (rc != 0) {
            log_msg(LOG_WARN, "act", "ingress CAKE setup failed on %s (rc=%d)", ifb_iface, rc);
            return 0;
        }
    }

    log_msg(LOG_INFO, "act", "ingress IFB ready: %s <- %s @ %d kbit",
            ifb_iface, wan_iface, bandwidth_kbit);
    return 1;
}

int act_apply_ingress_policy(const char *ifb_iface, persona_t persona,
                             int bandwidth_kbit, int no_tc, int force_fail) {
    if (!ifb_iface) {
        return 0;
    }
    if (!is_valid_iface(ifb_iface)) {
        log_msg(LOG_WARN, "act", "invalid ingress interface name: '%s'", ifb_iface);
        return 0;
    }
    if (force_fail) {
        return 0;
    }

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
        log_msg(LOG_INFO, "act", "tc disabled, would set ingress target %dms persona=%s",
                target_ms, persona_label);
        return 1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc change dev %s root cake bandwidth %dkbit diffserv4 target %dms interval %dms",
             ifb_iface, bandwidth_kbit, target_ms, interval_ms);
    int rc = system(cmd);
    if (rc != 0) {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc replace dev %s root cake bandwidth %dkbit diffserv4 target %dms interval %dms",
                 ifb_iface, bandwidth_kbit, target_ms, interval_ms);
        rc = system(cmd);
        if (rc != 0) {
            log_msg(LOG_WARN, "act", "ingress CAKE tin failed on %s (rc=%d)", ifb_iface, rc);
            return 0;
        }
    }

    log_msg(LOG_INFO, "act", "ingress cake tin: persona=%s target=%dms interval=%dms bw=%dkbit on %s",
            persona_label, target_ms, interval_ms, bandwidth_kbit, ifb_iface);
    return 1;
}

void act_teardown_ingress_ifb(const char *wan_iface, const char *ifb_iface, int no_tc) {
    if (!wan_iface || !ifb_iface) {
        return;
    }
    if (!is_valid_iface(wan_iface) || !is_valid_iface(ifb_iface)) {
        log_msg(LOG_WARN, "act", "teardown: invalid iface wan='%s' ifb='%s'", wan_iface, ifb_iface);
        return;
    }
    if (no_tc) {
        log_msg(LOG_INFO, "act", "tc disabled, would teardown IFB %s <- %s", ifb_iface, wan_iface);
        return;
    }

    char cmd[256];

    /* Remove redirect filter from WAN ingress qdisc */
    snprintf(cmd, sizeof(cmd), "tc filter del dev %s parent ffff: 2>/dev/null", wan_iface);
    (void)system(cmd);

    /* Remove ingress qdisc from WAN — restores default pfifo_fast behaviour */
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s ingress 2>/dev/null", wan_iface);
    (void)system(cmd);

    /* Bring down and delete the IFB device */
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ifb_iface);
    (void)system(cmd);

    snprintf(cmd, sizeof(cmd), "ip link del %s 2>/dev/null", ifb_iface);
    (void)system(cmd);

    log_msg(LOG_INFO, "act", "ingress IFB torn down: %s <- %s", ifb_iface, wan_iface);
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
