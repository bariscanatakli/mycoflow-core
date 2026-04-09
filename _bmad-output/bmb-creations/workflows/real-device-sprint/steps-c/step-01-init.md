---
name: 'step-01-init'
description: 'Initialize the real device sprint, check for existing run, create output report'

nextStepFile: './step-02-preflight.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
templateFile: '../templates/sprint-report.template.md'
continueFile: './step-01b-continue.md'
---

# Step 1: Initialize Sprint

## STEP GOAL:

To initialize the real device sprint workflow, check for an existing sprint run, and create the output report document.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are an embedded systems benchmark specialist
- ✅ Prescriptive and checklist-driven — provide exact commands
- ✅ You bring structured testing methodology, user brings MycoFlow and AX3000T domain knowledge
- ✅ Together we produce thesis-quality benchmark data

### Step-Specific Rules:

- 🎯 Focus ONLY on initialization and setup
- 🚫 FORBIDDEN to start any benchmarking or device commands
- 💬 Welcome user, explain what the sprint will cover

## EXECUTION PROTOCOLS:

- 🎯 Check for existing sprint report → route to continue if found
- 💾 Create output report from template
- 📖 Update frontmatter stepsCompleted when complete
- 🚫 This is the init step — sets up everything that follows

## CONTEXT BOUNDARIES:

- This is the first step — no prior context
- Output report will be progressively filled through subsequent steps
- Sprint covers: pre-flight, link characterization, capacity baseline, benchmarks, analysis, fix/iterate, final report

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly. Do not skip, reorder, or improvise unless user explicitly requests a change.

### 1. Check for Existing Sprint Report

Look for {outputFile}. If the file exists and contains a `stepsCompleted` array with entries:

- Load, read, and execute {continueFile} to resume from where we left off.

If the file does NOT exist or has an empty `stepsCompleted` array, continue to step 2.

### 2. Welcome and Explain

"**Welcome to the MycoFlow Real Device Sprint!**

This workflow will guide you through a structured sprint on your Xiaomi AX3000T. Here's what we'll cover:

1. **Pre-flight & Deploy** — SSH in, verify modules, resolve flow offloading, deploy binary
2. **Link Characterization** — Measure your real WAN link for CAKE configuration
3. **Device Capacity Baseline** — CPU/RAM overhead, conntrack scaling, frequency sweep
4. **Benchmark Scenarios** — 4 scenarios (FIFO/CAKE/Static/MycoFlow) x 5+ runs each
5. **Results Analysis** — Populate thesis table, confusion matrix, grading
6. **Fix & Iterate** — Log bugs, fix, re-run as needed
7. **Final Report** — Complete thesis-ready data export

**Before we start, I need a few things:**

- Do you have SSH access to the AX3000T ready?
- Is the cross-compiled mycoflowd binary available?
- Do you have at least one wired client device for benchmarking?

Let me know your setup status."

### 3. Create Output Report

Once user confirms readiness, create {outputFile} from {templateFile}.

Set the frontmatter:
- `stepsCompleted: ['step-01-init']`
- `lastStep: 'step-01-init'`
- `date: [current date]`
- `user_name: {user_name}`

### 4. Proceed to Pre-flight

"**Sprint report created. Let's get started with the pre-flight check on your AX3000T.**"

**Proceeding to pre-flight...**

#### Menu Handling Logic:

- After report creation, immediately load, read entire file, then execute {nextStepFile}

#### EXECUTION RULES:

- This is an auto-proceed init step with no user choices at the end
- Proceed directly to next step after report is created

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- Existing sprint report detected → routed to continuation
- OR new sprint report created from template
- User confirmed device readiness
- Frontmatter initialized with stepsCompleted

### ❌ SYSTEM FAILURE:

- Starting benchmarks in this step
- Not checking for existing report
- Creating report without confirming user readiness
- Not setting up frontmatter tracking

**Master Rule:** Init sets up everything. Don't rush past setup confirmation.
