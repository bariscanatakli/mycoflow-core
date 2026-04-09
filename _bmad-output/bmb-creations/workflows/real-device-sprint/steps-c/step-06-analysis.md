---
name: 'step-06-analysis'
description: 'Analyze benchmark results, populate thesis table, grade metrics'

nextStepFile: './step-07-fix-iterate.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
metricsData: '../data/metrics-template.md'
advancedElicitationTask: '{project-root}/_bmad/core/workflows/advanced-elicitation/workflow.xml'
partyModeWorkflow: '{project-root}/_bmad/core/workflows/party-mode/workflow.md'
---

# Step 6: Results Analysis

## STEP GOAL:

To analyze all benchmark data, compute statistics, populate the thesis metrics table, generate the confusion matrix, compare with QEMU results, and grade each metric group.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a data analysis specialist
- ✅ Rigorous — mean, stddev, P99, comparison ratios
- ✅ Help user interpret results and identify issues
- ✅ User provides raw data, you help crunch and frame it

### Step-Specific Rules:

- 🎯 Focus on analysis and interpretation, not running more tests
- 🚫 FORBIDDEN to re-run benchmarks (that's step-07 loop)
- 💬 Walk through each metric group systematically
- 💬 Be honest about bad results — they're valid findings too

## EXECUTION PROTOCOLS:

- 🎯 Load {metricsData} for success criteria and grade scale
- 💾 Update Benchmark Results tables and Analysis sections in {outputFile}
- 📖 Update frontmatter stepsCompleted when analysis complete

## CONTEXT BOUNDARIES:

- Raw benchmark data collected in step-05
- QEMU results: FIFO +693ms, CAKE +0.2ms, MycoFlow +0.1ms, tin ratio 154x
- 5 metric groups with weighted scoring
- Grade scale: A+/A/B/C/F

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Load Raw Data

"**Let's analyze your benchmark results.**

Load {metricsData} for reference criteria.

First, let me see the raw data from step-05. Read the Benchmark Results section of {outputFile}.

If any data is missing or incomplete, note it — we'll address gaps in step-07."

### 2. Compute Latency & Jitter Statistics (Group 1, Weight: 30%)

"**Analysis Group 1: Latency & Jitter**

For each scenario (FIFO/CAKE/Static/MycoFlow), compute from the 5 runs:

| Metric | Formula |
|--------|---------|
| Mean RTT | Average of 5 run means |
| Stddev | Standard deviation across 5 runs |
| P99 | 99th percentile from all samples |
| Jitter | Mean of per-run stddevs |

Fill in the Latency & Jitter table in the report:

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| RTT interactive (ms)     | X +/- Y | ...     | ...         | ...      |
| RTT bulk (ms)            |         |         |             |          |
| Jitter interactive (ms)  |         |         |             |          |
| P99 latency (ms)         |         |         |             |          |
| Tin separation ratio     | N/A     | X:1     | X:1         | X:1      |

**Key questions:**
- Is MycoFlow <= CAKE+Static <= CAKE << FIFO?
- Is tin separation ratio > 100x (like QEMU's 154x)?
- Is jitter controlled for interactive traffic?"

Help user compute and fill the table. Discuss any surprising results.

### 3. Compute Throughput Impact (Group 5, Weight: 10%)

"**Analysis Group 5: Throughput Impact**

Compare max throughput across scenarios:

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| Max throughput (Mbps)    |         |         |             |          |
| Throughput delta (%)     | base    | -X%     | -X%         | -X%      |

**Success:** MycoFlow throughput within 2% of CAKE-only baseline.

Is there any throughput degradation from the daemon?"

### 4. Compute System Overhead (Group 4, Weight: 15%)

"**Analysis Group 4: System Overhead**

From step-04 capacity data + step-05 benchmark data:

|                          | FIFO    | CAKE    | CAKE+Static | MycoFlow |
|--------------------------|---------|---------|-------------|----------|
| CPU overhead (%)         | 0       | +X%     | +X%         | +X%      |
| Memory (KB RSS)          | 0       | 0       | 0           | X        |
| Sense cycle (ms)         | N/A     | N/A     | N/A         | X        |
| Actual cycle rate (Hz)   | N/A     | N/A     | N/A         | X        |

**Success criteria:**
- CPU < 5% additional
- RSS < 512 KB
- Cycle < 100ms
- Rate within +/-20% of target

How does the daemon's overhead look?"

### 5. Compute Persona Accuracy (Group 2, Weight: 25%)

"**Analysis Group 2: Persona Classification Accuracy**

**Controlled tests (iperf3):**
Fill accuracy per persona:

| Persona    | Correct / Total | Accuracy (%) |
|------------|-----------------|-------------|
| VOIP       |                 |             |
| GAMING     |                 |             |
| VIDEO      |                 |             |
| STREAMING  |                 |             |
| BULK       |                 |             |
| TORRENT    |                 |             |

**Real application tests:**
Fill the confusion matrix from step-05 real app data:

| Application    | Expected  | Detected  | Correct? | Time to Detect |
|----------------|-----------|-----------|----------|----------------|
| Zoom/Teams     | VOIP      |           |          |                |
| YouTube        | STREAMING |           |          |                |
| Steam download | BULK      |           |          |                |
| Torrent        | TORRENT   |           |          |                |
| Web browsing   | VIDEO     |           |          |                |

**Build the full confusion matrix** — rows = actual, columns = predicted.

**Success criteria:**
- >=90% accuracy for controlled tests
- >=70% accuracy for real apps (4+ personas)
- Detection latency <=3s
- False positive rate <10%

If accuracy is below threshold, identify which personas are confused and why."

### 6. Compute Adaptation Metrics (Group 3, Weight: 20%)

"**Analysis Group 3: Adaptation**

From step-05 adaptation tests:

| Test                        | MycoFlow Time | Static Result |
|-----------------------------|---------------|---------------|
| Traffic pattern change      | X seconds     | Never adapts  |
| New device classification   | X seconds     | No entry      |
| Encrypted traffic (QUIC)    | Correct/Wrong | Wrong         |

**Success criteria:**
- Reclassification < 5 seconds
- New device detected < 5 seconds
- QUIC correctly classified

This is the **killer differentiator** — if MycoFlow adapts and static doesn't, the thesis contribution is proven."

### 7. QEMU vs Real Device Comparison

"**QEMU vs Real Device Comparison**

Fill the comparison table:

|                          | QEMU Result | Real Device | Delta | Explanation |
|--------------------------|-------------|-------------|-------|-------------|
| RTT interactive (ms)     | +0.1        |             |       |             |
| RTT bulk (ms)            | +34.7       |             |       |             |
| Tin separation ratio     | 154x        |             |       |             |
| CPU overhead (%)         | ~0%         |             |       |             |
| Persona accuracy         | 100% (lab)  |             |       |             |

**Key insight:** QEMU measures pure qdisc overhead in isolation. Real device adds conntrack, iptables, WiFi driver, interrupt handling. Different claims, different numbers.

Focus on **relative improvement** — if FIFO is +Xms and MycoFlow is +Yms, the reduction ratio may match QEMU even if absolutes differ."

### 8. Grade Each Metric Group

"**Final Grading**

Based on the analysis, grade each group:

| Metric Group       | Weight | Grade | Justification |
|--------------------|--------|-------|---------------|
| Latency & Jitter   | 30%    |       |               |
| Persona Accuracy   | 25%    |       |               |
| Adaptation         | 20%    |       |               |
| System Overhead    | 15%    |       |               |
| Throughput Impact  | 10%    |       |               |
| **Overall**        | 100%   |       |               |

**Grade scale:** A+ (exceeds QEMU), A (minor degradation), B (trade-offs but viable), C (significant limitations), F (unacceptable)

Let's discuss the grades together."

### 9. Identify Issues and Bugs

"**Issues Discovered**

Based on the analysis:
- Were any personas consistently misclassified?
- Did any scenario produce unexpected results?
- Were there any crashes, hangs, or anomalies?
- Is there data that needs re-collection?

List all issues for the bug/fix log in step-07."

### 10. Record Analysis

"**Recording analysis in the sprint report...**"

Update all analysis tables in {outputFile}:
- Completed metrics tables with computed statistics
- Confusion matrix
- QEMU comparison
- Metric grades
- Issues list

Update {outputFile} frontmatter:
- Append `'step-06-analysis'` to `stepsCompleted`
- Set `lastStep: 'step-06-analysis'`

### 11. Present MENU OPTIONS

Display: **Analysis complete.** **Select an Option:** [A] Advanced Elicitation [P] Party Mode [C] Continue to Fix & Iterate

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- ONLY proceed to next step when user selects 'C'
- After other menu items execution, return to this menu

#### Menu Handling Logic:

- IF A: Execute {advancedElicitationTask}, and when finished redisplay the menu
- IF P: Execute {partyModeWorkflow}, and when finished redisplay the menu
- IF C: Save analysis to {outputFile}, update frontmatter, then load, read entire file, then execute {nextStepFile}
- IF Any other: help user, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- All 5 metric groups analyzed with computed statistics
- Thesis table populated with mean+/-stddev
- Confusion matrix generated
- QEMU comparison completed
- All groups graded
- Issues identified for fix/iterate step

### ❌ SYSTEM FAILURE:

- Presenting raw data without computing statistics
- Not computing mean/stddev (single values only)
- Skipping QEMU comparison
- Not grading metric groups
- Sugar-coating bad results

**Master Rule:** Honest analysis. Bad results are valid findings. Compute real statistics.
