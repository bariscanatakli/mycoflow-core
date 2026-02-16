/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ewma.c — Exponentially Weighted Moving Average filter
 *
 * Implements the EWMA smoothing filter from the thesis:
 *   ê_t = α·e_t + (1-α)·ê_{t-1}
 *
 * Used to smooth raw RTT and jitter measurements before they
 * reach the control loop, reducing noisy oscillations.
 */
#include "myco_ewma.h"

void ewma_init(ewma_filter_t *f) {
    if (!f) {
        return;
    }
    f->value = 0.0;
    f->initialized = 0;
}

double ewma_update(ewma_filter_t *f, double sample, double alpha) {
    if (!f) {
        return sample;
    }
    if (!f->initialized) {
        f->value = sample;
        f->initialized = 1;
    } else {
        f->value = alpha * sample + (1.0 - alpha) * f->value;
    }
    return f->value;
}
