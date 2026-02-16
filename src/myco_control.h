/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_control.h — Reflexive control loop (hysteresis + policy)
 */
#ifndef MYCO_CONTROL_H
#define MYCO_CONTROL_H

#include "myco_types.h"

int  is_outlier(const metrics_t *metrics, const metrics_t *baseline, const myco_config_t *cfg);
void control_init(control_state_t *state, int initial_bw);
int  control_decide(control_state_t *state, const myco_config_t *cfg,
                    const metrics_t *metrics, const metrics_t *baseline,
                    persona_t persona, policy_t *desired,
                    char *reason, size_t reason_len);
void control_on_action_result(control_state_t *state, int success);

#endif /* MYCO_CONTROL_H */
