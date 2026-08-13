---
description: Start one or more new OloEngine tasks (default 1), each in its own git worktree (registry-aware, path-derived)
---
Start one or more new pieces of work on OloEngine, each in its own git worktree. Work
entirely from what the git/GitHub registry tells you — do not rely on me to list what to
avoid.

> **Scope of this command.** It picks the work and scaffolds the worktree + window. Everything
> after the handover — implement, verify, review, commit, push, PR, CI and CodeRabbit — belongs to
> `docs/process/task-loop.md`, which the worker session follows. This command never implements.
>
> **Local environment (maintainer's machine).** A few rules below describe one specific setup
> rather than the project: the **E: drive** two-worktree cap (§4), **VS Code Insiders** as the
> editor (§5b), and the **Opus 5 / Sonnet 5 / Fable 5** model rubric (§5a). Adapt them if you
> work somewhere else; everything else is repo-general.

## 0. How many tasks to start (the count argument)

This skill takes an optional integer argument — `$ARGUMENTS` — the number of tasks to
start in this run. Default to **1** when nothing is passed (or the argument isn't a
positive integer), which reproduces the original single-task behavior exactly. Call this
count **N** (so `/start-work` ⇒ N=1, `/start-work 5` ⇒ N=5).

Steps 1-2 (locate the base repo, sweep the registry) run **once** regardless of N. Step 3
picks **N mutually-distinct tasks** in a single pass. Steps 4-5 then run **once per chosen
task** — each task gets its own worktree, branch, `HANDOVER.md`, and freshly-opened VS
Code Insiders window, so N tasks ⇒ N parallel windows. If N exceeds the number of strong,
genuinely non-overlapping candidates you can find, start as many as you confidently can
and say how many you actually launched and why (don't invent filler work to hit the
number).

## 1. Locate the base repo (derive it, don't assume a path)

Run from wherever this session is (any worktree works):
    git rev-parse --path-format=absolute --git-common-dir
The parent of that .git directory is the BASE REPO; its parent folder is the
worktree root (e.g. D:\repos). Call the base repo $BASE and the root $ROOT.
Then refresh the registry:
    git -C $BASE fetch --prune origin

## 2. Discover everything already in progress OR recently finished — this IS the exclusion list

    git -C $BASE worktree list            # active worktrees + which drive each is on
    git -C $BASE branch -a                # every local + remote branch
    gh pr list --state open               # open PRs (in flight)
    gh issue list --state open --assignee "*"   # claimed issues
    gh pr list --state merged --limit 40  # recently FINISHED work — often still looks "open" in docs/TODOs
    gh issue list --state closed --limit 40     # closed issues — ditto
    python "$BASE/scripts/issue_scores.py" rank # scored, ranked backlog — canonical priority for GitHub issues (docs/process/issue-scoring.md)
Any topic represented by a worktree, a branch (local OR origin/*), an open PR, or an
assigned issue is OFF-LIMITS as in-flight. Anything in a recently-merged PR or closed
issue is OFF-LIMITS as already-done — this is your ground truth that a doc/TODO which
still reads "open" is actually finished (see step 3). Read the branch / PR titles — they
tell you what each session did or is doing. Do not duplicate any of it. There is no
separate hand-maintained list; if it isn't in the registry, treat it as fair game.

## 2b. Heal the registry as you sweep — don't let investigation findings evaporate

The step-2 sweep plus the code-vs-docs verification in step 3 routinely *prove* that an
open issue is already done, a `docs/` item still reads "open" after the work shipped, or a
`// TODO` is resolved. That proof is exactly what keeps the registry honest for the NEXT
loop — capture it, don't discard it once you've used it to deselect a task. (You are doing
the full sweep with the evidence fresh; this is the cheapest moment to fix it.)

**When the code + git history DEMONSTRABLY prove an open issue is resolved** — a merged PR
covers it, the failing CI runs predate the fix, the symbol/feature now exists in the code:

- **Auto-post a factual evidence comment** (additive, low-risk — do this without asking):
        gh issue comment <N> --repo <owner/repo> --body "Appears resolved by #<PR> (merged <date>): <one-line evidence>. The failing CI runs predate that merge — flagging for closure."
    State the EVIDENCE, never a verdict: "appears resolved by #X; failing runs predate it",
    not "closing this" (you are NOT closing it here). Quote concrete refs (PR #, merge date,
    file/symbol that now exists).
- **Do NOT close the issue.** Collect it into a *stale-registry list* and surface that list
    in the final report (§5d) recommending closure; close ONLY after the user confirms
    (closing is the outward-facing, decisive step — it stays an explicit opt-in like commit/push).
**When it's only PARTIALLY done** (an interim/retry landed but the root cause is still open —
e.g. a flaky test got a CI retry but the race is unfixed): comment what's done vs what
remains, leave it open, and do NOT add it to the close list.
**When you're not certain it's resolved:** don't comment — just note it in the report's stale
list so the user decides. Better a surfaced lead than a wrong public claim.
Same spirit for stale `docs/` checkboxes and resolved `// TODO`s: if they relate to the
chosen task, fixing them is in-scope for that task's PR (per CLAUDE.md "close the loop"); if
unrelated, list them in the report.

**Durable reusable facts that aren't a registry item** — "Jolt is now pinned to a fixed SHA",
"the build is warning-clean under both MSVC and clang", "the MCP camera tools already exist" —
go to **persistent memory**, not a GitHub comment, so the next loop doesn't re-derive them.

## 3. Pick the task(s)

Identify a slate of candidate tasks not already in progress (at least N + 2 so you have
room to choose), then pick **N** of them (N from step 0; default 1). The sources
below are sorted by **leverage** — how much finishing the task speeds up and de-risks
*every later task*, not how valuable it is in isolation. Because this skill runs on a
loop, prefer work that compounds: a faster CI run or a sharper MCP probe pays off on
every future change; a one-off feature pays off once. Front-load the tooling, testing,
and feedback-loop work that makes adding the *next* feature fast and bug-resistant.

**When N > 1 the chosen tasks MUST be mutually distinct and non-overlapping** — ideally
different subsystems and disjoint files — because they run simultaneously in separate
worktrees/branches and must not collide on the same code or produce conflicting PRs. If
two strong candidates would touch the same files, pick one and replace the other from
your slate.

**Selection rule — the issue score is canonical; tiers govern the rest.** For
GitHub-issue work the ranked backlog from `issue_scores.py rank` (§2) IS the priority
order: pick the top **unblocked** issue(s) not already in flight (step 2), honoring the
**Pull-override** (a `Fun ≥ 8` issue legitimately jumps the queue — a designed lever, not a
deviation; the rank already floats it). For N > 1, take the top-N unblocked that are also
mutually distinct per the rule above. See docs/process/issue-scoring.md.

> **Read the printed ORDER, not the score column — they can disagree.** The list is not
> sorted by score alone: an issue whose Cost of Delay is below the floor (flagged
> `low-value:<CoD>`) sorts *below every normal-value issue regardless of its number*, so you
> will see e.g. `#607 0.23` above `#451 0.83 low-value:5`. That is correct, not a sorting
> bug — a ratio can't tell "cheap and worth doing" from "cheap and not worth a slot", so the
> floor demotes near-valueless work that a small effort denominator would otherwise float
> (issue-scoring.md §3). **Take the topmost unblocked rows in printed order**, and treat the
> script's own `next:` line as the authoritative pick — it already applies both the
> Pull-override and the floor. A `low-value` issue is still a legitimate deliberate pick
> (a cheap Friday chore); it just must never displace real work by default.

The leverage **tiers below still govern NON-issue work** — warnings, SonarQube smells,
log/runner errors, doc sharpening, dead code — which has no GitHub issue and therefore no
score. They also explain *why* the scores land where they do: the score's **Capability**
axis is the "leverage" instinct quantified, so a high-Capability issue and a Tier-1
force-multiplier are the same bet. When a non-issue task is substantial, file it as an
issue and score it (born scored via the feature template) so the loop converges on one
ranked queue over time.

**Justify going down.** You MAY pick below the top of the rank — or a non-issue task over
the top-scored issue — when it's clearly higher leverage or more urgent; when you do, say
so explicitly (name what you skipped and why). And when the score fights reality — the top
issue is actually mostly done, or its axes look wrong — that's the §2b registry-healing
moment: fix the score in the issue body (or flag it for closure), don't silently override.

**Tier 1 — Force multipliers** (make every later task faster & less bug-prone — start here):

  1. Engine MCP / diagnostics server & C++ tooling MCPs — richer probes, new capture targets, LLM-driven inspection.
  2. CI/CD speed & reliability — runner time, build caching, parallelism, flaky-test fixes (mine the GitHub runner logs).
  3. Test infrastructure — coverage gaps, suite speed, new fixtures / harnesses, visual-regression plumbing.
  4. CLAUDE.md + docs/agent-rules — anything that makes the next agent run sharper.

**Tier 2 — Signal hygiene & latent bugs** (keep the feedback channels clean and honest):
  5. Warnings from building the project.
  6. OloEditor/OloEngine.log warnings & errors.
  7. GitHub runner-log warnings.
  8. SonarQube issues (dead code, smells, duplicate code).
  9. Stubs / placeholders that silently no-op (most likely indicating missing features, not cleanup potential).

**Tier 3 — Debt & maintainability** (compounds, but doesn't block):
  10. Review a module for code smells / perf / design.
  11. Refactor a module, slim the API, remove dead code (ideally by implementing the missing feature for it or integrating the code into the engine).
  12. Reduce total lines of code.
  13. General "bad architecture" review.
  14. CMake / project-wide config improvements.
  15. Update / better use deps.
  16. Modernize to newer C++ / MSVC.

**Tier 4 — Backlog & features** (highest individual value, lowest multiplier — reach here
when foundation work is thin or one task clearly stands out above everything above it):
  17. GitHub issues.
  18. Task markdown in docs/.
  19. TODO marks in code.
  20. Mine other engines (UE5.8, o3de, pbrt-v4, unity) for possible features.
  21. Read research papers from graphics journals for possible renderer features and refactors.
  22. Go through books (GPU Zen, raytracing gems, physycally based rendering 4th edition, etc.) for possible renderer features and refactors.
  
Don't be afraid of bigger tasks like new renderer features — they sit below the force-multipliers, not off the table.

**Docs and TODOs lie — verify "not done" against the code, never against a markdown file.**
Our development loop routinely *finishes* work without circling back to mark the doc, issue,
or `// TODO` as done — so an item that looks open in `docs/` (especially `*_FUTURE_IMPROVEMENTS.md`
and task lists) or a stale `// TODO` may already be implemented and merged. Before committing
to ANY doc- or TODO-sourced task: confirm it's actually unfinished *in the code* — grep for the
feature/symbol, read the relevant module, and cross-check the recently-merged PRs and
closed issues from step 2. Treat docs as a lead; treat the code + git history as ground truth.
A task that's already implemented isn't a task — it's a doc fix (drop down to "mark it done").

**Right-size the unit of work.** One worktree + window + PR is real overhead — don't spend it
on a single trivial fix. For Tier 2 signal-hygiene especially (warnings, smells), prefer a
BATCHED task that clears a whole class at once — "fix every `-Wfoo` in `<module>`" or
"resolve all SonarQube dead-code hits in `<subsystem>`" — over one warning per loop.

**For a genuinely large Tier 1/4 epic, default to keeping it whole, not slicing it.** The old
default here was "scope it to the first shippable slice and leave the rest as a follow-up
issue" — don't do that automatically anymore. Instead recommend a **long-running Fable 5
session that fans sub-pieces out to subagents** (via the Agent tool) to absorb the epic's
size and parallelism, landing the *entire* issue as one branch/PR that closes it outright.
This worked well in practice (2026-07-16 RPG-progression / atmosphere-sky rework, after an
initial first-slice-plus-follow-up split was explicitly rejected in favor of this) and avoids
the churn a slice-and-defer approach creates: follow-up-issue bookkeeping, partial-closure
tracking on the parent issue, and re-onboarding a future `/start-work` batch to the same
subsystem context from scratch. See the Fable 5 rubric entry below for how to structure the
subagent fan-out in the handover.

Only fall back to a first-slice-plus-follow-up split when:

- the epic's pieces are independently valuable AND the user/issue explicitly wants
    incremental delivery across separate PRs over time (e.g. #429 floating-origin, whose
    slices intentionally landed as many separate PRs over several weeks), or
- the work is inherently unbounded/exploratory with no clear single "done" (a research
    epic, an open-ended roadmap issue), or
- the user asks for a smaller slice directly.
When you do slice, say so explicitly in the HANDOVER and justify it the same way you'd
justify going below the top of the rank — and still file/score the follow-up issue so it
doesn't become an unscored blind spot (via the feature template, or an `olo-score` block
added to an existing issue).

## 4. Once chosen, create the worktree + branch (repeat per task)

**Run this whole step once for EACH of the N chosen tasks** — N worktrees, N branches.
Explore as much as you need in steps 2-3 before committing to a task — no need to rush
to "claim" it. The step-2 registry already reflects everything in flight (existing work
is always registered before this command is run). Use a DESCRIPTIVE slug so the branch
name alone explains the work to anyone reading the registry later.

Drive policy (the one genuine local rule): the E: drive is a fast dev drive and is
PREFERRED, but may hold at most TWO worktrees. **Re-derive the count fresh for every
worktree** by re-running `git -C $BASE worktree list` — worktrees you created earlier in
THIS run already count, so the second E: slot fills as you go and the rest land on D:.
Count the paths beginning "E:\". Pick the PARENT folder the new worktree will sit DIRECTLY
under:

- count < 2  → create on E:, under the E: mirror of $ROOT: $PARENT = "E:\" + leaf-of-$ROOT
                 (if $ROOT is D:\repos then $PARENT is E:\repos)
- count >= 2 → create on D:, directly under $ROOT: $PARENT = $ROOT

**Build the worktree path as an ABSOLUTE path and VALIDATE it before creating anything.**
This is the step that has gone wrong before: a relative / mis-joined path got resolved
against $BASE (the CWD), producing a worktree NESTED INSIDE the base repo —
`D:\repos\OloEngineBaseBase\reposOloEngine-<slug>` instead of the sibling
`D:\repos\OloEngine-<slug>` (note the stray "repos" and the missing separator). A nested
worktree then shows up as untracked junk in the base repo's `git status`. Guard against it
explicitly (PowerShell) — do NOT hand-concatenate strings:
    $WT = Join-Path $PARENT "OloEngine-<slug>"
    # Abort and re-derive if ANY of these is false:
    #   [System.IO.Path]::IsPathRooted($WT)                 -> path is absolute
    #   (Split-Path $WT -Parent)   -ieq $PARENT             -> direct child of the chosen parent
    #   (Split-Path $WT -Leaf)     -eq  "OloEngine-<slug>"  -> exact basename, no "repos" prefix
    #   -not $WT.StartsWith($BASE, [StringComparison]::OrdinalIgnoreCase)  -> NOT inside $BASE
    #   -not (Test-Path $WT)                                -> does not already exist
State the drive you chose and why and echo the validated absolute $WT, then create the
worktree with a branch that does NOT track origin/master (this matters — see "Push
safety" below). Pass $WT as an ABSOLUTE path so git never resolves it against the CWD:
    git -C $BASE worktree add "$WT" -b feature/<slug> --no-track origin/master

`--no-track` is the mechanism; nothing else is needed. (A follow-up `git branch --unset-upstream`
used to sit here as belt-and-suspenders, but with `--no-track` there is no upstream to unset, so it
only ever returned non-zero — which in a scripted or unattended handoff reads as a failed step.)

**Push safety — the branch must NOT track origin/master.** When you branch from a
remote-tracking ref, git auto-sets the new branch to track origin/master. Then a later
*bare* `git push` (under push.default = upstream/current/tracking) pushes your commit
STRAIGHT TO master, silently bypassing the PR / CI-on-PR flow. This has actually happened
on a real start-work branch. So: create with `--no-track` (above), and when it's time to
publish, push EXPLICITLY to a same-named remote branch and set its upstream there:
    git -C <worktreePath> push -u origin feature/<slug>
Never publish a start-work worktree with a bare `git push`.

**Who may publish:** not this session — `/start-work` scaffolds and hands off, and must not commit,
push or open a PR itself (§5d). The handed-off **worker** session is a different matter: on its own
`feature/*` branch it is *pre-authorized* to commit, `push -u origin feature/<slug>` and open the
PR, because completing that is the handoff contract (`CLAUDE.md` → *Committing and publishing*;
`docs/process/task-loop.md` Phase 4). Do not restate a blanket "ask first" rule in the HANDOVER —
it contradicts the loop and stalls an unattended session.

**Then junction the worktree's memory directory to the base repo's.** Claude Code keys persistent
memory by project *path*, so without this the new session gets an empty memory dir whose contents
die with the worktree — that stranded 229 files across 143 dead worktrees before it was caught. The
junction makes the worktree session **read and write the one shared store**, so it starts with every
durable fact already known and nothing it learns can be orphaned:

    $slugOf   = { param($p) $p -replace ':','-' -replace '[\\/]','-' }   # E:\repos\OloEngine-foo -> e--repos-OloEngine-foo
    $projects = "$env:USERPROFILE\.claude\projects"
    $base     = Join-Path $projects "$(& $slugOf $BASE)\memory"          # derived from $BASE (§1), not hardcoded
    $dir      = Join-Path $projects  (& $slugOf $WT)
    if (-not (Test-Path $base)) { throw "base memory dir not found: $base" }
    if (-not (Test-Path $dir))  { New-Item -ItemType Directory -Path $dir | Out-Null }
    $mem = Join-Path $dir 'memory'
    if (Test-Path $mem) {
        # FAIL CLOSED: something is already here. Only a junction pointing at $base is acceptable.
        $item = Get-Item $mem -Force
        if ($item.LinkType -ne 'Junction') {
            throw "$mem exists and is NOT a junction (LinkType='$($item.LinkType)') — refusing to continue; its contents would be orphaned when the worktree is removed"
        }
        $tgt = @($item.Target)[0]
        if ($tgt.TrimEnd('\') -ine $base.TrimEnd('\')) {
            throw "$mem is a junction to '$tgt', not to the base store '$base' — refusing to continue"
        }
    } else {
        New-Item -ItemType Junction -Path $mem -Target $base | Out-Null
    }

Failing closed matters more than it looks: silently accepting a **real directory** there gives the
new session a private memory store that dies with the worktree — the exact failure that stranded
229 files. A junction pointing somewhere *else* is worse still, since writes would land in another
worktree's store.

Do it **before** opening the window (§5b) — the memory dir is created lazily on first write, and a
real directory already sitting there blocks the junction. A Windows directory junction needs no
elevation, and both `Remove-Item -Recurse -Force` and `rm -rf` delete the *link* without following
it into the target (verified), so `/cleanup-worktree` removing the project dir can never eat the
shared store.

Do all subsequent work under that worktree path, and report the path + branch you created.

## 5. Hand over to a fresh session in the worktree (default conclusion — repeat per task)

**Do this once for EACH of the N chosen tasks**, so N tasks ⇒ N handover briefs and N new
windows. `/start-work` decides the task(s) and scaffolds the worktree(s); everything after
that — implement, verify, self-review, commit, push, open the PR, then drive it to green
through CI and CodeRabbit — happens in a fresh Claude session running in a VS Code Insiders
window opened ON each worktree. This keeps each task in its own window + branch and stops
the base-repo window from pointing at the wrong folder (its Source Control panel,
integrated terminal, and `.vscode/tasks.json` build tasks would otherwise stay on $BASE /
`master`).

**The worker session's contract is `docs/process/task-loop.md`** — it runs the whole way to a
green, self-reviewed, thread-clean PR without being re-prompted per step, and stops there.
Merging the PR and closing the issue stay the user's calls; `/cleanup-worktree` reclaims the
worktree afterwards. Your handover does NOT need to restate the loop — point at it.

**A handover is file-based — there is no live-memory transfer.** A new window runs its
own Claude process; it does NOT inherit this session's conversation. (`claude --continue`
/ `-r` resume *per-directory* and won't cross from $BASE into the worktree dir.) So pass
the task along by WRITING IT DOWN, then opening the window, then priming the new session.

**5a. Write the handover brief.** Create `HANDOVER.md` at the worktree root with
everything the next session needs to start cold — write it for a reader who knows nothing
about this conversation:

- **Task** — the chosen task, issue # + link (or other source), its **score + rank
    position** from `issue_scores.py rank` (or, for non-issue work, the leverage tier), and
    one line on why it was picked — cite the rank, or the Pull-override / "justify going
    down" reason if it wasn't simply the top unblocked issue.
- **Branch / worktree** — `feature/<slug>` at `<worktreePath>`, based on `origin/master` @ <sha>.
- **Recommended model + effort** — one of Opus 5 / Sonnet 5 / Fable 5 and an effort
    level, per the rubric below, so the new window's session can `/model` to it and set
    effort before starting. State one line of *why* (what about the task drives the choice).
- **Registry snapshot** — the off-limits list from step 2 (so the next session won't
    re-derive or collide). **When N > 1, also list the OTHER tasks/branches launched in
    this same batch** as off-limits — they were just created and run in parallel, so each
    sibling session must know not to touch the others' subsystems/files.
- **Plan** — the intended approach / steps, covering the FULL scope of the task (every
    acceptance criterion on the issue, if it's issue-sourced) unless you deliberately sliced
    per "Right-size the unit of work" above — in which case say so explicitly and name what's
    deferred. **If the plan is a whole-epic Fable 5 + subagents session (the default for a
    large Tier 1/4 epic now)**, structure this section as a subagent fan-out strategy, not
    just a linear step list: what to research in parallel first (read-only), which
    cross-cutting contracts the orchestrating session must fix itself before delegating
    anything, which sub-pieces are file-disjoint enough to hand to subagents once those
    contracts exist, and which edits (cross-binding touch-points, cross-part integration
    wiring) must stay sequential in the main session because a missed one fails silently.
- **State** — what's already done (usually nothing yet) and what's been verified.
- **Next steps** — the concrete first actions to take.
- **The loop** — one line: "Follow `docs/process/task-loop.md`, Phase 0 through Phase 7." That
    document owns implement → verify → self-review → commit → push → PR → CI/CodeRabbit → report,
    including the closing-the-loop steps (tick the source, capture the lesson in
    `docs/agent-rules/` and link it from both indexes). Don't restate it here. DO note anything
    task-specific that overrides or extends it — "this one needs visual evidence from four
    angles", or "the acceptance criteria are the issue's checkboxes; tick each with evidence and
    let the PR's `Closes #N` shut it on merge".
- **Guardrails (quote, don't paraphrase)** — from `CLAUDE.md` → *Committing and publishing*:
    inside this worktree on this `feature/*` branch, commit / push / open-PR are
    **pre-authorized** — that's the job, don't stop to ask. Still gated: **never a bare
    `git push`** (publish with `git push -u origin feature/<slug>`), and merging the PR, pushing
    to `master`, force-pushing, and closing the issue remain explicit user opt-ins. Plus: "read
    `CLAUDE.md` + its Definition of done before any non-trivial work."
  `HANDOVER.md` is a scratch note, NOT project content - it's gitignored.

**Model + effort rubric (apply to EVERY task, every batch — in the HANDOVER and the report).**
Match the recommendation to the task's *reasoning difficulty and verification burden*, NOT its
leverage tier — a high-leverage task can still be mechanical, and a small diff can still be
correctness-critical. Pick a model:

- **Opus 5** — hardest reasoning / subtle correctness / architecture / high blast-radius,
    or work gated on a mandatory visual-or-runtime verification loop where a plausible-but-wrong
    result is costly to catch (e.g. renderer changes that "pass tests but look broken",
    cross-subsystem invariants, tricky concurrency). Default for renderer-correctness and
    anything where the failure mode is silent.
- **Sonnet 5** — well-scoped engine work with a clear existing pattern to copy and test guards
    that catch mistakes (a codegen slice mirroring a prior slice, a settings-plumbing fix with a
    reference implementation, a new read-only tool mirroring ~36 siblings). **The sensible
    default for most start-work tasks.**
- **Fable 5** — two distinct use cases, don't conflate them:
    (a) the fast lane for mechanical, highly-patterned, low-ambiguity slices: rename sweeps,
    boilerplate, docs passes, a Tier-2 warning/smell batch, following a very explicit template
    under strong tests — reach for it when the *how* is obvious and only the *typing* is left;
    (b) **a long-running session driving a large Tier 1/4 epic kept whole** (per "Right-size
    the unit of work" above — this is the default now, not a fallback) by fanning sub-pieces
    out to subagents via the Agent tool instead of a human pre-slicing the scope. Structure the
    HANDOVER's Plan section around this: research fan-out first (parallel, read-only Explore
    agents mapping the existing patterns/touch-points the epic needs); the orchestrating Fable
    session fixes the cross-cutting core contracts itself (component shapes, scheduler
    registration, the seams between the epic's sub-parts) *before* delegating anything — this
    is where a wrong subagent guess is expensive to unwind; only once those contracts are fixed,
    delegate genuinely file-disjoint sub-pieces to subagents (a shared worktree means parallel
    subagents editing the *same* files will collide — sequence those, or use
    `isolation: 'worktree'` if concurrent edits are unavoidable); do the cross-binding
    touch-point edits and any cross-part integration wiring in the main session, sequentially,
    since that's exactly where a missed edge fails silently; verify every subagent's actual diff
    before trusting its summary. Any mandatory verification loop (visual, runtime) from the
    Opus guidance above still applies regardless of the driving model — a Fable-orchestrated
    session doesn't get to skip screenshot evidence on a rendering change.
Then pick an effort level: **high / xhigh** for tricky correctness, subtle bugs, a heavy
verify loop, or a whole-epic Fable session per (b) above (the orchestration/integration
decisions are hard even when individual delegated pieces are mechanical); **medium** for
standard feature work with tests; **low** for mechanical edits per Fable (a). When unsure,
round *up* one level for correctness-critical or hard-to-verify work. (These map to the same
three names the user selects in Claude Code — Opus 5 / Sonnet 5 / Fable 5 — plus the effort
control.)

**5b. Open the worktree in a NEW window.** The user runs VS Code Insiders — open one
window per chosen task, each on that task's own worktree path:
    code-insiders -n <worktreePath>
  `-n` forces a new window so this session and the base-repo window both stay alive (and,
  for N > 1, every task window stays alive alongside the others). Fall back to
  `code -n <worktreePath>` if `code-insiders` isn't on PATH. Running this is part of the
  requested handover — do it for each task, then say you did.

**5c. Prime the new session.** A new window's Claude does not auto-start. Tell the user to
switch to the new window, run `/model <opus|sonnet|fable>` and set the effort level to the
one you recommended for this task, then start the session with:
    Read ./HANDOVER.md and continue the task it describes.
That one line is the whole kickoff — `HANDOVER.md` points at `docs/process/task-loop.md`, and the
session then runs to a green, self-reviewed PR on its own. It's interactive, so the user watches
and can interrupt or redirect at any point; and it can be long — CI's SonarCloud and sanitizer
jobs take ~1.5–2 hours, so the session may still be legitimately working long after the code is
written.

**5d. Then stop — this session's job is done.** Do NOT start implementing any task in this
(base-repo) session; the new windows own them. Final report: for **each** of the N tasks,
the task chosen, its worktree path + branch, its **recommended model + effort** (per the
rubric above), and its kickoff prompt (a short table or list keyed by task is ideal for
N > 1) — **plus the stale-registry findings from §2b**: which issues you posted evidence
comments on, which you recommend closing (and ask before closing them), and any durable facts
you wrote to memory.
