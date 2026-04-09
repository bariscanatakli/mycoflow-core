---
name: 'step-07-fix-iterate'
description: 'Log bugs, apply fixes, decide whether to re-run benchmarks or finalize'

rerunStepFile: './step-05-benchmark.md'
finalStepFile: './step-08-final-report.md'
outputFile: '{output_folder}/sprint-report-{project_name}.md'
---

# Step 7: Fix & Iterate

## STEP GOAL:

To log all discovered bugs, apply code fixes, and decide whether to re-run benchmarks with the fixes or finalize the sprint report.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are a debugging and iteration specialist
- ✅ Help user prioritize bugs by severity and thesis impact
- ✅ Collaborative — user decides what to fix, you help plan the fix

### Step-Specific Rules:

- 🎯 Focus on logging bugs, planning fixes, and deciding next action
- 🚫 FORBIDDEN to run new benchmarks in this step
- 💬 Be honest about what can be fixed quickly vs what's a limitation
- 🔄 This step is the loop hub — can route back to benchmarks or forward to final report

## EXECUTION PROTOCOLS:

- 🎯 Review issues from step-06 analysis
- 💾 Update Bug/Fix Log section of {outputFile}
- 📖 Update frontmatter stepsCompleted when decision is made

## CONTEXT BOUNDARIES:

- Analysis from step-06 identified issues
- Some bugs may require code changes → recompile → redeploy
- Some issues may be documented as limitations (not bugs)
- User decides: fix and re-run, or accept and finalize

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Review Issues from Analysis

"**Fix & Iterate: Let's review what we found.**

From the analysis in step-06, these issues were identified:

[List issues from step-06]

Let's categorize each one."

### 2. Categorize Issues

"**For each issue, let's decide:**

| # | Issue | Category | Severity | Thesis Impact | Action |
|---|-------|----------|----------|---------------|--------|
|   |       | Bug / Limitation / Data Gap | High/Med/Low | Blocks thesis? | Fix / Document / Re-collect |

**Categories:**
- **Bug** — Code defect that can be fixed → recompile, redeploy, re-run
- **Limitation** — Inherent constraint (hardware, protocol) → document honestly in thesis
- **Data Gap** — Missing or insufficient data → re-run specific tests

Let's go through each issue."

Walk through each issue with the user and categorize it.

### 3. Plan Fixes (If Any Bugs)

"**For each bug to fix:**

| # | Bug | Root Cause | Proposed Fix | Files to Change | Recompile Needed? |
|---|-----|-----------|-------------|-----------------|-------------------|
|   |     |           |             |                 |                   |

Let's plan the fixes together. For each bug:
1. What's the root cause?
2. What's the minimal fix?
3. Which source files need changes?
4. Does this require recompilation and redeployment?"

### 4. Apply Fixes

"**Apply the planned fixes.**

For code fixes:
1. Edit the source files as planned
2. Cross-compile: `aarch64-openwrt-linux-musl-gcc ...`
3. Deploy to router: `scp mycoflowd root@<router-ip>:/usr/bin/mycoflowd`
4. Verify: `ssh root@<router-ip> 'mycoflowd --help || echo ok'`

For threshold/config adjustments:
1. Update the relevant configuration
2. Restart daemon to pick up changes

Let me know when fixes are applied."

### 5. Update Bug/Fix Log

"**Recording in the bug/fix log...**"

Update the Bug / Fix Log table in {outputFile}:

| # | Bug Description | Severity | Fix Applied | Status | Re-test Result |
|---|-----------------|----------|-------------|--------|----------------|
|   |                 |          |             | Fixed / Documented / Pending |  |

### 6. Document Limitations

"**For items categorized as limitations:**"

Append to Limitations & Operational Ceiling section of {outputFile}:
- What the limitation is
- Why it can't be fixed (hardware constraint, protocol limitation, etc.)
- Impact on thesis claims
- Suggested future work

### 7. Decision: Re-run or Finalize

"**Decision Time:**

Based on what we've done:

**Bugs fixed:** [count]
**Limitations documented:** [count]
**Data gaps:** [count]

**Your options:**

**[R] Re-run benchmarks** — Go back to step-05 with the fixed code. Re-run affected scenarios. This gives you updated results for the thesis.

**[F] Finalize** — Accept current results (with fixes noted). Proceed to final report. Use this if fixes are minor and don't significantly change the results, or if you're satisfied with the data.

Which do you choose?"

### 8. Present MENU OPTIONS

Display: **Select:** [R] Re-run Benchmarks (back to step-05) [F] Finalize (proceed to final report)

#### EXECUTION RULES:

- ALWAYS halt and wait for user input after presenting menu
- Route based on user's decision

#### Menu Handling Logic:

- IF R: Update {outputFile} frontmatter (append `'step-07-fix-iterate'` to stepsCompleted, set lastStep), increment sprintRun counter, then load, read entire file, then execute {rerunStepFile}
- IF F: Update {outputFile} frontmatter (append `'step-07-fix-iterate'` to stepsCompleted, set lastStep), then load, read entire file, then execute {finalStepFile}
- IF Any other: help user decide, then redisplay menu

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- All issues categorized (bug/limitation/data gap)
- Bugs have planned fixes with root cause analysis
- Fixes applied and verified (if any)
- Limitations documented honestly
- Bug/fix log updated in report
- Clear decision: re-run or finalize

### ❌ SYSTEM FAILURE:

- Ignoring issues from analysis
- Not categorizing issues
- Applying fixes without documenting them
- Running benchmarks in this step (that's step-05)
- Not giving user the re-run vs finalize choice

**Master Rule:** This is the decision hub. Log everything. User decides the path forward.
