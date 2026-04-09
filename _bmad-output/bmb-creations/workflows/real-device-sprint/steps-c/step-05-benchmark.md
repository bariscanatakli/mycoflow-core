---
name: 'step-05-benchmark'
description: 'Run 4-scenario benchmarks with controlled and real-application tests'

nextStepFile: './step-06-analysis.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
metricsData: '../data/metrics-template.md'
advancedElicitationTask: '{project-root}/_bmad/core/workflows/advanced-elicitation/workflow.xml'
partyModeWorkflow: '{project-root}/_bmad/core/workflows/party-mode/workflow.md'
---

# Step 5: Benchmark Scenarios

## STEP GOAL:

To run the 4-scenario benchmark suite (FIFO/CAKE/CAKE+Static DSCP/MycoFlow) with N>=5 runs each, including controlled iperf3 tests, real application tests, and adaptation tests.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a benchmark execution specialist
- ✅ Prescriptive — exact commands for each scenario
- ✅ Statistical rigor: N>=5 runs, warm-up pass, controlled conditions
- ✅ User brings the physical setup, you bring the methodology

### Step-Specific Rules:

- 🎯 Focus on running benchmarks and recording raw data
- 🚫 FORBIDDEN to analyze results (that's step-06)
- 🚫 FORBIDDEN to skip warm-up passes
- 💬 Guide through each scenario systematically, record everything

## EXECUTION PROTOCOLS:

- 🎯 Load {metricsData} for success criteria reference
- 💾 Append raw results to Benchmark Results section of {outputFile}
- 📖 Update frontmatter stepsCompleted when all scenarios complete
- 🚫 Each scenario must have N>=5 recorded runs

## CONTEXT BOUNDARIES:

- Link characterized, CAKE configured (step-03)
- Device capacity known (step-04)
- 4 scenarios: FIFO → CAKE → CAKE+Static → MycoFlow
- Wired clients for primary benchmarks
- Kill unnecessary services: `for s in uhttpd dnsmasq; do /etc/init.d/$s stop; done` (optional, reduces noise)

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Pre-Benchmark Setup

"**Benchmark Setup**

Before running scenarios, prepare the environment:

**On the router:**
```bash
# Optional: reduce system noise
/etc/init.d/uhttpd stop 2>/dev/null

# Verify CAKE is configured (from step-03)
tc -s qdisc show

# Ensure daemon is stopped (we'll start it for MycoFlow scenario)
killall mycoflowd 2>/dev/null

# Set conntrack max (from step-04)
echo 4096 > /proc/sys/net/netfilter/nf_conntrack_max
```

**On the wired client(s):**
```bash
# Verify iperf3 is installed
iperf3 --version

# Verify ping is available
ping -c 1 <router-ip>
```

Confirm your setup is ready."

### 2. Scenario 1: FIFO (No Shaping)

"**Scenario 1: FIFO — Worst Case Baseline**

Remove all qdiscs:
```bash
# On router
tc qdisc del dev <WAN_IFACE> root 2>/dev/null
tc qdisc del dev ifb0 root 2>/dev/null
tc qdisc show dev <WAN_IFACE>
# Should show default pfifo_fast or fq_codel
```

**Warm-up pass (not recorded):**
```bash
# Client: generate 30s of mixed traffic
iperf3 -c <router-ip> -t 30 -b 80M &
ping -c 30 -i 1 <router-ip>
```

**Recorded runs (N>=5). For each run:**
```bash
# Client 1 (interactive): ping while under load
# Client 2 (or same client, background): generate bulk traffic

# Background load (bulk):
iperf3 -c <router-ip> -t 60 -b [90% of bandwidth]M &

# Interactive measurement (while bulk runs):
ping -c 100 -i 0.1 <router-ip> | tee fifo_run_N.txt

# Bulk measurement:
iperf3 -c <router-ip> -t 30 | tee fifo_throughput_N.txt

# On router: capture stats
tc -s qdisc show dev <WAN_IFACE>
cat /proc/stat | head -1
ps -o pid,rss,comm | grep mycoflowd
```

Run 5 times. Report for each run:
- RTT mean, stddev, min, max, P99
- Throughput (Mbps)
- CPU snapshot

Paste results for all 5 runs."

### 3. Scenario 2: CAKE Only (No DSCP)

"**Scenario 2: CAKE Only — Shaping Without Classification**

Restore CAKE from step-03 config, but ensure NO DSCP marking:
```bash
# Reapply CAKE (from step-03 commands)
tc qdisc replace dev <WAN_IFACE> root cake bandwidth [X]Mbit rtt [Y]ms diffserv4 [overhead] nat wash
tc qdisc replace dev ifb0 root cake bandwidth [X]Mbit rtt [Y]ms diffserv4 [overhead] nat wash ingress

# Ensure NO iptables DSCP rules
iptables -t mangle -F
```

**Warm-up pass, then 5 recorded runs** (same procedure as Scenario 1).

For each run, also capture CAKE tin stats:
```bash
tc -s qdisc show dev <WAN_IFACE> | grep -A3 'Tin'
```

Paste results for all 5 runs."

### 4. Scenario 3: CAKE + Static DSCP Rules

"**Scenario 3: CAKE + Static DSCP — Manual Configuration Baseline**

This proves what a sysadmin could achieve without MycoFlow. Apply manual DSCP rules:
```bash
# Mark known gaming ports as CS4 (Voice tin in CAKE diffserv4)
iptables -t mangle -A POSTROUTING -p udp --dport 3478:3479 -j DSCP --set-dscp-class CS4

# Mark known video streaming as CS3 (Video tin)
iptables -t mangle -A POSTROUTING -p tcp --dport 443 -j DSCP --set-dscp-class CS3

# Mark bulk downloads as CS1 (Bulk tin)
iptables -t mangle -A POSTROUTING -p tcp --dport 6881:6889 -j DSCP --set-dscp-class CS1

# Verify rules
iptables -t mangle -L -n -v
```

**Warm-up pass, then 5 recorded runs.** Same measurements as previous scenarios.

Also capture tin stats to show DSCP→tin mapping:
```bash
tc -s qdisc show dev <WAN_IFACE>
```

Paste results for all 5 runs."

### 5. Scenario 4: MycoFlow (CAKE + Daemon)

"**Scenario 4: MycoFlow — Full Automatic Classification**

Remove static rules and start the daemon:
```bash
# Clear static DSCP rules
iptables -t mangle -F

# Start mycoflowd
mycoflowd &
DAEMON_PID=$!
sleep 3

# Verify daemon is running and classifying
cat /tmp/myco_state.json 2>/dev/null || echo "State file check"
```

**Warm-up pass, then 5 recorded runs.** Same measurements as previous scenarios.

Additional MycoFlow-specific captures per run:
```bash
# Daemon state
cat /tmp/myco_state.json

# Persona decisions log (if available)
# Daemon cycle timing (if logged to stdout)

# CAKE tin stats
tc -s qdisc show dev <WAN_IFACE>

# Conntrack snapshot
cat /proc/net/nf_conntrack | wc -l
```

Paste results for all 5 runs."

### 6. Real Application Tests

"**Real Application Tests**

Now test with real applications instead of iperf3. Run each app solo first, then combined.

**Test each app individually (MycoFlow scenario active):**

1. **Voice/Video call** (Zoom, Teams, or WebRTC test)
   - Start call, wait 30 seconds, check daemon classification
   ```bash
   cat /tmp/myco_state.json
   ```

2. **YouTube / streaming** (play a 1080p video)
   - Stream for 60 seconds, check classification

3. **Steam / game download** (or any large download)
   - Start download, check classification after 10 seconds

4. **Torrent** (legal torrent, e.g., Linux ISO)
   - Start torrent with 20+ peers, check classification

5. **Web browsing** (open 10+ tabs simultaneously)
   - Browse for 30 seconds, check classification

**For each app, record:**
- Expected persona
- Detected persona (from myco_state.json)
- Time to detection
- Conntrack entry count

Paste results for each application."

### 7. Adaptation Test

"**Adaptation Test — The Killer Differentiator**

This proves MycoFlow adapts while static rules don't.

**Test 1: Traffic pattern change**
```bash
# Start gaming traffic (iperf3 simulating gaming)
iperf3 -c <server> -u -b 500K -l 120 -t 120 &

# Wait for classification
sleep 10
cat /tmp/myco_state.json  # Should show GAMING

# Stop gaming, start torrent
kill %1
# Start torrent client
# Wait for reclassification
sleep 10
cat /tmp/myco_state.json  # Should show TORRENT

# Record time to reclassify
```

**Test 2: New device joins**
- Connect a new device (phone, tablet)
- Generate traffic from it
- Check how quickly MycoFlow classifies it
```bash
cat /tmp/myco_state.json  # Check for new device entry
```

**Test 3: Encrypted traffic (QUIC/HTTP3)**
- Open YouTube in Chrome (uses QUIC)
- All traffic is UDP 443
- Static DSCP rules can't distinguish video from other HTTPS
- Check if MycoFlow classifies correctly based on behavior
```bash
cat /tmp/myco_state.json
conntrack -L | grep 'dport=443.*proto=17'  # UDP 443 = QUIC
```

Record timing and classification results for each test."

### 8. Record All Benchmark Data

"**Recording all benchmark data in the sprint report...**"

Append to Benchmark Results section of {outputFile}:
- All 4 scenario results (5 runs each, raw data)
- Real application test results
- Adaptation test results
- Encrypted traffic test results

Update {outputFile} frontmatter:
- Append `'step-05-benchmark'` to `stepsCompleted`
- Set `lastStep: 'step-05-benchmark'`

### 9. Present MENU OPTIONS

Display: **All benchmark scenarios complete.** **Select an Option:** [A] Advanced Elicitation [P] Party Mode [C] Continue to Analysis

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- ONLY proceed to next step when user selects 'C'
- After other menu items execution, return to this menu

#### Menu Handling Logic:

- IF A: Execute {advancedElicitationTask}, and when finished redisplay the menu
- IF P: Execute {partyModeWorkflow}, and when finished redisplay the menu
- IF C: Save all benchmark data to {outputFile}, update frontmatter, then load, read entire file, then execute {nextStepFile}
- IF Any other: help user, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- All 4 scenarios run with N>=5 recorded runs each
- Warm-up pass before each scenario
- Real application tests completed
- Adaptation test shows reclassification timing
- Encrypted traffic test completed
- All raw data recorded in report

### ❌ SYSTEM FAILURE:

- Fewer than 5 runs per scenario
- Skipping warm-up passes
- Not testing real applications (iperf3-only)
- Analyzing results in this step (that's step-06)
- Skipping adaptation test

**Master Rule:** Raw data collection only. Record everything. Analysis comes next.
