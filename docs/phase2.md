# Phase 2 Plan — MycoFlow Daemon & Control Loop

Goal: Ship a working, non-ML reflexive loop on OpenWrt/Docker sim with persona-aware heuristics, hysteresis, rollback, and bounded CAKE actuation.

## Lifecycle & Config
- Minimal UCI schema: enabled, egress_iface, sample_hz, max_cpu; defaults + validation.
- Configurable loop interval and log level; hot-reload via uci/ubus trigger.
- Signal handling (SIGTERM/SIGINT) for graceful shutdown.

## Sensing (MycoSense v1)
- /proc/net/dev or netlink counters; ICMP RTT probe with jitter estimation.
- Idle baseline at cold start: short probe window to capture idle RTT/jitter per tin.
- Persona signals v0: DNS/SNI category, packet size histogram, inter-arrival variance, flow symmetry; sticky hysteresis and k-of-m to smooth class changes.

## Control & Safety
- Hysteresis/k-of-m filter for metric thresholds; PID/hysteresis implementation placeholder.
- Action scheduler: rate-limit (rho ops/s) + min cooldown (tau), bounded step sizes.
- Rollback/safe-mode: snapshot metrics + applied policy; watchdog freezes loop on CPU/metric outlier and reverts to last stable config.

## Actuation (MycoAct)
- tc-cake wrappers; dynamic bandwidth parameter update.
- Error handling and backoff on failed tc calls.

## Observability & Budget
- Structured logs (timestamp, source, metric); configurable verbosity.
- Optional metric dump (file/ubus) and dummy-metric harness for loop testing.
- Resource guardrails: CPU target <20% (peak 40%), RAM <64 MB; LRU cap for metric/flow tables.

## Integration Surface
- ubus surface: myco.status, myco.persona (list/add/delete), myco.policy (get/set/boost/throttle) with rate-limit + least-privilege ACL.
- init/service script; copy mycoflowd + default config into router image; docker-compose healthcheck and crash-loop backoff.

## Acceptance for Phase 2
- Daemon runs in sim, reacts to dummy metrics, logs decisions with causes.
- Persona v0 + hysteresis influence actuation (even if CAKE calls are stubbed).
- Rollback path exercised in test harness; healthcheck passes.