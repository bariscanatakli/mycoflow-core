/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_mark.h — Conntrack mark pusher (libnetfilter_conntrack wrapper)
 *
 * Thin wrapper around libnetfilter_conntrack's CT_UPDATE. Given a 5-tuple
 * and a 32-bit mark value, it finds the matching conntrack entry and sets
 * its `mark` field. The kernel's connmark match in the mangle table then
 * stamps DSCP on every packet of that flow (see myco_mangle.h).
 *
 * Build-conditional: if libnetfilter_conntrack headers are absent at build
 * time (HAVE_LIBNFCT undefined), the functions degrade to no-ops that log
 * a single warning and return success. The daemon stays alive; flows just
 * don't get DSCP-marked until the library is provisioned.
 *
 * Thread safety: one engine per thread. The classifier currently runs on
 * the main loop only, so a single engine suffices.
 */
#ifndef MYCO_MARK_H
#define MYCO_MARK_H

#include "myco_flow.h"

#include <stdint.h>

typedef struct mark_engine mark_engine_t;

/* Open a netlink conntrack handle. Requires CAP_NET_ADMIN.
 * Returns NULL if the library is unavailable or the socket can't open —
 * callers should treat NULL as "marking disabled" and continue. */
mark_engine_t *mark_engine_open(void);

/* Close handle and free resources. Safe to call on NULL. */
void mark_engine_close(mark_engine_t *eng);

/* Push `mark` onto the conntrack entry that matches `key`.
 * Returns 0 on success, -1 on error (no entry, permission denied, …).
 * No-op (returns 0) when eng is NULL. */
int mark_engine_set(mark_engine_t *eng, const flow_key_t *key, uint32_t mark);

/* Diagnostic: total number of successful set() calls since open. */
uint64_t mark_engine_stat_ok(const mark_engine_t *eng);

/* Diagnostic: total number of failed set() calls since open. */
uint64_t mark_engine_stat_err(const mark_engine_t *eng);

#endif /* MYCO_MARK_H */
