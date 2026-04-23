#include <stdio.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <sys/socket.h>

int main() {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return 1;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_PACKET && ifa->ifa_data != NULL) {
            struct rtnl_link_stats *stats = ifa->ifa_data;
            printf("%s: rx=%u tx=%u\n", ifa->ifa_name, stats->rx_bytes, stats->tx_bytes);
        }
    }
    freeifaddrs(ifaddr);
    return 0;
}
