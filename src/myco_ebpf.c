/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ebpf.c — eBPF load/attach/read/shutdown
 */
#include "myco_ebpf.h"
#include "myco_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_LIBBPF
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/bpf.h>
static struct bpf_object *g_bpf_obj = NULL;
static int g_map_fd = -1;
static int g_prog_pinned = 0;  /* 1 if pin to MYCO_BPF_PIN_PATH succeeded */
#define MYCO_BPF_PIN_PATH "/sys/fs/bpf/myco_tc_prog"
#endif

static int  g_ebpf_attached = 0;
static char g_ebpf_iface[32];
static char g_ebpf_dir[16];

int ebpf_init(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled) {
        return 0;
    }
#ifdef HAVE_LIBBPF
    if (access(cfg->ebpf_obj, R_OK) != 0) {
        log_msg(LOG_WARN, "ebpf", "ebpf obj not found: %s", cfg->ebpf_obj);
        return -1;
    }

    g_bpf_obj = bpf_object__open_file(cfg->ebpf_obj, NULL);
    if (!g_bpf_obj) {
        log_msg(LOG_WARN, "ebpf", "failed to open bpf obj: %s", cfg->ebpf_obj);
        return -1;
    }

    /* Set prog type explicitly so libbpf doesn't rely on section-name inference */
    struct bpf_program *prog;
    bpf_object__for_each_program(prog, g_bpf_obj) {
        bpf_program__set_type(prog, BPF_PROG_TYPE_SCHED_CLS);
    }

    if (bpf_object__load(g_bpf_obj) != 0) {
        log_msg(LOG_WARN, "ebpf", "failed to load bpf obj: %s", cfg->ebpf_obj);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        return -1;
    }

    log_msg(LOG_INFO, "ebpf", "bpf object loaded (no attach yet): %s", cfg->ebpf_obj);

    /* Pin the prog so ebpf_attach_tc reuses the same map instance.
     * Requires bpffs mounted at /sys/fs/bpf (e.g. "mount -t bpf none /sys/fs/bpf").
     * On failure TC falls back to obj-based load (separate map — counters read 0). */
    /* bpf_program__next(NULL, obj) returns the first program; available since libbpf 0.1 */
    struct bpf_program *first_prog = bpf_program__next(NULL, g_bpf_obj);
    if (first_prog) {
        unlink(MYCO_BPF_PIN_PATH);
        if (bpf_program__pin(first_prog, MYCO_BPF_PIN_PATH) == 0) {
            g_prog_pinned = 1;
            log_msg(LOG_INFO, "ebpf", "prog pinned: %s", MYCO_BPF_PIN_PATH);
        } else {
            log_msg(LOG_WARN, "ebpf",
                    "prog pin failed (bpffs not mounted?): %s — TC will use separate map instance, counters will read 0",
                    MYCO_BPF_PIN_PATH);
        }
    }

    g_map_fd = bpf_object__find_map_fd_by_name(g_bpf_obj, "myco_stats");
    if (g_map_fd < 0) {
        log_msg(LOG_WARN, "ebpf", "failed to find map: myco_stats");
    } else {
        log_msg(LOG_INFO, "ebpf", "found map myco_stats fd=%d", g_map_fd);
    }
    return 0;
#else
    if (cfg->ebpf_attach) {
        log_msg(LOG_INFO, "ebpf", "libbpf not available, using tc attach only");
        return 0;
    }
    log_msg(LOG_WARN, "ebpf", "ebpf enabled but libbpf not available");
    return -1;
#endif
}

int ebpf_attach_tc(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled || !cfg->ebpf_attach) {
        return 0;
    }
    if (g_ebpf_attached) {
        return 0;
    }

    if (access(cfg->ebpf_obj, R_OK) != 0) {
        log_msg(LOG_WARN, "ebpf", "ebpf obj not found: %s", cfg->ebpf_obj);
        return -1;
    }

    const char *dir = cfg->ebpf_tc_dir[0] ? cfg->ebpf_tc_dir : "ingress";
    strncpy(g_ebpf_iface, cfg->egress_iface, sizeof(g_ebpf_iface) - 1);
    g_ebpf_iface[sizeof(g_ebpf_iface) - 1] = '\0';
    strncpy(g_ebpf_dir, dir, sizeof(g_ebpf_dir) - 1);
    g_ebpf_dir[sizeof(g_ebpf_dir) - 1] = '\0';
    char cmd[512];
    int n;
    n = snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null", cfg->egress_iface);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        log_msg(LOG_WARN, "ebpf", "tc qdisc cmd truncated");
        return -1;
    }
    system(cmd);

#ifdef HAVE_LIBBPF
    /* Use the libbpf-pinned prog so TC and the map reader share the same instance */
    if (access(MYCO_BPF_PIN_PATH, F_OK) == 0) {
        n = snprintf(cmd, sizeof(cmd), "tc filter replace dev %s %s bpf da pinned %s",
                     cfg->egress_iface, dir, MYCO_BPF_PIN_PATH);
    } else {
        n = snprintf(cmd, sizeof(cmd), "tc filter replace dev %s %s bpf da obj %s sec tc",
                     cfg->egress_iface, dir, cfg->ebpf_obj);
    }
#else
    n = snprintf(cmd, sizeof(cmd), "tc filter replace dev %s %s bpf da obj %s sec tc",
                 cfg->egress_iface, dir, cfg->ebpf_obj);
#endif
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        log_msg(LOG_WARN, "ebpf", "tc filter cmd truncated");
        return -1;
    }
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_WARN, "ebpf", "tc attach failed (rc=%d)", rc);
        return -1;
    }
    g_ebpf_attached = 1;
    log_msg(LOG_INFO, "ebpf", "tc attach ok (%s)", dir);
    return 0;
}

void ebpf_tick(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled || !cfg->ebpf_attach) {
        return;
    }
    if (!g_ebpf_attached) {
        ebpf_attach_tc(cfg);
    }
}

int ebpf_read_stats(uint64_t *packets, uint64_t *bytes) {
#ifdef HAVE_LIBBPF
    if (g_map_fd < 0) {
        return -1;
    }
    uint32_t key = 0;
    struct { uint64_t packets; uint64_t bytes; } val;
    
    if (bpf_map_lookup_elem(g_map_fd, &key, &val) != 0) {
        return -1;
    }
    if (packets) *packets = val.packets;
    if (bytes) *bytes = val.bytes;
    return 0;
#else
    (void)packets;
    (void)bytes;
    return -1;
#endif
}

void ebpf_shutdown(void) {
    if (g_ebpf_attached && g_ebpf_iface[0]) {
        char cmd[512];
        const char *dir = g_ebpf_dir[0] ? g_ebpf_dir : "ingress";
        snprintf(cmd, sizeof(cmd), "tc filter del dev %s %s 2>/dev/null", g_ebpf_iface, dir);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s clsact 2>/dev/null", g_ebpf_iface);
        system(cmd);
    }
#ifdef HAVE_LIBBPF
    if (g_prog_pinned) {
        unlink(MYCO_BPF_PIN_PATH);
        g_prog_pinned = 0;
    }
    if (g_bpf_obj) {
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
    }
#endif
    g_ebpf_attached = 0;
}
