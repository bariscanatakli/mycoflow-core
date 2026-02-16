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

#endif /* MYCO_UBUS_H */
