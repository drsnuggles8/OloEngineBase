---
name: cleanup-worktree
description: Safely remove finished OloEngine git worktrees after their branches or pull requests have landed. Use to reclaim merged worktrees; never to discard unmerged or dirty work.
---

# Clean Up OloEngine Worktrees

Read `.claude/commands/cleanup-worktree.md` from the repository root before
acting. It is the authoritative completion gate, merged-PR verification,
worktree-removal, branch-deletion, and registry-healing procedure.

Apply it in full with one Codex adaptation: skip section 5b entirely. It only
cleans Claude's per-worktree state. Do not delete, link, or otherwise modify
any path under the Codex home directory while cleaning a worktree.

All of the source procedure's safety gates still apply: never remove the
current worktree, unmerged or unpushed commits, an open PR, or real dirty work.
Use force only for confirmed disposable build artifacts after the completion
gate has passed.
