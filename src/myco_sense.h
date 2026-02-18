/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_sense.h — Metric collection & baseline calibration
 */
#ifndef MYCO_SENSE_H
#define MYCO_SENSE_H

#include "myco_types.h"

int  sense_init(const char *iface, int dummy_metrics);
int  sense_sample(const char *iface, const char *probe_host,
                  double interval_s, int dummy_metrics, metrics_t *out);
int  sense_get_idle_baseline(const char *iface, const char *probe_host,
                             int samples, double interval_s,
                             int dummy_metrics, metrics_t *baseline);
void sense_update_baseline_sliding(metrics_t *baseline,
                                   const metrics_t *current,
                                   double decay);

#endif /* MYCO_SENSE_H */
