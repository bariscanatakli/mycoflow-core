---
name: 'step-04-capacity'
description: 'Establish device capacity baseline — CPU, RAM, conntrack scaling, daemon frequency sweep'

nextStepFile: './step-05-benchmark.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
---

# Step 4: Device Capacity Baseline

## STEP GOAL:

To establish the AX3000T's capacity baseline — CPU/RAM with and without the daemon, conntrack scaling limits, and the optimal daemon frequency.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a performance measurement specialist
- ✅ Prescriptive — exact commands, record every number
- ✅ These measurements prove the "lightweight" thesis claim

### Step-Specific Rules:

- 🎯 Focus ONLY on device capacity measurement
- 🚫 FORBIDDEN to run QoS benchmarks (that's step-05)
- 💬 Guide user through each measurement systematically

## EXECUTION PROTOCOLS:

- 🎯 Measure baseline WITHOUT daemon, then WITH daemon — delta is the overhead proof
- 💾 Append results to Device Capacity Baseline section of {outputFile}
- 📖 Update frontmatter stepsCompleted when complete

## CONTEXT BOUNDARIES:

- Pre-flight passed, CAKE configured with real link parameters
- Flow offloading status is known (disabled or confirmed safe)
- Daemon is deployed but not yet running in production
- These measurements feed directly into thesis System Overhead metrics (Group 4)

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. CPU/RAM Baseline — Without Daemon

"**Capacity Test 1: System Baseline (No Daemon)**

Kill mycoflowd if running, then measure idle system:
```bash
# Ensure daemon is not running
killall mycoflowd 2>/dev/null

# Wait for system to settle
sleep 5

# CPU snapshot (idle, 10 second sample)
cat /proc/stat | head -1 && sleep 10 && cat /proc/stat | head -1

# Simpler: use top for a 10-second snapshot
top -b -n 3 -d 3 | head -30

# RAM snapshot
free -k
cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvail|Buffers|Cached'
```

Now generate moderate traffic (from wired client):
```bash
# Client: generate background load
iperf3 -c <router-ip> -t 30 -b 50M &
```

While traffic flows, measure again:
```bash
top -b -n 3 -d 3 | head -30
free -k
```

Paste both idle AND under-load measurements."

### 2. CPU/RAM — With Daemon (Idle)

"**Capacity Test 2: Daemon Overhead (Idle Network)**

Start the daemon:
```bash
mycoflowd &
DAEMON_PID=$!
echo "Daemon PID: $DAEMON_PID"

# Wait for it to stabilize
sleep 5

# Daemon memory footprint
ps -o pid,rss,vsz,comm | grep mycoflowd

# System CPU (10 second sample)
top -b -n 3 -d 3 | head -30

# RAM
free -k
```

**Expected:** RSS should be < 512 KB. CPU overhead should be negligible on idle network.

Paste the output."

### 3. CPU/RAM — With Daemon Under Load

"**Capacity Test 3: Daemon Overhead Under Traffic**

Generate moderate traffic:
```bash
# From wired client
iperf3 -c <router-ip> -t 30 -b 50M &
```

While traffic flows:
```bash
# Daemon memory
ps -o pid,rss,vsz,comm | grep mycoflowd

# System CPU
top -b -n 3 -d 3 | head -30

# Conntrack entries
cat /proc/net/nf_conntrack | wc -l
```

Paste the output. We'll compare with the no-daemon baseline to compute CPU delta."

### 4. Conntrack Scale Test

"**Capacity Test 4: Conntrack Scaling**

We need to measure how long a sense cycle takes as conntrack table grows. This validates the 2Hz feasibility.

**Method:** Generate increasing numbers of connections, measure cycle time.

```bash
# Check current conntrack count
cat /proc/net/nf_conntrack | wc -l

# Check conntrack max
cat /proc/sys/net/netfilter/nf_conntrack_max
```

**For each conntrack level (100, 500, 1000, 2000 entries):**

Generate connections (from client, multiple parallel flows):
```bash
# Generate N parallel connections
for i in $(seq 1 N); do
  nc -z <router-ip> 80 &
done
```

Or use a flood tool / open browser tabs to increase conntrack entries.

While at target conntrack count:
```bash
# Measure time to parse conntrack
time cat /proc/net/nf_conntrack > /dev/null

# If daemon has timing logs, check cycle duration
# Otherwise, measure externally:
time mycoflowd --single-cycle 2>/dev/null || time cat /proc/net/nf_conntrack | wc -l
```

Report the cycle duration at each level:
- ~100 entries: ___ms
- ~500 entries: ___ms
- ~1000 entries: ___ms
- ~2000 entries: ___ms

Paste your measurements."

### 5. Daemon Frequency Sweep

"**Capacity Test 5: Frequency Sweep**

Test the daemon at different cycle rates to find the optimal balance between detection speed and CPU overhead.

**For each frequency (0.5Hz, 1Hz, 2Hz):**

If the daemon supports a frequency parameter:
```bash
# Stop current daemon
killall mycoflowd

# Start at specific frequency (adjust parameter name as needed)
MYCOFLOW_SENSE_HZ=0.5 mycoflowd &
sleep 30
top -b -n 3 -d 3 | grep mycoflowd
ps -o pid,rss,comm | grep mycoflowd
killall mycoflowd

MYCOFLOW_SENSE_HZ=1.0 mycoflowd &
sleep 30
top -b -n 3 -d 3 | grep mycoflowd
killall mycoflowd

MYCOFLOW_SENSE_HZ=2.0 mycoflowd &
sleep 30
top -b -n 3 -d 3 | grep mycoflowd
killall mycoflowd
```

If the daemon doesn't support frequency config, measure the default 2Hz and note this for future work.

Report:
- 0.5 Hz: CPU ___%, Detection accuracy: ___
- 1.0 Hz: CPU ___%, Detection accuracy: ___
- 2.0 Hz: CPU ___%, Detection accuracy: ___

Paste the output for each frequency."

### 6. Set Safe Conntrack Limit

"**Capacity Test 6: Set Safe Limits**

Based on the scaling test, set a safe conntrack maximum to prevent OOM:
```bash
# Recommended: 4096 (safe for 256MB RAM)
echo 4096 > /proc/sys/net/netfilter/nf_conntrack_max

# Make persistent
uci set firewall.@defaults[0].nf_conntrack_max='4096'
uci commit firewall
```

Confirm:
```bash
cat /proc/sys/net/netfilter/nf_conntrack_max
```

Paste the output."

### 7. Record Device Capacity Baseline

"**Recording capacity baseline in the sprint report...**"

Append to Device Capacity Baseline section of {outputFile}:
- CPU/RAM without daemon (idle + under load)
- CPU/RAM with daemon (idle + under load)
- CPU delta (the overhead proof)
- Daemon RSS memory
- Conntrack scale table (entries vs cycle duration)
- Frequency sweep results
- Conntrack max setting

Update {outputFile} frontmatter:
- Append `'step-04-capacity'` to `stepsCompleted`
- Set `lastStep: 'step-04-capacity'`

### 8. Present MENU OPTIONS

Display: **Device capacity baseline complete.** [C] Continue to Benchmark Scenarios

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- ONLY proceed to next step when user selects 'C'

#### Menu Handling Logic:

- IF C: Save capacity results to {outputFile}, update frontmatter, then load, read entire file, then execute {nextStepFile}
- IF Any other: help user, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- CPU/RAM measured with and without daemon
- CPU delta calculated (proves lightweight claim)
- Daemon RSS < 512 KB confirmed
- Conntrack scaling measured at 4 levels
- Frequency sweep completed (or noted as future work)
- Safe conntrack limit set
- All data recorded in report

### ❌ SYSTEM FAILURE:

- Not measuring the no-daemon baseline (can't compute delta)
- Skipping conntrack scale test
- Not setting safe conntrack limit
- Running QoS benchmarks in this step

**Master Rule:** Baseline first. Every number feeds the thesis System Overhead metrics.
