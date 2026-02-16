/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_config.h — Configuration loading (UCI + env + defaults)
 */
#ifndef MYCO_CONFIG_H
#define MYCO_CONFIG_H

#include "myco_types.h"

int config_load(myco_config_t *cfg);
int config_reload(myco_config_t *cfg);

#endif /* MYCO_CONFIG_H */
