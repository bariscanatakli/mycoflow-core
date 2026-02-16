/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_netlink.h — Netlink-based qdisc stats reader
 */
#ifndef MYCO_NETLINK_H
#define MYCO_NETLINK_H

#include <stdint.h>

int  netlink_init(void);
int  netlink_get_qdisc_stats(const char *iface,
                             uint32_t *backlog,
                             uint32_t *drops,
                             uint32_t *overlimits);
void netlink_close(void);

#endif /* MYCO_NETLINK_H */
