/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_sense.c — Metric collection & baseline calibration
 */
#include "myco_sense.h"
#include "myco_log.h"
#include "myco_netlink.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned long long g_prev_rx = 0;
static unsigned long long g_prev_tx = 0;
static double g_prev_rtt = 10.0;
static unsigned long long g_prev_cpu_total = 0;
static unsigned long long g_prev_cpu_idle = 0;
static int g_seeded = 0;

static int read_netdev(const char *iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return -1;
    }

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (line_no <= 2) {
            continue;
        }
        char name[64];
        unsigned long long rx = 0;
        unsigned long long tx = 0;
        if (sscanf(line, " %63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   name, &rx, &tx) == 3) {
            if (strcmp(name, iface) == 0) {
                *rx_bytes = rx;
                *tx_bytes = tx;
                fclose(fp);
                return 0;
            }
        }
    }
    fclose(fp);
    return -1;
}

static double read_cpu_pct(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0.0;
    }
    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);

    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return 0.0;
    }

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + system + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;

    if (g_prev_cpu_total == 0) {
        g_prev_cpu_total = total;
        g_prev_cpu_idle = idle_all;
        return 0.0;
    }

    unsigned long long totald = total - g_prev_cpu_total;
    unsigned long long idled = idle_all - g_prev_cpu_idle;

    g_prev_cpu_total = total;
    g_prev_cpu_idle = idle_all;

    if (totald == 0) {
        return 0.0;
    }

    double cpu_pct = (double)(totald - idled) * 100.0 / (double)totald;
    if (cpu_pct < 0.0) {
        cpu_pct = 0.0;
    }
    return cpu_pct;
}

static double dummy_rtt(void) {
    if (!g_seeded) {
        srand((unsigned int)time(NULL));
        g_seeded = 1;
    }
    double base = 10.0 + (rand() % 10);
    double spike = (rand() % 100 < 5) ? (double)(rand() % 60) : 0.0;
    return base + spike;
}

static double probe_rtt_ms(const char *iface, const char *host) {
    if (!iface || !host) {
        return -1.0;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 -I %s %s 2>/dev/null", iface, host);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1.0;
    }
    char line[256];
    double rtt = -1.0;
    while (fgets(line, sizeof(line), fp)) {
        char *pos = strstr(line, "time=");
        if (pos) {
            double value = 0.0;
            if (sscanf(pos, "time=%lf", &value) == 1) {
                rtt = value;
                break;
            }
        }
    }
    pclose(fp);
    return rtt;
}

/* ── Public API ─────────────────────────────────────────────── */

int sense_init(const char *iface, int dummy_metrics) {
    (void)iface;
    (void)dummy_metrics;
    g_prev_rx = 0;
    g_prev_tx = 0;
    g_prev_rtt = 10.0;
    g_prev_cpu_total = 0;
    g_prev_cpu_idle = 0;
    netlink_init();
    return 0;
}

int sense_sample(const char *iface, const char *probe_host, double interval_s, int dummy_metrics, metrics_t *out) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    unsigned long long rx = 0;
    unsigned long long tx = 0;
    if (read_netdev(iface, &rx, &tx) != 0) {
        log_msg(LOG_WARN, "sense", "netdev read failed for %s: %s", iface, strerror(errno));
    } else {
        if (g_prev_rx != 0 || g_prev_tx != 0) {
            out->rx_bps = ((double)(rx - g_prev_rx) * 8.0) / interval_s;
            out->tx_bps = ((double)(tx - g_prev_tx) * 8.0) / interval_s;
        }
        g_prev_rx = rx;
        g_prev_tx = tx;
    }

    if (dummy_metrics) {
        out->rtt_ms = dummy_rtt();
    } else {
        double rtt = probe_rtt_ms(iface, probe_host ? probe_host : "1.1.1.1");
        if (rtt < 0.0) {
            log_msg(LOG_WARN, "sense", "icmp probe failed, using fallback");
            out->rtt_ms = dummy_rtt();
        } else {
            out->rtt_ms = rtt;
        }
    }

    out->jitter_ms = fabs(out->rtt_ms - g_prev_rtt);
    g_prev_rtt = out->rtt_ms;

    out->cpu_pct = read_cpu_pct();

    /* Qdisc stats via netlink */
    netlink_get_qdisc_stats(iface,
                            &out->qdisc_backlog,
                            &out->qdisc_drops,
                            &out->qdisc_overlimits);

    return 0;
}

int sense_get_idle_baseline(const char *iface,
                            const char *probe_host,
                            int samples,
                            double interval_s,
                            int dummy_metrics,
                            metrics_t *baseline) {
    if (!baseline || samples <= 0) {
        return -1;
    }

    memset(baseline, 0, sizeof(*baseline));
    metrics_t m = {0};
    for (int i = 0; i < samples; i++) {
        if (sense_sample(iface, probe_host, interval_s, dummy_metrics, &m) == 0) {
            baseline->rtt_ms += m.rtt_ms;
            baseline->jitter_ms += m.jitter_ms;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)interval_s;
        ts.tv_nsec = (long)((interval_s - ts.tv_sec) * 1000000000.0);
        nanosleep(&ts, NULL);
    }
    baseline->rtt_ms /= (double)samples;
    baseline->jitter_ms /= (double)samples;
    return 0;
}
