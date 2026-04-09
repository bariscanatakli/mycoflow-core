---
stepsCompleted: ['step-01-discovery', 'step-02-classification', 'step-02-elicitation', 'step-03-requirements', 'step-04-tools', 'step-05-plan-review', 'step-06-design', 'step-07-foundation', 'step-08-build-step-01', 'step-09-build-steps-02-08']
created: 2026-03-28
status: COMPLETE
completedDate: 2026-03-28
approvedDate: 2026-03-28
---

# Workflow Creation Plan

## Discovery Notes

**User's Vision:**
A repeatable sprint workflow for deploying MycoFlow on the real Xiaomi AX3000T, running structured benchmarks (FIFO vs CAKE vs MycoFlow, multi-client persona scenarios), discovering and fixing real-world issues, and producing publishable benchmark results for the thesis (mycelium_report.tex).

**Who It's For:**
Baris — solo operator, hands-on with the physical AX3000T and client devices generating traffic.

**What It Produces:**
- Real-device benchmark results (latency, tin separation, persona detection accuracy) ready for the paper
- Bug/fix log for issues discovered during real hardware testing
- Repeatable — designed to re-run after fixes or for new test scenarios

**Key Insights:**
- QEMU benchmarks already complete (FIFO +693ms, CAKE +0.2ms, MycoFlow +0.1ms) — real device should validate/extend these
- Same scenarios (FIFO vs CAKE vs MycoFlow, 5-persona multi-client) plus potential new real-world scenarios
- Expects to discover edge cases: real traffic patterns, conntrack behavior, MT7981B performance limits
- Hardware target: Xiaomi AX3000T — MT7981B (2x Cortex-A53 @ 1.3GHz), 256MB RAM, 128MB NAND
- Results feed directly into thesis (mycelium_report.tex)

## Classification Decisions

**Workflow Name:** real-device-sprint
**Target Path:** _bmad/custom/src/workflows/real-device-sprint/

**4 Key Decisions:**
1. **Document Output:** true — produces benchmark reports + bug/fix log
2. **Module Affiliation:** standalone — custom project workflow
3. **Session Type:** continuable — spans multiple sessions (deploy, benchmark, fix, re-run)
4. **Lifecycle Support:** create-only — each run is a fresh pass; re-run for new results

**Structure Implications:**
- Needs `steps-c/` only (create-only)
- Needs `step-01b-continue.md` for resuming across sessions
- `stepsCompleted` tracking in output frontmatter
- Free-form document template for benchmark results output

## Pre-mortem Findings (Elicitation)

### Identified Risk Areas

**1. Deployment Failure Risks**
- Cross-compile binary segfault on device
- Missing kernel modules (conntrack, IFB, tc/CAKE) in AX3000T OpenWrt build
- iptables-nft vs nftables mismatch (known QEMU issue — real image may differ)
- Insufficient flash space for mycoflowd + dependencies

**2. Invalid Benchmark Results Risks**
- WiFi interference / uncontrolled network traffic pollutes measurements
- Insufficient client load to stress MT7981B
- No clean FIFO/CAKE-only baselines → can't prove improvement
- Client-side latency includes WiFi jitter, masking actual qdisc behavior

**3. Persona Detection Failures on Real Traffic**
- Real conntrack entries may differ from lab — silent parsing failure
- DNS/mDNS/ARP noise creates tiny flows confusing decision tree
- TORRENT threshold (flows>30) false-positive on heavy browsing
- VOIP detection (pkt<120 + bw<200k) matches NTP/SNMP/keepalives

**4. Hardware Limit Risks**
- 2Hz sense cycle + conntrack + tc commands saturates Cortex-A53
- 256MB RAM fills under heavy conntrack (thousands of flows)
- /tmp/ (tmpfs) state writes compete with system services

**5. Incomplete Thesis Data Risks**
- Missing metrics: jitter, CPU usage, memory footprint
- No reproducible methodology → reviewer can't verify
- Single-run data lacks statistical significance (need multiple runs + error bars)
- QEMU vs real-device methodology mismatch → not directly comparable

### Prevention → Workflow Step Implications

| Risk | Required Workflow Step |
|------|----------------------|
| Deploy fails | **Pre-flight checklist**: verify kernel modules, flash space, binary execution before any benchmarking |
| Meaningless results | **Environment control**: wired-only tests first, controlled traffic generation, eliminate WiFi variable |
| Persona misdetection | **Parsing validation**: verify conntrack parsing on real device, log raw persona decisions for review |
| Hardware limits | **Device monitoring**: capture CPU/RAM throughout tests, establish device capacity baseline |
| Incomplete data | **Metrics definition**: define exact metrics table BEFORE running tests, require N≥3 runs per scenario |
| Non-reproducible | **Full scripting**: zero manual steps during benchmarks, everything scripted and logged |

## Red Team vs Blue Team Findings (Elicitation)

### Benchmark Methodology Hardening

**1. Measurement Validity**
- Primary benchmarks MUST use wired clients (Ethernet) — WiFi tests are a separate labeled scenario
- Measure on-device where possible: `tc -s qdisc` for queue stats, `/tmp/myco_state.json` for daemon metrics
- Use 1-hop wired ping with timestamps, not through-WiFi measurements

**2. Comparison Fairness — 4 Scenarios Required**
- **FIFO** (no shaping) — baseline worst case
- **CAKE-only** (no DSCP marking) — shows what shaping alone does
- **CAKE + static DSCP rules** (manual iptables) — shows what a sysadmin could do without MycoFlow
- **MycoFlow** (CAKE + daemon) — shows automatic classification value
- The 4th scenario (CAKE+static-DSCP) is critical: proves MycoFlow's novelty is *automatic* classification, not just DSCP+CAKE

**3. Adaptation Test (Killer Proof)**
- Change traffic patterns mid-test (e.g., gamer stops, torrent starts)
- Static DSCP rules can't adapt → stale classification
- MycoFlow adapts within seconds → correct reclassification
- This is the strongest thesis contribution proof

**4. Real Application Validation**
- Must test with real apps: Zoom/Teams call, YouTube stream, Steam download, torrent client, browser tabs
- iperf3-only results will be challenged by reviewers
- Log every persona decision + raw conntrack data that produced it
- Report confusion matrix: ground truth vs daemon classification

**5. Statistical Rigor**
- N≥5 runs per scenario (upgraded from N≥3), report mean ± stddev
- Box plots or error bars in thesis figures
- Warm-up pass before recording (eliminate cold-start effects)
- Kill unnecessary OpenWrt services during tests

**6. Reproducibility Requirements**
- Environment snapshot at start of each run: OpenWrt version, `opkg list-installed`, kernel version
- CAKE parameters logged: `tc -s qdisc show` output
- Client specs + OS + tool versions in report header
- All benchmark scripts in `scripts/` — zero manual steps
- Full test harness published in repo

## First Principles Analysis: QEMU vs Real Hardware (Elicitation)

### Critical Differences That Change Everything

**1. CPU Contention (QEMU: unlimited → Real: shared)**
- 2x Cortex-A53 @ 1.3GHz shared with WiFi driver (mt76), NAT, conntrack, system services
- Daemon is a guest on a busy system, not the host
- **Required:** Measure CPU delta — system load *without* mycoflowd vs *with* it. This proves the "lightweight" thesis claim.

**2. Hardware Flow Offloading — POTENTIAL SHOWSTOPPER**
- MT7981B has a PPE (Packet Processing Engine) that can offload NAT/routing
- If `flow_offloading` is enabled, **conntrack counters freeze for offloaded flows** — packets bypass the kernel
- Daemon reads stale conntrack data → persona decisions become wrong
- **Required (FIRST THING ON DEVICE):** Check flow_offloading status. Either:
  - Disable it and measure the CPU cost of doing so, OR
  - Prove conntrack still updates with offloading (unlikely), OR
  - Document as a known limitation in thesis
- This must be resolved before ANY benchmarking begins

**3. Conntrack Table Scale**
- QEMU: 5-10 controlled flows. Real network: hundreds to thousands of entries
- Parsing `/proc/net/nf_conntrack` scales linearly with entry count
- At 1000+ entries, 2Hz cycle may not complete in time on A53
- **Required:** Measure sense cycle duration vs conntrack table size. Plot the curve. Find the ceiling.

**4. CAKE Configuration Must Match Real Link**
- QEMU CAKE params are meaningless on real hardware
- Must configure: actual WAN bandwidth, real RTT, correct overhead (PPPoE? VDSL? Fiber?)
- If ISP shapes upstream, CAKE fights a two-shaper problem
- **Required:** Link characterization step — measure actual WAN bandwidth, RTT to ISP gateway, determine encapsulation type

**5. Non-Deterministic Timing**
- Cortex-A53 may thermal throttle under sustained load (enclosed router, no fan)
- WiFi interrupts are bursty and high-priority — preempt the daemon
- 2Hz cycle might actually run at 1.5Hz or 2.5Hz under load
- **Required:** Log actual cycle timestamps in benchmarks. Report real cycle rate, not assumed 2Hz.

### Priority Order for Real Device

1. **Flow offloading check** (showstopper gate — must resolve first)
2. **Link characterization** (CAKE config depends on this)
3. **CPU overhead baseline** (proves lightweight claim)
4. **Conntrack scale test** (validates 2Hz feasibility)
5. **Timing validation** (confirms cycle rate under load)

## What If Scenarios — Contingency Plans (Elicitation)

### Contingency 1: Flow Offloading Can't Be Disabled Without Killing Performance
- **First response:** Reduce daemon frequency (2Hz → 1Hz → 0.5Hz) until CPU acceptable. Measure detection delay cost at each level.
- **Fallback:** Selective offloading — classify new flows in software, offload only after persona decided (nftables flowtable rules).
- **Thesis framing:** Trade-off curve: daemon frequency vs CPU overhead vs detection accuracy. This IS a valid contribution.

### Contingency 2: Persona Detection Accuracy <70% on Real Traffic
- **First response:** Retune decision tree thresholds on real app conntrack profiles. Run each app solo, capture profiles, adjust.
- **Fallback:** Collapse to 4 reliable personas (INTERACTIVE, MEDIA, BULK, TORRENT) if 6 can't be reliably separated.
- **Thesis framing:** "6 personas in controlled environments, 4 reliably detected in the wild" — honest finding, not a failure.

### Contingency 3: Static DSCP Rules Match MycoFlow Performance
- **First response:** Run adaptation test (traffic change mid-test) + encrypted traffic test (QUIC/HTTP3 — everything UDP 443).
- **Fallback:** New-device test (unknown device joins, static rules have no entry) + zero-config argument.
- **Thesis framing:** Dynamic classification > static when traffic patterns change or encryption hides ports. Script these as explicit scenarios.

### Contingency 4: 256MB RAM Fills Up (OOM)
- **First response:** Set `nf_conntrack_max` to safe limit (e.g., 4096). Standard OpenWrt practice. Measure daemon RSS at various conntrack sizes.
- **Fallback:** Add safety check in daemon — if <20MB free, reduce sense frequency. Graceful degradation.
- **Thesis framing:** Document operational ceiling: "Stable up to N concurrent connections on 256MB device."

### Contingency 5: Real Latency Results Worse Than QEMU
- **First response:** Separate metrics — "daemon overhead" (cycle timing) vs "end-to-end latency" (ping). Different claims, different numbers.
- **Fallback:** Focus on relative improvement ratio (e.g., FIFO +50ms → MycoFlow +3ms = 94% reduction).
- **Thesis framing:** QEMU vs Real Device comparison table with explanatory notes. QEMU = isolation test, real device = system test.

### Benchmark Scenario Additions from What-Ifs
- **Adaptation test:** Change traffic patterns mid-benchmark → proves dynamic > static
- **Encrypted traffic test:** QUIC/HTTP3 (all UDP 443) → proves behavior-based > port-based classification
- **New device test:** Unknown device joins mid-test → proves zero-config value
- **Frequency sweep:** Run daemon at 0.5Hz, 1Hz, 2Hz → plot accuracy vs CPU overhead curve

## Comparative Analysis Matrix — Thesis Metrics Framework (Elicitation)

### 4 Benchmark Scenarios
1. **FIFO** — worst case baseline (no shaping)
2. **CAKE-only** — shaping alone (no DSCP marking)
3. **CAKE + Static DSCP** — manual iptables rules (what a sysadmin could do)
4. **MycoFlow** — CAKE + daemon (automatic classification)

### Metric Groups

**Group 1: Latency & Jitter (Weight: 30%)**
| Metric | Method | Unit | Success |
|--------|--------|------|---------|
| RTT under load (interactive) | `ping -c 100 -i 0.1` wired gaming client, background load | ms mean±stddev | MycoFlow ≤ Static ≤ CAKE << FIFO |
| RTT under load (bulk) | Same from bulk client | ms | Bulk RTT > interactive = correct prioritization |
| Jitter (interactive) | stddev of RTT samples | ms | MycoFlow lowest |
| P99 latency | 99th percentile from ping | ms | No spikes for prioritized traffic |
| CAKE tin separation | `tc -s qdisc show` per-tin delay | ms per tin | Voice << Bulk (target: >100× ratio) |

**Group 2: Persona Classification Accuracy (Weight: 25%)**
| Metric | Method | Unit | Success |
|--------|--------|------|---------|
| Detection accuracy (iperf3) | Ground truth vs daemon log, controlled traffic | % per persona | ≥90% all 6 personas |
| Detection accuracy (real apps) | Ground truth vs daemon log, Zoom/YouTube/Steam/torrent | % per persona | ≥70% for 4+ personas |
| Confusion matrix | Actual vs classified cross-tab | matrix | Minimal off-diagonal |
| Detection latency | Flow start → correct persona | seconds | ≤3s |
| False positive rate | Noise flows (mDNS, NTP) misclassified | % | <10% |

**Group 3: Adaptation (Weight: 20%)**
| Metric | Method | Unit | Success |
|--------|--------|------|---------|
| Reclassification time | Gamer stops → torrents. Time until DSCP change. | seconds | MycoFlow <5s, Static: never |
| New device classification | Unknown device joins → first persona | seconds | <5s |
| Encrypted traffic handling | QUIC/HTTP3: distinguish video vs bulk | correct/incorrect | MycoFlow correct, static fails |

**Group 4: System Overhead (Weight: 15%)**
| Metric | Method | Unit | Success |
|--------|--------|------|---------|
| CPU overhead | `/proc/stat` delta: with vs without daemon | % total CPU | <5% additional |
| Memory footprint | `ps -o rss` mycoflowd | KB | <512 KB RSS |
| Sense cycle duration | Daemon log: time per cycle | ms | <100ms (fits 500ms at 2Hz) |
| Actual cycle rate | Daemon log: inter-cycle timestamps | Hz | ±20% of target |
| Conntrack scaling | Cycle duration at 100/500/1000/2000 entries | ms vs count | Linear, <100ms at max |

**Group 5: Throughput Impact (Weight: 10%)**
| Metric | Method | Unit | Success |
|--------|--------|------|---------|
| Max throughput (no daemon) | iperf3 TCP single stream, CAKE-only | Mbps | Baseline |
| Max throughput (with daemon) | iperf3 TCP single stream, MycoFlow | Mbps | Within 2% of baseline |
| Multi-flow fairness | 5 simultaneous iperf3, per-flow BW | Mbps/flow | Fair distribution per CAKE tins |

### Thesis Results Table Template
```
Table X: Real-Device Benchmark Results (Xiaomi AX3000T, N=5 runs, mean±stddev)

                          FIFO    CAKE    CAKE+Static   MycoFlow
─────────────────────────────────────────────────────────────────
RTT interactive (ms)      ±       ±       ±             ±
RTT bulk (ms)             ±       ±       ±             ±
Jitter interactive (ms)   ±       ±       ±             ±
P99 latency (ms)          ±       ±       ±             ±
Tin separation ratio      N/A     X:1     X:1           X:1
Persona accuracy (lab)    N/A     N/A     N/A           %
Persona accuracy (real)   N/A     N/A     N/A           %
Reclassification time     N/A     N/A     never         s
CPU overhead (%)          0       +X%     +X%           +X%
Memory (KB)               0       0       0             X
Throughput impact (%)     base    -X%     -X%           -X%
```

### Grade Scale
| Grade | Meaning |
|-------|---------|
| A+ | Excellent — matches or exceeds QEMU results |
| A | Very good — minor degradation, still strong |
| B | Acceptable — noticeable trade-offs but viable |
| C | Marginal — works with significant limitations |
| F | Fail — unacceptable for thesis claim |

## Requirements

**Flow Structure:**
- Pattern: Linear with gates + loop on benchmark/fix cycle
- Phases: Pre-flight/Deploy → Link Characterization → Device Capacity Baseline → Benchmark Scenarios → Results Analysis → Fix & Re-run (loop to Benchmarks) → Final Report
- Estimated steps: 7-8 step files
- Gates: Flow offloading resolution before any benchmarking

**User Interaction:**
- Style: Mixed — autonomous for scripted benchmarks, collaborative for fix decisions
- Decision points: Flow offloading resolution, fix-or-accept after each benchmark round, final report approval
- Checkpoint frequency: After each phase completion

**Inputs Required:**
- Required: Cross-compiled mycoflowd binary (aarch64), AX3000T with OpenWrt installed, wired client device(s)
- Optional: WiFi client devices (for separate WiFi scenario), real applications (Zoom, YouTube, Steam, torrent client)
- Prerequisites: SSH access to AX3000T, benchmark scripts in `scripts/`

**Output Specifications:**
- Type: Document
- Format: Structured — environment snapshot + metrics tables + bug/fix log
- Sections: Environment Snapshot, Link Characteristics, Device Capacity, Benchmark Results (per scenario), Persona Accuracy, Adaptation Tests, Bug/Fix Log, QEMU vs Real Comparison, Thesis Table
- Frequency: One report per sprint run

**Success Criteria:**
- All 5 metric groups populated with data
- N≥5 runs per scenario with mean±stddev
- Flow offloading impact resolved and documented
- Thesis results table filled and ready for mycelium_report.tex
- Any discovered bugs logged with fix status

**Instruction Style:**
- Overall: Prescriptive — clear checklists, no ambiguity on real device
- Notes: Each step should have explicit commands to run, expected outputs, and go/no-go criteria

## Workflow Structure Preview

**Phase 1: Pre-flight & Deploy**
- SSH into AX3000T, verify OpenWrt version
- Check kernel modules (conntrack, IFB, tc, CAKE)
- Check flow_offloading status ← SHOWSTOPPER GATE
- Deploy mycoflowd binary, verify it runs
- Environment snapshot (opkg list, kernel, etc.)

**Phase 2: Link Characterization**
- Measure actual WAN bandwidth (up/down)
- Measure RTT to ISP gateway
- Determine encapsulation type (PPPoE/VDSL/Fiber)
- Configure CAKE with real link parameters

**Phase 3: Device Capacity Baseline**
- CPU/RAM baseline without daemon
- CPU/RAM with daemon running (idle)
- Conntrack scale test: cycle duration vs table size
- Daemon frequency sweep (0.5Hz/1Hz/2Hz vs CPU)

**Phase 4: Benchmark Scenarios (LOOP)**
- Run 4 scenarios × N≥5 runs each: FIFO → CAKE → CAKE+Static DSCP → MycoFlow
- Per run: latency, jitter, P99, tin stats, throughput
- Controlled iperf3 tests + real application tests
- Adaptation test, encrypted traffic test, new device test

**Phase 5: Results Analysis**
- Populate thesis metrics table
- Generate confusion matrix for persona accuracy
- Compare with QEMU results
- Grade each metric group

**Phase 6: Fix & Iterate (LOOP back to Phase 4)**
- Log bugs discovered
- Fix code, recompile, redeploy
- Re-run affected benchmarks
- Update results

**Phase 7: Final Report**
- Complete thesis results table
- QEMU vs Real Device comparison
- Document limitations and operational ceiling
- Export for mycelium_report.tex

## Tools Configuration

**Core BMAD Tools:**
- **Party Mode:** excluded — hands-on hardware work, not brainstorming
- **Advanced Elicitation:** excluded — thorough elicitation already done in planning
- **Brainstorming:** excluded — same

**LLM Features:**
- **Web-Browsing:** excluded — all info is local
- **File I/O:** included — writing benchmark results, reading device output, generating report
- **Sub-Agents:** excluded — single operator, sequential work
- **Sub-Processes:** excluded — same

**Memory:**
- Type: continuable
- Tracking: stepsCompleted array, lastStep, lastContinued in output frontmatter
- Sidecar file for session state

**External Integrations:** None
**Installation Requirements:** None

## Workflow Structure Design

### File Structure
```
real-device-sprint/
├── workflow.md                    (entry point)
├── data/
│   └── metrics-template.md        (thesis table + metric definitions)
├── templates/
│   └── sprint-report.template.md  (output document template)
└── steps-c/
    ├── step-01-init.md            (Init - continuable)
    ├── step-01b-continue.md       (Continuation - resume)
    ├── step-02-preflight.md       (Pre-flight & Deploy) ← SHOWSTOPPER GATE
    ├── step-03-link-char.md       (Link Characterization)
    ├── step-04-capacity.md        (Device Capacity Baseline)
    ├── step-05-benchmark.md       (Benchmark Scenarios)
    ├── step-06-analysis.md        (Results Analysis)
    ├── step-07-fix-iterate.md     (Bug Log + Fix → branch)
    └── step-08-final-report.md    (Final Report - thesis export)
```

### Step Design

**step-01-init** (Init Continuable, Auto-proceed)
- Welcome, explain sprint workflow
- Check for existing sprint report → route to 01b if found
- Create output document from template
- Proceed to step-02

**step-01b-continue** (Continuation, Auto-proceed)
- Read stepsCompleted from output
- Welcome user back, show progress
- Route to last incomplete step

**step-02-preflight** (Middle Simple, C only)
- SSH into AX3000T, verify OpenWrt version
- Check kernel modules: conntrack, IFB, tc, CAKE
- **GATE:** Check flow_offloading status — must resolve before proceeding
- Deploy mycoflowd binary, verify execution
- Environment snapshot: opkg list-installed, kernel version, uname
- Record all results in output document

**step-03-link-char** (Middle Simple, C only)
- Measure actual WAN bandwidth (up/down) with iperf3 or speedtest
- Measure RTT to ISP gateway
- Determine encapsulation type (PPPoE/VDSL/Fiber/Ethernet)
- Configure CAKE with real link parameters
- Record: bandwidth, RTT, overhead settings, tc qdisc show output

**step-04-capacity** (Middle Simple, C only)
- CPU/RAM baseline without daemon running
- CPU/RAM with daemon running (idle, no traffic)
- Conntrack scale test: sense cycle duration at 100/500/1000/2000 entries
- Daemon frequency sweep: 0.5Hz/1Hz/2Hz vs CPU overhead
- Record all metrics in output document

**step-05-benchmark** (Middle Standard, A/P/C)
- Run 4 scenarios × N≥5 runs:
  1. FIFO (no shaping)
  2. CAKE-only (no DSCP)
  3. CAKE + static DSCP rules
  4. MycoFlow (CAKE + daemon)
- Per run: latency, jitter, P99, tin stats, throughput
- Controlled iperf3 tests
- Real application tests (Zoom, YouTube, Steam, torrent)
- Adaptation test: change traffic mid-test
- Encrypted traffic test: QUIC/HTTP3
- New device test: unknown device joins
- Record all raw data in output document

**step-06-analysis** (Middle Standard, A/P/C)
- Populate thesis metrics table (mean±stddev)
- Generate confusion matrix for persona accuracy
- Compare with QEMU benchmark results
- Grade each metric group (A+/A/B/C/F)
- QEMU vs Real Device comparison table
- Identify issues/bugs discovered

**step-07-fix-iterate** (Branch, Custom: R/F)
- Log all bugs discovered with severity
- Document fixes applied to codebase
- Recompile, redeploy if needed
- Menu: [R] Re-run benchmarks → back to step-05, [F] Finalize → step-08

**step-08-final-report** (Final, No menu)
- Complete thesis results table
- QEMU vs Real Device comparison with notes
- Document limitations and operational ceiling
- Format for mycelium_report.tex export
- Mark workflow complete

### Interaction Patterns
- Steps 02-04: C only — prescriptive checklists, no creative exploration
- Steps 05-06: A/P/C — may want deeper analysis or alternatives
- Step 07: Custom branch — R (re-run) or F (finalize)
- Step 08: Final — no menu, completion

### Data Flow
- Output document created in step-01, appended progressively through each step
- stepsCompleted updated in frontmatter after each step
- Metrics template loaded in step-05 for consistent data capture
- Step-07 loops back to step-05 with updated code/binary

### AI Role
- Embedded systems benchmark specialist
- Prescriptive and checklist-driven
- Provides exact commands to run on device
- Reports expected vs actual output for go/no-go decisions
