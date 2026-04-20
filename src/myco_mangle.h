/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_mangle.h — iptables mangle rule builder (mark → DSCP)
 *
 * Maintains the MYCOFLOW chain in the mangle table:
 *
 *   Chain POSTROUTING (policy ACCEPT)
 *     target     prot opt in  out    source  dest
 *     MYCOFLOW   all  --  any wan    any     any
 *
 *   Chain MYCOFLOW (1 references)
 *     DSCP  all  --  *  *  0.0.0.0/0  0.0.0.0/0  connmark match 0x1  DSCP set 0x2e
 *     DSCP  all  --  *  *  0.0.0.0/0  0.0.0.0/0  connmark match 0x2  DSCP set 0x28
 *     ...
 *
 * The service_t → DSCP mapping is profile-dependent: gaming profile maps
 * GAME_RT to EF, remote-work maps VIDEO_CONF to EF, etc. On profile
 * switch, mangle_apply() flushes MYCOFLOW and refills it.
 *
 * Reversible: mangle_clear() detaches the chain and deletes it. Called
 * on shutdown or SIGTERM to leave the router in a clean state.
 */
#ifndef MYCO_MANGLE_H
#define MYCO_MANGLE_H

#include <stddef.h>
#include <stdint.h>

/* One mark→DSCP mapping entry. */
typedef struct {
    uint32_t ct_mark;   /* connmark value (1..11 today, service_t) */
    uint8_t  dscp;      /* 6-bit DSCP (0..63). CS0=0, CS4=32, EF=46 */
} mark_dscp_rule_t;

/* Apply the rule set. Flushes existing MYCOFLOW chain (if any),
 * reinstalls rules, and ensures POSTROUTING has a jump on egress_iface.
 * Uses iptables(-nft); `nft` native is a future optimization.
 *
 * Returns 0 on success, -1 on any subcommand failure (chain left in
 * an unknown state — caller should log + retry next cycle).
 *
 * `egress_iface` must be alphanumeric + ". _ -" only (validated).
 */
int mangle_apply(const mark_dscp_rule_t *rules, size_t n,
                 const char *egress_iface);

/* Tear down MYCOFLOW chain. Idempotent — missing chain is not an error. */
int mangle_clear(const char *egress_iface);

/* ── Profile-aware rebuild (Phase 4c) ────────────────────────────
 * Usage:
 *   1. mangle_profile_begin()              — flush dispatch + old profile chains
 *   2. mangle_profile_rules(name, rules, n)— install one sub-chain per profile
 *   3. mangle_profile_bind_ip(ip, name)    — src-IP → profile sub-chain jump
 *   4. mangle_profile_bind_default(name)   — catch-all for unbound IPs
 *   5. mangle_profile_commit(iface)        — hook dispatch from POSTROUTING
 *
 * All sub-chains live under MYCOFLOW_PROF_<name>. The dispatch chain
 * (MYCOFLOW_DISPATCH) is the single POSTROUTING jump target.
 *
 * Profile names are validated (alphanumeric + '-' '_' up to 24 chars).
 * IP strings use the same validator as myco_act.c.
 */
int mangle_profile_begin(void);
int mangle_profile_rules(const char *profile_name,
                         const mark_dscp_rule_t *rules, size_t n);
int mangle_profile_bind_ip(const char *ip, const char *profile_name);
int mangle_profile_bind_default(const char *profile_name);
int mangle_profile_commit(const char *egress_iface);

/* Internal (exposed for tests): validate profile name. */
int mangle_profile_name_is_safe(const char *name);

/* Internal (exposed for tests): validate dotted-quad IP. */
int mangle_ip_is_safe(const char *ip);

/* Internal (exposed for unit tests): validate interface name. Returns 1
 * if safe, 0 otherwise. Matches the same predicate used in myco_act.c. */
int mangle_iface_is_safe(const char *iface);

#endif /* MYCO_MANGLE_H */
