/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_mangle.c — iptables mangle rule builder
 *
 * We shell out to iptables(-nft) rather than linking libnftnl directly:
 *   - OpenWrt ships iptables-nft as the default frontend.
 *   - The rule set is tiny (≤12 rows) and rebuilt only on profile
 *     switches, so fork+exec cost is immaterial.
 *   - Shell commands are log-friendly; a ct admin can paste the
 *     equivalent line to reproduce state.
 *
 * Atomicity: we wipe the MYCOFLOW chain before refill, which briefly
 * removes DSCP marking for in-flight packets. The chain jump in
 * POSTROUTING is preserved across rebuilds, so packets whose connmark
 * hasn't been set yet fall through and get DSCP=CS0 (best effort) —
 * the same fate as today. Acceptable for a design that applies 1–2
 * Hz at most.
 */
#include "myco_mangle.h"
#include "myco_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MYCO_CHAIN "MYCOFLOW"

int mangle_iface_is_safe(const char *iface) {
    if (!iface || iface[0] == '\0') return 0;
    for (int i = 0; iface[i] != '\0'; i++) {
        if (i >= 15) return 0;                        /* IFNAMSIZ - 1 */
        char c = iface[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* Run a shell command, return its exit status (0 on success). */
static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (rc != 0) {
        log_msg(LOG_WARN, "mangle", "cmd failed (rc=%d): %s", rc, cmd);
    }
    return rc;
}

/* Silent variant — errors expected (e.g. "chain already exists"). */
static int run_cmd_silent(const char *cmd) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s >/dev/null 2>&1", cmd);
    return system(buf);
}

int mangle_apply(const mark_dscp_rule_t *rules, size_t n, const char *egress_iface) {
    if (!rules && n > 0)                return -1;
    if (!mangle_iface_is_safe(egress_iface)) {
        log_msg(LOG_WARN, "mangle", "invalid egress iface: '%s'", egress_iface ? egress_iface : "(null)");
        return -1;
    }
    if (n > 32) {
        log_msg(LOG_WARN, "mangle", "too many rules (%zu), capped at 32", n);
        n = 32;
    }

    char cmd[512];

    /* Create chain (ignore "already exists") */
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -N %s", MYCO_CHAIN);
    run_cmd_silent(cmd);

    /* Flush existing rules */
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -F %s", MYCO_CHAIN);
    if (run_cmd(cmd) != 0) {
        return -1;
    }

    /* Refill */
    for (size_t i = 0; i < n; i++) {
        const mark_dscp_rule_t *r = &rules[i];
        if (r->dscp > 63) {
            log_msg(LOG_WARN, "mangle", "skipping rule %zu (dscp=%u out of range)", i, r->dscp);
            continue;
        }
        snprintf(cmd, sizeof(cmd),
                 "iptables -t mangle -A %s -m connmark --mark %u "
                 "-j DSCP --set-dscp %u",
                 MYCO_CHAIN, r->ct_mark, r->dscp);
        if (run_cmd(cmd) != 0) {
            return -1;
        }
    }

    /* Ensure POSTROUTING jumps to our chain on egress (idempotent) */
    snprintf(cmd, sizeof(cmd),
             "iptables -t mangle -C POSTROUTING -o %s -j %s",
             egress_iface, MYCO_CHAIN);
    if (run_cmd_silent(cmd) != 0) {
        snprintf(cmd, sizeof(cmd),
                 "iptables -t mangle -A POSTROUTING -o %s -j %s",
                 egress_iface, MYCO_CHAIN);
        if (run_cmd(cmd) != 0) {
            return -1;
        }
    }

    log_msg(LOG_INFO, "mangle", "applied %zu rule(s) on %s", n, egress_iface);
    return 0;
}

int mangle_clear(const char *egress_iface) {
    if (!mangle_iface_is_safe(egress_iface)) {
        return -1;
    }

    char cmd[512];

    /* Detach from POSTROUTING (ignore if absent) */
    snprintf(cmd, sizeof(cmd),
             "iptables -t mangle -D POSTROUTING -o %s -j %s",
             egress_iface, MYCO_CHAIN);
    run_cmd_silent(cmd);

    /* Flush + delete chain */
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -F %s", MYCO_CHAIN);
    run_cmd_silent(cmd);
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -X %s", MYCO_CHAIN);
    run_cmd_silent(cmd);

    log_msg(LOG_INFO, "mangle", "cleared MYCOFLOW chain");
    return 0;
}
