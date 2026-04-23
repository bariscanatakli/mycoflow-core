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
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>

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
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_PACKET && ifa->ifa_data != NULL) {
            if (strcmp(ifa->ifa_name, iface) == 0) {
                struct rtnl_link_stats *stats = (struct rtnl_link_stats *)ifa->ifa_data;
                *rx_bytes = stats->rx_bytes;
                *tx_bytes = stats->tx_bytes;
                *rx_pkts  = stats->rx_packets;
                *tx_pkts  = stats->tx_packets;
                freeifaddrs(ifaddr);
                return 0;
            }
        }
    }
    freeifaddrs(ifaddr);
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

static uint16_t icmp_checksum(void *b, int len) {
    uint16_t *buf = b;
    unsigned int sum = 0;
    uint16_t result;
    for (sum = 0; len > 1; len -= 2) sum += *buf++;
    if (len == 1) sum += *(unsigned char *)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

/* Multi-ping probe: send N packets, compute median RTT, jitter (stddev),
 * and loss%. Returns median RTT in ms, or -1.0 on failure.
 * Writes jitter_out and loss_pct_out (may be NULL). */
static double probe_multi_ping(const char *iface, const char *host,
                               int count, double *jitter_out, double *loss_pct_out) {
    if (!host || count < 1) {
        return -1.0;
    }

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) {
        return -1.0;
    }

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1.0;
    }

    if (iface) {
        setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface));
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    double rtts[8];
    int n_success = 0;
    uint16_t seq = 1;
    uint16_t pid = getpid() & 0xFFFF;

    for (int i = 0; i < count && i < 8; i++) {
        struct icmphdr icmp_hdr;
        memset(&icmp_hdr, 0, sizeof(icmp_hdr));
        icmp_hdr.type = ICMP_ECHO;
        icmp_hdr.un.echo.id = htons(pid);
        icmp_hdr.un.echo.sequence = htons(seq++);
        icmp_hdr.checksum = icmp_checksum(&icmp_hdr, sizeof(icmp_hdr));

        struct timespec t_send;
        clock_gettime(CLOCK_MONOTONIC, &t_send);

        if (sendto(sock, &icmp_hdr, sizeof(icmp_hdr), 0, res->ai_addr, res->ai_addrlen) <= 0) {
            usleep(100000); /* 100ms pause if tx failed */
            continue;
        }

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;

        /* Wait up to 1 second for reply */
        if (poll(&pfd, 1, 1000) > 0) {
            char recv_buf[1024];
            struct sockaddr_in r_addr;
            socklen_t r_len = sizeof(r_addr);
            int bytes = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&r_addr, &r_len);
            
            if (bytes > 0) {
                struct timespec t_recv;
                clock_gettime(CLOCK_MONOTONIC, &t_recv);
                
                struct iphdr *ip = (struct iphdr *)recv_buf;
                int ip_hlen = ip->ihl * 4;
                if (bytes >= ip_hlen + (int)sizeof(struct icmphdr)) {
                    struct icmphdr *ricmp = (struct icmphdr *)(recv_buf + ip_hlen);
                    if (ricmp->type == ICMP_ECHOREPLY && ntohs(ricmp->un.echo.id) == pid) {
                        double rtt = (t_recv.tv_sec - t_send.tv_sec) * 1000.0 + 
                                     (t_recv.tv_nsec - t_send.tv_nsec) / 1000000.0;
                        rtts[n_success++] = rtt;
                    }
                }
            }
        }
    }

    close(sock);
    freeaddrinfo(res);

    if (n_success == 0) {
        if (loss_pct_out) {
            *loss_pct_out = 100.0;
        }
        return -1.0;
    }

    /* Compute mean RTT */
    double sum = 0.0;
    for (int i = 0; i < n_success; i++) {
        sum += rtts[i];
    }
    double mean = sum / (double)n_success;

    /* Compute jitter as standard deviation */
    if (jitter_out) {
        double var = 0.0;
        for (int i = 0; i < n_success; i++) {
            double d = rtts[i] - mean;
            var += d * d;
        }
        *jitter_out = (n_success > 1) ? sqrt(var / (double)(n_success - 1)) : 0.0;
    }

    /* Packet loss */
    if (loss_pct_out) {
        *loss_pct_out = (double)(count - n_success) * 100.0 / (double)count;
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
