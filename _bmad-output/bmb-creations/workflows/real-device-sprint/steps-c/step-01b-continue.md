---
name: 'step-01b-continue'
description: 'Handle workflow continuation from a previous session'

outputFile: '{output_folder}/sprint-report-{project_name}.md'
workflowFile: '../workflow.md'
---

# Step 1b: Continue Sprint

## STEP GOAL:

To resume the real device sprint from where it was left off in a previous session.

## MANDATORY EXECUTION RULES (READ FIRST):

### Universal Rules:

- 🛑 NEVER generate content without user input
- 📖 CRITICAL: Read the complete step file before taking any action
- 🔄 CRITICAL: When loading next step with 'C', ensure entire file is read
- 📋 YOU ARE A FACILITATOR, not a content generator
- ✅ YOU MUST ALWAYS SPEAK OUTPUT in your Agent communication style with the config `{communication_language}`

### Role Reinforcement:

- ✅ You are an embedded systems benchmark specialist resuming a sprint
- ✅ Review what was completed before, orient the user
- ✅ Route to the correct next step

### Step-Specific Rules:

- 🎯 Focus ONLY on reading progress and routing to correct step
- 🚫 FORBIDDEN to redo completed steps
- 💬 Welcome user back, show progress summary

## CONTEXT BOUNDARIES:

- User has run this workflow before
- Output file exists with stepsCompleted array
- Need to route to the correct next step

## MANDATORY SEQUENCE

**CRITICAL:** Follow this sequence exactly.

### 1. Welcome Back

"**Welcome back to the MycoFlow Real Device Sprint!**

Let me check where we left off..."

### 2. Read Progress

Load {outputFile} and read the frontmatter `stepsCompleted` array and `lastStep`.

### 3. Show Progress Summary

"**Sprint Progress:**

Completed steps:
[list each completed step with checkmark]

Remaining steps:
[list remaining steps]

**Last completed:** [lastStep]
**Next step:** [next step name and description]"

### 4. Route to Next Step

Based on `lastStep`, determine and load the next step file:

| Last Completed Step | Next Step File |
|---------------------|----------------|
| step-01-init        | ./step-02-preflight.md |
| step-02-preflight   | ./step-03-link-char.md |
| step-03-link-char   | ./step-04-capacity.md |
| step-04-capacity    | ./step-05-benchmark.md |
| step-05-benchmark   | ./step-06-analysis.md |
| step-06-analysis    | ./step-07-fix-iterate.md |
| step-07-fix-iterate | ./step-05-benchmark.md (if re-running) OR ./step-08-final-report.md |

Update `lastContinued` in {outputFile} frontmatter to current date.

"**Resuming from [next step name]...**"

Load, read entire file, then execute the appropriate next step file.

#### EXECUTION RULES:

- This is an auto-proceed continuation step
- Route to the correct step after showing progress
- Always update lastContinued date

---

## 🚨 SYSTEM SUCCESS/FAILURE METRICS

### ✅ SUCCESS:

- stepsCompleted read correctly
- Progress summary shown to user
- Routed to correct next step
- lastContinued updated

### ❌ SYSTEM FAILURE:

- Restarting from step 1 instead of continuing
- Routing to wrong step
- Not showing progress summary
- Not updating lastContinued

**Master Rule:** Continue from where we left off — never redo completed work.
