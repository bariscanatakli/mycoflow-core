/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_mark.c — libnetfilter_conntrack wrapper implementation
 *
 * Two implementations compiled under a single header:
 *   HAVE_LIBNFCT defined → real CT_UPDATE via libnetfilter_conntrack
 *   otherwise             → no-op stubs that log a single warning
 *
 * The no-op path lets the daemon build and run on dev hosts without
 * the library installed. On OpenWrt (with the SDK) HAVE_LIBNFCT is set
 * and we push real marks.
 */
#include "myco_mark.h"
#include "myco_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct mark_engine {
    void    *handle;     /* nfct_handle* when HAVE_LIBNFCT, else NULL */
    uint64_t ok_count;
    uint64_t err_count;
    int      stubbed;    /* 1 = library absent, set() is a no-op */
};

#ifdef HAVE_LIBNFCT
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <libmnl/libmnl.h>

mark_engine_t *mark_engine_open(void) {
    struct nfct_handle *h = nfct_open(CONNTRACK, 0);
    if (!h) {
        log_msg(LOG_WARN, "mark", "nfct_open failed: %s", strerror(errno));
        return NULL;
    }
    mark_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) {
        nfct_close(h);
        return NULL;
    }
    eng->handle  = h;
    eng->stubbed = 0;
    log_msg(LOG_INFO, "mark", "conntrack mark engine ready (libnetfilter_conntrack)");
    return eng;
}

void mark_engine_close(mark_engine_t *eng) {
    if (!eng) return;
    if (eng->handle) nfct_close((struct nfct_handle *)eng->handle);
    free(eng);
}

int mark_engine_set(mark_engine_t *eng, const flow_key_t *key, uint32_t mark) {
    if (!eng || !key) return 0;
    if (eng->stubbed)  return 0;

    struct nf_conntrack *ct = nfct_new();
    if (!ct) {
        eng->err_count++;
        return -1;
    }

    nfct_set_attr_u8 (ct, ATTR_L3PROTO,  AF_INET);
    nfct_set_attr_u32(ct, ATTR_IPV4_SRC, key->src_ip);   /* already NBO */
    nfct_set_attr_u32(ct, ATTR_IPV4_DST, key->dst_ip);
    nfct_set_attr_u8 (ct, ATTR_L4PROTO,  key->protocol);
    nfct_set_attr_u16(ct, ATTR_PORT_SRC, htons(key->src_port));
    nfct_set_attr_u16(ct, ATTR_PORT_DST, htons(key->dst_port));
    nfct_set_attr_u32(ct, ATTR_MARK,     mark);

    int rc = nfct_query((struct nfct_handle *)eng->handle, NFCT_Q_UPDATE, ct);
    nfct_destroy(ct);

    if (rc < 0) {
        eng->err_count++;
        return -1;
    }
    eng->ok_count++;
    return 0;
}

#else /* ! HAVE_LIBNFCT ─────────────────────────────────────────── */

mark_engine_t *mark_engine_open(void) {
    static int warned = 0;
    if (!warned) {
        log_msg(LOG_WARN, "mark", "libnetfilter_conntrack not present — ct marks disabled");
        warned = 1;
    }
    mark_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) return NULL;
    eng->stubbed = 1;
    return eng;
}

void mark_engine_close(mark_engine_t *eng) {
    free(eng);
}

int mark_engine_set(mark_engine_t *eng, const flow_key_t *key, uint32_t mark) {
    (void)key; (void)mark;
    if (!eng) return 0;
    /* Count as "ok" so tests can observe call volume even in stub mode. */
    eng->ok_count++;
    return 0;
}

#endif /* HAVE_LIBNFCT */

uint64_t mark_engine_stat_ok(const mark_engine_t *eng) {
    return eng ? eng->ok_count : 0;
}

uint64_t mark_engine_stat_err(const mark_engine_t *eng) {
    return eng ? eng->err_count : 0;
}
