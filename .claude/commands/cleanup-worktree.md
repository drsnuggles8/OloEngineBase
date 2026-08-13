---
description: Remove finished/merged OloEngine worktrees and free the E: slot (path-derived)
argument-hint: <branch-slug, optional — omit to sweep ALL completed worktrees>
---
Clean up finished OloEngine worktrees now that their work is merged.

## 0. Locate the base repo (derive it, don't assume a path)

    git rev-parse --path-format=absolute --git-common-dir
The parent of that .git directory is the BASE REPO; call it $BASE. Run every command
below with `git -C $BASE ...`. If THIS session is running inside a worktree you're
about to remove, stop and tell me to re-run this from a different window (the base repo
or another worktree) — git can't remove the worktree you're standing in.

## 1. Decide the target set

**Refresh first, on BOTH paths** — every completion check in §2 compares against `origin/master`,
so a stale remote makes a merged branch look unmerged (and vice versa):

    git -C $BASE fetch --prune origin
    git -C $BASE worktree list

- **Slug given ("$1")** → target only feature/$1 and its worktree.
- **No slug** → this is a SWEEP: classify every non-base worktree per §2 and remove all that pass,
  in one batch (don't ask one-by-one).

## 2. Classify each candidate — only "completed" worktrees may be auto-removed

A worktree is **completed (safe to remove)** only when ALL FOUR hold:
  a. **The branch actually landed.** Either `git -C $BASE merge-base --is-ancestor feature/<slug> origin/master`
     (exit 0), **or** a MERGED PR for that head whose `headRefOid` equals the branch's local HEAD:

         gh pr list --state merged --head feature/<slug> --repo <owner/repo> --json number,headRefOid,mergedAt

     The PR form is not optional politeness — a **squash merge defeats `--is-ancestor`**, so a
     squash-merged branch reads as unmerged to (a), to `rev-list`, and to `branch -d`. If the
     ancestor check fails, check the merged PR before concluding the work is unmerged.
  b. **No OPEN PR for the branch.** `gh pr list --state open --head feature/<slug> --repo <owner/repo>`
     must be empty. An open PR means the work is still in flight regardless of what the branch
     looks like locally — **never remove it**, even if (a), (c) and (d) all pass.
  c. **Working tree clean.** `git -C <worktreePath> status --porcelain` is empty
     (ignoring genuine build artifacts you've confirmed disposable).
  d. **Nothing unpushed.** `git -C $BASE rev-list --count origin/master..feature/<slug>` is 0
     — skip this one when (a) was satisfied by the squash-merged-PR form, where it cannot be 0.

**TRAP — "merged HEAD but dirty tree" is NOT completed.** A worktree whose branch HEAD
is an ancestor of master can still hold a large pile of UNCOMMITTED feature work (the
branch was cut from master and the feature was never committed). `--is-ancestor` passing
is necessary but NOT sufficient — you MUST also check (b). If `status --porcelain` shows
modified tracked files or new untracked source/tests, that worktree is live WIP: **STOP,
do not remove it, and report it** even though its branch looks "merged".

For anything that fails the gate, leave it and say why. Never silently delete:

- uncommitted changes or untracked non-artifact files (open WIP),
- unpushed/unmerged commits (`git -C <worktreePath> log origin/master..HEAD`),
- a branch with an OPEN PR or no PR and unmerged commits.
Only discard unmerged work if I explicitly tell you to.

## 3. Remove each completed worktree (frees the disk, and the E: slot if it was on E)

    git -C $BASE worktree remove <worktreePath>
If it refuses because of leftover build artifacts / untracked files and you've
confirmed they're disposable, re-run with --force. (Do NOT --force past real WIP — that's
the §2 trap.)

## 4. Delete the merged branch and prune stale registrations

    git -C $BASE branch -d feature/<slug>        # -d (safe): refuses if not merged
    git -C $BASE worktree prune
    git -C $BASE fetch --prune origin            # drop the deleted remote branch
Run branch -d per removed worktree. `branch -d` refusing is a signal the branch wasn't
actually merged — re-check §2, don't reach for -D unless I told you to discard.

## 5. Close the loop for each removed worktree — heal the registry (same policy as `/start-work` §2b)

A worktree only gets removed here because its PR **merged** — so the work is DONE, and any
issue/doc/TODO that still reads "open" is now stale. Heal it. (If the PR used a closing
keyword like `Closes #N`, GitHub already closed the issue on merge; the gap you're fixing is
the PR that referenced an issue with a bare `#N` and never closed it.)

For each REMOVED worktree, find its merged PR and the issues it touched:
    gh pr list --state merged --head feature/<slug> --repo <owner/repo> --json number,title,mergedAt,body,closingIssuesReferences
**Two different sources, two different levels of trust — do not merge them:**

- **`closingIssuesReferences`** — GitHub's own resolution of the PR's closing keywords. These are
    the issues the PR *declares* it completes. Safe to auto-comment on.
- **A bare `#N` anywhere in the title/body** — this is only a *mention*. It is routinely a
    cross-reference ("unlike #123", "follows #456", "blocked by #789", a linked umbrella tracker),
    NOT a claim of completion. **Never auto-comment on these.** Classify each one explicitly
    against the merged diff; comment only on the ones you can show the PR actually delivered, and
    otherwise leave them untouched and list them in the report for the user to judge.

For each candidate that is **still OPEN** (`gh issue view <N> --json state`):

- **Single-purpose issue the merge fully delivered** → **auto-post an evidence comment**
    (additive, low-risk — no ask):
        gh issue comment <N> --repo <owner/repo> --body "Completed by #<PR> (merged <date>); worktree removed. Flagging for closure."
    then add it to a *close-recommendation list* and **close ONLY after I confirm** (closing
    is the outward, decisive step — an explicit opt-in, like commit/push).
- **Umbrella / multi-item issue** the merge only partially advanced — one checklist item of
    many, e.g. a `#308`-style follow-ups tracker → comment WHICH item is now done, **leave it
    open**, and do NOT add it to the close list. Merging one item ≠ the issue is finished.
- **Uncertain** the merge fully resolves it → don't comment; just list it in the report so I
    decide. Better a surfaced lead than a wrong public claim.

**Docs / TODOs — flag, don't fix here.** This skill runs in $BASE on `master`, and the
commit/push guardrail applies to master too — do NOT edit or commit project files in this
session. If you spot a `docs/` checkbox or a `// TODO` the merged work resolved, **report it**
as stale so it gets ticked in a follow-up PR; don't fix it in place.

## 5b. Remove the worktree's Claude project dir — and check its memory was actually shared

`/start-work` §4 junctions each worktree's `~/.claude/projects/<slug>/memory` at the BASE repo's
memory dir, so a worktree session reads and writes the one shared store and nothing it learns is
orphaned. Removing the project dir deletes the *junction*, never the target — both
`Remove-Item -Recurse -Force` and `rm -rf` decline to follow it (verified).

The slug is the worktree's absolute path with `:` → `-` and each `\`/`/` → `-`
(e.g. `E:\repos\OloEngine-foo` → `e--repos-OloEngine-foo`). For each worktree you removed:

    $slug = "<worktreePath>" -replace ':','-' -replace '[\\/]','-'
    $mem  = "$env:USERPROFILE\.claude\projects\$slug\memory"
    (Get-Item $mem -ErrorAction SilentlyContinue).LinkType     # expect: Junction

- **`Junction`** → nothing to salvage. Delete the project dir:
  `Remove-Item -Recurse -Force "$env:USERPROFILE\.claude\projects\$slug"`
- **A real directory** (the worktree predates the junction, or it was created before `/start-work`
  could link it) → its contents are about to become unreachable and **this is the last moment
  anything can read them**. Triage before deleting; do NOT bulk-copy, since roughly 2 in 3 such
  files are worthless and a few are actively wrong:
  - task/branch status ("what shipped on feature/x") → drop, the work merged
  - already covered by `CLAUDE.md`, `docs/agent-rules/` or a guide → drop, but grep before assuming
  - contradicted by current code → drop, and fix the wrong claim wherever else it lives
  - a durable **engine** gotcha → belongs in the repo: add it to the relevant
    `docs/agent-rules/notes-*.md` and link it from both indexes. Per §5 this session must not
    commit — **report it for a follow-up PR**
  - a machine/tool/CI fact, a flaky test, or a user preference → copy into the base memory dir,
    normalise frontmatter to `metadata: { type: user|feedback|project|reference }`, add a
    `MEMORY.md` line
- **Missing entirely** → the session never wrote memory. Just delete the project dir.

Report which slugs were junctioned (the expected case) and anything you salvaged or recommended for
a follow-up PR.

## 6. Report the final state

    git -C $BASE worktree list
List what was removed, what was SKIPPED and why (esp. any §2-trap WIP worktrees), and how
many worktrees remain on E:\ (the cap is 2). Then the §5 loop-closing results: which issues
got evidence comments, which you **recommend closing (ask before closing)**, which umbrella
issues you commented-but-kept-open, and any stale docs/TODOs to fix in a follow-up.
