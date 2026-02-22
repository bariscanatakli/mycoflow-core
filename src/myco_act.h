/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_act.h — CAKE actuation & metric dumping
 */
#ifndef MYCO_ACT_H
#define MYCO_ACT_H

#include "myco_types.h"

int  act_apply_policy(const char *iface, const policy_t *policy, int no_tc, int force_fail);
int  act_apply_persona_tin(const char *iface, persona_t persona,
                           int bandwidth_kbit, int no_tc, int force_fail);
/* Ingress shaping via IFB — one-time plumbing setup */
int  act_setup_ingress_ifb(const char *wan_iface, const char *ifb_iface,
                           int bandwidth_kbit, int no_tc, int force_fail);
/* Apply persona-driven CAKE target to the IFB device */
int  act_apply_ingress_policy(const char *ifb_iface, persona_t persona,
                              int bandwidth_kbit, int no_tc, int force_fail);
/* Remove IFB redirect and device on clean shutdown */
void act_teardown_ingress_ifb(const char *wan_iface, const char *ifb_iface, int no_tc);
void dump_metrics(const myco_config_t *cfg, const metrics_t *metrics,
                  persona_t persona, const char *reason);

#endif /* MYCO_ACT_H */
