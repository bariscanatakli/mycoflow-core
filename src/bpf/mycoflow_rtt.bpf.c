/*
 * MycoFlow — Passive TCP RTT measurement
 * mycoflow_rtt.bpf.c — measures forwarding-path RTT without active probing
 *
 * Approach
 * --------
 * Two TC hooks on the WAN interface (egress = client→server, ingress
 * = server→client). For each TCP flow we:
 *
 *   1. On egress: stamp tx_ts_ns and tx_seq_end = seq + payload_len
 *      + (SYN|FIN ? 1 : 0)  — the ACK we expect to see for this pkt.
 *   2. On ingress ACK: if ack_seq >= tx_seq_end, RTT = now - tx_ts_ns.
 *      Clear tx_ts_ns so the next outgoing pkt sets a fresh sample.
 *   3. EWMA-smooth (RFC 6298: srtt = (7*srtt + rtt) / 8).
 *
 * The map is keyed by 5-tuple canonicalized so the LAN side is "client"
 * and the WAN side is "server" regardless of direction. Userspace reads
 * srtt_ms per flow through bpf_map_lookup_elem.
 *
 * Limitations (accepted for v1)
 *   - TCP only. UDP has no seq/ack so no passive RTT is derivable.
 *   - One outstanding sample per flow: high-bandwidth flows overwrite
 *     their own tx_ts_ns before the ACK lands. srtt stays representative
 *     because the next round still gets a fresh sample; we just lose
 *     some sample density. Acceptable for QoS decisions at ~1Hz.
 *   - IPv4 only. IPv6 support is a straightforward addition but kept
 *     out of v1 to keep the verifier happy.
 */
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define FLOW_RTT_MAX 4096

struct myco_rtt_key {
    __u32 client_ip;    /* LAN side (NBO) */
    __u32 server_ip;    /* WAN side (NBO) */
    __u16 client_port;  /* NBO */
    __u16 server_port;  /* NBO */
    __u8  protocol;     /* always 6 for this map */
    __u8  pad[3];
};

struct myco_rtt_value {
    __u64 tx_ts_ns;     /* 0 when current outgoing sample has been consumed */
    __u32 tx_seq_end;   /* ack we expect for the outgoing pkt */
    __u32 srtt_ms;      /* EWMA-smoothed RTT (0 until first sample) */
    __u32 samples;      /* running count of successful RTT samples */
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, FLOW_RTT_MAX);
    __type(key, struct myco_rtt_key);
    __type(value, struct myco_rtt_value);
} myco_rtt SEC(".maps");

/* Parse Ethernet + IPv4 + TCP. Fills iph_cp/tcph_cp with bounded copies.
 * Returns 0 on success, -1 if not an IPv4 TCP packet or header truncated. */
static __always_inline int parse_ipv4_tcp(struct __sk_buff *skb,
                                          struct iphdr *iph_cp,
                                          struct tcphdr *tcph_cp) {
    void *data     = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) return -1;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return -1;

    struct iphdr *iph = (struct iphdr *)(eth + 1);
    if ((void *)(iph + 1) > data_end) return -1;
    if (iph->protocol != IPPROTO_TCP) return -1;
    if (iph->ihl < 5) return -1;

    __u32 ihl = (__u32)iph->ihl * 4;
    struct tcphdr *tcph = (struct tcphdr *)((void *)iph + ihl);
    if ((void *)(tcph + 1) > data_end) return -1;

    *iph_cp  = *iph;
    *tcph_cp = *tcph;
    return 0;
}

/* Egress: client → server. Record tx_ts_ns + tx_seq_end. */
SEC("tc")
int myco_rtt_egress(struct __sk_buff *skb) {
    struct iphdr iph;
    struct tcphdr tcph;
    if (parse_ipv4_tcp(skb, &iph, &tcph) < 0) return TC_ACT_OK;

    __u32 ihl       = (__u32)iph.ihl * 4;
    __u32 ip_total  = bpf_ntohs(iph.tot_len);
    __u32 tcp_hlen  = (__u32)tcph.doff * 4;
    if (tcp_hlen < 20 || ip_total < ihl + tcp_hlen) return TC_ACT_OK;
    __u32 payload   = ip_total - ihl - tcp_hlen;

    /* Pure ACKs with no payload and no SYN/FIN don't generate an ACK
     * for us to RTT-match against. Skip them. */
    if (payload == 0 && !tcph.syn && !tcph.fin) return TC_ACT_OK;

    struct myco_rtt_key key = {};
    key.client_ip   = iph.saddr;
    key.server_ip   = iph.daddr;
    key.client_port = tcph.source;
    key.server_port = tcph.dest;
    key.protocol    = IPPROTO_TCP;

    __u32 seq_end = bpf_ntohl(tcph.seq) + payload;
    if (tcph.syn) seq_end += 1;
    if (tcph.fin) seq_end += 1;

    struct myco_rtt_value new_v = {};
    new_v.tx_ts_ns   = bpf_ktime_get_ns();
    new_v.tx_seq_end = seq_end;

    struct myco_rtt_value *existing = bpf_map_lookup_elem(&myco_rtt, &key);
    if (existing) {
        /* Preserve smoothed history; overwrite outgoing pointer. */
        new_v.srtt_ms = existing->srtt_ms;
        new_v.samples = existing->samples;
    }
    bpf_map_update_elem(&myco_rtt, &key, &new_v, BPF_ANY);
    return TC_ACT_OK;
}

/* Ingress: server → client. If this ACK acks our last tx_seq_end, compute RTT. */
SEC("tc")
int myco_rtt_ingress(struct __sk_buff *skb) {
    struct iphdr iph;
    struct tcphdr tcph;
    if (parse_ipv4_tcp(skb, &iph, &tcph) < 0) return TC_ACT_OK;
    if (!tcph.ack) return TC_ACT_OK;

    /* Key from the client's perspective: swap src/dst since this is RX. */
    struct myco_rtt_key key = {};
    key.client_ip   = iph.daddr;
    key.server_ip   = iph.saddr;
    key.client_port = tcph.dest;
    key.server_port = tcph.source;
    key.protocol    = IPPROTO_TCP;

    struct myco_rtt_value *v = bpf_map_lookup_elem(&myco_rtt, &key);
    if (!v) return TC_ACT_OK;
    if (v->tx_ts_ns == 0) return TC_ACT_OK;   /* already consumed */

    __u32 ack = bpf_ntohl(tcph.ack_seq);
    if (ack < v->tx_seq_end) return TC_ACT_OK;

    __u64 now = bpf_ktime_get_ns();
    if (now <= v->tx_ts_ns) return TC_ACT_OK;
    __u64 rtt_ns = now - v->tx_ts_ns;
    __u32 rtt_ms = (__u32)(rtt_ns / 1000000);
    if (rtt_ms == 0)  rtt_ms = 1;       /* clamp sub-ms to 1 */
    if (rtt_ms > 10000) return TC_ACT_OK; /* implausible — ignore */

    /* RFC 6298 EWMA: srtt = 7/8*srtt + 1/8*rtt */
    __u32 new_srtt;
    if (v->srtt_ms == 0) {
        new_srtt = rtt_ms;
    } else {
        new_srtt = (v->srtt_ms * 7 + rtt_ms) / 8;
    }
    v->srtt_ms  = new_srtt;
    v->tx_ts_ns = 0;     /* consumed — wait for next egress pkt */
    v->samples += 1;
    return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
