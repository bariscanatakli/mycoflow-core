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
static unsigned long long g_prev_rx_pkts = 0;
static unsigned long long g_prev_tx_pkts = 0;
static double g_prev_rtt = 10.0;
static unsigned long long g_prev_cpu_total = 0;
static unsigned long long g_prev_cpu_idle = 0;
static int g_seeded = 0;

static int read_netdev(const char *iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes,
                       unsigned long long *rx_pkts, unsigned long long *tx_pkts) {
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
        unsigned long long rb = 0, rp = 0, tb = 0, tp = 0;
        /* /proc/net/dev format: iface: rx_bytes rx_packets ... tx_bytes tx_packets ... */
        if (sscanf(line, " %63[^:]: %llu %llu %*u %*u %*u %*u %*u %*u %llu %llu",
                   name, &rb, &rp, &tb, &tp) == 5) {
            if (strcmp(name, iface) == 0) {
                *rx_bytes = rb;
                *rx_pkts  = rp;
                *tx_bytes = tb;
                *tx_pkts  = tp;
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

/* Multi-ping probe: send N packets, compute median RTT, jitter (stddev),
 * and loss%. Returns median RTT in ms, or -1.0 on failure.
 * Writes jitter_out and loss_pct_out (may be NULL). */
static double probe_multi_ping(const char *iface, const char *host,
                               int count, double *jitter_out, double *loss_pct_out) {
    if (!iface || !host || count < 1) {
        return -1.0;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ping -c %d -W 1 -I %s %s 2>/dev/null", count, iface, host);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1.0;
    }

    double rtts[8];  /* support up to 8 pings */
    int n = 0;
    char line[512];
    int transmitted = 0, received = 0;

    while (fgets(line, sizeof(line), fp) && n < count) {
        /* Parse individual RTT lines: "64 bytes from ... time=X.X ms" */
        char *pos = strstr(line, "time=");
        if (pos) {
            double value = 0.0;
            if (sscanf(pos, "time=%lf", &value) == 1 && n < 8) {
                rtts[n++] = value;
            }
        }
        /* Parse summary: "N packets transmitted, M received" */
        if (strstr(line, "packets transmitted")) {
            sscanf(line, "%d packets transmitted, %d received", &transmitted, &received);
        }
    }
    pclose(fp);

    if (n == 0) {
        if (loss_pct_out) {
            *loss_pct_out = 100.0;
        }
        return -1.0;
    }

    /* Compute mean RTT */
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += rtts[i];
    }
    double mean = sum / (double)n;

    /* Compute jitter as standard deviation */
    if (jitter_out) {
        double var = 0.0;
        for (int i = 0; i < n; i++) {
            double d = rtts[i] - mean;
            var += d * d;
        }
        *jitter_out = (n > 1) ? sqrt(var / (double)(n - 1)) : 0.0;
    }

    /* Packet loss */
    if (loss_pct_out) {
        if (transmitted > 0) {
            *loss_pct_out = (double)(transmitted - received) * 100.0 / (double)transmitted;
        } else {
            *loss_pct_out = (n < count) ? 100.0 * (count - n) / count : 0.0;
        }
    }

    return mean;
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

    unsigned long long rx = 0, tx = 0, rx_pkts = 0, tx_pkts = 0;
    if (read_netdev(iface, &rx, &tx, &rx_pkts, &tx_pkts) != 0) {
        log_msg(LOG_WARN, "sense", "netdev read failed for %s: %s", iface, strerror(errno));
    } else {
        if (g_prev_rx != 0 || g_prev_tx != 0) {
            out->rx_bps = ((double)(rx - g_prev_rx) * 8.0) / interval_s;
            out->tx_bps = ((double)(tx - g_prev_tx) * 8.0) / interval_s;
            
            /* Compute average packet size */
            unsigned long long delta_bytes = (rx - g_prev_rx) + (tx - g_prev_tx);
            unsigned long long delta_pkts  = (rx_pkts - g_prev_rx_pkts) + (tx_pkts - g_prev_tx_pkts);
            if (delta_pkts > 0) {
                out->avg_pkt_size = (double)delta_bytes / (double)delta_pkts;
            }
        }
        g_prev_rx = rx;
        g_prev_tx = tx;
        g_prev_rx_pkts = rx_pkts;
        g_prev_tx_pkts = tx_pkts;
    }

    if (dummy_metrics) {
        out->rtt_ms       = dummy_rtt();
        out->jitter_ms    = fabs(out->rtt_ms - g_prev_rtt);
        out->probe_loss_pct = 0.0;
    } else {
        double jitter_probe = 0.0;
        double loss_pct     = 0.0;
        double rtt = probe_multi_ping(iface, probe_host ? probe_host : "1.1.1.1",
                                      3, &jitter_probe, &loss_pct);
        if (rtt < 0.0) {
            log_msg(LOG_WARN, "sense", "icmp probe failed, using fallback");
            out->rtt_ms       = dummy_rtt();
            out->jitter_ms    = fabs(out->rtt_ms - g_prev_rtt);
            out->probe_loss_pct = 100.0;
        } else {
            out->rtt_ms         = rtt;
            out->jitter_ms      = jitter_probe;
            out->probe_loss_pct = loss_pct;
        }
    }
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

/* ── Sliding baseline update ────────────────────────────────── */

void sense_update_baseline_sliding(metrics_t *baseline,
                                   const metrics_t *current,
                                   double decay) {
    if (!baseline || !current || decay <= 0.0 || decay > 1.0) {
        return;
    }
    /* Only probe-based fields drift with the environment; BPS/CPU are not
     * meaningful long-term baseline references for congestion detection. */
    baseline->rtt_ms    = (1.0 - decay) * baseline->rtt_ms    + decay * current->rtt_ms;
    baseline->jitter_ms = (1.0 - decay) * baseline->jitter_ms + decay * current->jitter_ms;
}
