/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_device.c — Per-device persona tracking & DSCP classification
 *
 * Aggregates flow table entries by source IP to build per-device metrics,
 * then infers each device's persona independently. When a device's persona
 * changes, iptables mangle rules are updated so CAKE diffserv4 places
 * that device's packets in the appropriate latency/throughput tin.
 */
#include "myco_device.h"
#include "myco_log.h"
#include "myco_persona.h"
#include "myco_hint.h"
#include "myco_dns.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_ACTIVE_FLOW_MIN_DELTA_BYTES 256

/* ── Helpers ───────────────────────────────────────────────── */

static device_entry_t *find_device(device_table_t *dt, uint32_t ip) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt->devices[i].active && dt->devices[i].ip == ip) {
            return &dt->devices[i];
        }
    }
    return NULL;
}

static device_entry_t *alloc_device(device_table_t *dt, uint32_t ip, double now) {
    /* Find empty slot */
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!dt->devices[i].active) {
            memset(&dt->devices[i], 0, sizeof(device_entry_t));
            dt->devices[i].ip = ip;
            dt->devices[i].active = 1;
            dt->devices[i].last_seen = now;
            dt->devices[i].applied_dscp = PERSONA_UNKNOWN;
            persona_init(&dt->devices[i].persona_state);
            dt->count++;
            return &dt->devices[i];
        }
    }

    /* Table full — evict oldest device */
    int oldest = 0;
    double oldest_time = dt->devices[0].last_seen;
    for (int i = 1; i < MAX_DEVICES; i++) {
        if (dt->devices[i].active && dt->devices[i].last_seen < oldest_time) {
            oldest_time = dt->devices[i].last_seen;
            oldest = i;
        }
    }

    char old_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dt->devices[oldest].ip, old_ip_str, sizeof(old_ip_str));
    log_msg(LOG_DEBUG, "device", "evicting %s (LRU) for new device", old_ip_str);

    memset(&dt->devices[oldest], 0, sizeof(device_entry_t));
    dt->devices[oldest].ip = ip;
    dt->devices[oldest].active = 1;
    dt->devices[oldest].last_seen = now;
    dt->devices[oldest].applied_dscp = PERSONA_UNKNOWN;
    persona_init(&dt->devices[oldest].persona_state);
    return &dt->devices[oldest];
}

/* ── Public API ────────────────────────────────────────────── */

void device_table_init(device_table_t *dt) {
    if (!dt) {
        return;
    }
    memset(dt, 0, sizeof(*dt));
}

void device_table_aggregate(device_table_t *dt, const flow_table_t *ft,
                            double now, dns_cache_t *dns_cache) {
    if (!dt || !ft) {
        return;
    }

    /* Reset per-device counters (but keep persona state) */
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt->devices[i].active) {
            dt->devices[i].flow_count    = 0;
            dt->devices[i].total_bytes   = 0;
            dt->devices[i].total_packets = 0;
            dt->devices[i].tx_bytes      = 0;
            dt->devices[i].rx_bytes      = 0;
            dt->devices[i].udp_flows     = 0;
            dt->devices[i].tcp_flows     = 0;
            dt->devices[i].udp_bytes     = 0;
            dt->devices[i].udp_packets   = 0;
            dt->devices[i].udp_avg_pkt   = 0.0;
            dt->devices[i].avg_pkt_size  = 0.0;
            dt->devices[i].bandwidth_bps = 0.0;
            dt->devices[i].tx_rx_ratio   = 1.0;
            dt->devices[i].elephant_flow = 0;
            memset(dt->devices[i].hint_votes, 0, sizeof(dt->devices[i].hint_votes));
            dt->devices[i].dominant_hint = PERSONA_UNKNOWN;
            dt->devices[i].has_hint = 0;
        }
    }

    /* First pass: accumulate flows per device (by src_ip) */
    for (int i = 0; i < FLOW_TABLE_SIZE; i++) {
        if (!ft->entries[i].active) {
            continue;
        }
        const flow_entry_t *fe = &ft->entries[i];
        uint64_t flow_delta = fe->tx_delta + fe->rx_delta;
        if (flow_delta < DEVICE_ACTIVE_FLOW_MIN_DELTA_BYTES) {
            continue;
        }
        uint32_t src_ip = fe->key.src_ip;

        device_entry_t *dev = find_device(dt, src_ip);
        if (!dev) {
            dev = alloc_device(dt, src_ip, now);
        }

        dev->flow_count++;
        dev->total_bytes   += fe->bytes + fe->rx_bytes;
        dev->total_packets += fe->packets + fe->rx_packets;
        dev->tx_bytes      += fe->tx_delta;
        dev->rx_bytes      += fe->rx_delta;
        dev->last_seen      = now;

        if (fe->key.protocol == 17) {
            dev->udp_flows++;
            /* Use delta bytes for UDP avg_pkt calculation.
             * Cumulative bytes/packets are polluted by handshake/ack packets
             * from early in the flow's lifetime. Delta bytes reflect the
             * current traffic pattern (video segments vs game state). */
            dev->udp_bytes += fe->tx_delta + fe->rx_delta;
        } else if (fe->key.protocol == 6) {
            dev->tcp_flows++;
        }
    }

    /* Second pass: compute derived metrics per device */
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_entry_t *dev = &dt->devices[i];
        if (!dev->active || dev->flow_count == 0) {
            continue;
        }

        /* Average packet size */
        if (dev->total_packets > 0) {
            dev->avg_pkt_size = (double)dev->total_bytes / (double)dev->total_packets;
        }

        /* UDP bytes-per-flow: delta bytes / active UDP flows.
         * High value (~10KB+) = large QUIC video segments → STREAMING
         * Low value (~1KB) = small game state updates → GAMING */
        if (dev->udp_flows > 0) {
            dev->udp_avg_pkt = (double)dev->udp_bytes / (double)dev->udp_flows;
        }

        /* Bandwidth estimate: (TX + RX bytes) / 0.5s interval * 8 bits */
        dev->bandwidth_bps = (double)(dev->tx_bytes + dev->rx_bytes) * 8.0 / 0.5;

        /* TX/RX ratio: >4 = heavy uploader (BULK), <0.25 = heavy downloader (STREAMING) */
        dev->tx_rx_ratio = (double)dev->tx_bytes / (double)(dev->rx_bytes + 1);

        /* Per-device elephant flow detection + port hint collection.
         * Both piggyback on the same per-flow scan — zero extra iterations.
         * Delta-based avoids stale cumulative bytes diluting the ratio when
         * many old flows exist but only one is actively transferring. */
        uint64_t max_delta = 0;
        uint64_t total_delta = dev->tx_bytes + dev->rx_bytes; /* already delta-based */
        for (int j = 0; j < FLOW_TABLE_SIZE; j++) {
            if (!ft->entries[j].active) {
                continue;
            }
            if (ft->entries[j].key.src_ip != dev->ip) {
                continue;
            }
            uint64_t flow_delta = ft->entries[j].tx_delta + ft->entries[j].rx_delta;
            if (flow_delta < DEVICE_ACTIVE_FLOW_MIN_DELTA_BYTES) {
                continue;
            }
            if (flow_delta > max_delta) {
                max_delta = flow_delta;
            }

            /* Port hint: look up dst_port for this flow */
            persona_t hint = hint_from_port(ft->entries[j].key.protocol,
                                            ft->entries[j].key.dst_port);

            /* DNS hint: if port hint is UNKNOWN (e.g., port 443), try
             * the DNS cache for a domain-based hint. DNS resolves the
             * 443 ambiguity that port hints cannot. */
            if (hint == PERSONA_UNKNOWN && dns_cache) {
                hint = dns_cache_lookup(dns_cache, ft->entries[j].key.dst_ip);
            }

            if (hint != PERSONA_UNKNOWN) {
                dev->hint_votes[(int)hint]++;
            }
        }
        if (total_delta > 0 &&
            (double)max_delta / (double)total_delta >= 0.60) {
            dev->elephant_flow = 1;
        }

        /* Resolve dominant hint: highest vote count, UNKNOWN excluded.
         * Tie-break by priority (lower enum = higher priority). */
        int best_votes = 0;
        persona_t best_hint = PERSONA_UNKNOWN;
        for (int p = 1; p < PERSONA_COUNT; p++) {
            if (dev->hint_votes[p] > best_votes) {
                best_votes = dev->hint_votes[p];
                best_hint = (persona_t)p;
            } else if (dev->hint_votes[p] == best_votes && best_votes > 0) {
                /* Tie: prefer higher priority (lower enum value) */
                if (p < (int)best_hint) {
                    best_hint = (persona_t)p;
                }
            }
        }
        dev->dominant_hint = best_hint;
        dev->has_hint = (best_hint != PERSONA_UNKNOWN) ? 1 : 0;
    }
}

int device_table_update_personas(device_table_t *dt, const myco_config_t *cfg) {
    if (!dt) {
        return 0;
    }

    int changes = 0;

    for (int i = 0; i < MAX_DEVICES; i++) {
        device_entry_t *dev = &dt->devices[i];
        if (!dev->active || dev->flow_count == 0) {
            continue;
        }

        /* Build a per-device metrics_t from aggregated data.
         * Per-device fields: avg_pkt_size, active_flows, elephant_flow,
         *   tx_bps, rx_bps (derived from tx/rx bytes at 2Hz = 0.5s).
         * RTT/jitter/ebpf_pkt_rate are system-wide — omitted here. */
        metrics_t dev_metrics;
        memset(&dev_metrics, 0, sizeof(dev_metrics));
        dev_metrics.avg_pkt_size = dev->avg_pkt_size;
        dev_metrics.active_flows = dev->flow_count;
        dev_metrics.elephant_flow = dev->elephant_flow;
        dev_metrics.udp_flows = dev->udp_flows;
        dev_metrics.udp_avg_pkt = dev->udp_avg_pkt;
        dev_metrics.tx_bps = (double)dev->tx_bytes * 8.0 / 0.5;
        dev_metrics.rx_bps = (double)dev->rx_bytes * 8.0 / 0.5;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &dev->ip, ip_str, sizeof(ip_str));

        persona_t prev = dev->persona;
        dev->override_active = 0;

        if (cfg && cfg->num_device_overrides > 0) {
            for (int j = 0; j < cfg->num_device_overrides; j++) {
                if (strcmp(cfg->device_overrides[j].ip, ip_str) == 0 &&
                    cfg->device_overrides[j].persona != PERSONA_UNKNOWN) {
                    dev->override_active = 1;
                    dev->persona = cfg->device_overrides[j].persona;
                    break;
                }
            }
        }

        if (!dev->override_active) {
            dev->persona = persona_update(&dev->persona_state, &dev_metrics,
                                          dev->dominant_hint);
        }

        if (dev->persona != prev) {
            log_msg(LOG_INFO, "device", "%s persona: %s -> %s%s",
                    ip_str, persona_name(prev), persona_name(dev->persona),
                    dev->override_active ? " (override)" : "");
            changes++;
        }
    }

    return changes;
}

persona_t device_table_dominant_persona(const device_table_t *dt) {
    if (!dt) {
        return PERSONA_UNKNOWN;
    }

    /* Lower enum = higher latency priority (VOIP=1, GAMING=2, ...).
     * Find the lowest non-zero persona among active devices with traffic. */
    persona_t best = PERSONA_UNKNOWN;
    for (int i = 0; i < MAX_DEVICES; i++) {
        const device_entry_t *dev = &dt->devices[i];
        if (!dev->active || dev->flow_count == 0) {
            continue;
        }
        persona_t p = dev->persona;
        if (p == PERSONA_UNKNOWN) {
            continue;
        }
        if (best == PERSONA_UNKNOWN || p < best) {
            best = p;
        }
    }
    return best;
}

void device_table_evict_stale(device_table_t *dt, double now, double max_age_s) {
    if (!dt) {
        return;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (dt->devices[i].active &&
            (now - dt->devices[i].last_seen) > max_age_s) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dt->devices[i].ip, ip_str, sizeof(ip_str));
            log_msg(LOG_DEBUG, "device", "evicting stale device %s", ip_str);
            dt->devices[i].active = 0;
            dt->count--;
        }
    }
}

/* ── DSCP application ──────────────────────────────────────── */

/* Map persona to DSCP class string for iptables --set-dscp-class.
 * CAKE diffserv4 tin mapping (precedence-based):
 *   EF  (46) → Voice tin  — VoIP strict priority
 *   CS4 (32) → Voice tin  — gaming low-latency
 *   CS3 (24) → Video tin  — video call
 *   CS2 (16) → Video tin  — streaming
 *   CS1 (8)  → Bulk tin   — bulk upload / torrent
 *   CS0 (0)  → Best Effort — default (no rule needed) */
static const char *persona_dscp_class(persona_t p) {
    switch (p) {
        case PERSONA_VOIP:      return "ef";
        case PERSONA_GAMING:    return "cs4";
        case PERSONA_VIDEO:     return "cs3";
        case PERSONA_STREAMING: return "cs2";
        case PERSONA_BULK:      return "cs1";
        case PERSONA_TORRENT:   return "cs1";
        default:                return NULL;    /* CS0 = no rule needed */
    }
}

void device_apply_all_dscp(const device_table_t *dt, int no_tc) {
    if (!dt) {
        return;
    }

    if (no_tc) {
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!dt->devices[i].active) {
                continue;
            }
            const char *cls = persona_dscp_class(dt->devices[i].persona);
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dt->devices[i].ip, ip_str, sizeof(ip_str));
            log_msg(LOG_INFO, "device", "tc disabled, would mark %s -> DSCP %s",
                    ip_str, cls ? cls : "cs0");
        }
        return;
    }

    /* Flush and rebuild the chain — simpler and safer than per-rule diff */
    (void)system("iptables -t mangle -F mycoflow_dscp 2>/dev/null");

    for (int i = 0; i < MAX_DEVICES; i++) {
        const device_entry_t *dev = &dt->devices[i];
        if (!dev->active) {
            continue;
        }

        const char *cls = persona_dscp_class(dev->persona);
        if (!cls) {
            continue;  /* UNKNOWN → CS0, no explicit rule needed */
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &dev->ip, ip_str, sizeof(ip_str));

        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "iptables -t mangle -A mycoflow_dscp -s %s -j DSCP --set-dscp-class %s",
                 ip_str, cls);
        int rc = system(cmd);
        if (rc != 0) {
            log_msg(LOG_WARN, "device", "DSCP rule failed for %s (rc=%d)", ip_str, rc);
        } else {
            log_msg(LOG_INFO, "device", "DSCP mark: %s -> %s (%s)",
                    ip_str, cls, persona_name(dev->persona));
        }
    }
}
