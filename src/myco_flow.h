/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_flow.h — Userspace LRU flow table
 */
#ifndef MYCO_FLOW_H
#define MYCO_FLOW_H

#include <stdint.h>

#define FLOW_TABLE_SIZE 256  /* max concurrent flows tracked */

/* 5-tuple flow key */
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
} flow_key_t;

/* Per-flow statistics */
typedef struct {
    flow_key_t key;
    uint64_t   packets;
    uint64_t   bytes;
    double     last_seen;   /* monotonic seconds */
    int        active;      /* 1 = occupied slot */
} flow_entry_t;

typedef struct {
    flow_entry_t entries[FLOW_TABLE_SIZE];
    int          count;
} flow_table_t;

void flow_table_init(flow_table_t *ft);
int  flow_table_update(flow_table_t *ft, const flow_key_t *key,
                       uint64_t packets, uint64_t bytes, double now);
const flow_entry_t *flow_table_lookup(const flow_table_t *ft,
                                      const flow_key_t *key);
int  flow_table_populate_conntrack(flow_table_t *ft, double now);
int  flow_table_active_count(const flow_table_t *ft);
void flow_table_evict_stale(flow_table_t *ft, double now, double max_age_s);

#endif /* MYCO_FLOW_H */
