---
name: resume-worktrees
description: After a reboot or closed windows, re-open a VS Code Insiders window for every unfinished OloEngine worktree that is not already open, and say how to resume each worker session.
argument-hint: "[branch-slug] (omit to resume every unfinished worktree)"
disable-model-invocation: true
---

Re-open the editor windows for in-progress OloEngine worktrees. Opening windows is non-destructive:
show the plan, then do it without asking.

## 1. Classify the worktrees with the shared gate

    git rev-parse --path-format=absolute --git-common-dir    # its parent is $BASE
    git -C $BASE fetch --prune origin
    python "$BASE/.claude/scripts/worktree_status.py" --json  # or --slug <slug> when one was given

This is the gate `/cleanup-worktree` uses, so a worktree it refuses to remove is one this skill
offers to re-open and nothing falls between the two. Candidates are every `incomplete-*` worktree.
Skip `completed` ones (mention them so the user can run `/cleanup-worktree`) and every `detached`
one (a baseline checkout holds no task).

## 2. Find the windows that are already open

    pwsh -NoProfile -File $BASE/.claude/skills/resume-worktrees/scripts/open-vscode-folders.ps1

It prints the folder name of every open VS Code / Insiders window. A worktree is open when its
folder leaf name (`OloEngine-<slug>`) is in that list. If no worktree folder names appear while
windows are clearly open, the user may have customised `window.title`: say so rather than opening
duplicates.

## 3. Show the plan, then open

Print one row per worktree: status, open (yes/no), and whether it has a `HANDOVER.md`. Open each
candidate that is not open:

    code-insiders -n "<path>"        # fall back to: code -n "<path>"

If nothing needs opening, say so and stop.

## 4. Report, and resume the sessions rather than restarting them

List what was opened, what was already open, and what was skipped as completed or detached.

For each opened worktree, the worker's real state lives in its **previous conversation**, not in
`HANDOVER.md` (whose *State* section dates from scaffolding). Tell the user to resume it in the new
window: open the session history and pick the session named after the slug (`/start-work` asks for
`/rename <slug>`), or run `claude --resume <slug>` / `claude --continue` in a terminal in that
worktree. An active `/goal` is restored on resume. Only when no earlier session exists, start fresh
with the kickoff from `/start-work` §5c.
