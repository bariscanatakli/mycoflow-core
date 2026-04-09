# Metrics Reference

## 4 Benchmark Scenarios
1. **FIFO** — no shaping (worst case baseline)
2. **CAKE-only** — shaping, no DSCP marking
3. **CAKE + Static DSCP** — manual iptables rules
4. **MycoFlow** — CAKE + daemon (automatic classification)

## Metric Groups & Success Criteria

### Group 1: Latency & Jitter (Weight: 30%)
| Metric | Method | Success |
|--------|--------|---------|
| RTT under load (interactive) | `ping -c 100 -i 0.1` wired gaming client | MycoFlow <= Static <= CAKE << FIFO |
| RTT under load (bulk) | Same from bulk client | Bulk RTT > interactive |
| Jitter (interactive) | stddev of RTT | MycoFlow lowest |
| P99 latency | 99th percentile | No spikes for prioritized |
| CAKE tin separation | `tc -s qdisc show` per-tin delay | Voice << Bulk (>100x) |

### Group 2: Persona Classification (Weight: 25%)
| Metric | Method | Success |
|--------|--------|---------|
| Accuracy (iperf3) | Ground truth vs daemon log | >=90% all 6 |
| Accuracy (real apps) | Ground truth vs daemon log | >=70% for 4+ |
| Confusion matrix | Actual vs classified | Minimal off-diagonal |
| Detection latency | Flow start to correct persona | <=3s |
| False positive rate | Noise flows misclassified | <10% |

### Group 3: Adaptation (Weight: 20%)
| Metric | Method | Success |
|--------|--------|---------|
| Reclassification time | Traffic change, time to DSCP update | <5s (static: never) |
| New device classification | Unknown device joins | <5s |
| Encrypted traffic | QUIC/HTTP3 distinction | MycoFlow correct |

### Group 4: System Overhead (Weight: 15%)
| Metric | Method | Success |
|--------|--------|---------|
| CPU overhead | /proc/stat delta | <5% additional |
| Memory footprint | ps -o rss | <512 KB |
| Sense cycle duration | Daemon log | <100ms |
| Actual cycle rate | Inter-cycle timestamps | +/-20% of target |
| Conntrack scaling | Duration at 100-2000 entries | Linear, <100ms |

### Group 5: Throughput Impact (Weight: 10%)
| Metric | Method | Success |
|--------|--------|---------|
| Max throughput (baseline) | iperf3 TCP, CAKE-only | Baseline |
| Max throughput (daemon) | iperf3 TCP, MycoFlow | Within 2% |
| Multi-flow fairness | 5 simultaneous iperf3 | Fair per CAKE tins |

## Grade Scale
| Grade | Meaning |
|-------|---------|
| A+ | Matches or exceeds QEMU results |
| A | Minor degradation, still strong |
| B | Noticeable trade-offs but viable |
| C | Works with significant limitations |
| F | Unacceptable for thesis claim |

## Statistical Requirements
- N >= 5 runs per scenario
- Report mean +/- stddev
- Warm-up pass before recording
- Kill unnecessary OpenWrt services during tests
