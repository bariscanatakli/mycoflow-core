/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_device.h — Per-device persona tracking & DSCP classification
 *
 * Each LAN device is a "mycelial feed point": the system learns its traffic
 * pattern and marks its packets with the appropriate DSCP class so CAKE
 * diffserv4 routes them into the correct latency/throughput tin.
 */
#ifndef MYCO_DEVICE_H
#define MYCO_DEVICE_H

#include "myco_types.h"
#include "myco_flow.h"

#include <stdint.h>

#define MAX_DEVICES 32  /* max LAN devices tracked simultaneously */

/* Per-device state entry */
typedef struct {
    uint32_t        ip;               /* LAN device IP (network byte order) */
    int             active;           /* 1 = occupied slot */
    double          last_seen;        /* monotonic timestamp of last activity */

    /* Aggregated metrics (recomputed each cycle from flow table) */
    int             flow_count;       /* active flows for this device */
    uint64_t        total_bytes;      /* total bytes across all device flows */
    uint64_t        total_packets;    /* total packets across all device flows */
    double          avg_pkt_size;     /* total_bytes / total_packets */
    int             elephant_flow;    /* 1 if one flow carries >60% of device bytes */

    /* Per-device persona inference */
    persona_state_t persona_state;    /* reuses existing history[5] + majority vote */
    persona_t       persona;          /* current inferred persona */
    persona_t       applied_dscp;     /* last DSCP persona applied via iptables */
} device_entry_t;

typedef struct {
    device_entry_t  devices[MAX_DEVICES];
    int             count;            /* number of active device slots */
} device_table_t;

/* Initialize device table (zero all slots) */
void device_table_init(device_table_t *dt);

/* Aggregate flow table entries by src_ip into per-device metrics.
 * Resets per-device counters before aggregation. */
void device_table_aggregate(device_table_t *dt, const flow_table_t *ft, double now);

/* Run persona inference for each active device.
 * Returns the number of devices whose persona changed this cycle. */
int device_table_update_personas(device_table_t *dt);

/* Evict devices not seen for max_age_s seconds */
void device_table_evict_stale(device_table_t *dt, double now, double max_age_s);

/* Apply iptables DSCP rules for all devices whose persona differs from
 * applied_dscp. Uses the mycoflow_dscp mangle chain. */
void device_apply_all_dscp(const device_table_t *dt, int no_tc);

#endif /* MYCO_DEVICE_H */
