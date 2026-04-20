/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_profile.c — Device priority profiles implementation
 */
#include "myco_profile.h"
#include "myco_log.h"
#include "myco_mangle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── DSCP constants (RFC 4594 / DiffServ) ────────────────────── */
#define DSCP_CS0   0
#define DSCP_CS1   8
#define DSCP_CS2  16
#define DSCP_CS3  24
#define DSCP_CS4  32
#define DSCP_CS5  40
#define DSCP_CS6  48
#define DSCP_CS7  56
#define DSCP_EF   46
#define DSCP_AF41 34
#define DSCP_AF42 36

/* ── Helpers ─────────────────────────────────────────────────── */

static void trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) { s[--len] = '\0'; }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start) memmove(s, s + start, len - start + 1);
}

service_t profile_parse_service(const char *s) {
    if (!s) return SVC_UNKNOWN;
    for (int i = 1; i < SERVICE_COUNT; i++) {
        if (strcasecmp(s, service_name((service_t)i)) == 0) {
            return (service_t)i;
        }
    }
    return SVC_UNKNOWN;
}

int profile_parse_dscp(const char *s) {
    if (!s || !*s) return -1;
    if (strcasecmp(s, "EF")   == 0) return DSCP_EF;
    if (strcasecmp(s, "CS0")  == 0 || strcasecmp(s, "BE") == 0) return DSCP_CS0;
    if (strcasecmp(s, "CS1")  == 0) return DSCP_CS1;
    if (strcasecmp(s, "CS2")  == 0) return DSCP_CS2;
    if (strcasecmp(s, "CS3")  == 0) return DSCP_CS3;
    if (strcasecmp(s, "CS4")  == 0) return DSCP_CS4;
    if (strcasecmp(s, "CS5")  == 0) return DSCP_CS5;
    if (strcasecmp(s, "CS6")  == 0) return DSCP_CS6;
    if (strcasecmp(s, "CS7")  == 0) return DSCP_CS7;
    if (strcasecmp(s, "AF41") == 0) return DSCP_AF41;
    if (strcasecmp(s, "AF42") == 0) return DSCP_AF42;
    /* Numeric */
    char *end;
    long n = strtol(s, &end, 10);
    if (*end == '\0' && n >= 0 && n <= 63) return (int)n;
    return -1;
}

/* ── Built-in default profiles ───────────────────────────────── */

static void set_dscp_common(profile_t *p) {
    /* Defaults shared across profiles; profiles override selectively. */
    p->service_dscp[SVC_UNKNOWN]         = DSCP_CS0;
    p->service_dscp[SVC_GAME_RT]         = DSCP_EF;
    p->service_dscp[SVC_VOIP_CALL]       = DSCP_EF;
    p->service_dscp[SVC_VIDEO_CONF]      = DSCP_CS4;
    p->service_dscp[SVC_VIDEO_LIVE]      = DSCP_CS3;
    p->service_dscp[SVC_VIDEO_VOD]       = DSCP_CS2;
    p->service_dscp[SVC_WEB_INTERACTIVE] = DSCP_CS0;
    p->service_dscp[SVC_BULK_DL]         = DSCP_CS1;
    p->service_dscp[SVC_FILE_SYNC]       = DSCP_CS1;
    p->service_dscp[SVC_TORRENT]         = DSCP_CS1;
    p->service_dscp[SVC_GAME_LAUNCHER]   = DSCP_CS1;
    p->service_dscp[SVC_SYSTEM]          = DSCP_CS0;
}

static void set_winners(profile_t *p, const service_t *list, int n) {
    for (int i = 0; i < n && i < MAX_WINNER_PRIORITY; i++) {
        p->winner_priority[i] = list[i];
    }
    p->num_winners = n;
}

void profile_load_defaults(profile_set_t *ps) {
    if (!ps) return;
    memset(ps, 0, sizeof(*ps));

    /* 0: gaming — RT game first */
    profile_t *g = &ps->profiles[0];
    strncpy(g->name, "gaming", PROFILE_NAME_MAX - 1);
    set_dscp_common(g);
    {
        const service_t list[] = {
            SVC_GAME_RT, SVC_VOIP_CALL, SVC_VIDEO_CONF,
            SVC_VIDEO_LIVE, SVC_VIDEO_VOD, SVC_WEB_INTERACTIVE,
        };
        set_winners(g, list, (int)(sizeof(list) / sizeof(list[0])));
    }

    /* 1: remote_work — conf call first */
    profile_t *rw = &ps->profiles[1];
    strncpy(rw->name, "remote_work", PROFILE_NAME_MAX - 1);
    set_dscp_common(rw);
    rw->service_dscp[SVC_VIDEO_CONF] = DSCP_EF;  /* bump for zoom */
    {
        const service_t list[] = {
            SVC_VIDEO_CONF, SVC_VOIP_CALL, SVC_WEB_INTERACTIVE,
            SVC_GAME_RT, SVC_VIDEO_LIVE, SVC_VIDEO_VOD,
        };
        set_winners(rw, list, (int)(sizeof(list) / sizeof(list[0])));
    }

    /* 2: family_media — video first */
    profile_t *fm = &ps->profiles[2];
    strncpy(fm->name, "family_media", PROFILE_NAME_MAX - 1);
    set_dscp_common(fm);
    {
        const service_t list[] = {
            SVC_VIDEO_LIVE, SVC_VIDEO_VOD, SVC_VIDEO_CONF,
            SVC_VOIP_CALL, SVC_GAME_RT, SVC_WEB_INTERACTIVE,
        };
        set_winners(fm, list, (int)(sizeof(list) / sizeof(list[0])));
    }

    /* 3: auto — no winner priority; derivation uses latency sensitivity */
    profile_t *au = &ps->profiles[3];
    strncpy(au->name, "auto", PROFILE_NAME_MAX - 1);
    set_dscp_common(au);
    au->num_winners = 0;

    ps->num_profiles = 4;
    ps->default_idx = 3;   /* "auto" */
}

const profile_t *profile_find(const profile_set_t *ps, const char *name) {
    if (!ps || !name) return NULL;
    for (int i = 0; i < ps->num_profiles; i++) {
        if (strcasecmp(ps->profiles[i].name, name) == 0) {
            return &ps->profiles[i];
        }
    }
    return NULL;
}

void profile_resolve_bindings(profile_set_t *ps) {
    if (!ps) return;
    for (int i = 0; i < ps->num_bindings; i++) {
        device_profile_binding_t *b = &ps->bindings[i];
        const profile_t *p = profile_find(ps, b->profile_name);
        if (p) {
            b->profile_idx = (int)(p - ps->profiles);
        } else {
            b->profile_idx = -1;
        }
    }
}

const profile_t *profile_for_ip(const profile_set_t *ps, const char *ip) {
    if (!ps) return NULL;
    if (ip) {
        for (int i = 0; i < ps->num_bindings; i++) {
            const device_profile_binding_t *b = &ps->bindings[i];
            if (strcmp(b->ip, ip) == 0 && b->profile_idx >= 0) {
                return &ps->profiles[b->profile_idx];
            }
        }
    }
    if (ps->default_idx >= 0 && ps->default_idx < ps->num_profiles) {
        return &ps->profiles[ps->default_idx];
    }
    return NULL;
}

/* ── UCI parsing ─────────────────────────────────────────────── */

static int profile_index_by_name(profile_set_t *ps, const char *name) {
    for (int i = 0; i < ps->num_profiles; i++) {
        if (strcasecmp(ps->profiles[i].name, name) == 0) return i;
    }
    return -1;
}

static int profile_ensure(profile_set_t *ps, const char *name) {
    int idx = profile_index_by_name(ps, name);
    if (idx >= 0) return idx;
    if (ps->num_profiles >= MAX_PROFILES) return -1;
    idx = ps->num_profiles++;
    memset(&ps->profiles[idx], 0, sizeof(ps->profiles[idx]));
    strncpy(ps->profiles[idx].name, name, PROFILE_NAME_MAX - 1);
    /* New profile starts from common defaults so user only overrides what matters */
    set_dscp_common(&ps->profiles[idx]);
    return idx;
}

/* Strip single/double quotes around a UCI value in place. */
static void unquote(char *v) {
    size_t len = strlen(v);
    if (len >= 2 && (v[0] == '\'' || v[0] == '"') &&
        v[len - 1] == v[0]) {
        v[len - 1] = '\0';
        memmove(v, v + 1, len - 1);
    }
}

/* Parse a single "mycoflow.@profile[N].KEY=VALUE" line into ps. */
static void parse_profile_line(profile_set_t *ps, int idx,
                               const char *key, char *val) {
    if (idx < 0 || idx >= MAX_PROFILES) return;
    unquote(val);

    /* Section anchor: mycoflow.@profile[0]=profile
     * OR a separately named section: mycoflow.profile_gaming=profile
     * We only look at the inner key-value pairs here.
     */
    if (strcmp(key, "name") == 0) {
        /* We anchor profiles by index; a name option renames. */
        strncpy(ps->profiles[idx].name, val, PROFILE_NAME_MAX - 1);
        ps->profiles[idx].name[PROFILE_NAME_MAX - 1] = '\0';
        return;
    }

    if (strcmp(key, "winner_priority") == 0) {
        /* UCI list values arrive as repeated lines — each call appends */
        service_t svc = profile_parse_service(val);
        if (svc == SVC_UNKNOWN) return;
        profile_t *p = &ps->profiles[idx];
        if (p->num_winners < MAX_WINNER_PRIORITY) {
            p->winner_priority[p->num_winners++] = svc;
        }
        return;
    }

    if (strncmp(key, "dscp_", 5) == 0) {
        service_t svc = profile_parse_service(key + 5);
        if (svc == SVC_UNKNOWN) return;
        int d = profile_parse_dscp(val);
        if (d < 0) return;
        ps->profiles[idx].service_dscp[svc] = (uint8_t)d;
        return;
    }
}

/* Parse "mycoflow.@device[N].KEY=VALUE" lines. */
static void parse_device_line(profile_set_t *ps, int idx,
                              const char *key, char *val) {
    if (idx < 0 || idx >= MAX_DEVICE_PROFILE_BINDS) return;
    unquote(val);
    if (idx >= ps->num_bindings) ps->num_bindings = idx + 1;

    if (strcmp(key, "ip") == 0) {
        strncpy(ps->bindings[idx].ip, val, sizeof(ps->bindings[idx].ip) - 1);
    } else if (strcmp(key, "profile") == 0) {
        strncpy(ps->bindings[idx].profile_name, val,
                sizeof(ps->bindings[idx].profile_name) - 1);
    }
}

void profile_load_uci(profile_set_t *ps) {
    if (!ps) return;

    FILE *fp = popen("uci -q show mycoflow 2>/dev/null", "r");
    if (!fp) return;

    /* First pass: collect @profile anchors so indices map to profile slots.
     * We lazily build a compact map: uci_idx → ps->profiles index.
     * Reuses ensure() so name= clauses rename slots in place. */
    int prof_map[MAX_PROFILES];
    for (int i = 0; i < MAX_PROFILES; i++) prof_map[i] = -1;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (strncmp(line, "mycoflow.@profile[", 18) != 0 &&
            strncmp(line, "mycoflow.@device[",  17) != 0) {
            continue;
        }

        /* Extract index */
        char *obr = strchr(line, '[');
        char *cbr = obr ? strchr(obr, ']') : NULL;
        if (!obr || !cbr) continue;
        int uci_idx = atoi(obr + 1);

        /* Rest: ].key=value */
        char *dot = strchr(cbr, '.');
        if (!dot) continue;
        char *eq = strchr(dot + 1, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = dot + 1;
        char *val = eq + 1;

        if (line[9] == '@' && line[10] == 'p') {
            /* mycoflow.@profile[N].KEY */
            if (uci_idx < 0 || uci_idx >= MAX_PROFILES) continue;
            int slot = prof_map[uci_idx];
            if (slot < 0) {
                /* Anchor: create with placeholder name "profileN" */
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "profile%d", uci_idx);
                slot = profile_ensure(ps, tmp);
                if (slot < 0) continue;
                prof_map[uci_idx] = slot;
            }
            parse_profile_line(ps, slot, key, val);
        } else {
            /* mycoflow.@device[N].KEY */
            parse_device_line(ps, uci_idx, key, val);
        }
    }
    pclose(fp);

    profile_resolve_bindings(ps);

    /* Honor `option default <name>` on the root mycoflow section */
    fp = popen("uci -q get mycoflow.mycoflow.default_profile 2>/dev/null", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp)) {
            trim(buf);
            int idx = profile_index_by_name(ps, buf);
            if (idx >= 0) ps->default_idx = idx;
        }
        pclose(fp);
    }
}

int profile_load(profile_set_t *ps) {
    if (!ps) return -1;
    profile_load_defaults(ps);
    profile_load_uci(ps);
    return 0;
}

/* ── Winner derivation ───────────────────────────────────────── */

service_t profile_derive_winner(const profile_t *p,
                                const int counts[SERVICE_COUNT]) {
    if (!counts) return SVC_UNKNOWN;

    if (p && p->num_winners > 0) {
        for (int i = 0; i < p->num_winners; i++) {
            service_t s = p->winner_priority[i];
            if ((int)s > 0 && (int)s < SERVICE_COUNT && counts[s] > 0) {
                return s;
            }
        }
        /* None of the priority services active → fall through to auto */
    }

    /* Auto: pick lowest-enum-index active (most latency-sensitive). */
    for (int i = 1; i < SERVICE_COUNT; i++) {
        if (counts[i] > 0) return (service_t)i;
    }
    return SVC_UNKNOWN;
}

persona_t profile_derive_persona(const profile_t *p,
                                 const int counts[SERVICE_COUNT]) {
    return service_to_persona(profile_derive_winner(p, counts));
}

/* ── Mangle rebuild driver (Phase 4c) ────────────────────────── */

/* Build a mark_dscp_rule_t[] from a profile's service_dscp table. The
 * connmark comes from service_to_ct_mark(); we skip SVC_UNKNOWN (mark=0)
 * since unmarked flows fall through to POSTROUTING's default path. */
static size_t profile_build_rules(const profile_t *p,
                                  mark_dscp_rule_t *out, size_t max) {
    size_t n = 0;
    for (int s = 1; s < SERVICE_COUNT && n < max; s++) {
        uint8_t mark = service_to_ct_mark((service_t)s);
        if (mark == 0) continue;
        out[n].ct_mark = mark;
        out[n].dscp    = p->service_dscp[s];
        n++;
    }
    return n;
}

int profile_apply_mangle(const profile_set_t *ps, const char *egress_iface) {
    if (!ps) return -1;

    if (mangle_profile_begin() != 0) {
        log_msg(LOG_WARN, "profile", "mangle_profile_begin failed");
        return -1;
    }

    /* 1. Install per-profile sub-chains. */
    for (int i = 0; i < ps->num_profiles; i++) {
        const profile_t *p = &ps->profiles[i];
        mark_dscp_rule_t rules[SERVICE_COUNT];
        size_t n = profile_build_rules(p, rules, SERVICE_COUNT);
        if (mangle_profile_rules(p->name, rules, n) != 0) {
            log_msg(LOG_WARN, "profile",
                    "mangle_profile_rules failed for '%s'", p->name);
            return -1;
        }
    }

    /* 2. Bind per-device src-IP jumps to their resolved profile. */
    for (int i = 0; i < ps->num_bindings; i++) {
        const device_profile_binding_t *b = &ps->bindings[i];
        if (b->profile_idx < 0 || b->profile_idx >= ps->num_profiles) continue;
        const profile_t *p = &ps->profiles[b->profile_idx];
        if (mangle_profile_bind_ip(b->ip, p->name) != 0) {
            log_msg(LOG_WARN, "profile",
                    "bind_ip failed: %s → %s", b->ip, p->name);
            /* Non-fatal — keep going so default still gets installed. */
        }
    }

    /* 3. Catch-all default profile. */
    if (ps->default_idx >= 0 && ps->default_idx < ps->num_profiles) {
        const char *def_name = ps->profiles[ps->default_idx].name;
        if (mangle_profile_bind_default(def_name) != 0) {
            log_msg(LOG_WARN, "profile",
                    "bind_default failed for '%s'", def_name);
            return -1;
        }
    }

    /* 4. Hook dispatch into POSTROUTING. */
    if (mangle_profile_commit(egress_iface) != 0) {
        return -1;
    }

    log_msg(LOG_INFO, "profile",
            "mangle rebuilt (%d profile(s), %d binding(s)) on %s",
            ps->num_profiles, ps->num_bindings, egress_iface);
    return 0;
}
