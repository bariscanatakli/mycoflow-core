---
name: 'step-03-link-char'
description: 'Characterize the real WAN link and configure CAKE with accurate parameters'

nextStepFile: './step-04-capacity.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
---

# Step 3: Link Characterization

## STEP GOAL:

To measure the actual WAN link characteristics (bandwidth, RTT, encapsulation) and configure CAKE with accurate parameters for the real device.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a network measurement specialist
- ✅ Prescriptive — exact commands, expected outputs
- ✅ CAKE configuration must match real link — QEMU params are meaningless here

### Step-Specific Rules:

- 🎯 Focus ONLY on link measurement and CAKE configuration
- 🚫 FORBIDDEN to run benchmarks or test MycoFlow
- 💬 Guide user through each measurement, validate results

## EXECUTION PROTOCOLS:

- 🎯 Measure before configuring — never guess link parameters
- 💾 Append results to Link Characteristics section of {outputFile}
- 📖 Update frontmatter stepsCompleted when complete

## CONTEXT BOUNDARIES:

- Pre-flight passed — device is ready, modules loaded, daemon deployed
- CAKE uses `rtt` parameter (not `target`/`interval`)
- Overhead depends on encapsulation type (PPPoE, VDSL, Fiber, Ethernet)
- If ISP shapes upstream, CAKE may fight a two-shaper problem

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Measure WAN Bandwidth

"**Link Characterization Step 1: WAN Bandwidth**

We need to measure your actual internet speed. Run from a **wired client** (not WiFi):

**Option A — Using iperf3 to external server:**
```bash
# If you have access to an iperf3 server
iperf3 -c <server> -t 10          # Download
iperf3 -c <server> -t 10 -R       # Upload
```

**Option B — Using speedtest-cli:**
```bash
# On the wired client
speedtest-cli --simple
```

**Option C — From the router itself:**
```bash
# Install on router if not present
opkg install iperf3
iperf3 -c <server> -t 10
iperf3 -c <server> -t 10 -R
```

Run at least **3 measurements** and report the average.

Paste your results (download Mbps, upload Mbps)."

### 2. Measure RTT to ISP Gateway

"**Link Characterization Step 2: RTT**

Measure round-trip time from the router:
```bash
# RTT to ISP gateway (first hop)
ping -c 20 -i 0.2 $(ip route | grep default | awk '{print $3}')

# RTT to a reliable external host
ping -c 20 -i 0.2 1.1.1.1
```

Report:
- Gateway RTT (mean and stddev)
- External RTT (mean and stddev)

Paste the output."

### 3. Determine Encapsulation Type

"**Link Characterization Step 3: Encapsulation**

What type of internet connection do you have?

**A) PPPoE** (common with DSL/VDSL)
```bash
# Check if PPPoE is in use
ifconfig | grep pppoe
uci show network | grep proto
```
Overhead: 34 bytes (`overhead 34 mpu 64`)

**B) VDSL with PTM**
Overhead: 30 bytes (`overhead 30 mpu 64 atm`)

**C) Fiber/Ethernet (direct)**
Overhead: 18 bytes (`overhead 18`)

**D) Not sure**
Check with:
```bash
uci show network.wan.proto
uci show network.wan6.proto
```

Tell me your connection type or paste the output."

### 4. Configure CAKE

"**Link Characterization Step 4: CAKE Configuration**

Based on your measurements:
- Download: [X] Mbps
- Upload: [Y] Mbps
- RTT: [Z] ms
- Encapsulation: [type]

Here are the CAKE commands. We'll set bandwidth to **95% of measured** to leave headroom:

**Egress (upload) shaping:**
```bash
tc qdisc replace dev <WAN_IFACE> root cake bandwidth [95% of upload]Mbit rtt [RTT]ms diffserv4 [overhead params] nat wash
```

**Ingress (download) shaping via IFB:**
```bash
tc qdisc replace dev ifb0 root cake bandwidth [95% of download]Mbit rtt [RTT]ms diffserv4 [overhead params] nat wash ingress
```

**Verify configuration:**
```bash
tc -s qdisc show dev <WAN_IFACE>
tc -s qdisc show dev ifb0
```

NOTE: The env var is `MYCOFLOW_EGRESS_IFACE` (not WAN_IFACE).

Apply these commands and paste the `tc -s qdisc show` output for both interfaces."

### 5. Validate CAKE Is Active

"**Link Characterization Step 5: Validation**

Run a quick bandwidth test while CAKE is active:
```bash
# From wired client, generate load
iperf3 -c <router-ip> -t 5 -b [bandwidth]M

# On router, check CAKE stats are updating
tc -s qdisc show dev <WAN_IFACE> | grep -A5 'Cake'
```

**Expected:** CAKE should show packets processed, backlog, and tin statistics.

If CAKE shows zero packets, the qdisc is not attached to the correct interface. Troubleshoot before proceeding.

Paste the output."

### 6. Record Link Characteristics

"**Recording link characteristics in the sprint report...**"

Append to Link Characteristics section of {outputFile}:
- WAN Bandwidth (Down/Up)
- RTT to ISP Gateway
- Encapsulation type
- CAKE configuration commands used
- `tc qdisc show` output for both interfaces

Update {outputFile} frontmatter:
- Append `'step-03-link-char'` to `stepsCompleted`
- Set `lastStep: 'step-03-link-char'`

### 7. Present MENU OPTIONS

Display: **Link characterization complete. CAKE configured with real parameters.** [C] Continue to Device Capacity Baseline

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- ONLY proceed to next step when user selects 'C'

#### Menu Handling Logic:

- IF C: Save link results to {outputFile}, update frontmatter, then load, read entire file, then execute {nextStepFile}
- IF Any other: help user, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- WAN bandwidth measured (3+ measurements averaged)
- RTT measured to gateway and external host
- Encapsulation type determined
- CAKE configured with real parameters (95% bandwidth, real RTT, correct overhead)
- CAKE validated — packets flowing through tins
- All recorded in report

### ❌ SYSTEM FAILURE:

- Using QEMU CAKE parameters on real device
- Guessing bandwidth instead of measuring
- Not validating CAKE is actually active
- Proceeding with zero-packet CAKE stats

**Master Rule:** Measure first, configure second. Never guess link parameters.
