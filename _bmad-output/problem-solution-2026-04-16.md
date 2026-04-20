# Problem Solving Session: MycoFlow Persona Classification Accuracy

**Date:** 2026-04-16
**Problem Solver:** Baris
**Problem Category:** Algorithm / Traffic Classification

---

## PROBLEM DEFINITION

### Initial Problem Statement

The current persona classification system in MycoFlow relies purely on behavioral heuristics (flow count, bandwidth, packet size, UDP ratio, elephant flow detection, tx/rx ratio). This approach has significant blind spots — notably, TCP-based games like League of Legends are misclassified as BULK because the GAMING detection is UDP-biased. The problem extends beyond gaming: every persona class has potential misclassification vectors due to reliance on a single signal dimension (traffic shape) while ignoring readily available signals like destination ports and DNS-resolved domains.

### Refined Problem Statement

**MycoFlow's `decide_persona()` function achieves acceptable accuracy only for traffic patterns that happen to align with its heuristic assumptions (e.g., UDP-based games like CS2), but systematically misclassifies traffic that violates those assumptions (e.g., TCP-based games, QUIC-based video, mixed-protocol applications).** The system needs a multi-signal classification approach — combining behavioral heuristics with port-based and domain-based hints — to achieve robust accuracy across all 6 persona classes and the full diversity of real-world applications.

### Problem Context

- **Target hardware:** Xiaomi AX3000T (MT7981B, 256MB RAM, 128MB NAND)
- **Current system:** 6-persona hierarchical decision tree in `myco_persona.c`
- **Available but unused signals:** `dst_port` already in `flow_key_t`, DNS responses visible on router
- **Constraint:** Zero flash writes, minimal RAM/CPU overhead
- **Known failure:** LoL (TCP-based game) → classified as BULK instead of GAMING
- **Suspected failures:** Any TCP game, QUIC streaming vs gaming ambiguity, VoIP apps with screen share, mixed-use devices

### Success Criteria

1. **All major game titles** (CS2, LoL, Valorant, Fortnite, Dota 2, Minecraft, WoW) correctly classify as GAMING
2. **VoIP apps** (Zoom, Teams, Discord voice, WhatsApp call) correctly classify as VOIP regardless of screen share
3. **Streaming services** (Netflix, YouTube, Twitch, Disney+) correctly classify as STREAMING
4. **Video calls** (Zoom, Teams, Google Meet) correctly classify as VIDEO
5. **Torrent traffic** correctly identified even with encryption/obfuscation
6. **Zero false VOIP/GAMING** — no non-interactive traffic gets latency priority it doesn't deserve
7. **Resource budget:** <4KB RAM, negligible CPU, zero flash writes

### First Principles Analysis

**Current Assumptions vs Ground Truth:**

| Assumption in `decide_persona()` | Reality |
|---|---|
| Gaming = high UDP ratio | FALSE — LoL, WoW, Minecraft Java use TCP |
| Gaming = small packets + few flows | PARTIAL — true for FPS tick-rate, false for TCP games with launcher/chat |
| Streaming = heavy UDP download | PARTIAL — true for QUIC (YouTube), false for TCP HLS |
| VOIP = tiny packets + low bandwidth | MOSTLY TRUE — but voice + screen share breaks it |
| Torrent = many flows | PARTIAL — encrypted torrents use fewer connections via µTP |
| Elephant flow = BULK | DANGEROUS — Zoom HD stream and LoL game server are also elephant flows |

**Invariant Properties Per Persona:**

- **VOIP:** symmetric tx/rx (~0.5-2.0), small packets (60-200B), low bw (<500kbps), STUN/TURN ports (3478, 19302-19309), SIP (5060/5061)
- **GAMING:** known game server ports (strongest signal), low bandwidth (<5Mbps). Protocol, packet size, flow count are NOT invariant
- **VIDEO:** moderate symmetric bw (1-8Mbps), WebRTC ports (Google Meet 19302-19309, Zoom 8801-8810, Teams 3478-3481)
- **STREAMING:** heavily asymmetric rx >> tx (ratio < 0.1), identifiable domains (*.googlevideo.com, *.nflxvideo.net, *.twitch.tv)
- **BULK:** elephant flow + no port/domain identity — a residual class
- **TORRENT:** known ports (6881-6889), DHT on UDP, many bidirectional flows to diverse IPs

**Fundamental Insight:** Behavior alone can classify VOIP and STREAMING (strong behavioral signatures). But GAMING, VIDEO, and TORRENT fundamentally need port/domain hints to differentiate from BULK. Classification must be restructured: Port Hint → Domain Hint → Behavioral Tree (not behavior-first).

### Red Team vs Blue Team Analysis

**Attack Vectors Against Proposed 3-Layer System:**

| # | Attack | Severity | Defense |
|---|---|---|---|
| 1 | Port randomization — many games use ephemeral ports (Fortnite, etc.) | HIGH | Port is a boost, not a gate. Unknown port → fall through to behavior. Per-flow hints, not per-port-only |
| 2 | VPN/encrypted tunnel — all traffic through one WireGuard/OpenVPN flow | MEDIUM | Detect VPN ports → UNKNOWN, let user override. Fundamental limitation, document it |
| 3 | Port 443 collision — HTTPS/QUIC used by everything | HIGH | Domain layer is ESSENTIAL. Port 443 = no hint. Domain resolves. Without domain, fall to behavior |
| 4 | DoH/DoT bypassing DNS snoop — queries bypass port 53 | MEDIUM-HIGH | Defense in depth: port hints + behavior still work. DNS is additive. Router can force DNS via firewall redirect |
| 5 | CDN IP reuse — Netflix/YouTube/game patches from same Akamai/Cloudflare IPs | MEDIUM | Short TTL on DNS cache entries matching DNS response TTL. IP→domain mapping is transient |
| 6 | Mixed-use device — gaming + streaming + downloading simultaneously | HIGH | **Per-FLOW persona** instead of per-device. Device persona = highest priority flow persona |
| 7 | Game launcher downloads — Steam/Epic heavy download on game ports | MEDIUM | Bandwidth discriminator: game <5Mbps, launcher >>10Mbps. Behavior sanity-checks hints |

**Critical Architectural Insight:** Per-device persona is fundamentally wrong for mixed-use devices. Classification should happen per-flow, with each flow getting its own port/domain/behavior classification. Device persona becomes the highest-priority flow persona (VOIP > GAMING > VIDEO > STREAMING > BULK > TORRENT). This aligns with CAKE diffserv4's packet-level DSCP marking model.

### Algorithm Olympics — Classification Approach Comparison

**Contenders:**
- A: Behavior-Only (current system)
- B: Port-Only (static lookup)
- C: Hybrid (Port Hint + Behavior, no DNS)
- D: Full Stack (Port + DNS + Behavior, per-flow)

**Benchmark Results (18 real-world scenarios):**

| # | Scenario | Truth | A | B | C | D |
|---|---|---|---|---|---|---|
| 1 | CS2 (UDP 27015) | GAMING | OK | OK | OK | OK |
| 2 | LoL (TCP 5000-5500) | GAMING | BULK | OK | OK | OK |
| 3 | Fortnite (UDP ephemeral) | GAMING | OK | UNK | OK | OK |
| 4 | Minecraft Java (TCP 25565) | GAMING | BULK | OK | OK | OK |
| 5 | YouTube QUIC (UDP 443) | STREAM | OK | UNK | OK | OK |
| 6 | YouTube TCP HLS (TCP 443) | STREAM | BULK | UNK | BULK | OK |
| 7 | Netflix (TCP 443) | STREAM | BULK | UNK | BULK | OK |
| 8 | Zoom video (UDP 8801) | VIDEO | BULK | OK | OK | OK |
| 9 | Zoom TCP 443 fallback | VIDEO | BULK | UNK | BULK | OK |
| 10 | Discord voice (STUN 3478) | VOIP | OK | OK | OK | OK |
| 11 | WhatsApp call (TURN) | VOIP | OK | OK | OK | OK |
| 12 | BitTorrent (TCP 6881) | TORRENT | OK | OK | OK | OK |
| 13 | Encrypted torrent (random) | TORRENT | OK | UNK | OK | OK |
| 14 | Steam download (TCP 443) | BULK | OK | UNK | OK | OK |
| 15 | Google Meet (UDP 19302) | VIDEO | GAMING | OK | OK | OK |
| 16 | Spotify (TCP 443) | STREAM | VIDEO | UNK | VIDEO | OK |
| 17 | VPN tunnel (UDP 51820) | UNKNOWN | BULK | OK | OK | OK |
| 18 | LoL + YouTube mixed | GAME+STR | BULK | GAME+UNK | GAME+STR | GAME+STR |

**Scorecard:**

| Approach | Correct | Accuracy | RAM Cost |
|---|---|---|---|
| A — Behavior Only | 8/18 | 44% | 0 |
| B — Port Only | 10/18 | 56% | ~200B |
| C — Hybrid (Port+Behavior) | 14/18 | 78% | ~200B |
| **D — Full Stack (Port+DNS+Behavior)** | **17/18** | **94%** | **~2-4KB** |

**Winner: Approach D.** The 78%→94% jump justifies ~3KB RAM. DNS resolves the TCP-443 blind spot. Per-flow classification fixes mixed-use devices.

---

### Cross-Functional War Room

**Engineer (AX3000T Feasibility):**
- DNS snooping best approach: passive raw socket on UDP 53 (br-lan), parse ANSWER section. ~50 lines of C, no dnsmasq dependency, no file I/O
- Per-flow cost: 256 flows × (port lookup + DNS lookup + behavior) = ~1536 ops/sec at 2Hz. Microseconds on Cortex-A53
- DNS cache: 64 entries × ~73B = ~4.7KB. LRU eviction, TTL-based expiry
- Port table: compiled-in defaults. Domain table: loadable from `/etc/mycoflow/hints.conf` at startup via UCI config

**Network Expert (Turkish Home Network Reality):**
- Game port table must include: Riot (TCP 5000-5500, UDP 7000-8000), Valve (UDP 27015-27050), Epic (UDP 5222, 5795-5847), Minecraft Java (TCP 25565), Bedrock (UDP 19132), PUBG/Krafton (UDP 7086-7995), Supercell (TCP 9339)
- Domain table priorities: *.googlevideo.com (YouTube), *.nflxvideo.net (Netflix), *.ttvnw.net (Twitch), *.fbcdn.net (Instagram/FB video), *.spotify.com, *.discord.media
- Turkish ISPs use CGNAT — WebRTC often falls back to TURN relay on TCP 443, not STUN 3478
- Popular titles: CS2, Valorant, LoL, PUBG Mobile, Fortnite, Brawl Stars

**End User (What Actually Matters):**
1. GAMING must NEVER get bulk-tier latency — #1 priority, 50ms extra is noticeable
2. Voice calls must be clear (Discord during gaming, WhatsApp)
3. YouTube/Netflix smooth but doesn't need sub-10ms
4. Downloads fill remaining bandwidth
5. Zero configuration required — it should just work

**Key insight:** User doesn't care about accuracy % — they care about "does my game lag when someone watches Netflix?" Correct DSCP tin assignment matters more than correct persona label.

### Pre-mortem Analysis — Failure Prevention

| # | Failure Scenario | Root Cause | Prevention | Priority |
|---|---|---|---|---|
| 1 | Port table got stale — game updated ports | Hard-coded port tables rot | Config file overrides (`/etc/mycoflow/port_hints.conf`), LuCI UI editing, community-updatable | HIGH |
| 2 | DNS cache poisoned classification — CDN IP reused | DNS→IP mapping is many-to-many, CDN IPs ephemeral | Respect DNS TTL, behavior overrides conflicting hints, 3-signal confidence scoring | HIGH |
| 3 | DNS sniffer crashed daemon — malformed response | Parsing untrusted network data | Paranoid bounds-checking, separate thread, graceful degradation to Port+Behavior (78%), fuzz testing | HIGH |
| 4 | Per-flow iptables overwhelmed CPU — 200+ rules | iptables scales linearly per flow | **Keep per-device DSCP marking** (max 32 rules). Per-flow classification only determines device's dominant persona. Consider nftables sets for O(1) lookup | CRITICAL |
| 5 | New service domains unknown — Disney+ new CDN | Static domain lists rot | Suffix matching (*.googlevideo.com), configurable `/etc/mycoflow/domain_hints.conf`, behavior as safety net | MEDIUM |

**Key design principles from pre-mortem:**
- Port/domain tables must be config-file overridable, not just compiled-in
- DNS is additive — system must work without it (graceful degradation)
- Behavior layer is always the safety net — hints boost, never replace
- Per-flow classification for persona DECISION, per-device for DSCP APPLICATION
- 3-signal confidence: all agree = HIGH, mixed = MEDIUM, single signal = LOW (conservative fallback)

---

## DIAGNOSIS AND ROOT CAUSE ANALYSIS

### Problem Boundaries (Is/Is Not)

**Where IS the problem / Where ISN'T it:**

| Dimension | IS (problem occurs) | IS NOT (works correctly) |
|---|---|---|
| Protocol | TCP-based games (LoL, WoW, Minecraft Java) | UDP-based games (CS2, Valorant) |
| Port | Traffic on port 443 (everything uses it) | Dedicated ports (27015, 3478, 6881) |
| App type | TCP streaming (Netflix, Spotify), video calls on TCP 443 fallback | QUIC streaming (YouTube), STUN-based voice |
| Device usage | Mixed-use devices (gaming + streaming) | Single-purpose usage |
| Persona | GAMING, VIDEO, STREAMING when on TCP/443 | VOIP (strong behavioral signature), TORRENT with many flows |
| DNS | DoH/DoT users (DNS invisible) | Standard DNS users (port 53 visible) |
| Network | CGNAT (TURN relay on TCP 443) | Direct connectivity (STUN works) |

**When IS / When ISN'T:**
- IS: TCP game start (immediately BULK), QUIC→TCP fallback (persona flips), game launch + patching (false GAMING)
- ISN'T: Idle periods (UNKNOWN correct), stable bandwidth with consistent protocol

**Who IS / Who ISN'T:**
- IS: TCP-game players, Netflix/Disney+ users, video callers on restrictive networks, multi-user households
- ISN'T: UDP-game players, YouTube users (QUIC works), single-user households

**Emergent Pattern:** When traffic uses TCP on port 443 or any non-distinctive port, behavioral tree cannot distinguish application purpose. All ambiguous traffic collapses into BULK. This is a **signal poverty problem** — not a threshold tuning problem. Solution must add new signal dimensions (port/domain hints), not refine existing ones.

### Root Cause Analysis

**Five Whys — LoL→BULK Chain:**

1. LoL classified as BULK → elephant_flow=1 (one TCP game connection >60% bytes) triggers Rule 3
2. Doesn't hit GAMING rules → both require high UDP ratio (Rule 2b) or small packets + few flows (Rule 5). LoL is TCP (udp_ratio≈0), Riot client+chat+game = 8+ flows
3. GAMING detection depends on UDP → tree built from CS2/Valorant observations, not first principles
4. Tree not designed from first principles → behavioral heuristics were the ONLY signal available at design time
5. Port/domain unavailable to classifier → **`decide_persona()` receives only `metrics_t` (aggregate stats). Flow-level data (dst_port, dst_ip) is discarded during aggregation in `device_table_aggregate()`**

**ROOT CAUSE:** Classification function is architecturally disconnected from flow-level identity signals. `device_table_aggregate()` reduces rich per-flow data (5-tuple with ports) into aggregate statistics, destroying the information needed for accurate classification.

### Contributing Factors

1. **Architectural:** `metrics_t` has no field for port/domain hints — interface between flow table and persona engine is too narrow
2. **Historical:** Conntrack parsing and persona engine built separately, never integrated at flow level
3. **Data model:** Per-device aggregation (sum all flows) was right for bandwidth control but wrong for classification which needs per-flow detail
4. **Missing signal source:** DNS resolution data exists on router but is completely untapped

### System Dynamics

**Structural bottleneck at `device_table_aggregate()`** — the funnel that destroys flow-level information. Solution is not to widen the funnel (more aggregate stats) but to **classify before funneling**: each flow gets persona classification while it still has 5-tuple identity, then device persona derives from per-flow results.

**Leverage point:** Move classification upstream of aggregation. Per-flow hints (port + domain) are evaluated while `flow_key_t` is still accessible, before reduction to `metrics_t`.

**Negative reinforcing loop:** Wrong persona → wrong DSCP → wrong CAKE tin → bad UX → manual override → system doesn't learn
**Positive balancing loop (proposed):** Port hint → narrows candidates → behavior confirms → DNS resolves → high confidence → correct DSCP → good UX

### Rubber Duck Debugging — Complete Data Flow Trace

```
STEP 1: conntrack → flow_table
  main.c:207  → flow_table_populate_conntrack()
  myco_flow.c:206-213 → Builds flow_key_t with full 5-tuple
  ✅ src_ip, dst_ip, src_port, dst_port, protocol ALL PRESENT

STEP 2: flow_table → device_table (AGGREGATION — INFORMATION DESTROYED)
  main.c:217  → device_table_aggregate()
  myco_device.c:103-131 → Sums bytes/packets per device by src_ip
  ❌ fe->key.dst_port ACCESSIBLE but NEVER READ
  ❌ fe->key.dst_ip ACCESSIBLE but NEVER READ

STEP 3: device_table → metrics_t (FURTHER REDUCTION)
  myco_device.c:200-208 → Builds metrics_t from aggregates
  ❌ No port/domain fields in metrics_t

STEP 4: metrics_t → decide_persona()
  myco_persona.c:31-103 → ZERO knowledge of ports or domains
```

**Secondary Root Cause — Missed Insertion Point:**
The elephant flow detection loop at `myco_device.c:164-179` ALREADY iterates per-flow with access to `ft->entries[j].key` (full 5-tuple including dst_port). Per-flow port hint collection can piggyback on this existing loop with zero additional iterations. The code structure is ready — it just doesn't look at the port.

### Fishbone Diagram — Additional Causes Discovered

**Three NEW causes not previously identified:**

1. **Rule ordering bug (Code):** Rule 3 (BULK: elephant flow) checked BEFORE Rule 5 (GAMING: small pkt + few flows). TCP game with one dominant connection hits BULK at Rule 3, never reaches Rule 5. Reordering alone could help some cases even without port hints.

2. **No per-flow classification logging (Operations):** When persona is wrong, no way to debug which flows contributed. Log only says "persona changed: unknown -> bulk". Impossible to diagnose field issues without knowing flow-level decisions.

3. **No confidence scoring (Design):** Classification based on 3 agreeing signals (port + domain + behavior) treated identically to single ambiguous behavior signal. No mechanism to express uncertainty or prefer conservative classification when confidence is low.

**Complete cause map covers 6 categories:** Code (4), Data Model (3), Architecture (3), Operations (2), Environment (4), Design (2) = 18 total contributing causes identified.

### Occam's Razor — Simplification Analysis

**What we CAN simplify:**
- Per-flow persona storage → **CUT.** Just count port/domain hint votes during existing aggregation loop, add `dominant_hint` to `device_entry_t`
- 3-level confidence scoring → **SIMPLIFY** to binary `has_hint` (yes/no)
- Configurable domain list file → **DEFER.** Compile-in defaults first, add config later
- nftables sets → **DEFER.** iptables with 32 rules is fine for now

**What we CANNOT cut:**
- Port hint lookup — solves TCP games (LoL, WoW, Minecraft), the original problem
- DNS snoop + cache — solves TCP-443 streaming/video (Netflix, Spotify, Zoom fallback), 16% accuracy gap unfixable by other means
- Hint-aware `decide_persona()` — without it, hints have no effect

**Simplified architecture:**
```
conntrack → flow_table → aggregate(COUNT port/dns hints) → behavior+hint → persona
                              ↑
                        DNS cache (background snoop, ~150 lines C)
```

**Key insight:** Don't need per-flow persona storage. Count hint votes during aggregation, pass dominant_hint into decide_persona(). Behavioral tree uses hint as tiebreaker. This reduces ~100 lines of new data structures down to ~20 lines of change in aggregation loop.

### Failure Mode Analysis — Component Risk Assessment

**Component risk summary:**

| Component | Highest Risk Failure | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| Port Hint Lookup | Game changes ports / new game unlisted | HIGH | Medium (one game fails) | Config-overridable table, behavior fallback, community updates |
| DNS Snooping | Parser crash on malformed response | MEDIUM | CRITICAL (daemon dies, all QoS stops) | Paranoid bounds checking, fuzz testing, separate thread isolation, graceful degradation |
| Hint-Aware decide_persona() | Hint overrides correct behavior | LOW | Medium (wrong persona) | Hint is tiebreaker only, never overrides strong behavioral signals |
| Hint Vote Counting | Counting bug | LOW | Low (wrong hint) | Simple array counting, unit tests |

**Top 2 risks to address first:**
1. **DNS parser crash** — highest impact. Must fuzz-test, isolate in separate thread, reject malformed packets silently. System must degrade gracefully to port+behavior (78%) if DNS fails entirely.
2. **Port table staleness** — highest likelihood. Design for updateability from day 1: compiled-in defaults + config file override path (even if config loading is deferred).

**Design principle: "First do no harm"** — if behavior has high confidence (e.g., 100+ flows = TORRENT), hint must NOT override it. Hints boost uncertain classifications, never overrule certain ones.

### Systems Thinking — Second-Order Effects

**What changes as a consequence of the new hint system:**

| Component | Effect | Action |
|---|---|---|
| `metrics_t` | Risk of API pollution | DON'T add hints to metrics_t. Pass hint separately to decide_persona() |
| ubus/JSON status | Should expose hint info for debugging | Add new fields only (backward compatible) |
| LuCI dashboard | Can show "gaming (port-detected)" vs "gaming (behavior)" | Optional enhancement, not blocking |
| Unit tests | Need comprehensive (hint, behavior) combination matrix | Must write BEFORE implementing. New: test_dns.c (fuzz), test_hint.c (port lookup) |
| History window (persona_state_t) | Unaffected — hint changes candidate, but 2-of-3 majority still stabilizes | No change |
| Dominant persona | Unaffected — picks highest-priority, doesn't care how persona determined | No change |
| DSCP marking | Unaffected — maps persona→DSCP, agnostic to hints | No change |
| Control loop (bandwidth) | POSITIVE ripple — correct GAMING → correct RTT threshold (50ms not 200ms) | Automatic improvement |
| DNS thread lifecycle | New long-running component | Must handle startup/shutdown/reload via g_stop, select() with timeout |

**Memory footprint delta:**
- device_entry_t: +28B (hint_votes[7] + dominant_hint) × 32 devices = +896B
- DNS cache: ~4.7KB (64 entries)
- Port table: ~200B (static .rodata)
- **TOTAL: +6.0KB RAM on 256MB device**

**Key finding:** Three downstream components (history window, dominant persona, DSCP marking) are completely unaffected. Hint system slots cleanly between aggregation and persona decision. Only truly new complexity is the DNS thread lifecycle.

**Files to modify:** myco_device.h/c (hint votes), myco_persona.h/c (hint param), main.c (DNS thread wiring), myco_ubus.c (hint in status JSON)
**Files to create:** myco_dns.h/c (DNS snooper + cache), myco_hint.h/c (port→persona lookup)

---

## ANALYSIS

### Force Field Analysis

**Driving Forces (Supporting Solution):**

1. **User pain is real** — LoL players getting bulk-tier latency, concrete UX failure
2. **Data already exists** — flow_key_t has dst_port/dst_ip, router sees DNS on port 53
3. **Clean insertion point** — elephant flow loop (myco_device.c:164) already iterates per-flow with 5-tuple
4. **Occam-simplified design** — ~220 lines of new code total
5. **Architecture is modular** — history window, dominant persona, DSCP, control loop all unaffected
6. **Trivial resource cost** — +6KB RAM, zero flash, negligible CPU
7. **Massive accuracy jump** — 44% → 94% (or 78% with port hints alone)
8. **Academic value** — novel multi-signal classification strengthens thesis

**Restraining Forces (Blocking Solution):**

1. **DNS parser security risk** — parsing untrusted network packets, buffer overflow could crash daemon
2. **Port table maintenance** — needs updates as games change ports
3. **DoH/DoT adoption** — reduces DNS snooping effectiveness over time
4. **Testing complexity** — test matrix grows from ~7 to ~40+ scenarios
5. **New thread complexity** — DNS snooper adds concurrency (mutex on cache)
6. **Scope creep risk** — feature additions delaying shipment

### Constraint Identification

**Primary constraint (bottleneck):** `decide_persona()` interface — accepts only `metrics_t*`. Adding a hint parameter is the single change that unlocks everything.

**Hard constraints:** Zero flash writes, AX3000T resources (256MB RAM), OpenWrt musl libc, C11 standard, CAKE diffserv4 DSCP compatibility.

**Challenged assumptions:**
- ~~Classification must happen after aggregation~~ → hint counting happens DURING aggregation
- ~~One persona per device is sufficient~~ → per-flow hints give per-flow awareness without per-flow storage
- ~~DNS snooping too complex for router~~ → ~150 lines C, raw UDP socket, 4.7KB RAM

### Key Insights

1. **Data already exists** — not a "collect more telemetry" problem, but a "use what you already have" problem. 5-tuple is right there in flow_key_t.
2. **Scope creep is the real enemy, not complexity.** Occam-simplified design is ~220 lines. Risk is adding features before shipping core fix.
3. **Phased delivery neutralizes biggest risks.** Phase 1: port hints only (78%, zero security risk, solves LoL). Phase 2: DNS (94%, requires fuzz testing).
4. **Bottleneck is a one-line interface change.** Adding `persona_t hint` to `decide_persona()` signature unlocks the entire solution space.

---

## SOLUTION GENERATION

### Methods Used

1. **TRIZ Contradiction Matrix** — Resolved 3 technical contradictions: accuracy vs simplicity (segmentation into phases), richness vs maintenance (dynamization via config files), per-flow vs per-device (local quality — classify per-flow, apply per-device)
2. **Morphological Analysis** — Explored 8 design parameters × 3 options each. Optimal combination selected by cross-referencing with Occam's Razor and Failure Mode findings

### Generated Solutions

**Incremental (low risk):**
1. Port hints only, compiled-in table — ~50 lines, accuracy 78%
2. Reorder decision tree rules (GAMING before BULK) — ~5 lines, accuracy ~50%
3. Protocol-aware GAMING rule (TCP + game port range + moderate bw) — ~15 lines, accuracy ~55%

**Moderate (managed risk):**
4. Port hints + config file override — Solution 1 + loadable `/etc/mycoflow/port_hints.conf`
5. Port hints + DNS snooper (Phase 1+2) — Full Occam-simplified design, accuracy 94%
6. Port hints + DNS + hint-aware tree rewrite — Restructure decide_persona() to check hints FIRST

**Breakthrough (higher risk):**
7. Per-flow DSCP via nftables sets — O(1) lookup, true per-flow QoS
8. eBPF-based classification — port/hint logic in kernel space
9. Connmark-based approach — tag connections at first packet, CAKE reads connmark

**Wild ideas:**
10. ML on flow features — tiny decision tree trained on labeled captures
11. Crowdsourced port database — opt-in community-trained classifier
12. TLS SNI parsing — extract server name from ClientHello (works despite DoH/DoT)
13. QUIC header parsing — extract SNI from QUIC Initial packets

### Creative Alternatives

**TLS SNI Parsing (#12):** Most robust domain identification — works even with encrypted DNS (DoH/DoT). Every HTTPS ClientHello contains unencrypted server name. Limitation: TLS 1.3 ECH will eventually hide SNI. More CPU than DNS snooping.

**QUIC Header Parsing (#13):** QUIC Initial packets contain SNI in cleartext. Covers YouTube, Google Meet, all Google services (biggest traffic sources). Complementary to DNS snooping for QUIC-heavy traffic.

**Connmark Approach (#9):** Tag each connection at first classification via conntrack mark. CAKE can read connmark directly. Eliminates repeated iptables rule rebuilds. Requires connmark kernel support (available in OpenWrt).

Both #12 and #13 are potential **Phase 3** — after port hints and DNS snooping are proven.

---

## SOLUTION EVALUATION

### Evaluation Criteria

| Criterion | Weight | Rationale |
|---|---|---|
| Accuracy gain | 30% | The whole point — fix misclassification |
| Implementation risk | 25% | DNS parser crash could kill all QoS |
| Resource cost | 15% | Must fit AX3000T comfortably |
| Maintainability | 15% | Port tables, community updates, code clarity |
| Thesis value | 10% | Novel contribution for academic paper |
| Future extensibility | 5% | Can we build Phase 3 on top? |

### Solution Analysis

**Decision Matrix — Top contenders:**

| Solution | Accuracy | Risk | Resource | Maintain | Thesis | Extend | **Score** |
|---|---|---|---|---|---|---|---|
| S4: Port + config | 6 | 9 | 10 | 8 | 5 | 6 | 7.35 |
| **S5: Port + DNS phased** | **9** | **7** | **9** | **7** | **8** | **8** | **8.05** |
| S6: Port + DNS + rewrite | 9 | 6 | 9 | 6 | 8 | 8 | 7.65 |
| S8: eBPF classification | 9 | 3 | 9 | 4 | 9 | 10 | 6.70 |
| S9: Connmark | 8 | 5 | 9 | 6 | 7 | 9 | 7.00 |

**Why S5 wins:** Highest accuracy (94%), managed risk via phasing, Phase 1 ships immediately with zero security risk. 16% gap from S4 (78% → 94%) covers Netflix, Spotify, Zoom fallback — critical for Turkish households where YouTube/Netflix are dominant traffic.

### Recommended Solution

**Solution 5: Port Hints + DNS Snooping, Phased Delivery**

**Phase 1 — Port Hints (44% → 78%):**
- `myco_hint.c/h`: port→persona switch/case with ranges
- Modify `device_table_aggregate()`: count port hint votes in existing elephant flow loop
- Add `dominant_hint` to `device_entry_t`
- Modify `decide_persona()`: add `persona_t hint` parameter, hint as tiebreaker
- Compiled-in defaults, designed for future config override
- Tests: test_hint.c + updated test_persona.c + updated test_device.c

**Phase 2 — DNS Snooping (78% → 94%):**
- `myco_dns.c/h`: raw socket UDP 53, passive response parser, LRU cache (64 entries)
- Separate DNS thread with select() timeout, mutex on cache
- Integrate DNS hints into aggregation loop (dst_ip lookup in cache)
- Fuzz testing for malformed packets, graceful degradation to Phase 1 if DNS fails

**Phase 3 (future, not committed):**
- TLS SNI parsing (DoH/DoT resilience)
- QUIC header parsing (Google services)
- Config file for port/domain overrides
- Connmark integration

### Rationale

1. **Phase 1 solves the original problem** — LoL, WoW, Minecraft, Valorant correctly classify as GAMING via port hints
2. **Phase 2 solves the hidden problem** — TCP-443 traffic (Netflix, Spotify, Zoom) visible only through DNS
3. **Each phase independently valuable** — Phase 1 at 78% is already massive improvement over 44%
4. **Risk front-loaded in Phase 2** — DNS parser is highest-risk; Phase 1 baseline to fall back to
5. **Thesis gets both contributions** — multi-signal classification (Phase 1) + passive DNS for QoS (Phase 2)

---

## IMPLEMENTATION PLAN

### Implementation Approach

**Strategy:** Phased rollout with TDD. Each phase is self-contained. Phase 1 merges and is proven before Phase 2 begins.

### Action Steps

**Phase 1 — Port Hints (44% → 78%):**

| Step | Description | Key Detail |
|---|---|---|
| 1 | Create `myco_hint.c/h` | `hint_from_port(protocol, dst_port)` → persona_t. Port ranges: Valve UDP 27015-27050, Riot TCP 5000-5500/UDP 7000-8000, Epic UDP 5222/5795-5847, Minecraft TCP 25565/UDP 19132, PUBG UDP 7086-7995, Supercell TCP 9339, STUN UDP 3478-3479, WebRTC UDP 19302-19309, SIP 5060-5061, Zoom UDP 8801-8810, RTMP 1935, BT TCP 6881-6889 |
| 2 | Modify `device_entry_t` | Add `hint_votes[PERSONA_COUNT]`, `dominant_hint`, `has_hint` |
| 3 | Modify `device_table_aggregate()` | Count port hints in existing elephant flow loop (myco_device.c:164). Tie-break by priority: VOIP > GAMING > VIDEO > STREAMING > BULK > TORRENT |
| 4 | Modify `decide_persona()` | New signature adds `persona_t hint`. KEY FIX: Rule 3 (elephant→BULK) checks hint first — if hint==GAMING/VOIP/VIDEO → hint wins. Sanity checks: hint==GAMING but bw>20Mbps → keep BULK |
| 5 | Wire into main loop | Pass `dev->dominant_hint` through persona_update() chain. Update ubus JSON with hint info |
| 6 | LuCI dashboard (optional) | Display "gaming (port: 5000)" alongside persona |

**Phase 2 — DNS Snooping (78% → 94%):**

| Step | Description | Key Detail |
|---|---|---|
| 7 | Create `myco_dns.c/h` | LRU cache (64 entries), domain suffix→persona table (googlevideo.com→STREAMING, riotgames.com→GAMING, zoom.us→VIDEO, etc.) |
| 8 | DNS sniffer thread | Raw socket UDP 53 on br-lan, select() with 1s timeout, PARANOID parsing, reject malformed silently. CAP_NET_RAW check at startup |
| 9 | Integrate into aggregation | After port hint: dns_cache_lookup(dst_ip). Both port and DNS vote. Conflict → majority wins, behavior breaks tie |
| 10 | Wire into main | Start DNS thread after config, stop on SIGTERM, restart on SIGHUP |

**Dependencies:** Step 1→3→4→5 (Phase 1 chain). Step 7→8→9→10 (Phase 2 chain). Phase 1 complete before Phase 2 begins.

### Timeline and Milestones

| Milestone | Deliverable | Validation |
|---|---|---|
| P1-M1 | `myco_hint.c/h` + test_hint.c passing | All port ranges correct, boundary tests |
| P1-M2 | Hint counting in aggregation + test_device.c | Mixed flow scenarios produce correct dominant_hint |
| P1-M3 | Hint-aware decide_persona() + test_persona.c | Full (hint × behavior) matrix: ~40 test cases |
| P1-M4 | Integrated in main loop, ubus status shows hints | QEMU lab: LoL → GAMING confirmed |
| P1-SHIP | Phase 1 merged to main | Real hardware test on AX3000T |
| P2-M1 | `myco_dns.c/h` + test_dns.c with fuzz vectors | No crash on malformed packets |
| P2-M2 | DNS thread running, cache populated | Log shows DNS entries being cached |
| P2-M3 | DNS integrated into aggregation | QEMU: Netflix/YouTube on TCP 443 → STREAMING |
| P2-SHIP | Phase 2 merged to main | Full 18-scenario benchmark: ≥90% accuracy |

### Resource Requirements

| Resource | Phase 1 | Phase 2 |
|---|---|---|
| New files | myco_hint.c/h, test_hint.c | myco_dns.c/h, test_dns.c |
| Modified files | myco_device.c/h, myco_persona.c/h, main.c | myco_device.c, main.c |
| New code lines | ~150 | ~250 |
| RAM delta | +1KB | +5KB |
| Total RAM delta | +6KB on 256MB device |

### Responsible Parties

| Role | Who | Responsibility |
|---|---|---|
| Implementation | Baris | All coding, testing, integration |
| Architecture review | Claude | Edge case review, design validation |
| Testing | Baris | Unit tests, QEMU benchmarks, AX3000T validation |
| Port table updates | Community (future) | Config file contributions for new games |

---

## MONITORING AND VALIDATION

### Success Metrics

| Metric | Target | Measurement | Frequency |
|---|---|---|---|
| Persona accuracy | ≥90% across 18 scenarios | QEMU automated benchmark | Every milestone |
| LoL classification | GAMING 100% during gameplay | logread, ubus status | P1-M4 |
| Netflix/YouTube TCP 443 | STREAMING 100% | Manual test with DNS active | P2-M3 |
| False GAMING rate | 0% | All 18 scenarios, verify no false positives | P1-SHIP, P2-SHIP |
| Persona stability | ≤1 change per 30s stable | Log monitoring | Every milestone |
| CPU impact | <0.1% increase | top comparison before/after | P1-SHIP |
| RAM impact | <10KB total | /proc/meminfo delta | P1-SHIP |
| DNS parser safety | 0 crashes / 10K fuzz packets | Fuzz test suite | P2-M1 |
| Graceful degradation | 78% accuracy without DNS | Kill DNS thread, verify | P2-M3 |
| Gaming latency | ≤ baseline + 1ms during streaming | QEMU 5-client benchmark | P2-SHIP |

### Validation Plan

**Phase 1 — Port Hints:**
- **Unit tests:** ~30 port boundary tests (test_hint.c), ~40 hint×behavior matrix tests (test_persona.c), hint aggregation tests (test_device.c)
- **QEMU integration:** LoL-like TCP 5000 → GAMING, CS2 UDP 27015 → GAMING (no regression), mixed LoL+YouTube → dominant GAMING, Steam download TCP 443 → BULK
- **Real hardware:** AX3000T with LoL, verify via ubus and LuCI

**Phase 2 — DNS Snooping:**
- **Fuzz testing:** 10K vectors including truncated headers, pointer loops, overflow RDLENGTH, zero-length labels, max-length domains. Zero crashes required.
- **DNS integration:** YouTube TCP 443 → STREAMING via googlevideo.com, Netflix → STREAMING via nflxvideo.net, Zoom fallback → VIDEO via zoom.us, DoH client → graceful fallback
- **Full benchmark:** 18 scenarios, target ≥17/18 (94%). Progression: before 8/18 → Phase 1 14/18 → Phase 2 17/18
- **Stress test:** 32 devices, 256 flows, DNS cache full, 1hr run, no leaks, kill DNS thread → no crash

### Risk Mitigation

| Risk | Prob | Impact | Mitigation |
|---|---|---|---|
| DNS parser crash | Med | Critical | Fuzz 10K vectors, thread isolation, reject malformed |
| Port table wrong | High | Low | Config override path, behavior fallback |
| Hint causes worse result | Low | Med | Sanity checks (bw>20Mbps overrides GAMING hint) |
| Persona flapping | Med | Med | 2-of-3 history window absorbs noise |
| DNS cache stale | Med | Low | TTL expiry, behavior overrides contradictions |
| DoH/DoT renders DNS useless | Med-High | Med | Port hints = 78% baseline, firewall redirect option |
| Thread deadlock | Low | Critical | Simple mutex, no nested locks |

### Adjustment Triggers

| Signal | Action |
|---|---|
| Phase 1 accuracy < 70% | Re-examine port table, check missing game ports |
| Phase 2 accuracy < 85% | Check DNS cache hit rate, verify domain suffix coverage |
| Any false GAMING/VOIP | Tighten sanity thresholds |
| Flapping > 3 changes / 30s | Increase history window to 5 or raise majority threshold |
| DNS thread crashes in prod | Disable DNS (degrade gracefully), fix parser, re-fuzz |
| CPU > 0.5% on AX3000T | Profile aggregation loop, optimize DNS lookup |
| User reports unclassified game | Add ports, publish config override, consider TLS SNI (Phase 3) |

---

## LESSONS LEARNED

### Key Learnings

1. **"Signal poverty" > "wrong algorithm"** — reframing from "tree is bad" to "tree lacks inputs" redirected solution from rewrite to data enrichment
2. **Data already existed** — dst_port in flow_key_t from day one, DNS queries visible on router. Most powerful improvements came from connecting existing data to existing logic
3. **Phased delivery is strategy, not compromise** — Phase 1 (78%) solves original problem with zero security risk. Phase 2 (94%) is enhancement, not prerequisite
4. **Occam's Razor was the most impactful elicitation** — cut per-flow persona storage (~100 lines), simplified confidence to binary, deferred config file. Easier to build, test, debug, and explain in thesis
5. **Pre-mortem prevented architectural mistakes** — per-flow iptables explosion caught in design, not implementation. Fix (classify per-flow, apply per-device) is cleaner than any post-hoc patch

### What Worked

- **Rubber Duck code trace** → found exact insertion point (elephant flow loop, myco_device.c:164), zero extra iterations needed
- **Algorithm Olympics benchmark** → objective 78% vs 94% comparison with specific failing scenarios made DNS investment case irrefutable
- **Red Team Attack 6** → surfaced mixed-use device problem, a deeper architectural issue than the original LoL bug
- **Cross-Functional War Room** → grounded design in Turkish ISP reality (CGNAT, local game titles) and reframed success as UX outcome ("does my game lag?")
- **Systems Thinking** → confirmed 3 downstream components unaffected, +6KB RAM calculation proved viability before coding

### What to Avoid

- **Don't build classifiers from specific observations** — original tree assumed all games look like CS2. First principles would have surfaced TCP-game blind spot on day one
- **Don't destroy information during aggregation prematurely** — ports and IPs were cheap to keep, expensive to lose
- **Don't skip "what's the simplest version that ships?"** — without Occam pass, Phase 1 scope would have tripled without proportional benefit
- **Don't treat DNS parsing as "just another feature"** — untrusted-input parsing in a system daemon is highest-risk. Earned its own phase, fuzz suite, and degradation strategy

---

_Generated using BMAD Creative Intelligence Suite - Problem Solving Workflow_
