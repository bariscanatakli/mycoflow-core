---
stepsCompleted: []
lastStep: ''
lastContinued: ''
date: ''
user_name: ''
sprintRun: 1
deviceModel: 'Xiaomi AX3000T'
openwrtVersion: ''
flowOffloadingStatus: ''
---

# MycoFlow Real Device Sprint Report

## Environment Snapshot

**Device:** {{deviceModel}}
**OpenWrt Version:** {{openwrtVersion}}
**Kernel:** [to be filled]
**Date:** {{date}}
**Sprint Run:** #{{sprintRun}}

### Installed Packages
[opkg list-installed output]

### Kernel Modules
[conntrack, IFB, tc, CAKE status]

### Flow Offloading
**Status:** {{flowOffloadingStatus}}
**Resolution:** [what was decided and why]

## Link Characteristics

**WAN Bandwidth (Down):** [Mbps]
**WAN Bandwidth (Up):** [Mbps]
**RTT to ISP Gateway:** [ms]
**Encapsulation:** [PPPoE/VDSL/Fiber/Ethernet]
**CAKE Configuration:**
```
[tc qdisc show output]
```

## Device Capacity Baseline

### CPU/RAM Without Daemon
[measurements]

### CPU/RAM With Daemon (Idle)
[measurements]

### Conntrack Scale Test
| Conntrack Entries | Sense Cycle Duration (ms) |
|-------------------|---------------------------|
| 100               |                           |
| 500               |                           |
| 1000              |                           |
| 2000              |                           |

### Frequency Sweep
| Daemon Frequency | CPU Overhead (%) | Detection Accuracy |
|------------------|-------------------|--------------------|
| 0.5 Hz           |                   |                    |
| 1.0 Hz           |                   |                    |
| 2.0 Hz           |                   |                    |

## Benchmark Results

### Latency & Jitter (N=5 runs, mean +/- stddev)

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| RTT interactive (ms)     |         |         |             |          |
| RTT bulk (ms)            |         |         |             |          |
| Jitter interactive (ms)  |         |         |             |          |
| P99 latency (ms)         |         |         |             |          |
| Tin separation ratio     | N/A     |         |             |          |

### Throughput Impact

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| Max throughput (Mbps)    |         |         |             |          |
| Throughput delta (%)     | base    |         |             |          |
| Multi-flow fairness      |         |         |             |          |

### System Overhead

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| CPU overhead (%)         | 0       |         |             |          |
| Memory (KB RSS)          | 0       | 0       | 0           |          |
| Sense cycle (ms)         | N/A     | N/A     | N/A         |          |
| Actual cycle rate (Hz)   | N/A     | N/A     | N/A         |          |

## Persona Classification Accuracy

### Controlled Tests (iperf3)
| Persona    | Accuracy (%) | Notes |
|------------|-------------|-------|
| VOIP       |             |       |
| GAMING     |             |       |
| VIDEO      |             |       |
| STREAMING  |             |       |
| BULK       |             |       |
| TORRENT    |             |       |

### Real Application Tests
| Application       | Expected Persona | Detected Persona | Correct? |
|-------------------|------------------|------------------|----------|
| Zoom/Teams        | VOIP             |                  |          |
| YouTube           | STREAMING        |                  |          |
| Steam download    | BULK             |                  |          |
| Torrent client    | TORRENT          |                  |          |
| Web browsing      | VIDEO            |                  |          |

### Confusion Matrix
```
              VOIP  GAMING  VIDEO  STREAM  BULK  TORRENT
VOIP           -
GAMING              -
VIDEO                       -
STREAMING                           -
BULK                                        -
TORRENT                                           -
```

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
|   |                 |          |             |        |                |

## Limitations & Operational Ceiling

[Document any limitations discovered, maximum concurrent connections, CPU ceiling, etc.]

## Thesis Export Notes

[Notes for integrating results into mycelium_report.tex]
