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

/* ── Profile-aware rebuild (Phase 4c) ─────────────────────────── */

#define MYCO_DISPATCH "MYCOFLOW_DISPATCH"
#define MYCO_PROF_PREFIX "MYCOFLOW_PROF_"

int mangle_profile_name_is_safe(const char *name) {
    if (!name || name[0] == '\0') return 0;
    int i;
    for (i = 0; name[i] != '\0'; i++) {
        if (i >= 24) return 0;
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return 0;
        }
    }
    return i > 0;
}

int mangle_ip_is_safe(const char *ip) {
    if (!ip || ip[0] == '\0') return 0;
    int dots = 0, digits_run = 0;
    for (int i = 0; ip[i] != '\0'; i++) {
        if (i >= 15) return 0;
        char c = ip[i];
        if (c == '.') {
            if (digits_run == 0) return 0;
            digits_run = 0;
            dots++;
        } else if (c >= '0' && c <= '9') {
            if (++digits_run > 3) return 0;
        } else {
            return 0;
        }
    }
    return (dots == 3 && digits_run > 0);
}

/* Delete every chain whose name starts with PREFIX. We enumerate with
 * `iptables -t mangle -S`, collect matching chain names, detach any
 * references in DISPATCH, then flush + delete. */
static void drop_chains_with_prefix(const char *prefix) {
    FILE *fp = popen("iptables -t mangle -S 2>/dev/null", "r");
    if (!fp) return;
    char line[256];
    char cmd[256];
    char chains[16][48];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "-N ", 3) != 0) continue;
        char *name = line + 3;
        name[strcspn(name, "\r\n")] = '\0';
        if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
        if (n >= 16) break;
        strncpy(chains[n], name, sizeof(chains[0]) - 1);
        chains[n][sizeof(chains[0]) - 1] = '\0';
        n++;
    }
    pclose(fp);
    for (int i = 0; i < n; i++) {
        snprintf(cmd, sizeof(cmd), "iptables -t mangle -F %s", chains[i]);
        run_cmd_silent(cmd);
        snprintf(cmd, sizeof(cmd), "iptables -t mangle -X %s", chains[i]);
        run_cmd_silent(cmd);
    }
}

int mangle_profile_begin(void) {
    char cmd[256];

    /* Ensure the dispatch chain exists + is empty. */
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -N %s", MYCO_DISPATCH);
    run_cmd_silent(cmd);
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -F %s", MYCO_DISPATCH);
    if (run_cmd(cmd) != 0) return -1;

    /* Drop old per-profile sub-chains. Order matters: dispatch is
     * already flushed so no jump references remain. */
    drop_chains_with_prefix(MYCO_PROF_PREFIX);
    return 0;
}

int mangle_profile_rules(const char *profile_name,
                         const mark_dscp_rule_t *rules, size_t n) {
    if (!mangle_profile_name_is_safe(profile_name)) {
        log_msg(LOG_WARN, "mangle", "invalid profile name");
        return -1;
    }
    if (n > 32) n = 32;

    char chain[64];
    snprintf(chain, sizeof(chain), "%s%s", MYCO_PROF_PREFIX, profile_name);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -N %s", chain);
    run_cmd_silent(cmd);
    snprintf(cmd, sizeof(cmd), "iptables -t mangle -F %s", chain);
    if (run_cmd(cmd) != 0) return -1;

    for (size_t i = 0; i < n; i++) {
        const mark_dscp_rule_t *r = &rules[i];
        if (r->dscp > 63) continue;
        snprintf(cmd, sizeof(cmd),
                 "iptables -t mangle -A %s -m connmark --mark %u "
                 "-j DSCP --set-dscp %u",
                 chain, r->ct_mark, r->dscp);
        if (run_cmd(cmd) != 0) return -1;
    }
    return 0;
}

int mangle_profile_bind_ip(const char *ip, const char *profile_name) {
    if (!mangle_ip_is_safe(ip) || !mangle_profile_name_is_safe(profile_name)) {
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "iptables -t mangle -A %s -s %s -j %s%s",
             MYCO_DISPATCH, ip, MYCO_PROF_PREFIX, profile_name);
    return run_cmd(cmd);
}

int mangle_profile_bind_default(const char *profile_name) {
    if (!mangle_profile_name_is_safe(profile_name)) return -1;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "iptables -t mangle -A %s -j %s%s",
             MYCO_DISPATCH, MYCO_PROF_PREFIX, profile_name);
    return run_cmd(cmd);
}

int mangle_profile_commit(const char *egress_iface) {
    if (!mangle_iface_is_safe(egress_iface)) {
        log_msg(LOG_WARN, "mangle", "invalid egress iface: '%s'",
                egress_iface ? egress_iface : "(null)");
        return -1;
    }
    char cmd[256];

    /* Idempotently hook dispatch into POSTROUTING. */
    snprintf(cmd, sizeof(cmd),
             "iptables -t mangle -C POSTROUTING -o %s -j %s",
             egress_iface, MYCO_DISPATCH);
    if (run_cmd_silent(cmd) != 0) {
        snprintf(cmd, sizeof(cmd),
                 "iptables -t mangle -A POSTROUTING -o %s -j %s",
                 egress_iface, MYCO_DISPATCH);
        if (run_cmd(cmd) != 0) return -1;
    }

    log_msg(LOG_INFO, "mangle", "profile dispatch active on %s", egress_iface);
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
