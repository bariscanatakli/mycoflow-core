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

// Per-device table registration for JSON dump
struct device_table_t;  /* forward declare to avoid circular include */
void myco_set_device_table(const void *dt, int enabled);

/* Flow-aware service table registration for JSON dump. Opaque void*
 * keeps myco_classifier.h out of the ubus translation unit. Passing
 * NULL (or enabled=0) clears the pointer — the JSON output then omits
 * the "flows" array entirely. */
void myco_set_flow_table(const void *fst, int enabled);

#endif /* MYCO_UBUS_H */
