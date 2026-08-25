---
name: start-work
description: Select and scaffold one or more registry-aware OloEngine tasks in separate git worktrees, then hand them to Codex. Use when asked to start work, choose the next task, or create a task worktree; not to implement work in the current base-repository task.
---

# Start OloEngine Work

Read `.claude/commands/start-work.md` from the repository root before doing any
work. It is the authoritative task-selection, registry-sweep, worktree-safety,
and handover procedure. Follow it in full except for the Codex adaptations
below.

## Codex adaptations

- Skip the `.claude/projects/.../memory` junction in section 4. Do not create,
  link, or delete any Codex state outside the repository.
- Keep `HANDOVER.md` as the cross-agent handover artifact, but quote the
  guardrails from `AGENTS.md`, not `CLAUDE.md`.
- Map the source's model rubric to Codex recommendations: `gpt-5.6-sol` with
  high or xhigh effort for subtle/high-blast-radius work; `gpt-5.6-terra` with
  medium effort for normal, well-scoped engine work; and `gpt-5.6-luna` with
  low effort for mechanical, strongly patterned work. Recommendations do not
  authorize changing a task's model.
- The source's "fresh Claude session" means a new Codex task rooted at the new
  worktree. Create it only when the user explicitly asked to create task(s).
  Otherwise, stop after writing the handover brief and report the path, branch,
  model recommendation, and exact prompt for the user to use in a new task.
- Opening a separate editor window remains optional user-interface convenience;
  it is not a substitute for the file-based handover.

This task only selects, scaffolds, and hands off work. Do not implement the
chosen task in the base-repository task after the handover is ready.
