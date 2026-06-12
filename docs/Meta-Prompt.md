# Meta-Prompt: How to produce a step-by-step agent execution plan

Use this prompt template with Opus. Fill in the `{{goal}}` section with your
actual objective, then append the instruction block below.

---

## Template

I would like to produce a plan to {{goal}}.

Write an execution plan in `tmp/plan/` structured for sequential handoff to
independent Sonnet agents. Each agent is cleared between steps — no shared
context except what's on disk.

### Requirements for the plan

1. **`AGENT-PROMPT.md`** — A single universal prompt given identically to every
   agent. It should instruct the agent to:
   - Read `checklist.md` and find the first unchecked step
   - Read that step file and any prior handoff file
   - Execute the step (worktree, test, implement, test, commit, merge back)
   - Mark the checklist, read the next step, write a handoff file

2. **`checklist.md`** — One line per step with `- [ ]` checkboxes and the
   step file name. Agents mark their step done before finishing.

3. **`step-NN.md`** files (one per step) — Each must be fully self-contained
   for a cleared agent. Include:
   - **Project context**: what the project is, what branch to integrate to
   - **Why**: the motivation for this specific change (reviewer comment, design goal, etc.)
   - **Setup**: exact commands to create a worktree and branch off the integration branch
   - **Verify GREEN baseline**: exact build/test commands and expected result
   - **The change**: precise description of what to edit, found by content not line number
     (line numbers shift between steps). Include enough surrounding context to locate blocks.
   - **Verify GREEN after**: same build/test commands, must still pass
   - **Spot checks**: grep commands to confirm the change took effect
   - **Commit and merge back**: exact git commands including commit message (use HEREDOC)
   - **Cleanup**: worktree removal
   - **Handoff**: instruction to read the next step file and write `handoff-NN-to-MM.md`

4. **`README.md`** — Documents the strategy, baseline state, file index.

### Constraints on step design

- Build directories go in `/build/<branch-name>/` for isolation from other work.
  Each step's worktree gets its own build dir (e.g., `/build/step-01/foo/`).
- Steps go GREEN to GREEN. Each step must leave tests passing.
- Steps should be ordered by dependency — if step N changes a file that step M
  reads, N comes first.
- Multiple edits to the same file across steps are fine; instruct agents to
  find blocks by content (not line number) since prior steps shift things.
- Each step should be one coherent logical change (not multiple unrelated edits).
- Prefer smaller steps that are easy to verify over large steps that might fail.
- If a step needs new tests, say "write tests first" explicitly.
- The commit message should explain WHY, not just WHAT.

### What NOT to include

- Don't write the actual code changes as diffs — describe what to find and
  what to replace it with using fenced code blocks.
- Don't include a "final verification" step — the last real step's GREEN check
  is sufficient.
- Don't plan for history cleanup — that happens after all agents finish and is
  out of scope.

### Before writing the plan

- Explore the codebase to understand current state
- Identify the exact files and functions that need to change
- Verify the current test suite passes (note the count)
- Determine the correct integration branch
- Create the integration branch if it doesn't exist


---
