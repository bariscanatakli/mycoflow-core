/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * mycoflow.bpf.c — Minimal BPF program to count packets/bytes
 */
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <bpf/bpf_helpers.h>

/* Map to store global stats (packets, bytes) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1); /* Index 0: global stats */
    __type(key, __u32);
    __type(value, struct { __u64 packets; __u64 bytes; });
} myco_stats SEC(".maps");

SEC("tc")
int tc_ingress(struct __sk_buff *skb) {
    __u32 key = 0;
    struct { __u64 packets; __u64 bytes; } *val;

    val = bpf_map_lookup_elem(&myco_stats, &key);
    if (val) {
        __sync_fetch_and_add(&val->packets, 1);
        __sync_fetch_and_add(&val->bytes, skb->len);
    }
    return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
