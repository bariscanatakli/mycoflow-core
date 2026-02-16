/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_log.h — Structured logging
 */
#ifndef MYCO_LOG_H
#define MYCO_LOG_H

#include "myco_types.h"

void log_init(int level);
void log_set_level(int level);
void log_msg(int level, const char *source, const char *fmt, ...);

#endif /* MYCO_LOG_H */
