/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_log.c — Structured logging
 */
#include "myco_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int g_log_level = LOG_INFO;

static const char *level_name(int level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default:        return "UNK";
    }
}

void log_init(int level) {
    g_log_level = level;
}

void log_set_level(int level) {
    g_log_level = level;
}

void log_msg(int level, const char *source, const char *fmt, ...) {
    if (level > g_log_level) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);

    fprintf(stdout, "%s.%03ld [%s] %s: ",
            time_buf, ts.tv_nsec / 1000000L, level_name(level), source);

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "\n");
    fflush(stdout);
}
