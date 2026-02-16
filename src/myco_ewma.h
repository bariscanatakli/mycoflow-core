/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ewma.h — Exponentially Weighted Moving Average filter
 *
 * Thesis formula: ê_t = α·e_t + (1-α)·ê_{t-1}
 */
#ifndef MYCO_EWMA_H
#define MYCO_EWMA_H

typedef struct {
    double value;
    int    initialized;
} ewma_filter_t;

void   ewma_init(ewma_filter_t *f);
double ewma_update(ewma_filter_t *f, double sample, double alpha);

#endif /* MYCO_EWMA_H */
