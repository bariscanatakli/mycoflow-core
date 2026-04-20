/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_rtt.c — Per-flow RTT engine
 *
 * Two modes:
 *
 *   HAVE_LIBBPF + bpf_obj_path present
 *     Loads mycoflow_rtt.bpf.o, attaches TC egress + ingress on the WAN
 *     interface, and reads the "myco_rtt" map for srtt_ms per 5-tuple.
 *     This is the production path on OpenWrt.
 *
 *   Stub
 *     Everything else (dev host without libbpf, bpf obj missing, etc.).
 *     In-process stub table — rtt_engine_inject_stub() stamps values so
 *     the classifier's auto-correction path can be unit-tested without
 *     a kernel probe.
 *
 * Internal key layout mirrors the BPF map exactly (see mycoflow_rtt.bpf.c):
 *   client_ip = LAN-side addr (NBO), server_ip = WAN-side addr (NBO),
 *   ports NBO, protocol always 6.
 *
 * flow_key_t → BPF key translation. flow_key_t stores src/dst in NBO and
 * ports in HOST byte order (per myco_flow.h). The BPF side speaks NBO
 * for everything, so we htons the ports at lookup time.
 */
#include "myco_rtt.h"
#include "myco_log.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RTT_STUB_SIZE 64

typedef struct {
    flow_key_t key;
    uint32_t   rtt_ms;
    int        used;
} rtt_stub_entry_t;

#ifdef HAVE_LIBBPF
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#define MYCO_RTT_PIN_DIR      "/sys/fs/bpf/mycoflow_rtt"
#define MYCO_RTT_MAP_PIN      "/sys/fs/bpf/mycoflow_rtt_map"

/* Must match mycoflow_rtt.bpf.c struct myco_rtt_key/value. */
struct bpf_rtt_key {
    uint32_t client_ip;
    uint32_t server_ip;
    uint16_t client_port;
    uint16_t server_port;
    uint8_t  protocol;
    uint8_t  pad[3];
};
struct bpf_rtt_value {
    uint64_t tx_ts_ns;
    uint32_t tx_seq_end;
    uint32_t srtt_ms;
    uint32_t samples;
};
#endif

struct rtt_engine {
    rtt_stub_entry_t stub[RTT_STUB_SIZE];
    int              bpf_backed;      /* 1 = live map available */

#ifdef HAVE_LIBBPF
    struct bpf_object *obj;
    int                map_fd;        /* fd of the myco_rtt map */
    char               iface[32];
#endif
};

static int keys_equal(const flow_key_t *a, const flow_key_t *b) {
    return a->src_ip == b->src_ip && a->dst_ip == b->dst_ip &&
           a->src_port == b->src_port && a->dst_port == b->dst_port &&
           a->protocol == b->protocol;
}

#ifdef HAVE_LIBBPF
/* Attach the egress + ingress TC filters by shelling out to `tc`. We
 * pin the programs so the same map instance is shared. libbpf's native
 * TC attach API (bpf_tc_*) is newer and less reliable across distros. */
static int rtt_attach_tc(const char *iface) {
    if (!iface || !*iface) return -1;
    char cmd[512];

    /* Ensure clsact qdisc exists (idempotent). */
    snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null", iface);
    (void)system(cmd);

    snprintf(cmd, sizeof(cmd),
             "tc filter replace dev %s egress bpf da pinned "
             MYCO_RTT_PIN_DIR "/myco_rtt_egress 2>/dev/null", iface);
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd),
             "tc filter replace dev %s ingress bpf da pinned "
             MYCO_RTT_PIN_DIR "/myco_rtt_ingress 2>/dev/null", iface);
    if (system(cmd) != 0) return -1;

    return 0;
}

static void rtt_detach_tc(const char *iface) {
    if (!iface || !*iface) return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "tc filter del dev %s egress 2>/dev/null", iface);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd),
             "tc filter del dev %s ingress 2>/dev/null", iface);
    (void)system(cmd);
}

static int rtt_load_bpf(rtt_engine_t *eng, const char *bpf_obj_path,
                        const char *iface) {
    if (access(bpf_obj_path, R_OK) != 0) {
        log_msg(LOG_INFO, "rtt", "bpf obj not available: %s — stub mode",
                bpf_obj_path);
        return -1;
    }

    eng->obj = bpf_object__open_file(bpf_obj_path, NULL);
    if (!eng->obj) {
        log_msg(LOG_WARN, "rtt", "bpf_object__open_file failed: %s",
                bpf_obj_path);
        return -1;
    }

    /* Force SCHED_CLS on every program so libbpf doesn't need to infer. */
    struct bpf_program *prog;
    bpf_object__for_each_program(prog, eng->obj) {
        bpf_program__set_type(prog, BPF_PROG_TYPE_SCHED_CLS);
    }

    if (bpf_object__load(eng->obj) != 0) {
        log_msg(LOG_WARN, "rtt", "bpf_object__load failed");
        bpf_object__close(eng->obj);
        eng->obj = NULL;
        return -1;
    }

    /* Pin each program under MYCO_RTT_PIN_DIR/<name> so `tc ... pinned …`
     * can reference them. Requires /sys/fs/bpf to be mounted. */
    (void)mkdir(MYCO_RTT_PIN_DIR, 0755);
    bpf_object__for_each_program(prog, eng->obj) {
        const char *name = bpf_program__name(prog);
        char pin_path[128];
        snprintf(pin_path, sizeof(pin_path), "%s/%s", MYCO_RTT_PIN_DIR, name);
        unlink(pin_path);
        if (bpf_program__pin(prog, pin_path) != 0) {
            log_msg(LOG_WARN, "rtt",
                    "bpf prog pin failed for %s (bpffs mounted?)", name);
            bpf_object__close(eng->obj);
            eng->obj = NULL;
            return -1;
        }
    }

    eng->map_fd = bpf_object__find_map_fd_by_name(eng->obj, "myco_rtt");
    if (eng->map_fd < 0) {
        log_msg(LOG_WARN, "rtt", "myco_rtt map not found");
        bpf_object__close(eng->obj);
        eng->obj = NULL;
        return -1;
    }

    if (rtt_attach_tc(iface) != 0) {
        log_msg(LOG_WARN, "rtt", "tc attach failed on %s", iface);
        bpf_object__close(eng->obj);
        eng->obj = NULL;
        return -1;
    }

    strncpy(eng->iface, iface, sizeof(eng->iface) - 1);
    log_msg(LOG_INFO, "rtt",
            "bpf rtt engine attached: iface=%s obj=%s", iface, bpf_obj_path);
    return 0;
}
#endif /* HAVE_LIBBPF */

rtt_engine_t *rtt_engine_open(const char *bpf_obj_path,
                              const char *egress_iface) {
    rtt_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) return NULL;

#ifdef HAVE_LIBBPF
    eng->map_fd = -1;
    if (bpf_obj_path && egress_iface && egress_iface[0] &&
        rtt_load_bpf(eng, bpf_obj_path, egress_iface) == 0) {
        eng->bpf_backed = 1;
        return eng;
    }
#else
    (void)bpf_obj_path; (void)egress_iface;
#endif

    log_msg(LOG_INFO, "rtt", "stub engine (no live RTT feed)");
    return eng;
}

void rtt_engine_close(rtt_engine_t *eng) {
    if (!eng) return;
#ifdef HAVE_LIBBPF
    if (eng->bpf_backed) {
        rtt_detach_tc(eng->iface);
        if (eng->obj) bpf_object__close(eng->obj);
    }
#endif
    free(eng);
}

uint32_t rtt_engine_lookup_ms(rtt_engine_t *eng, const flow_key_t *key) {
    if (!eng || !key) return 0;
    if (key->protocol != 6) return 0;   /* TCP only */

#ifdef HAVE_LIBBPF
    if (eng->bpf_backed && eng->map_fd >= 0) {
        struct bpf_rtt_key bk = {0};
        bk.client_ip   = key->src_ip;         /* already NBO */
        bk.server_ip   = key->dst_ip;
        bk.client_port = htons(key->src_port);
        bk.server_port = htons(key->dst_port);
        bk.protocol    = 6;

        struct bpf_rtt_value bv = {0};
        if (bpf_map_lookup_elem(eng->map_fd, &bk, &bv) == 0) {
            return bv.srtt_ms;
        }
        return 0;
    }
#endif

    /* Stub lookup. */
    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        const rtt_stub_entry_t *e = &eng->stub[i];
        if (!e->used) continue;
        if (keys_equal(&e->key, key)) return e->rtt_ms;
    }
    return 0;
}

void rtt_engine_inject_stub(rtt_engine_t *eng, const flow_key_t *key,
                            uint32_t rtt_ms) {
    if (!eng || !key) return;
    /* BPF-backed engines ignore injection — real data flows through the
     * kernel, not this call. Tests that use injection pass NULL bpf path
     * and get the stub engine. */
    if (eng->bpf_backed) return;

    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        rtt_stub_entry_t *e = &eng->stub[i];
        if (e->used && keys_equal(&e->key, key)) {
            e->rtt_ms = rtt_ms;
            return;
        }
    }
    for (int i = 0; i < RTT_STUB_SIZE; i++) {
        rtt_stub_entry_t *e = &eng->stub[i];
        if (!e->used) {
            e->key    = *key;
            e->rtt_ms = rtt_ms;
            e->used   = 1;
            return;
        }
    }
}
