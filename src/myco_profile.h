/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_profile.h — Device priority profiles (Phase 4)
 *
 * A profile describes how a device wants to behave when multiple
 * concurrent services are active. It contains:
 *
 *   - winner_priority[]: ordered service_t list. The first service in
 *     this list that is currently active on the device becomes the
 *     device's persona for monitoring / backoff decisions. This keeps
 *     persona selection dynamic (fed from live classifier output) while
 *     still expressing user intent ("gaming beats YouTube on my PC").
 *
 *   - service_dscp[]: per-service DSCP value used by the MYCOFLOW mangle
 *     chain. The same flow → same mark, but the mark → DSCP table can
 *     be rewritten when the active profile changes (Phase 4c).
 *
 * Built-in profiles (defaults, overrideable via UCI):
 *
 *   gaming        game_rt > voip_call > video_conf > rest
 *   remote_work   video_conf > voip_call > web_interactive > rest
 *   family_media  video_live > video_vod > video_conf > rest
 *   auto          (no priority override — derive from active mix)
 *
 * UCI schema (see docs/architecture-v3-flow-aware.md §7):
 *
 *   config profile 'gaming'
 *       list winner_priority 'game_rt'
 *       list winner_priority 'voip_call'
 *       option dscp_game_rt 'EF'
 *       option dscp_video_conf 'CS4'
 *
 *   config device
 *       option ip '192.168.1.10'
 *       option profile 'gaming'
 */
#ifndef MYCO_PROFILE_H
#define MYCO_PROFILE_H

#include "myco_service.h"
#include "myco_types.h"

#include <stdint.h>

#define MAX_PROFILES              8
#define MAX_WINNER_PRIORITY       SERVICE_COUNT
#define MAX_DEVICE_PROFILE_BINDS  MAX_DEVICE_OVERRIDES
#define PROFILE_NAME_MAX          32

typedef struct {
    char       name[PROFILE_NAME_MAX];
    service_t  winner_priority[MAX_WINNER_PRIORITY];
    int        num_winners;
    uint8_t    service_dscp[SERVICE_COUNT];    /* 0..63 per service_t */
} profile_t;

typedef struct {
    char ip[16];
    char profile_name[PROFILE_NAME_MAX];
    int  profile_idx;                          /* -1 = unresolved */
} device_profile_binding_t;

typedef struct {
    profile_t                profiles[MAX_PROFILES];
    int                      num_profiles;
    int                      default_idx;      /* index of "auto" or first */

    device_profile_binding_t bindings[MAX_DEVICE_PROFILE_BINDS];
    int                      num_bindings;
} profile_set_t;

/* Populate `ps` with the four built-in profiles. Always safe to call. */
void profile_load_defaults(profile_set_t *ps);

/* Apply UCI overrides on top of defaults. Call profile_load_defaults()
 * first. Silent on absence of UCI — used for dev hosts without uci. */
void profile_load_uci(profile_set_t *ps);

/* Convenience: run both steps. Returns 0 on success. */
int  profile_load(profile_set_t *ps);

/* Look up a profile by name. Returns NULL if not found. */
const profile_t *profile_find(const profile_set_t *ps, const char *name);

/* Look up binding for an IP (string form, e.g. "192.168.1.10").
 * Returns the profile_t it resolves to, or the default profile if no
 * binding exists. Never returns NULL. */
const profile_t *profile_for_ip(const profile_set_t *ps, const char *ip);

/* Resolve each binding's profile_idx from its profile_name. Call after
 * mutating profiles or bindings. */
void profile_resolve_bindings(profile_set_t *ps);

/* Parse a DSCP string like "EF", "CS4", "AF41", or a raw number "46"
 * into a 0..63 value. Returns -1 on parse error. */
int profile_parse_dscp(const char *s);

/* Parse a service_t name ("game_rt", "voip_call", …). Matches
 * service_name(). Returns SVC_UNKNOWN on no match. */
service_t profile_parse_service(const char *s);

#endif /* MYCO_PROFILE_H */
