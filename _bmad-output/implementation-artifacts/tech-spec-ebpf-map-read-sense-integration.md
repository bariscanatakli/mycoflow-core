---
title: 'eBPF Map Reading — Sense Module Integration'
slug: 'ebpf-map-read-sense-integration'
created: '2026-02-18'
status: 'completed'
stepsCompleted: [1, 2, 3, 4, 5, 6]
tech_stack: ['C (C11)', 'GCC / clang-bpf', 'libbpf', 'eBPF/TC', 'CMake 3.10+', 'minunit.h']
files_to_modify: ['src/myco_types.h', 'src/main.c']
code_patterns: ['bpf_map_lookup_elem', 'metrics_t struct extension', 'HAVE_LIBBPF guard', 'em-dash section divider', 'unsigned long long cast for %llu']
test_patterns: ['minunit.h / mu_assert / mu_run_test', 'memset zero-init of metrics_t in tests', 'integration-only for eBPF (no mock)']
---

# Tech-Spec: eBPF Map Reading — Sense Module Integration

**Created:** 2026-02-18

## Overview

### Problem Statement

`ebpf_read_stats()` in `src/myco_ebpf.c` (lines 118–135) correctly reads
packet/byte counters from the `myco_stats` BPF_MAP_TYPE_ARRAY via
`bpf_map_lookup_elem()`. However, in `ebpf_tick()` (lines 100–116) the results
are only logged and then discarded. `metrics_t` in `src/myco_types.h` has no
eBPF-sourced fields, so the sense module and control loop never receive
kernel-side traffic counters. This is the Phase 3 integration gap.

### Solution

Add two raw cumulative counter fields (`ebpf_rx_pkts`, `ebpf_rx_bytes`) to
`metrics_t`. In `main.c`, after `sense_sample()`, call `ebpf_read_stats()` and
populate those fields. When eBPF is unavailable (no `HAVE_LIBBPF` or map fd not
open), `ebpf_read_stats()` returns -1 and the fields stay zero — no fallback
logic needed. Update the main-loop log line to include the new counters.

### Scope

**In Scope:**
- Add `ebpf_rx_pkts` (uint64_t) and `ebpf_rx_bytes` (uint64_t) to `metrics_t`
  in `src/myco_types.h`
- Call `ebpf_read_stats()` in `main.c` immediately after `sense_sample()` and
  populate the two new fields on the `metrics` struct
- Update the `log_msg` line in the main loop to print the new eBPF counters

**Out of Scope:**
- Replacing `/proc/net/dev` reads with eBPF data (TC only covers one direction)
- Rate/delta computation in the eBPF module (raw cumulative is sufficient)
- Per-flow or hash-map eBPF reading
- Changes to `src/bpf/mycoflow.bpf.c` (kernel program is correct as-is)
- Changes to the sense module's internal logic (`myco_sense.c`)

---

## Context for Development

### Codebase Patterns

- **Conditional compile**: All libbpf-dependent code is guarded with
  `#ifdef HAVE_LIBBPF`. `ebpf_read_stats()` already handles the `#else` case
  by returning -1 — callers must check the return value.
- **metrics_t extension pattern**: New metrics fields are added directly to the
  `metrics_t` struct in `myco_types.h`. The struct is stack-allocated in main.c
  (`metrics_t metrics;`) and zero-initialised via `memset` inside `sense_sample()`.
  Existing tests (`test_control.c`) use `memset(&metrics, 0, sizeof(metrics))` —
  new fields will be zero by default, no test changes needed.
- **Section divider style** (required by project-context.md): field group comments
  in `myco_types.h` use `/* ── Comment ─── */` with em-dash characters. The new
  field comment must follow this pattern exactly.
- **`%llu` cast rule** (required by project-context.md): `uint64_t` on this
  platform is `unsigned long`; passing it to `%llu` requires an explicit
  `(unsigned long long)` cast to build warning-clean under `-Wall -Wextra`.
- **`myco_types.h` is single source of truth**: never define the same type in
  module headers. All `metrics_t` fields live only here.
- **Loop ordering in main.c (lines 147–156)**:
  ```
  sense_sample(...)     // line 148 — fills metrics from /proc
  ebpf_tick(&cfg)       // line 152 — attaches tc + logs (currently discards)
  flow_table_populate   // line 155
  ```
  eBPF read call goes between lines 148 and 152.
- **Log format**: The main-loop `log_msg` at line 192 uses `LOG_INFO, "loop"`.
  New eBPF fields appended to the same line for observability. Never use
  `printf`/`fprintf` in daemon code — always `log_msg`.
- **No dynamic allocation**: new fields are plain `uint64_t` on the stack —
  no malloc, consistent with project resource budget rules.

### Files to Reference

| File | Purpose |
| ---- | ------- |
| `src/myco_types.h` | `metrics_t` struct definition — add new fields here (lines 47–59) |
| `src/myco_ebpf.h` | Declares `ebpf_read_stats(uint64_t *packets, uint64_t *bytes)` — no changes |
| `src/myco_ebpf.c` | Implementation of `ebpf_read_stats()` (lines 118–135) — no changes |
| `src/main.c` | Main loop — insert call after line 148; extend log at line 192 |
| `src/bpf/mycoflow.bpf.c` | Kernel BPF program; defines `myco_stats` map — reference only |
| `src/tests/test_control.c` | Shows `memset(&metrics, 0, sizeof(metrics))` pattern — no changes needed |

### Technical Decisions

| Decision | Choice | Rationale |
| -------- | ------ | --------- |
| Augment vs Replace `/proc` | Augment | TC program is ingress-only; `/proc/net/dev` still needed for tx_bps |
| Wiring location | `main.c` | No new module dependencies; eBPF module stays self-contained |
| Counter representation | Raw cumulative uint64_t | Consistent with kernel-side atomics; no duplicate prev-tracking state |
| Fallback when no libbpf | Fields stay 0 | `ebpf_read_stats()` returns -1; fields remain zero from memset |

---

## Implementation Plan

### Tasks

> Ordered by dependency — complete Task 1 before Tasks 2 and 3.
> Tasks 2 and 3 are independent of each other once Task 1 is done.

- [x] Task 1: Add `ebpf_rx_pkts` and `ebpf_rx_bytes` fields to `metrics_t`
  - File: `src/myco_types.h`
  - Action: After the `avg_pkt_size` field (line 58), insert:
    ```c
    /* ── eBPF map counters (raw cumulative; 0 when libbpf unavailable) ── */
    uint64_t ebpf_rx_pkts;
    uint64_t ebpf_rx_bytes;
    ```
  - Notes: Use em-dash `──` characters as shown — not hyphens. `uint64_t` is
    already available via the existing `#include <stdint.h>` at line 9.

- [x] Task 2: Call `ebpf_read_stats()` in the main loop to populate new fields
  - File: `src/main.c`
  - Action: After the `sense_sample()` call (line 148) and before
    `ebpf_tick(&cfg)` (line 152), insert:
    ```c
    /* Populate eBPF counters into metrics (no-op if libbpf unavailable) */
    ebpf_read_stats(&metrics.ebpf_rx_pkts, &metrics.ebpf_rx_bytes);
    ```
  - Notes: `myco_ebpf.h` is already included at line 17 — no new `#include`
    needed. Return value is intentionally ignored; fields are already zero from
    `sense_sample()`'s `memset`. `ebpf_tick()` still calls `ebpf_read_stats()`
    internally — the resulting double-read is accepted (see Notes section).

- [x] Task 3: Extend the main-loop log line to include eBPF counters
  - File: `src/main.c`
  - Action: Replace the `log_msg` call at line 192 with:
    ```c
    log_msg(LOG_INFO, "loop",
            "rtt=%.2f(raw=%.2f)ms jitter=%.2f(raw=%.2f)ms tx=%.0fbps rx=%.0fbps"
            " cpu=%.1f%% qbl=%u qdr=%u flows=%d persona=%s bw=%dkbit"
            " reason=%s ebpf_pkts=%llu ebpf_bytes=%llu",
            metrics.rtt_ms, raw_rtt, metrics.jitter_ms, raw_jitter,
            metrics.tx_bps, metrics.rx_bps, metrics.cpu_pct,
            metrics.qdisc_backlog, metrics.qdisc_drops,
            flow_table_active_count(&flow_table),
            persona_name(persona), control_state.current.bandwidth_kbit, reason,
            (unsigned long long)metrics.ebpf_rx_pkts,
            (unsigned long long)metrics.ebpf_rx_bytes);
    ```
  - Notes: The `(unsigned long long)` cast is mandatory for `-Wall -Wextra`
    compliance. Do not add `<inttypes.h>` or use `PRIu64` — out of scope.

### Acceptance Criteria

- [x] AC-1: Given `myco_types.h` is compiled with `-Wall -Wextra`, when a
  `metrics_t` variable is declared and zero-initialised with `memset`, then
  the build produces zero warnings and the struct contains `ebpf_rx_pkts`
  and `ebpf_rx_bytes` as `uint64_t` fields.

- [x] AC-2: Given the daemon starts with `ebpf_enabled=1`, a valid `ebpf_obj`
  path, `HAVE_LIBBPF` defined, the TC hook attached to the interface, and the
  traffic-gen script has produced ingress packets, when one full iteration of
  the main loop completes, then `metrics.ebpf_rx_pkts > 0` and the loop log
  line contains non-zero `ebpf_pkts=` and `ebpf_bytes=` values.
  **VERIFIED** (Docker/ubuntu:22.04, privileged, bpffs mounted):
  `ebpf_pkts=2 → 5 → 8 → 12 → 14 → 15 → 16 → 17` during 8× ping traffic.

- [x] AC-3: Given the daemon starts with `HAVE_LIBBPF` not defined (or
  `ebpf_enabled=0`), when one full iteration of the main loop completes, then
  `metrics.ebpf_rx_pkts == 0` and `metrics.ebpf_rx_bytes == 0` with no crash
  and no error-level log from the new call site.

- [x] AC-4: Given the daemon is running in any configuration, when the main
  loop emits its per-iteration summary log, then the log line contains the
  literal substrings `ebpf_pkts=` and `ebpf_bytes=`.

---

## Additional Context

### Dependencies

- No new library dependencies
- No changes to `CMakeLists.txt`
- `myco_ebpf.h` already included in `main.c` (line 17)
- Task 1 must complete before Tasks 2 and 3

### Testing Strategy

Integration-only — no BPF map mock harness exists in the project.

- **With libbpf**: build with `HAVE_LIBBPF` defined, run daemon in Docker
  environment (`mycoflow-openwrt` container), attach TC hook, generate traffic
  via existing traffic-gen scripts, confirm non-zero `ebpf_pkts=` /
  `ebpf_bytes=` in loop logs
- **Without libbpf**: build without `HAVE_LIBBPF`, confirm zero fields, no
  crash, no error log from the new call site
- **Existing tests**: run `cmake --build build && ctest --test-dir build` to
  confirm no regressions — no test file changes are needed
- **Unit tests**: not applicable — no BPF map fd mock exists; do not create
  one for this change

### Pre-existing Bugs Fixed During Verification

Three pre-existing bugs in the Phase 3 scaffold were discovered and fixed as
part of AC-2 integration testing:

1. **Missing BTF in BPF object** (`src/CMakeLists.txt`): `clang` command lacked
   `-g`, so no BTF section was emitted; libbpf refused to load the object.
   Fix: added `-g` to the clang BPF compile command.

2. **libbpf prog-type inference failure** (`src/myco_ebpf.c`): `SEC("tc")` was
   not recognized by libbpf 0.5 on ubuntu:22.04. Fix: added
   `bpf_program__set_type(prog, BPF_PROG_TYPE_SCHED_CLS)` loop before
   `bpf_object__load()`.

3. **Dual-load map mismatch** (`src/myco_ebpf.c`): `ebpf_init()` loaded the
   BPF object via libbpf (creating map instance A), while `ebpf_attach_tc()`
   called `tc filter ... obj FILE` which reloaded the same file (map instance B).
   Traffic updated B; `ebpf_read_stats()` read from A — always 0. Fix: after
   `bpf_object__load()`, pin the first program to `/sys/fs/bpf/myco_tc_prog`;
   `ebpf_attach_tc()` uses `bpf da pinned <path>` when the pin exists, so TC
   and the map reader share the same program and map instance.
   Requires bpffs mounted at `/sys/fs/bpf`.

### Notes

- **Double-read (resolved)**: the internal `ebpf_read_stats()` call was removed
  from `ebpf_tick()` during the adversarial review fix phase. Only `main.c`
  reads the map per tick now.
- **TC ingress only**: the BPF program (`mycoflow.bpf.c`) counts TC ingress
  packets only. `ebpf_rx_pkts` reflects ingress packets, not egress — field
  naming uses `rx_` prefix intentionally.
- **Future work**: delta computation (pps/bps from eBPF) or per-CPU map
  reading can be layered on top of this integration without breaking anything
  established here.

---

## Review Notes

- Adversarial review completed (step 5)
- Findings: 13 total, 9 fixed, 4 deferred
- Resolution approach: auto-fix [F]

### Fixed
- F1: Removed redundant `ebpf_read_stats` from `ebpf_tick()` — single read per tick now in `main.c`
- F3: Added `LOG_WARN` when `bpf_program__pin()` fails with clear message about bpffs
- F4/F9: Added `g_prog_pinned` flag; `unlink` on shutdown only if pin succeeded
- F6: BPF compile now strips DWARF for Release builds (`llvm-strip --strip-debug`) while preserving BTF; `-g` kept always as BTF is required by libbpf
- F10: `snprintf` return values checked in all `tc` command builds
- F11: Replaced `bpf_object__for_each_program` + `break` with `bpf_program__next(NULL, obj)`
- F12: `ebpf_read_stats()` failure in `main.c` now zeros both fields instead of leaving stale values
- F13: Pin failure log message includes "bpffs not mounted?" hint

### Deferred
- F2: Pre-existing `system()` command injection via unsanitised `egress_iface`/`ebpf_obj` — pre-dates this spec, hardcoded pin path in new branch is safe
- F5: `BPF_MAP_TYPE_ARRAY` vs `BPF_MAP_TYPE_PERCPU_ARRAY` — requires BPF kernel program changes
- F7: Cumulative vs per-interval logging — cosmetic; documented in field naming (`rx_pkts` not `pps`)
- F8: `bpf_program__set_type()` deprecated in libbpf ≥0.8 — required for libbpf 0.5 compat (ubuntu 22.04 / older OpenWrt)
