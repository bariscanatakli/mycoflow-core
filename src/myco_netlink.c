/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_netlink.c — Read qdisc stats via NETLINK_ROUTE
 *
 * Uses raw netlink sockets to send RTM_GETQDISC and parse
 * TCA_STATS / TCA_STATS2 attributes for backlog, drops, overlimits.
 */
#include "myco_netlink.h"
#include "myco_log.h"

#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>

#include <sys/socket.h>

/* Legacy tc_stats structure (always present via TCA_STATS) */
struct tc_stats_legacy {
    uint64_t bytes;
    uint32_t packets;
    uint32_t drops;
    uint32_t overlimits;
    uint32_t bps;
    uint32_t pps;
    uint32_t qlen;
    uint32_t backlog;
};

/* ── Internal state ─────────────────────────────────────────── */

static int g_nl_fd = -1;
static uint32_t g_nl_seq = 1;

/* ── Init / Close ───────────────────────────────────────────── */

int netlink_init(void) {
    if (g_nl_fd >= 0) {
        return 0; /* already open */
    }

    g_nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (g_nl_fd < 0) {
        log_msg(LOG_WARN, "netlink", "socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(g_nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_msg(LOG_WARN, "netlink", "bind failed: %s", strerror(errno));
        close(g_nl_fd);
        g_nl_fd = -1;
        return -1;
    }

    log_msg(LOG_INFO, "netlink", "netlink socket ready");
    return 0;
}

void netlink_close(void) {
    if (g_nl_fd >= 0) {
        close(g_nl_fd);
        g_nl_fd = -1;
    }
}

/* ── Request / Parse ────────────────────────────────────────── */

/*
 * Build and send RTM_GETQDISC request for all qdiscs on a given ifindex.
 * NLM_F_DUMP requests all qdiscs; we filter in parse phase.
 */
static int send_qdisc_request(int ifindex) {
    struct {
        struct nlmsghdr nlh;
        struct tcmsg    tcm;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct tcmsg));
    req.nlh.nlmsg_type  = RTM_GETQDISC;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = g_nl_seq++;
    req.tcm.tcm_family  = AF_UNSPEC;
    req.tcm.tcm_ifindex = ifindex;

    if (send(g_nl_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        log_msg(LOG_WARN, "netlink", "send RTM_GETQDISC: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Parse rtattr chain looking for TCA_STATS (legacy struct tc_stats).
 * This attribute is always present and contains backlog, drops, overlimits.
 */
static int parse_qdisc_attrs(struct rtattr *rta, int len,
                             uint32_t *backlog,
                             uint32_t *drops,
                             uint32_t *overlimits) {
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == TCA_STATS) {
            if (RTA_PAYLOAD(rta) >= sizeof(struct tc_stats_legacy)) {
                struct tc_stats_legacy *st = (struct tc_stats_legacy *)RTA_DATA(rta);
                *backlog    = st->backlog;
                *drops      = st->drops;
                *overlimits = st->overlimits;
                return 0;
            }
        }
    }
    return -1; /* TCA_STATS not found */
}

/*
 * Receive netlink response and aggregate stats for matching ifindex.
 * We sum stats from all qdiscs on the interface (root + children).
 */
static int recv_and_parse(int ifindex,
                          uint32_t *backlog,
                          uint32_t *drops,
                          uint32_t *overlimits) {
    char buf[16384];
    int found = 0;

    *backlog = 0;
    *drops = 0;
    *overlimits = 0;

    for (;;) {
        ssize_t len = recv(g_nl_fd, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            log_msg(LOG_WARN, "netlink", "recv: %s", strerror(errno));
            return -1;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) {
                return found ? 0 : -1;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
                log_msg(LOG_WARN, "netlink", "NLMSG_ERROR: %d", err->error);
                return -1;
            }
            if (nlh->nlmsg_type != RTM_NEWQDISC) {
                continue;
            }

            struct tcmsg *tcm = (struct tcmsg *)NLMSG_DATA(nlh);
            if (tcm->tcm_ifindex != ifindex) {
                continue;
            }

            struct rtattr *rta = (struct rtattr *)((char *)tcm + NLMSG_ALIGN(sizeof(*tcm)));
            int rta_len = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*tcm)));

            uint32_t bl = 0, dr = 0, ol = 0;
            if (parse_qdisc_attrs(rta, rta_len, &bl, &dr, &ol) == 0) {
                *backlog    += bl;
                *drops      += dr;
                *overlimits += ol;
                found = 1;
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────── */

int netlink_get_qdisc_stats(const char *iface,
                            uint32_t *backlog,
                            uint32_t *drops,
                            uint32_t *overlimits) {
    if (g_nl_fd < 0) {
        return -1;
    }
    if (!iface || !backlog || !drops || !overlimits) {
        return -1;
    }

    unsigned int ifindex = if_nametoindex(iface);
    if (ifindex == 0) {
        log_msg(LOG_DEBUG, "netlink", "if_nametoindex(%s) failed", iface);
        return -1;
    }

    if (send_qdisc_request((int)ifindex) != 0) {
        return -1;
    }

    return recv_and_parse((int)ifindex, backlog, drops, overlimits);
}
