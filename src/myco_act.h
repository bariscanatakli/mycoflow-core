/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_act.h — CAKE actuation & metric dumping
 */
#ifndef MYCO_ACT_H
#define MYCO_ACT_H

#include "myco_types.h"

int  act_apply_policy(const char *iface, const policy_t *policy, int no_tc, int force_fail);
void dump_metrics(const myco_config_t *cfg, const metrics_t *metrics,
                  persona_t persona, const char *reason);

#endif /* MYCO_ACT_H */
