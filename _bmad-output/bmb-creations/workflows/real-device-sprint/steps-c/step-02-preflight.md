---
name: 'step-02-preflight'
description: 'Pre-flight check and deploy mycoflowd on the AX3000T'

nextStepFile: './step-03-link-char.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
---

# Step 2: Pre-flight & Deploy

## STEP GOAL:

To verify the AX3000T is ready for benchmarking — check kernel modules, resolve the flow offloading showstopper, deploy mycoflowd, and capture the environment snapshot.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are an embedded systems deployment specialist
- ✅ Prescriptive — provide exact commands to run on the device
- ✅ Report expected vs actual output for go/no-go decisions

### Step-Specific Rules:

- 🎯 Focus ONLY on device verification and deployment
- 🚫 FORBIDDEN to start benchmarking or measuring performance
- 🚫 FORBIDDEN to proceed past flow offloading gate without resolution
- 💬 Provide exact SSH commands, check output against expectations

## EXECUTION PROTOCOLS:

- 🎯 Follow the checklist exactly — each item is a go/no-go gate
- 💾 Append results to Environment Snapshot section of {outputFile}
- 📖 Update frontmatter stepsCompleted when all checks pass

## CONTEXT BOUNDARIES:

- Device: Xiaomi AX3000T — MT7981B (2x Cortex-A53 @ 1.3GHz), 256MB RAM
- OS: OpenWrt (version to be confirmed)
- Binary: aarch64-openwrt-linux-musl cross-compiled mycoflowd
- Step-01 created the output report — we append to it

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly. Do not skip, reorder, or improvise unless user explicitly requests a change.

### 1. SSH Access Verification

"**Pre-flight Check 1: SSH Access**

Run from your local machine:
```bash
ssh root@<router-ip> 'uname -a'
```

**Expected:** Linux output showing `aarch64` architecture and OpenWrt kernel.

Paste the output here."

Wait for user response. If SSH fails, help troubleshoot before proceeding.

### 2. OpenWrt Version & Package List

"**Pre-flight Check 2: OpenWrt Environment**

Run on the router:
```bash
cat /etc/openwrt_release
opkg list-installed | head -50
```

And for the full package snapshot:
```bash
opkg list-installed
```

Paste the output. I'll record it in the report."

Record OpenWrt version in {outputFile} Environment Snapshot section.

### 3. Kernel Module Verification

"**Pre-flight Check 3: Required Kernel Modules**

Run on the router:
```bash
lsmod | grep -E 'conntrack|nf_|ifb|sch_cake|act_mirred|cls_'
```

**Required modules — all must be present:**
- `nf_conntrack` — conntrack tracking (daemon reads this)
- `sch_cake` — CAKE qdisc (QoS shaping)
- `ifb` — Intermediate Functional Block (ingress shaping)
- `act_mirred` — traffic mirroring to IFB
- `cls_fw` or `cls_u32` — packet classification

**If any are missing:**
```bash
opkg install kmod-sched-cake kmod-ifb kmod-nf-conntrack
```

Paste the lsmod output."

Record module status in {outputFile}. If critical modules cannot be installed, this is a **BLOCKER** — document and halt.

### 4. Flow Offloading Check — SHOWSTOPPER GATE

"**Pre-flight Check 4: FLOW OFFLOADING — CRITICAL GATE**

This is the most important check. If hardware flow offloading is enabled, conntrack counters freeze for offloaded flows and the daemon reads stale data.

Run on the router:
```bash
# Check firewall config
uci show firewall | grep flow_offloading

# Check if offload module is loaded
lsmod | grep flow
```

**Possible outcomes:**

**A) `flow_offloading=0` or not set** — GOOD. No action needed. Proceed.

**B) `flow_offloading=1` (software only)** — needs investigation. Software offloading may still allow conntrack updates. Test with:
```bash
# Generate traffic, then check if conntrack bytes update
conntrack -L | head -5
# Wait 2 seconds, check again
sleep 2 && conntrack -L | head -5
```

**C) `flow_offloading_hw=1` (hardware PPE offloading)** — SHOWSTOPPER. Must disable:
```bash
uci set firewall.@defaults[0].flow_offloading='0'
uci set firewall.@defaults[0].flow_offloading_hw='0'
uci commit firewall
/etc/init.d/firewall restart
```

Then measure CPU impact of disabling offloading (captured in step-04).

Paste the output. We cannot proceed until this is resolved."

Record flow offloading status and resolution in {outputFile}. **DO NOT proceed if unresolved.**

### 5. iptables/nftables Verification

"**Pre-flight Check 5: Packet Marking**

Check which firewall backend is active:
```bash
# Check for iptables-nft (needed for DSCP marking)
iptables --version
nft list ruleset | head -20
```

**Expected:** OpenWrt 23.05+ uses nftables by default. `iptables-nft` compatibility layer should be available.

If iptables commands fail:
```bash
opkg install iptables-nft
```

Paste the output."

### 6. Deploy mycoflowd Binary

"**Pre-flight Check 6: Deploy mycoflowd**

From your local machine:
```bash
scp mycoflowd root@<router-ip>:/usr/bin/mycoflowd
ssh root@<router-ip> 'chmod +x /usr/bin/mycoflowd'
```

Verify it runs:
```bash
ssh root@<router-ip> 'mycoflowd --help || mycoflowd --version || echo "binary deployed"'
```

**If segfault:** Cross-compilation issue. Check:
- Compiled with `aarch64-openwrt-linux-musl-gcc`?
- Linked against musl (not glibc)?
- Correct OpenWrt SDK version?

Paste the output."

### 7. Quick Sanity Test

"**Pre-flight Check 7: Quick Sanity Run**

Start the daemon briefly and check it doesn't crash:
```bash
# Run for 5 seconds, check output
timeout 5 mycoflowd 2>&1 || true
echo "Exit code: $?"

# Check it can read conntrack
cat /proc/net/nf_conntrack | wc -l
```

**Expected:** Daemon runs without segfault, can read conntrack entries.

Paste the output."

### 8. Record Environment Snapshot

"**Recording environment snapshot in the sprint report...**"

Append all collected data to the Environment Snapshot section of {outputFile}:
- OpenWrt version
- Kernel version
- Installed packages
- Kernel module status
- Flow offloading status and resolution
- iptables/nftables backend
- mycoflowd deployment status

Update {outputFile} frontmatter:
- Append `'step-02-preflight'` to `stepsCompleted`
- Set `lastStep: 'step-02-preflight'`
- Fill in `openwrtVersion` and `flowOffloadingStatus`

### 9. Present MENU OPTIONS

Display: **Pre-flight complete. All checks passed.** [C] Continue to Link Characterization

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- ONLY proceed to next step when user selects 'C'

#### Menu Handling Logic:

- IF C: Save all pre-flight results to {outputFile}, update frontmatter, then load, read entire file, then execute {nextStepFile}
- IF Any other: help user, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- All 7 pre-flight checks passed
- Flow offloading resolved (disabled or confirmed safe)
- mycoflowd deployed and runs without crash
- Environment snapshot recorded in report
- Ready to proceed to link characterization

### ❌ SYSTEM FAILURE:

- Proceeding past flow offloading gate without resolution
- Not recording environment snapshot
- Skipping kernel module verification
- Starting benchmarks in this step

**Master Rule:** Every check is a gate. Do not skip. Flow offloading is the showstopper — resolve it or stop.
