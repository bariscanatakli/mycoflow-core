---
name: 'step-08-final-report'
description: 'Finalize the sprint report and prepare thesis-ready data export'

outputFile: '{output_folder}/sprint-report-{project_name}.md'
---

# Step 8: Final Report

## STEP GOAL:

To finalize the sprint report, ensure all sections are complete, prepare the QEMU vs Real Device comparison, document limitations, and produce thesis-ready data for mycelium_report.tex.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a technical writing specialist for academic publication
- ✅ Help frame results for thesis quality
- ✅ Honest about limitations — they strengthen the paper
- ✅ User brings the results, you help present them clearly

### Step-Specific Rules:

- 🎯 Focus on polishing and completing the report
- 🚫 FORBIDDEN to re-run tests or modify data
- 💬 Help user write clear, thesis-quality descriptions of results

## EXECUTION PROTOCOLS:

- 🎯 Review entire {outputFile} for completeness
- 💾 Polish and finalize all sections
- 📖 Mark workflow as complete in frontmatter

## CONTEXT BOUNDARIES:

- All benchmark data collected and analyzed
- Bug/fix log complete
- Limitations documented
- This is the final step — no next step file
- Output feeds into mycelium_report.tex

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Completeness Check

"**Final Report: Completeness Check**

Let me review the sprint report for completeness."

Read {outputFile} and check each section:

- [ ] Environment Snapshot — OpenWrt version, packages, modules, flow offloading
- [ ] Link Characteristics — Bandwidth, RTT, encapsulation, CAKE config
- [ ] Device Capacity Baseline — CPU/RAM delta, conntrack scaling, frequency sweep
- [ ] Benchmark Results — All 4 scenarios × 5 runs, with mean+/-stddev
- [ ] Persona Classification — Controlled + real app accuracy, confusion matrix
- [ ] Adaptation Tests — Traffic change, new device, encrypted traffic
- [ ] QEMU vs Real Device — Comparison table with explanations
- [ ] Metric Grades — All 5 groups graded
- [ ] Bug / Fix Log — All issues logged with status
- [ ] Limitations — Documented honestly

"**Report completeness:**
[List checked/unchecked items]

Any sections that need filling in?"

### 2. Polish Results Tables

"**Polishing results tables for thesis quality...**

For each table, ensure:
- All cells filled (no blanks — use N/A if not applicable)
- Units clearly stated
- Mean +/- stddev format consistent
- Comparison ratios calculated

Review the main thesis table:
```
Table X: Real-Device Benchmark Results (Xiaomi AX3000T, N=5 runs)
```

Does this table look complete and ready for the paper?"

### 3. Write QEMU vs Real Device Analysis

"**QEMU vs Real Device: Writing the Comparison**

This section needs clear, honest text explaining:

1. **What's the same:** Both show MycoFlow improves latency over FIFO, CAKE tin separation works
2. **What's different:** Absolute numbers differ (and why — real hardware adds system overhead)
3. **Key insight:** Focus on relative improvement ratio, not absolute ms values

Let me draft a comparison paragraph for your thesis. Review and adjust:

[Draft 2-3 paragraphs comparing QEMU and real-device results, emphasizing:
- QEMU = controlled isolation test
- Real device = system-level validation
- Relative improvements are the meaningful metric
- Any differences explained by hardware realities (conntrack, WiFi driver, thermal)]"

### 4. Write Limitations Section

"**Documenting Limitations**

Every good thesis honestly states its limitations. Based on the sprint:

[Draft limitations based on what was discovered:
- Flow offloading trade-off (if applicable)
- Persona accuracy on encrypted traffic
- Hardware-specific results (MT7981B, may differ on other platforms)
- Conntrack scaling ceiling
- Any other issues discovered]

Review and adjust."

### 5. Write Thesis Export Notes

"**Thesis Export Notes**

Notes for integrating into mycelium_report.tex:

1. **Table X** (main results table) → goes in Chapter [X], Section [X.X]
2. **Confusion matrix** → goes in evaluation section
3. **QEMU comparison** → bridge between simulation and real-world validation
4. **Adaptation test** → strongest contribution evidence
5. **Grade summary** → abstract/conclusion material

Figures to generate:
- Box plots for latency across 4 scenarios
- Conntrack scaling curve (entries vs cycle time)
- Confusion matrix heatmap
- Adaptation timeline (persona classification over time)

Any specific LaTeX formatting requirements?"

### 6. Final Polish

"**Final polish of the complete report...**"

Review {outputFile} for:
1. Flow and coherence between sections
2. Consistent formatting and terminology
3. No duplicate data
4. Proper headers (## Level 2 for main sections)
5. Smooth transitions
6. Professional academic tone

### 7. Mark Complete

Update {outputFile} frontmatter:
- Append `'step-08-final-report'` to `stepsCompleted`
- Set `lastStep: 'step-08-final-report'`
- Set `status: COMPLETE`

### 8. Sprint Complete

"**Sprint Complete!**

Your real device sprint report is finalized at:
`{outputFile}`

**Summary:**
- **Device:** Xiaomi AX3000T (MT7981B)
- **Scenarios tested:** FIFO / CAKE / CAKE+Static / MycoFlow
- **Runs per scenario:** [N]
- **Overall grade:** [grade]
- **Bugs found/fixed:** [count]
- **Limitations documented:** [count]

**Next steps:**
1. Extract tables and figures for mycelium_report.tex
2. Generate box plots and charts from raw data
3. Write thesis chapter referencing these results
4. If needed, re-run this workflow for additional scenarios

**Thank you for running the MycoFlow Real Device Sprint!**"

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- All report sections complete and polished
- QEMU comparison written with honest analysis
- Limitations documented
- Thesis export notes provided
- Report marked as COMPLETE
- User has clear next steps for thesis integration

### ❌ SYSTEM FAILURE:

- Leaving blank sections in the report
- Modifying benchmark data during polish
- Not writing the QEMU comparison
- Hiding limitations
- Not providing thesis integration guidance

**Master Rule:** This is the capstone. Every section complete, honest, and thesis-ready.
