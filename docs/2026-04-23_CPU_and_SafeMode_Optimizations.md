# Performance and Stability Optimizations (April 23, 2026)

## 1. Safe Mode Hysteresis (Debounce & Streak)
- **Problem:** Spikes in CPU or RTT caused the daemon to enter safe mode immediately and permanently.
- **Solution:** Implemented integer streak tracking. 
  - `SAFE_MODE_ENTER_STREAK=3`: The system must observe 3 consecutive limit breaches to enter safe mode.
  - `SAFE_MODE_EXIT_STREAK=8`: The system automatically exits safe mode and resumes QoS after 8 consecutive clean cycles.
- **Impact:** System automatically recovers from transient spikes. The daemon is no longer stuck.

## 2. Zero-Copy Flow Parsing (Netlink Conntrack)
- **Problem:** String-parsed `/proc/net/nf_conntrack` every second, causing massive CPU spikes.
- **Solution:** Switched to `libnetfilter_conntrack` and Netlink binary dumps (`NFCT_Q_DUMP`).
- **Impact:** 100% elimination of string parsing (`strstr`, `sscanf`). Zero-allocation, binary flow table population.

## 3. Raw C ICMP Sockets for RTT
- **Problem:** Called `popen("ping -c 3 ...")` every second. Spawning a new shell process is extremely expensive.
- **Solution:** Replaced with raw C sockets (`IPPROTO_ICMP`). The daemon crafts, sends, and polls `ICMP_ECHO` natively.
- **Impact:** Completely eliminated process forking and stdout pipe string parsing. Precision moved to nanoseconds.

## 4. Binary Network Interface Stats
- **Problem:** `read_netdev()` used `fopen("/proc/net/dev")` and `sscanf`.
- **Solution:** Switched to Linux `getifaddrs()` returning `AF_PACKET` stats via `struct rtnl_link_stats`.
- **Impact:** Eliminated char arrays and string validations for counting interfaces.
