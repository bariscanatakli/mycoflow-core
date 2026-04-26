/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ubus.h — ubus API surface (OpenWrt IPC)
 */
#ifndef MYCO_UBUS_H
#define MYCO_UBUS_H

#include "myco_types.h"

#ifdef HAVE_UBUS
void ubus_start(myco_config_t *cfg, control_state_t *control);
void ubus_stop(void);
#else
static inline void ubus_start(myco_config_t *cfg, control_state_t *control) { (void)cfg; (void)control; }
static inline void ubus_stop(void) {}
#endif

// Fallback mechanism when ubus is not available
void myco_dump_json(void);

/* Control-file fallback: reads /tmp/myco_control.json (written by LuCI),
 * applies any pending overrides/policy mutations, then deletes the file
 * so each command is consumed once. Called from the main loop each cycle.
 *
 * Recognized JSON keys:
 *   "persona_override":  string (voip|gaming|video|streaming|bulk|torrent|clear)
 *   "policy_set_kbit":   integer (absolute target — clamped to UCI min/max)
 *   "policy_boost_kbit": integer (delta added to current bandwidth)
 *   "policy_throttle_kbit": integer (delta subtracted)
 */
void myco_apply_control_file(void);

// Per-device table registration for JSON dump
struct device_table_t;  /* forward declare to avoid circular include */
void myco_set_device_table(const void *dt, int enabled);

// Register control_state and config so myco_apply_control_file() can
// mutate them when LuCI requests a manual policy change. Pass NULL to
// disable manual control.
void myco_set_control_handles(void *control_state, const void *cfg);

/* Flow-aware service table registration for JSON dump. Opaque void*
 * keeps myco_classifier.h out of the ubus translation unit. Passing
 * NULL (or enabled=0) clears the pointer — the JSON output then omits
 * the "flows" array entirely. */
void myco_set_flow_table(const void *fst, int enabled);

#endif /* MYCO_UBUS_H */
