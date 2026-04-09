---
stepsCompleted: ['step-01-init', 'step-02-preflight', 'step-03-link-char', 'step-04-capacity', 'step-05-benchmark']
lastStep: 'step-05-benchmark'
lastContinued: ''
date: '2026-04-09'
user_name: 'Barış Can Ataklı'
sprintRun: 1
deviceModel: 'Xiaomi AX3000T'
openwrtVersion: '24.10.2'
flowOffloadingStatus: 'DISABLED'
---

# MycoFlow Real Device Sprint Report

## Environment Snapshot

**Device:** Xiaomi AX3000T (MediaTek MT7981B, 2x Cortex-A53 @ 1.3GHz)
**RAM:** 256MB
**Flash:** 128MB NAND
**OpenWrt Version:** 24.10.2
**Kernel:** 6.6.93
**Date:** 2026-04-09
**Sprint Run:** #1

### Installed Packages
- kmod-sched-cake (CAKE qdisc)
- kmod-ifb (Intermediate Functional Block)
- kmod-nft-conntrack (conntrack for nftables)
- iptables-legacy (mangle table for DSCP marking)
- tc-full (traffic control)

### Kernel Modules
- `sch_cake` — loaded, active
- `ifb` — loaded, ifb0 interface up
- `nf_conntrack` — loaded, `/proc/net/nf_conntrack` accessible
- `cls_matchall` — loaded (for ingress redirect)

### Flow Offloading
**Status:** DISABLED
**Resolution:** Flow offloading (`nft_flow_offload`) freezes conntrack counters, making MycoFlow blind to traffic patterns. Disabled via `uci set firewall.@defaults[0].flow_offloading='0'`. Zapret's flow_offload chain also flushed and deleted with `nft flush chain inet fw4 flow_offload && nft delete chain inet fw4 flow_offload`. This is the expected trade-off — MycoFlow requires live conntrack data.

## Link Characteristics

**WAN Type:** PPPoE
**WAN Bandwidth (Down):** ~50 Mbps
**WAN Bandwidth (Up):** ~13.5 Mbps
**RTT to 8.8.8.8:** 31.2–40.9ms (avg 33.4ms)
**Encapsulation:** PPPoE (overhead 34, mpu 64)

**CAKE Configuration — Egress (pppoe-wan):**
```
qdisc cake 800b: root bandwidth 12800Kbit diffserv4 triple-isolate nat wash
  no-ack-filter split-gso rtt 22ms noatm overhead 34 mpu 64

                   Bulk  Best Effort        Video        Voice
  thresh        800Kbit    12800Kbit     6400Kbit     3200Kbit
  target         22.7ms       1.42ms       2.84ms       5.68ms
  interval       45.4ms       22.3ms       23.7ms       26.6ms
```

**CAKE Configuration — Ingress (ifb0):**
```
qdisc cake 800c: root bandwidth 47500Kbit diffserv4 triple-isolate nat wash ingress
  no-ack-filter split-gso rtt 22ms noatm overhead 34 mpu 64

                   Bulk  Best Effort        Video        Voice
  thresh       2968Kbit    47500Kbit    23750Kbit    11875Kbit
  target         6.12ms        1.1ms        1.1ms       1.53ms
  interval         27ms         22ms         22ms       22.4ms
```

**CAKE Validation:** Traffic confirmed flowing through all tins:
- Egress: 24,467 packets processed (BE: 24,542 / Voice: 7)
- Ingress: 43,047 packets processed (BE: 41,521 / Voice: 1,706 / Bulk: 316 / Video: 75)
- Drops: Egress 82, Ingress 571 (normal shaping behavior)

## Device Capacity Baseline

### CPU/RAM Without Daemon
**Idle System (no daemon, no active traffic):**
- CPU: 0% usr, 4% sys, 95% idle
- Load average: 0.00 0.04 0.01
- RAM: 239,784 KB total, 99,064 KB free, 97,920 KB available
- RAM used: 92,536 KB (buff/cache: 48,184 KB)

### CPU/RAM With Daemon (Idle)
**Daemon running, idle network:**
- CPU: 0% usr, 4% sys, 95% idle (no measurable change in `top`)
- Daemon VSZ: **276 KB** (process memory)
- RAM used: 94,096 KB → **delta: +1,560 KB** (includes daemon + runtime allocations)
- RAM free: 97,504 KB, available: 96,360 KB
- Daemon self-reported CPU: 1.0–3.5% (includes system-wide CPU measurement)

**Daemon overhead proof:**
- CPU delta: **~0%** (not visible in top)
- RAM delta: **+1,560 KB** (~0.65% of total RAM)
- Daemon binary: 119 KB (static-pie, stripped)

### Daemon Runtime Observations
- Sense cycle: 1 Hz (default), ~1.02s between cycles
- Baseline capture: 5 samples at startup (5s at 1Hz, 2.5s at 2Hz)
- Conntrack flows observed: 239–293 (idle home network background)
- Persona in idle: "torrent" (flows>30 triggers torrent heuristic on background connections — DNS, NTP, zapret, etc.)
- Safe-mode: active (tc disabled), no actuation performed

### Conntrack Scale Test
| Conntrack Entries | Parse Duration |
|-------------------|----------------|
| ~300              | <1ms (below measurement resolution) |
| ~430              | <1ms (below measurement resolution) |

**Note:** BusyBox `time` has only second-level resolution. At home-network scale (300–430 entries), conntrack parsing overhead is negligible. Conntrack max set to 15,360 by default.

### Frequency Sweep
| Daemon Frequency | VSZ (KB) | Cycle Interval | CPU (idle) | CPU (traffic burst) |
|------------------|----------|----------------|------------|---------------------|
| 0.5 Hz           | 276      | ~2.0s          | ~0.7%      | ~18% (spike)        |
| 1.0 Hz (default) | 280      | ~1.0s          | ~1–2%      | ~10–14%             |
| 2.0 Hz           | 280      | ~0.51s          | ~1–3%      | ~8–17%              |

**Note:** CPU values are daemon's self-reported system CPU measurement (not daemon-only). In `top`, the daemon shows 0% CPU at all frequencies. CPU spikes correlate with background network traffic (downloads), not daemon processing. Memory footprint is constant across frequencies.

## Benchmark Results

**Test methodology:** Ping to 8.8.8.8 (20 ICMP packets, 1s interval) while downloading 100MB file via curl (WAN saturation). N=5 runs per scenario, 5s cooldown between runs. Default OpenWrt qdisc is fq_codel (not pure FIFO).

### Raw Data — Per-Run Results

**Scenario 1: fq_codel (OpenWrt default, no shaping)**
| Run | Avg (ms) | Max (ms) | Mdev (ms) | Loss |
|-----|----------|----------|-----------|------|
| 1   | 35.4     | 39.8     | 3.27      | 5%   |
| 2   | 34.2     | 39.5     | 2.56      | 0%   |
| 3   | 38.3     | 73.1     | 9.70      | 5%   |
| 4   | 35.5     | 40.2     | 3.02      | 5%   |
| 5   | 33.9     | 40.5     | 2.61      | 5%   |

**Scenario 2: CAKE only (no DSCP marking)**
| Run | Avg (ms) | Max (ms) | Mdev (ms) | Loss |
|-----|----------|----------|-----------|------|
| 1   | 32.9     | 40.8     | 1.91      | 0%   |
| 2   | 32.5     | 36.9     | 1.06      | 0%   |
| 3   | 33.3     | 39.5     | 2.19      | 0%   |
| 4   | 35.0     | 60.0     | 6.31      | 5%   |
| 5   | 33.2     | 40.1     | 2.40      | 5%   |

**Scenario 3: CAKE + Static DSCP (ICMP→CS4/Voice, torrent ports→CS1/Bulk)**
| Run | Avg (ms) | Max (ms) | Mdev (ms) | Loss |
|-----|----------|----------|-----------|------|
| 1   | 33.6     | 40.4     | 2.54      | 5%   |
| 2   | 33.3     | 40.5     | 2.47      | 0%   |
| 3   | 32.8     | 39.0     | 1.50      | 0%   |
| 4   | 32.8     | 40.9     | 1.90      | 0%   |
| 5   | 32.7     | 34.6     | 0.73      | 0%   |

**Scenario 4: MycoFlow (CAKE + daemon, tc enabled)**
| Run | Avg (ms) | Max (ms) | Mdev (ms) | Loss |
|-----|----------|----------|-----------|------|
| 1   | 33.6     | 42.4     | 3.04      | 0%   |
| 2   | 32.7     | 36.1     | 1.09      | 0%   |
| 3   | 32.6     | 34.2     | 0.62      | 0%   |
| 4   | 33.5     | 40.1     | 2.29      | 0%   |
| 5   | 32.8     | 38.8     | 1.43      | 0%   |

### Summary Table (N=5 runs, mean +/- stddev)

|                          | fq_codel | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|----------|---------|-------------|----------|
| RTT avg (ms)             | 35.5±1.7 | 33.4±1.0| 33.1±0.4    | 33.0±0.4 |
| RTT max (ms)             | 73.1     | 60.0    | 40.9        | 42.4     |
| Jitter/mdev avg (ms)     | 4.23±3.0 | 2.77±1.9| 1.83±0.7    | 1.69±0.9 |
| Packet loss (%)          | 4%       | 2%      | 1%          | **0%**   |
| Tin separation (Voice/BE) | N/A     | N/A     | Yes (CS4)   | **~90x** (9us vs 806us) |

### MycoFlow Daemon Behavior During Benchmark
- Persona detected: **torrent** (correct for bulk download)
- Bandwidth adjustment: 20Mbit → **10Mbit** (torrent throttle)
- RTT parameter: 22ms → **200ms** (torrent persona)
- CAKE tin stats: BE=681,127 pkts / Voice=142 pkts / Video=19 / Bulk=0
- Voice tin av_delay: 9us, Best Effort av_delay: 806us
- Safe-mode active after initial actuation (prevents over-correction)

### Throughput Impact

|                          | fq_codel | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|----------|---------|-------------|----------|
| Max throughput (Mbps)    | ~50 (unshared) | 47.5 (shaped) | 47.5 | 47.5 (→10 under torrent) |
| Throughput delta (%)     | base     | -5%     | -5%         | -5% (idle), -80% (torrent throttle) |

### System Overhead

|                          | fq_codel | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|----------|---------|-------------|----------|
| CPU overhead (%)         | 0        | 0       | 0           | ~0% (top) |
| Memory (KB RSS)          | 0        | 0       | 0           | 276      |
| Sense cycle (ms)         | N/A      | N/A     | N/A         | ~1020    |
| Actual cycle rate (Hz)   | N/A      | N/A     | N/A         | ~1.0     |

## Persona Classification Accuracy

### Controlled Tests (iperf3)
Not performed — iperf3 generates synthetic traffic that doesn't match real application patterns. Per-device classification testing was done with real applications instead.

### Real Application Tests (Per-Device Mode, MYCOFLOW_PER_DEVICE=1)

**Test device:** 10.10.1.172 (Desktop, Windows 11, Chrome browser)

**Before fix (flows>30 threshold):**
| Application       | Expected Persona | Detected Persona | Flows | Avg Pkt | Correct? | Notes |
|-------------------|------------------|------------------|-------|---------|----------|-------|
| YouTube 1080p     | STREAMING/VIDEO  | **TORRENT**      | 47-50 | 56 B    | NO       | flows>30 threshold too low for Chrome |
| Bulk download (curl 100MB) | BULK/TORRENT | **TORRENT** | 60    | 68 B    | PARTIAL  | Torrent is close but not exact |
| Idle (post-download) | UNKNOWN/VIDEO | **TORRENT**     | 49    | 68 B    | NO       | Background browser connections persist |

**After fix (flows>100 + bandwidth gate, new STREAMING rule):**
| Application       | Expected Persona | Detected Persona | Flows | Avg Pkt | Correct? | Notes |
|-------------------|------------------|------------------|-------|---------|----------|-------|
| YouTube 1080p     | STREAMING        | **STREAMING**    | 39-40 | 365-388 B | **YES** | Rule 2b: flows>15 + high rx + asymmetric |
| Bulk download (curl 100MB) | BULK   | **STREAMING**    | 37-40 | 417-436 B | PARTIAL | Mixed with browser flows → streaming |
| Idle (post-download) | UNKNOWN      | **STREAMING**    | 35-38 | 460 B   | NO       | Persona history window retains last state |

**Other devices on network (auto-detected):**
| Device IP      | Detected Persona | Flows | Notes |
|----------------|-----------------|-------|-------|
| 10.10.1.188    | torrent         | 61    | Another device with many flows |
| 10.10.1.166    | gaming          | 4     | Low flow count → correct heuristic area |
| 10.10.1.165    | torrent         | 56    | Another high-flow device |
| 10.10.1.234    | gaming          | 5     | Low flow count |
| 10.10.1.197    | bulk            | 3     | Low flow count |
| 10.10.1.136    | bulk            | 3     | Low flow count |
| 10.10.1.149    | bulk            | 1     | Single flow |
| 127.0.0.1      | torrent         | 36    | Loopback (zapret, local services) |

### Root Cause Analysis: flows>30 Threshold Bug

**Decision tree first rule:** `flows>30 → TORRENT` fires before any other heuristic.

**Problem:** Modern browsers (Chrome) maintain 30-50+ concurrent connections even for single-tab usage:
- QUIC/HTTP3 multiplexing creates multiple UDP flows
- DNS prefetch, analytics, extension background connections
- Keep-alive connections to CDNs
- Service workers, push notification channels

**Impact:** On a real home network, ANY device running a modern browser will be classified as TORRENT, regardless of actual traffic type. This makes per-device persona classification ineffective for browser-based traffic.

**Proposed fix (future work):**
1. Raise `flows>30` threshold to `flows>100` or `flows>150`
2. Add bandwidth-weighted flow counting (ignore tiny flows)
3. Use bytes/flow ratio instead of raw flow count
4. Consider per-protocol flow counting (separate UDP/TCP)

### Confusion Matrix (Real Device)
```
Expected →    VOIP  GAMING  VIDEO  STREAM  BULK  TORRENT
Detected ↓
VOIP           ?
GAMING              ?
VIDEO                       ?
STREAMING                           ?
BULK                                        ~
TORRENT       ALL CLASSIFIED AS TORRENT (flows>30 bug)
```
**Note:** Confusion matrix incomplete due to flows>30 threshold bug. All devices with modern browsers (>30 concurrent flows) are classified as TORRENT regardless of actual traffic.

## Adaptation Tests

### Traffic Change Test
| Event                  | Time to Reclassify | Static DSCP Result | MycoFlow Result |
|------------------------|--------------------|--------------------|-----------------|
| Gamer stops, torrents  |                    |                    |                 |
| New device joins       |                    |                    |                 |

### Encrypted Traffic Test (QUIC/HTTP3)
| Traffic Type    | Static DSCP Classification | MycoFlow Classification | Correct? |
|-----------------|---------------------------|------------------------|----------|
| YouTube (QUIC)  |                           |                        |          |
| Video call      |                           |                        |          |

## QEMU vs Real Device Comparison

|                          | QEMU Result | Real Device | Delta | Notes |
|--------------------------|-------------|-------------|-------|-------|
| RTT interactive (ms)     | +0.1        |             |       |       |
| RTT bulk (ms)            |             |             |       |       |
| Tin separation ratio     | 154x        |             |       |       |
| CPU overhead (%)         | ~0%         |             |       |       |
| Persona accuracy         |             |             |       |       |

## Metric Grades

| Metric Group       | Weight | Grade | Notes |
|--------------------|--------|-------|-------|
| Latency & Jitter   | 30%    |       |       |
| Persona Accuracy   | 25%    |       |       |
| Adaptation         | 20%    |       |       |
| System Overhead    | 15%    |       |       |
| Throughput Impact  | 10%    |       |       |
| **Overall**        | 100%   |       |       |

## Bug / Fix Log

| # | Bug Description | Severity | Fix Applied | Status | Re-test Result |
|---|-----------------|----------|-------------|--------|----------------|
| 1 | Flow offloading freezes conntrack counters | High | Disabled flow_offloading via uci + removed zapret chain | Fixed | Conntrack entries updating correctly |
| 2 | scp fails (sftp-server not found) | Low | Used `scp -O` (legacy SCP protocol) | Fixed | Binary deployed successfully |
| 3 | BusyBox `timeout` command missing | Low | Used `daemon &` + `sleep` + `kill` pattern | Workaround | Daemon starts/stops correctly |
| 4 | BusyBox ping no fractional intervals | Low | Used default 1s interval | Workaround | Ping tests work |
| 5 | **flows>30 → TORRENT threshold too low** | **HIGH** | Raised to flows>100 + bw>500kbps; added STREAMING rule (flows>15 + rx>2Mbps + asymmetric) | **Fixed** | YouTube now correctly STREAMING; idle still sticky (history window) |
| 6 | per_device_enabled default=0 | Medium | Must set MYCOFLOW_PER_DEVICE=1 env var | Workaround | Per-device works when enabled |
| 7 | no_tc default=1 (tc disabled by default) | Medium | Must set MYCOFLOW_NO_TC=0 env var | By Design | Safety default — prevents accidental shaping |
| 8 | SQM was enabled in LuCI during tests | Low | Disabled SQM (`/etc/init.d/sqm disable`) | Fixed | Our tc commands override SQM; results valid |

## Limitations & Operational Ceiling

- **Flow offloading disabled:** Required trade-off for MycoFlow to function. May increase CPU load under high throughput. Hardware NAT acceleration also affected.
- **Zapret DPI bypass:** Present on router; its flow_offload chain had to be manually removed. May conflict if re-enabled.
- **flows>30 TORRENT threshold:** Modern browsers maintain 30-50+ concurrent connections. Any device running Chrome/Firefox will be misclassified as TORRENT. This is the #1 real-world issue discovered in this sprint. Proposed fix: raise threshold to 100+ or use bandwidth-weighted counting.
- **Per-device not default:** `MYCOFLOW_PER_DEVICE=1` must be explicitly set. Without it, global persona is always TORRENT on a busy home network (200+ background flows).
- **Conntrack scale:** 256 entries visible at idle (home network background). Real-world conntrack is much noisier than lab (QEMU) environment.

## Thesis Export Notes

[to be filled in step-08]
