# MycoFlow

**Bio-Inspired Reflexive QoS Daemon for OpenWrt**

MycoFlow is a C11 userspace daemon that brings automatic, traffic-aware Quality of Service to home and SOHO routers running OpenWrt. It continuously observes network conditions (RTT, jitter, bandwidth, active flows) and adapts CAKE qdisc parameters in real time — with no manual traffic rules or per-device static assignments.

> Target hardware: Xiaomi AX3000T — MediaTek MT7981B (2× Cortex-A53 @ 1.3 GHz), 256 MB RAM, 128 MB NAND

---

## How It Works

The daemon runs a reflexive loop at a configurable sample rate (default 2 Hz):

```
Sense → Infer → Act → Stabilize
```

1. **Sense** — Reads per-interface counters, probes RTT/jitter via ICMP, queries conntrack for active flows, and sniffs DNS responses passively.
2. **Infer** — Classifies each flow into a *service type* (GAME\_RT, VOIP\_CALL, VIDEO\_VOD, BULK\_DL, …) using three weighted signals: DNS hostname, port hint, and traffic behavior. Per-device personas are derived from the active service mix and the device's configured priority profile.
3. **Act** — Sets CAKE diffserv4 tin targets and pushes per-flow CONNMARK values via `libnetfilter_conntrack` so the kernel routes each packet to the correct tin without per-packet CPU overhead.
4. **Stabilize** — Applies a sliding EWMA baseline and an action cooldown to prevent oscillation.

### 6-Persona Classification

| Persona | RTT target | Typical traffic |
|---------|-----------|-----------------|
| VOIP | 20 ms | Discord voice, SIP |
| GAMING | 50 ms | CS2, Valorant, LoL |
| VIDEO | 75 ms | Zoom, Teams, live streams |
| STREAMING | 150 ms | YouTube VOD, Netflix |
| BULK | 200 ms | apt upgrade, GitHub clone |
| TORRENT | 200 ms | BitTorrent |

DSCP markings: `EF → Voice`, `CS4 → Voice`, `CS3 → Video`, `CS2 → Video`, `CS1 → Bulk`.

### 4-Layer Flow-Aware Architecture (v3)

```
L4 — CAKE diffserv4 qdisc (kernel, egress WAN iface)
       Voice / Video / BestEffort / Bulk tins
           ▲
L3 — DSCP Applier  (iptables mangle + CONNMARK)
       ct mark → DSCP  (≤12 fixed rules, never changes)
           ▲
L2 — Service→Mark Resolver  (daemon, per device profile)
       service_t + priority profile → ct mark
           ▲
L1 — Flow Service Detector  (daemon, 3-signal weighted vote)
       DNS cache (0.6) + port hint (0.3) + behavior (0.1) → service_t
```

---

## Flash Safety

MycoFlow writes **zero bytes to flash**:

| Output | Destination |
|--------|-------------|
| Logs | stdout → procd → logd (RAM buffer) |
| State JSON | `/tmp/myco_state.json` (tmpfs = RAM) |
| Metric file | disabled by default; use `/tmp/` if enabled |

All learned state lives in RAM and resets on reboot — intentionally, to avoid stale decisions after network changes.

---

## Benchmark Results

Tests run on a real OpenWrt 23.05.5 VM (QEMU, x86-64, CAKE diffserv4):

| Scenario | Idle (ms) | Under load (ms) | Δ | Grade |
|----------|-----------|-----------------|---|-------|
| FIFO (no QoS) | 0.45 | 693.82 | +693 ms | **F** |
| CAKE only | 0.43 | 0.63 | +0.2 ms | **A+** |
| CAKE + MycoFlow | 0.58 | 0.71 | +0.1 ms | **A+** |

MycoFlow halves the residual latency increase vs CAKE-alone while saturating the WAN link with bulk UDP traffic. CAKE tin statistics confirm correct classification: gaming traffic reaches the Voice tin (≈0.2 ms average delay) while bulk traffic is contained to the Bulk tin (≈34.7 ms average delay) — a **154× separation**.

---

## Project Structure

```
mycoflow-core/
├── src/                    # C11 daemon (mycoflowd)
│   ├── main.c              # Entry point, Sense→Infer→Act→Stabilize loop
│   ├── myco_sense.c/h      # RTT/jitter/bandwidth sampling
│   ├── myco_persona.c/h    # 6-persona classifier with history window
│   ├── myco_flow.c/h       # Conntrack flow table
│   ├── myco_device.c/h     # Per-device aggregation and DSCP apply
│   ├── myco_service.c/h    # Flow service detector (3-signal)
│   ├── myco_classifier.c/h # Flow-aware tick: service→mark→RTT feedback
│   ├── myco_mark.c/h       # CONNMARK push via libnetfilter_conntrack
│   ├── myco_mangle.c/h     # iptables mangle chain management
│   ├── myco_dns.c/h        # Passive DNS sniffer + hostname cache
│   ├── myco_hint.c/h       # Port → service hint table
│   ├── myco_profile.c/h    # Device priority profiles
│   ├── myco_act.c/h        # tc/CAKE actuation
│   ├── myco_control.c/h    # Adaptive bandwidth control + safe mode
│   ├── myco_config.c/h     # UCI + environment variable config
│   ├── myco_ewma.c/h       # Exponential weighted moving average
│   ├── myco_rtt.c/h        # Per-flow RTT engine (eBPF-assisted)
│   ├── myco_ebpf.c/h       # eBPF packet counter integration
│   ├── myco_netlink.c/h    # Netlink helpers
│   ├── myco_ubus.c/h       # OpenWrt ubus RPC bridge
│   ├── myco_log.c/h        # Structured logger
│   ├── myco_types.h        # Shared types (metrics_t, policy_t, persona_t…)
│   ├── bpf/                # eBPF programs (packet counter, RTT probe)
│   └── tests/              # Unit tests (minunit)
├── luci-app-mycoflow/      # LuCI web dashboard (2 s polling)
├── scripts/
│   ├── benchmark.sh        # Docker lab benchmark runner
│   ├── qemu-bench.sh       # QEMU OpenWrt benchmark
│   ├── realworld-bench.sh  # Bare-metal multi-app benchmark
│   ├── phase3_multirun.sh  # N-run statistical experiment
│   └── run_integration.sh  # Integration test harness
├── docker/qemu-lab/        # QEMU OpenWrt lab (Dockerfile + setup scripts)
├── docs/                   # Architecture, benchmark reports, sprint notes
│   ├── architecture-v3-flow-aware.md
│   ├── qemu-benchmark.md
│   └── qemu-benchmark-6persona.md
└── CMakeLists.txt
```

---

## Build

### Native (x86-64, development)

```bash
cmake -B build && cmake --build build
```

### Cross-compile for OpenWrt aarch64 (Xiaomi AX3000T)

```bash
cmake -B build-aarch64 \
  -DCMAKE_C_COMPILER=aarch64-openwrt-linux-musl-gcc \
  -DSTATIC_BUILD=ON
cmake --build build-aarch64
```

The resulting `mycoflowd` binary is statically linked and has no glibc dependency.

### Optional features detected at configure time

| Feature | CMake variable | Effect |
|---------|---------------|--------|
| `libnetfilter_conntrack` | `HAVE_LIBNFCT` | Real ct mark push (required for flow-aware mode) |
| `libbpf` + `clang` | `HAVE_LIBBPF` | eBPF packet counter + RTT probe |
| `libubus` | `HAVE_UBUS` | OpenWrt ubus RPC interface |

---

## Tests

```bash
cmake --build build && ctest --test-dir build -V
```

All 13 unit test targets cover: EWMA filter, actuation, control decisions, config parsing, persona classifier, port hints, DNS cache, per-device aggregation, service detector, RTT engine, mangle chain, profile resolver, and the full flow classifier tick.

---

## Configuration

MycoFlow reads `/etc/config/mycoflow` (UCI format) with environment variable overrides.

Key settings:

| Option | Default | Description |
|--------|---------|-------------|
| `enabled` | `1` | Enable/disable daemon |
| `egress_iface` | `pppoe-wan` | WAN interface for CAKE qdisc |
| `bandwidth_kbit` | `100000` | Egress bandwidth cap (kbit/s) |
| `ingress_bandwidth_kbit` | `0` | Ingress cap via IFB (0 = disabled) |
| `sample_hz` | `2` | Sense loop frequency |
| `per_device_enabled` | `0` | Per-device DSCP marking |
| `flow_aware_enabled` | `0` | Flow-level service detection (v3) |
| `baseline_update_interval` | `60` | Sliding baseline refresh (cycles) |
| `action_cooldown_s` | `5.0` | Minimum seconds between actuations |

Environment variable override: `MYCOFLOW_EGRESS_IFACE` (overrides `egress_iface`).

---

## Deploy to Router

```bash
scp build-aarch64/src/mycoflowd root@10.10.1.1:/tmp/mycoflowd-new
ssh root@10.10.1.1 'mv /tmp/mycoflowd-new /usr/bin/mycoflowd && /etc/init.d/mycoflowd restart'
```

Send `SIGHUP` to reload config without restarting: `kill -HUP $(pidof mycoflowd)`.

---

## Resource Footprint

Measured on MediaTek MT7981B at 2 Hz sample rate:

- **RAM**: ~336 bytes of additional state for flow-aware mode
- **CPU**: < 0.0001% average overhead
- **Flash writes**: 0

---

## License

Academic / research use. See `ieee_paper.pdf` for the accompanying paper.
