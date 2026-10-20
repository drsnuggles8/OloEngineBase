---
name: cleanup-worktree
description: Remove finished (merged, clean, no open PR) OloEngine worktrees, delete their branches, reclaim disk, and heal the issue registry for what merged. Deletes directories and branches, so manual only.
argument-hint: "[branch-slug] (omit to sweep every completed worktree)"
disable-model-invocation: true
---

Clean up finished OloEngine worktrees now that their work is merged.

## 0. Locate the base repo

    git rev-parse --path-format=absolute --git-common-dir    # its parent is $BASE

Run git as `git -C $BASE ...`. If THIS session is inside a worktree you are about to remove, stop
and ask the user to re-run from another window: git cannot remove the worktree you stand in.

## 1. Classify every worktree with the shared gate

    git -C $BASE fetch --prune origin                              # every check compares against origin/master
    python "$BASE/.claude/scripts/worktree_status.py"              # or --slug <slug> when one was given

The script (read-only) prints one row per non-base worktree with a status and the reasons. It is
the same gate `/resume-worktrees` uses, so a worktree this skill refuses is one that skill re-opens.
`completed` means all of: HEAD on `origin/master` (or a merged PR whose head is this HEAD, the
squash form); no open PR for the branch; nothing changed or untracked except disposable output
(`HANDOVER.md`, `OloEditor/assets/tests/visual/`, `OloEditor/SupportedTargetTestGame/`).

- **Slug given** (`$ARGUMENTS`): act on `feature/<slug>` only.
- **No slug**: a sweep. Remove every `completed` worktree in one batch; do not ask one by one.

**Never remove anything not `completed`**, and say why for each. `incomplete-dirty` is the trap the
gate exists for: a branch that never got a commit sits at a master commit and passes the ancestor
test while the tree holds a pile of uncommitted feature work. Discard unmerged work only on the
user's explicit word.

Kinds: `feature` is a `/start-work` branch. `other` (a bot branch such as `renovate/*`, a hand-made
branch) is removed only when `completed`, and its branch is left alone (§3). `detached` is a
before-baseline checkout: `completed` when clean and on master, and then removable.

## 2. Remove each completed worktree

    git -C $BASE worktree remove <path>

If it refuses only because of the disposable output the script counted, re-run with `--force`.
Never `--force` past a `wip_files` entry.

**"Permission denied" / "Device or resource busy" on an already-emptied directory**: the
registration is gone but some process still has the directory as its working directory, almost
always a leftover `bash.exe` / `sleep.exe` / `pwsh.exe` from the removed worktree's own session (not
the git fsmonitor daemon: killing those does not release it). Leaving the empty shell makes the next
`/start-work` on that slug fail, so resolve it:

    pwsh -NoProfile -File $BASE/.claude/scripts/free-locked-dir.ps1 -Path '<path>'         # report holders
    pwsh -NoProfile -File $BASE/.claude/scripts/free-locked-dir.ps1 -Path '<path>' -Kill   # kill them, delete dirs

Run the report form first. If a holder looks live and unrelated (an editor, a running build, an
`OloEditor.exe`), stop and report it instead. Exit code 1 means a directory survived: say so.

## 3. Delete the branch and prune

    git -C $BASE branch -d feature/<slug>
    git -C $BASE worktree prune
    git -C $BASE fetch --prune origin

Only for `feature/*`; never delete an `other` branch (a bot owns it). `-d` refuses an unmerged
branch, and for the *ancestor* form a refusal is a real signal: re-check §1. For the *merged-PR*
form (`landed: merged-pr #N`) `-d` always refuses, because a squash leaves no ancestry; only there
is `-D` correct, and only because the script already matched the merged PR's head to this HEAD and
found no open PR.

## 4. Heal the registry for what merged (same policy as `/start-work` §2b)

For each removed `feature/*` worktree, find its merged PR and what it closes:

    gh pr list --state merged --head feature/<slug> --json number,title,mergedAt,body
    gh api graphql -f query='query{repository(owner:"<owner>",name:"<repo>"){pullRequest(number:<N>){closingIssuesReferences(first:20){nodes{number title state}}}}}'

(`closingIssuesReferences` is not a `gh pr list/view` field on this box; use GraphQL.) Its `state` is
the issue's current state: `CLOSED` is the healthy case. Two sources, two levels of trust:

- **`closingIssuesReferences`** are issues the PR declares it completes. For each still `OPEN`:
  a single-purpose issue the merge fully delivered gets an evidence comment without asking
  (`Completed by #<PR> (merged <date>); worktree removed. Flagging for closure.`) and goes on the
  close list. An umbrella issue it only advanced gets a comment naming the item done and stays open.
- **A bare `#N` in the title or body** is only a mention ("unlike #123", "blocked by #789"). Never
  auto-comment; classify it against the merged diff and list the uncertain ones for the user.

Close only after the user confirms, with a trailing `# OLO_USER_APPROVED` on the `gh issue close`
line (`olo-command-guards` refuses it otherwise).

This session runs on `master`: do not edit or commit project files. Report stale `docs/` checkboxes
and resolved `// TODO`s for a follow-up PR instead.

## 5. Remove the worktree's Claude project directory

Each worktree session leaves `~/.claude/projects/<slug>/` (transcripts). Its `memory/`, if present,
is a junction or symlink into the shared store, made by the user's SessionStart hook; deleting the
project directory removes the link, never the target. Slug: the worktree path with `:`, `\` and `/`
each replaced by `-` and the drive letter lowercased (`C:\repos\OloEngine-foo` →
`c--repos-OloEngine-foo`).

- `memory/` is a link, or absent → `Remove-Item -Recurse -Force "$env:USERPROFILE\.claude\projects\<slug>"`.
- `memory/` is a **real directory with files** (a session from before memory was shared): this is
  the last moment they can be read. Triage, do not bulk-copy: drop task status and anything already
  in `CLAUDE.md`, `docs/agent-rules/` or contradicted by the code; report a durable engine gotcha for
  a follow-up PR into `docs/agent-rules/`; copy a machine/tool/CI fact or a user preference into the
  shared store with a `MEMORY.md` line. Then delete.

Echo each constructed path before deleting it.

## 6. Report

`git -C $BASE worktree list`, then: what was removed, what was skipped and why (with the script's
reasons, especially `incomplete-dirty`), how many worktrees remain and their disk use, the evidence
comments posted, the issues recommended for closure (ask before closing), umbrella issues commented
but kept open, and stale docs/TODOs for a follow-up.
