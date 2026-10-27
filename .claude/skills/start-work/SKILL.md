---
name: start-work
description: Start one or more new OloEngine tasks (default 1), each in its own git worktree. Sweeps and heals the scored issue registry, picks the work, scaffolds worktree + HANDOVER.md and opens a window per task. Never implements.
argument-hint: "[count]"
disable-model-invocation: true
---

Start one or more new pieces of work on OloEngine, each in its own git worktree. Work entirely from
what the git/GitHub registry tells you; do not rely on the user to list what to avoid.

> **Scope.** This skill picks the work and scaffolds the worktree + window. Everything after the
> handover (implement, verify, review, commit, push, PR, CI and CodeRabbit) belongs to
> `docs/process/task-loop.md`, which the worker session follows. This skill never implements, and
> never commits, pushes or opens a PR itself.
>
> **Local environment (maintainer's machine).** Three rules describe one setup rather than the
> project: the `C:\repos` worktree root (§4), VS Code Insiders (§5b) and the model rubric
> ([model-rubric.md](model-rubric.md)). Adapt them elsewhere; everything else is repo-general.

## 0. How many tasks (the count argument)

`$ARGUMENTS` is an optional positive integer **N**, default 1. Steps 1-2 run once; step 3 picks
**N mutually distinct tasks** in one pass; steps 4-5 run once per task, so N tasks ⇒ N worktrees,
N branches, N `HANDOVER.md` files and N windows. If you cannot find N strong, non-overlapping
candidates, start as many as you confidently can and say how many and why. Never invent filler work.

## 1. Locate the base repo

    git rev-parse --path-format=absolute --git-common-dir    # its parent is $BASE (C:\repos\OloEngineBaseBase)
    git -C $BASE fetch --prune origin

## 2. Sweep the registry: this IS the exclusion list

    git -C $BASE worktree list
    git -C $BASE branch -a
    gh pr list --state open
    gh issue list --state open --assignee "*"
    gh pr list --state merged --limit 40       # recently FINISHED work: docs/TODOs often still read "open"
    gh issue list --state closed --limit 40
    python "$BASE/scripts/issue_scores.py" rank   # scored backlog: canonical priority (docs/process/issue-scoring.md)
    python "$BASE/scripts/issue_scores.py" lint   # everything that makes the backlog lie

A topic with a worktree, a branch (local or `origin/*`), an open PR or an assigned issue is
**off-limits as in flight**. A topic in a recently merged PR or closed issue is **off-limits as
done**: that is your ground truth when a doc or TODO still reads "open". There is no separate
hand-kept list.

**`lint` is not optional and is not `rank`.** `rank` lists only issues carrying an `olo-score`
block, so an unscored issue is not ranked low, it is invisible. `gh issue create --body` bypasses the
templates that score issues at birth. Score whatever `lint` returns against
[issue-scoring.md](../../../docs/process/issue-scoring.md) §4 **before** picking, write the block
into the issue body (the only source of truth), and treat the re-run `rank` as canonical. #847,
unscored, scored rank #2 once it was scored.

**Clear the rest of `lint` before picking: the user hates stale issues** (issue-scoring.md §6.4).

- **`merged:`** (work merged since the score): read the PR and the code. Done → evidence comment
  plus the close list. Something remains → `issue_scores.py set N <changed axes>`.
- **`stale:`** (score older than 45 days): re-check the premise against the code, then `set N ...`
  (or `set N` alone) or recommend closure.
- **`PULL?`** (unconfirmed Pull): never rule on it. Ask the user in the one upfront question
  round, then `issue_scores.py pull N` (yes) or `pull --off N` (no), only on their explicit word,
  with `# OLO_USER_APPROVED`.
- **Stale `blocked_by`** (blocker closed): drop it with `set N blocked_by=[...]`, and name what
  still holds the issue, if anything. A blocker that merged as a no-go is not a green light.
- **Unresolvable `blocked_by`**: fix the lookup (`gh auth status`, network, typo) and re-run.

Use `set` rather than hand-editing a body: it keeps every field and writes UTF-8. If you must read a
body yourself, decode gh's output as UTF-8 explicitly; bare `text=True` decodes as cp1252 and turns
every em-dash into `â€"`, which you then write back.

## 2b. Heal the registry as you sweep

The sweep routinely *proves* an open issue done, a `docs/` item stale or a `// TODO` resolved.
Capture that proof now; it is the cheapest moment.

- **Code + history demonstrably prove an open issue resolved** (a merged PR covers it, the failing
  runs predate the fix, the symbol now exists): post a factual evidence comment without asking —
  `gh issue comment <N> --body "Appears resolved by #<PR> (merged <date>): <evidence>. Flagging for closure."`
  — state evidence, never a verdict, and add it to the close list for the report. **Never close it
  here**: closing waits for the user.
- **Partially done**: comment what is done and what remains; leave it open; not on the close list.
- **Not certain**: no comment; list it in the report so the user decides.
- Stale `docs/` checkboxes and resolved `// TODO`s: in scope for the chosen task's PR if related,
  otherwise list them in the report.
- Durable reusable facts that are not registry items go to persistent memory.

## 3. Pick the task(s)

Build a slate of at least N + 2 candidates not in flight, then pick N.

**The issue score is canonical.** For GitHub-issue work, `issue_scores.py rank` is the priority
order: take the top **unblocked** issues not in flight. Take `rank`'s own `next:` line as the
authoritative single pick; it already applies the Pull-override (a user-confirmed `Fun ≥ 8`,
`fun_by: user`, jumps the queue), the low-value floor and the Tier-1 slot.

> **Read the printed ORDER, not the score column.** An issue under the Cost-of-Delay floor
> (`low-value:<CoD>`) sorts below every normal issue whatever its ratio, so `#607 0.23` can sit above
> `#451 0.83 low-value:5`. Take the topmost unblocked rows in printed order. A `low-value` issue is a
> legitimate deliberate pick; it must not displace real work by default.

**Tier-1 slot** (issue-scoring.md §3.7). For N = 1, follow `next:` when it switches to the
`tier-1:` candidate. For N ≥ 2, one slot always goes to the `tier-1:` candidate. An MCP pick counts.
Work that is Tier 1 by nature (CI, test infra, MCP, dev tooling) but lacks the `tooling` label is
mislabelled: add the label during the sweep.

**N > 1: the tasks must be mutually distinct and non-overlapping**, ideally different subsystems and
disjoint files. If two candidates would touch the same files, keep one and replace the other.

**Justify going down.** You may pick below the top of the rank, or a non-issue task over it, when it
is clearly higher leverage or more urgent; say what you skipped and why. When a score fights reality
(the top issue is mostly done, its axes look wrong), fix the score (§2b) rather than silently
overriding it.

**Non-issue work** has no score, so the leverage tiers govern it. The score's Capability axis is the
same instinct quantified. File and score a substantial non-issue task so the loop converges on one
queue.

- **Tier 1, force multipliers** (start here): engine MCP / diagnostics and C++ tooling MCPs; CI/CD
  speed and reliability (mine the runner logs); test infrastructure; `CLAUDE.md` + `docs/agent-rules`.
- **Tier 2, signal hygiene and latent bugs**: build warnings; `OloEditor/OloEngine.log` warnings
  and errors; runner-log warnings; SonarQube issues; stubs that silently no-op (usually a missing
  feature, not cleanup).
- **Tier 3, debt**: module review for smells/perf/design; refactor, slim, remove dead code (ideally
  by implementing the missing feature); reduce lines; architecture review; CMake/config; deps;
  modern C++.
- **Tier 4, backlog and features**: GitHub issues; task markdown in `docs/`; `// TODO`s; features
  mined from other engines (UE5.8, o3de, pbrt-v4, Unity), papers and books (GPU Zen, Ray Tracing
  Gems, PBR 4th ed.). Big renderer features are below the multipliers, not off the table.

**Docs and TODOs lie: verify "not done" against the code.** The loop routinely finishes work without
ticking the doc, issue or TODO. Before committing to a doc- or TODO-sourced task, grep for the
feature, read the module and cross-check the merged PRs from §2. Already implemented means it is a
doc fix, not a task.

**Right-size the unit of work.** A worktree + window + PR is real overhead. Batch Tier 2 hygiene by
class ("every `-Wfoo` in `<module>`"), not one warning per loop.

**Keep a large Tier 1/4 epic whole by default**: one long-running Fable 5.1 session that fans
sub-pieces out to subagents and lands the whole issue as one PR
([model-rubric.md](model-rubric.md#the-whole-epic-fable-51-session)). Slice only when the issue or
user explicitly wants incremental PRs (as #429 floating-origin did), when the work is unbounded
research with no single "done", or when the user asks for a slice. When you slice, say so in the
HANDOVER, justify it like going down the rank, and file and score the follow-up.

## 4. Create the worktree + branch (once per task)

Use a descriptive slug: the branch name alone should explain the work. From the **Bash tool**:

    bash "$BASE/.claude/skills/start-work/scripts/new-task-worktree.sh" <slug>

It creates `C:/repos/OloEngine-<slug>` on `feature/<slug>` from `origin/master` with no upstream,
refuses a path inside `$BASE`, an existing path or an existing branch, and prints the absolute path.
Echo that path and the base sha in your report. The script's header says why each guard exists
(ccache's `base_dir`, a once-nested worktree, a bare push that once landed on `master`).

Nothing to set up for memory: Claude Code derives the auto-memory directory from the git
repository, so a worktree session reads and writes the base repo's store (verified 2026-10-20).

## 5. Hand over to a fresh session in the worktree (once per task)

A new window runs its own Claude process and does not inherit this conversation, so the handover is
a file. The worker's contract is `docs/process/task-loop.md`: it runs to a green, self-reviewed,
thread-clean PR and stops there. Merging and closing stay the user's.

**5a. Write `HANDOVER.md` at the worktree root** (git-ignored) for a reader who knows nothing about
this conversation:

- **Task**: the task, issue # + link (or other source), its score + rank position from `rank` (or
  the leverage tier for non-issue work), and one line on why it was picked.
- **Branch / worktree**: `feature/<slug>` at `<path>`, based on `origin/master` @ `<sha>`.
- **Recommended model + effort**: per [model-rubric.md](model-rubric.md), with one line of why.
- **Registry snapshot**: the off-limits list from §2. For N > 1, also every sibling task launched in
  this batch with its worktree path and the directories/files it is expected to change. A worker
  that finds a bug in a sibling's files files it and messages that sibling instead of fixing it
  (task-loop Phase 1a, *owned elsewhere*).
- **Plan**: the approach, covering the full scope (every acceptance criterion on the issue) unless
  you deliberately sliced (§3), in which case name what is deferred. For a whole-epic session,
  structure it as the fan-out in [model-rubric.md](model-rubric.md#the-whole-epic-fable-51-session).
- **Verification axes**: the grid for **this** task, decided here, not at reporting time. Start from
  task-loop §2a's table (renderer `{OpenGL, Vulkan} x {Forward, Forward+, Deferred}`, serialization
  `{scene YAML, asset pack, save-game}`, scripting `{C#, Lua}`, physics `{Box2D, Jolt}`, assets
  `{loose, cooked}`) and write **every** dimension the subsystem's row lists, including the
  conditional ones (MSAA, upscale, resolution, a prior on-disk version), each as a cell or with a
  one-line reason it is unreachable. Mark each cell **artefact-backed** or **live-only** (a
  Vulkan cell is artefact-backed when its scene suite runs in the `--olo-rhi=vulkan` process,
  `OloEngine/tests/vulkan_scene_filter.txt`; live-only otherwise, with the reason). A cell known unreachable
  up front (no hardware, backend lacks the feature) is decided here with its reason. A single-path
  change says so in one line ("single path: forward only, 2D feature"); omitting the section is not
  that decision.
- **State**: what is already done and verified (usually nothing).
- **Next steps**: the concrete first actions.
- **The loop**: "Follow `docs/process/task-loop.md`, Phase 0 through Phase 7." Add only what this
  task overrides or extends ("visual evidence from four angles", "tick each acceptance checkbox
  with evidence; the PR's `Closes #N` shuts it on merge").
- **Guardrails**: one line: "`CLAUDE.md` → *Committing and publishing* applies; on this `feature/*`
  branch commit, `git push -u origin feature/<slug>` and opening the PR are pre-authorized." Do not
  restate the rules: `CLAUDE.md` loads in every session and `olo-command-guards` enforces them.

**5b. Open the worktree in a new window**: `code-insiders -n <path>` (fall back to `code -n`).
`-n` keeps this window and every other task window alive. Do it, then say you did.

**5c. Kickoff.** A new window's Claude does not auto-start. Tell the user, per task: in the new
window run `/model <model>`, `/effort <level>`, `/rename <slug>` (so `--resume` finds the session by
name after a reboot), make sure the session is in auto mode, then start it with this goal. The
worker runs unattended for hours (CI alone is ~1.5–2 h), and `/goal` keeps it going: a separate
evaluator checks the condition after every turn, waits while CI polls run in the background and
checks in on stalled ones.

```text
/goal Follow ./HANDOVER.md and docs/process/task-loop.md to the Phase 6 exit gate. Done when this conversation shows that gate met: the PR is MERGEABLE, every check is SUCCESS, the unresolved review-thread count printed by the Phase 6 query is 0, and the PR body has a complete verification matrix plus the Found and fixed and Filed, not fixed sections. Also done if the session states a blocker that only the user can clear.
```

For a session the user will drive by hand instead: `Read ./HANDOVER.md and continue the task it describes.`

**5d. Then stop.** Do not implement anything in this session. Final report: for **each** task, the
task, worktree path + branch, recommended model + effort and the kickoff (a table for N > 1); plus
the §2b findings: evidence comments posted, issues recommended for closure (ask before closing),
the unscored issues `lint` surfaced with the scores and resulting ranks you gave them, what you did
with each `merged:` / `stale:` finding, every `PULL?` for the user to confirm, the tier-1 slot state
and whether you followed it, and any durable facts written to memory.
