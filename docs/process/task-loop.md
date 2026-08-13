# The task loop — from `HANDOVER.md` to a green, reviewed PR

The contract for a **worker session**: a Claude session running in a task worktree, started by
`/start-work`, kicked off with *"Read ./HANDOVER.md and continue the task it describes."*

`/start-work` picks the work and scaffolds the worktree. **This document owns everything after
that**, and the session is expected to run the whole way to a green, self-reviewed, review-clean
PR without being re-prompted at each step.

**Where it stops: merging.** The loop ends at "PR is green, reviewed, all threads resolved."
Merging the PR and closing the issue stay explicit user opt-ins. `/cleanup-worktree` reclaims the
worktree afterwards.

Absorbed the former `/finish-pr` and `/pr-status` commands — they were a separate manual stage for
work this loop now does inline.

---

## Phase 0 — Orient

Read `HANDOVER.md`, then `CLAUDE.md` (especially *Definition of done*) and any
`docs/agent-rules/` file the task touches — use
[agent-rules/README.md](../agent-rules/README.md) to find them by failure mode when the subsystem
isn't obvious. Set the model + effort `HANDOVER.md` recommends.

Confirm you are in the worktree, not the base repo:

```bash
git rev-parse --path-format=absolute --git-common-dir --show-toplevel
```

If `--show-toplevel` equals the base repo, **stop** — you would put branch work on `master`.

## Phase 1 — Implement

Work the plan in `HANDOVER.md`, covering its **full** scope. If you discover the plan is wrong,
say so and adapt — the handover is a brief, not a spec. If you discover the task is already done,
stop and report that instead of manufacturing work.

Honor the *Definition of done* cross-binding checks as you go: an ECS component change has
touch-points the pre-commit hook cannot catch.

## Phase 2 — Verify locally, before anyone else sees it

Build and run the suite via the `run-oloengine` skill.

**Trust the build tool's own exit code, nothing else.** Three separate mechanisms here report a
*failed* build as success: a trailing pipe returns the pipe's status, not the compiler's; a
PowerShell truncating filter (`Select-Object -First N`, `Select-String … -First N`) both masks the
code *and* tears the build down early once N lines have streamed; and the background-task harness
summarises whatever the shell handed it. Capture the code alone, before anything can mask it, then
grep the log:

```bash
if ! cmake --build build --target OloEngine-Tests --config Debug --parallel 6 > /tmp/b.log 2>&1; then
    echo "BUILD FAILED"; tail -40 /tmp/b.log; exit 1
fi
test -f build/OloEngine/tests/Debug/OloEngine-Tests.exe || { echo "no artifact despite exit 0"; exit 1; }
```

Note the shape: the build's own status is tested **directly**, and nothing runs after it that could
become the block's status. Writing `cmake --build …` then `echo "EXIT=$?"` on the next line makes
the *echo* the last command, so the block succeeds even when the build didn't — the same class of
bug this section is about.

Confirm success by the target artifact existing, **and** by a zero exit — neither alone. Never
attach a truncating filter to a command whose side effects matter.

Then, per `CLAUDE.md`:

- **Rendering / visual change** — CPU or contract tests are *not* sufficient. Capture screenshot
  evidence from multiple angles and **look at the images**. For live-editor inspection, the
  `run-oloengine` skill's `attach` action starts the editor with the MCP diagnostics server;
  `olo_screenshot`, `olo_camera_*`, `olo_render_capture_target` and `olo_shader_errors` let you
  inspect the real frame and intermediate buffers. Write access needs **both**
  `OLO_MCP_AUTOSTART=1` and `OLO_MCP_ALLOW_WRITES=1` set *before* launch — the second is read
  inside the autostart block, so exporting it against a running editor does nothing.
- **New or renamed test `.cpp`** — classify it (`// OLO_TEST_LAYER:` comment preferred) or
  pre-commit blocks the commit.
- Before concluding "it drew nothing" or "the change had no effect", check
  [live-verification-noise-floor.md](../agent-rules/live-verification-noise-floor.md) — an
  iconified editor answers every read tool with a stale frame.

## Phase 3 — Self-review your own diff

**Required, before the PR exists.** CodeRabbit catches a narrower class of issue than a real
review, and reviewing first means less churn afterwards.

```bash
git fetch origin && git diff origin/master...HEAD --stat
```

Run `/code-review` at an effort that fits the diff — `medium` for a small focused change, `high`
for a substantive or multi-file one. (`ultra` is user-triggered and billed; recommend it in your
report if the diff warrants it, don't launch it.)

Fold every real finding into a fix. A finding you judge wrong or out of scope gets an explicit
one-line dismissal — **never a silent drop**. Re-run Phase 2 afterwards: review fixes are code
changes and can break the build.

## Phase 4 — Commit, push, open the PR

This is pre-authorized on a `feature/*` branch in a task worktree — see `CLAUDE.md` → *Committing
and publishing*. It is **not** pre-authorized anywhere else.

**Never `git add -A` after a test run.** A full `OloEngine-Tests` run regenerates tracked PNGs
under `OloEditor/assets/tests/visual/` and leaves ~10 of them modified even when nothing changed —
two runs of the *identical* binary moved `VirtualGeometry_Debug_ClusterId` by 158/255 and
`Fluid_Waterline` by 80/255. Committing that noise puts a meaningless binary diff in the PR and
implies a visual change that did not happen, which is actively misleading in a renderer PR (it
happened on #732 and needed a revert commit). Stage deliberately:

```bash
git status --short          # look at it
git add <the files you actually changed>
git checkout -- OloEditor/assets/tests/visual/   # unless a golden legitimately moved
git commit -F - <<'MSG'
<type>(<scope>): <summary>

<why, not what — the diff says what>
MSG
```

**Always pass `-m` or `-F`.** A bare `git commit` opens `$EDITOR`, which in an unattended session
hangs forever with no output.

A golden that legitimately moved is a deliberate, explained part of the diff — see
[procedural-generator-golden-coupling.md](../agent-rules/procedural-generator-golden-coupling.md).

The `Stop` hook runs `pre-commit run --all-files`; if it reformats anything, re-stage and commit
again. That abort-and-re-add cycle is expected, not a failure.

**Never a bare `git push`** — under `push.default` it can fire straight at `master`, which has
actually happened on a real branch here:

```bash
git push -u origin feature/<slug>
```

Open the PR with a body that states what changed and why, the verification you did (name the
evidence — test names, screenshot paths), and a closing keyword for the issue it finishes:

```bash
gh pr create --title "<type>(<scope>): <summary>" --body "...

Closes #<N>"
```

Use `Closes #N` only when the PR genuinely completes the issue. For one item of an umbrella
tracker, reference it as a bare `#N` and say which item landed — GitHub must not auto-close it.

### The review guide — end the PR body with it

The median PR here is ~2,600 lines across ~30 files. CodeRabbit will review the *diff*; what it
structurally cannot supply is **the author's own uncertainty**, and you have just self-reviewed
this change (Phase 3) so you know it. Do not throw that away. Close the body with:

```markdown
## Review guide

**Where I'd look hardest**
1. `<file:line>` — <why this is the riskiest part>
2. …  (2–3 entries, ranked; not a file list)

**What I verified, and how** — <named evidence: test names, screenshot paths, the check that would
have failed if this were wrong>

**Least confident about** — <the thing you'd want a second opinion on, or "nothing" and why>

**Deliberately not tested** — <what you skipped and the reason>
```

Be honest in "least confident" — an empty section because you didn't want to look uncertain wastes
the whole point. If a section genuinely has nothing, say so in a few words rather than deleting it.

## Phase 5 — Drive the PR to green

CI and CodeRabbit run in parallel. Work both until the exit gate in Phase 6 passes.

> ### The governing rule: one decision, one push, per round
>
> A round costs 2–3 hours of wall clock and you do not control its length — Actions runs
> ~137–201 min and CodeRabbit reviews at roughly **one per hour**. So the only thing you control is
> **how many rounds the PR takes**, and the measured median is 3 commits per merged PR. Every
> avoidable push is another 2–3 hours.
>
> Each push costs you **twice**: it burns a CodeRabbit review slot, *and* it supersedes the
> in-flight CI run. That is not theoretical — PR #785's checks show `CANCELLED` at 37–38 minutes
> into a 137-minute build, thrown away by a push.
>
> Therefore: **gather everything, decide once, push once.** The sub-steps below are ordered.

### 5a. Wait for a complete picture — do not act on the first red

**A single failed check does not mean the run is over.** TSan failing tells you nothing about
whether ASan and UBSan will; they may still be running. If you fix TSan and push immediately you
cancel the siblings, learn nothing from them, and discover the next failure in the *following*
round — having also spent a CodeRabbit hour.

So poll until every check that can reveal a code problem has reached a terminal state
(`SUCCESS`/`FAILURE`/`CANCELLED`/`TIMED_OUT`), and only then decide.

```bash
gh pr checks <#> --repo <owner/repo>
gh pr view <#> --repo <owner/repo> --json mergeable,statusCheckRollup
```

**Never push into a healthy in-flight run.** If CodeRabbit returns a nit at 30 minutes while
Windows still has 100 to go, the nit waits. The one exception is a run already known to be
worthless — every remaining job has failed, or the change you must make invalidates it anyway.

Poll with background execution, not a foreground sleep, at a cadence matched to what is left:
minutes while the build is live, much longer once only the long poles remain.

### 5b. Classify every failure before you touch code

Three buckets, and only one of them is yours to fix:

- **Infrastructure** — a dependency/vcpkg setup disconnect, the `packages.microsoft.com` apt 403,
  a runner drop, SonarCloud hitting the 6 h cap. **Response: `gh run rerun --failed`. No code
  change, no push.** "Fixing" an infra flake in code spends a whole round on nothing.
- **Known flake** — cross-check the failing test against the flake memories before assuming it is
  yours: shared-temp-dir races cover `FrameExportTest`, the FloatValidation fixtures and friends;
  a separate set covers the machine-local GL/perf failures. Response: re-run, and say so in the
  report **with evidence**, never silently.
- **Real** — a compile error, a genuine test failure, a sanitizer report. This is the only bucket
  that earns a code change.

Get the actual log before classifying — don't guess from the job name. **`gh run view --log`
returns empty in this repo**; use the API:

```bash
gh api repos/<owner>/<repo>/actions/jobs/<job-id>/logs
```

**Fix properly, never paper over.** A retry, a loosened assertion, a disabled test or a widened
tolerance is acceptable *only* for a confirmed infra/known-flake failure, stated explicitly with
evidence. "It passed on re-run" is not a diagnosis.

### 5c. Batch, then push once

Collect **all** real failures across **all** jobs, plus **all** CodeRabbit findings from 5d, fix
the lot, and push a single time. Three separate fix commits is three CodeRabbit hours and three CI
restarts for work that could have been one round.

### 5d. CI mechanics

- **`mergeable == CONFLICTING`** — fix first; conflicts often stop CI from running at all.
  Resolve by **merging master in**, never rebase, never force-push:

  ```bash
  git fetch origin && git merge origin/master
  ```

  Understand both sides of each conflict; don't blindly take one. If either side touched an ECS
  component, re-check the cross-binding touch-points — a clean textual merge can silently drop
  one. Rebuild after resolving.

  **Resolve conflicts within the turn you created them.** The `Stop` hook clang-formats the whole
  repo regardless of merge state, so a C/C++ file left mid-conflict across a turn boundary comes
  back mangled: `=======` becomes `== == == =` and `>>>>>>> origin/master` becomes
  `>>>>>>> origin / master` (clang-format reads `=` and `/` as operators). Git still tracks it as
  unmerged, but the markers are no longer textually intact and an `Edit` match on them will fail.

- **Be patient with the long pole.** SonarCloud (~201 min median) and the Linux sanitizer jobs
  (~170 min median, and one observed at 499) are usually the last `IN_PROGRESS` checks. That is
  normal, not a hang — do not re-dispatch or cancel them.

### 5e. Review threads

**Use unresolved review threads as the signal — never the comment count.** CodeRabbit posts an
auto-summary and SonarCloud posts a Quality Gate comment on *every* PR; counting comments flags
every PR as needing work.

```bash
gh api graphql --paginate   -f query='query($o:String!,$r:String!,$n:Int!,$endCursor:String){repository(owner:$o,name:$r){pullRequest(number:$n){reviewThreads(first:100,after:$endCursor){pageInfo{hasNextPage endCursor} nodes{id isResolved path line comments(first:1){nodes{author{login} body}}}}}}}'   -F o=<owner> -F r=<repo> -F n=<#>   --jq '.data.repository.pullRequest.reviewThreads.nodes[]|select(.isResolved==false)|"\(.id)  \(.path):\(.line)  [\(.comments.nodes[0].author.login)]"'
```

Every thread must reach one of two end states — never leave one untouched:

- **Fix** — the comment is right. Make the change; it ships with the next push.
- **Rebut** — wrong, a false positive, or out of scope. Reply on the thread with the reasoning.

Then resolve every thread you handled, both fixed and rebutted:

```bash
# reply
gh api graphql -f query='mutation($t:ID!,$b:String!){addPullRequestReviewThreadReply(input:{pullRequestReviewThreadId:$t,body:$b}){comment{id}}}' -F t=<threadId> -F b="False positive: <one-line reason>."
# resolve
gh api graphql -f query='mutation($t:ID!){resolveReviewThread(input:{threadId:$t}){thread{isResolved}}}' -F t=<threadId>
```

**Verify each finding against the actual code before acting on it — a severity label is not
evidence, and the *premise* is what must be checked.** Two worked examples from this repo:

- PR #373's `operator==`/`blkEq` "🔴 Critical" was a false positive; the helper takes two `this`
  members only to delimit a byte range and memcmps against `o` at the same offset.
- PR #400's "🔴 Critical — add a protocol-version guard, `Scale` was recently added to the
  transform wire" was built on a **hallucinated premise**: the diff only *moved* an existing
  `ar << Scale.x/y/z` above the `if (ar.IsLoading())` block so the loaded value could be
  finiteness-sanitized. The wire bytes were unchanged. The tell is in CodeRabbit's own
  "🧩 Analysis chain" block — when it reports a tiny output length (e.g. `Length of output: 163`),
  its git-history commands returned nothing, because its sandbox often has no usable history. It
  then infers history that isn't there.

**CodeRabbit usually resolves its own threads.** On this repo it flips `isResolved=true` itself
within a minute or two of a substantive reply — for rebuttals (follow-up ends
`<!-- <review_comment_withdrawn> -->`) and fix-acks (`<!-- <review_comment_addressed> -->`) alike.
So reply, wait, then re-query rather than always calling `resolveReviewThread` yourself.

**The corollary is a trap for the Phase 6 gate:** because a reply alone can resolve a thread, a
fix that is only committed locally can leave the count at 0 while the remote PR still lacks it.
A zero unresolved count never substitutes for *pushed*. Confirm the fix is on the remote before
treating the gate as passed.

## Phase 6 — Exit gate

You may **not** report the task done while any of these is false:

1. `mergeable == MERGEABLE`.
2. Every check `SUCCESS`.
3. Unresolved review-thread count is **0**:

   ```bash
   gh api graphql --paginate      -f query='query($o:String!,$r:String!,$n:Int!,$endCursor:String){repository(owner:$o,name:$r){pullRequest(number:$n){reviewThreads(first:100,after:$endCursor){pageInfo{hasNextPage endCursor} nodes{isResolved}}}}}'      -F o=<owner> -F r=<repo> -F n=<#>      --jq '[.data.repository.pullRequest.reviewThreads.nodes[]|select(.isResolved==false)]|length' | paste -sd+ | bc
   ```

4. The self-review (Phase 3) covers the **current** head — if you pushed fixes after it, re-review
   the new commits and post an updated summary.

"CI green and mergeable" never means done while a thread is open. The one legitimate exception is
a thread CodeRabbit posted against your final push that hasn't landed yet: name it explicitly and
say it is outstanding — do not claim all threads are resolved.

Post one self-review summary comment so the review leaves a visible trace (additive, no ask):

```bash
gh pr comment <#> --repo <owner/repo> --body "## 🤖 Self-review @ \`$(git rev-parse HEAD)\`
Reviewed the PR diff at <effort> effort.
- **Findings:** <n> · **Fixed:** <one-liner per fix>
- **Dismissed:** <finding> — <reason>"
```

Do **not** submit a formal GitHub approval (`gh pr review --approve`) — an approval can satisfy
branch protection, so it stays a gated opt-in like merging.

## Phase 7 — Close the loop, then report and stop

Before reporting — these are the steps that get forgotten, which is why docs go stale:

1. **Mark the source done.** Tick the `docs/` checkbox, delete the resolved `// TODO`. The issue
   itself closes via the PR's closing keyword on merge; if the PR only advanced an umbrella
   tracker, comment which item landed and leave it open.
2. **Capture any reusable lesson — repo first, memory second.**

   A non-obvious **engine** gotcha — an MSVC quirk, a missed touch-point, a wrong assumption the
   docs led you to → **`docs/agent-rules/`**. A real failure story gets its own postmortem file;
   an incremental "this will bite you" fact gets appended to the relevant
   [`notes-*.md`](../agent-rules/README.md) subsystem doc. Link a new file from **both** indexes:
   one line in `CLAUDE.md` → *Companion guides*, and a row in the failure-mode tables. It ships
   with the PR, so it survives and it is reviewable.

   `CLAUDE.md` gets the one-line pointer only — **never the lesson itself**. It loads into every
   session and has been cut back from bloat once already.

   A **machine / tool / CI** fact — a flaky job, a `gh` quirk, something about this box — goes to
   persistent memory, with a one-line entry in `MEMORY.md`.

   > **Your memory dir is a junction to the base repo's shared store** (`/start-work` §4 creates
   > it), so anything you write is immediately visible to the base repo and every other worktree,
   > and nothing is lost when this worktree is removed. Two consequences: **you already start with
   > every durable fact the project knows** — check before re-deriving; and **`MEMORY.md` is
   > shared**, so if several worktrees are running, append your line rather than rewriting the
   > file, and don't be surprised by entries you didn't add.
   >
   > If `~/.claude/projects/<this-slug>/memory` is a *real directory* rather than a junction, this
   > worktree predates the change — say so in your report so `/cleanup-worktree` salvages it.

Then **report and stop**. State: the PR number and link, what you changed, how you verified it
(naming the evidence), what CI and review needed, and anything left for the user — which is
normally just *merge it*. Do not merge, and do not close the issue.

---

## Notes on the tooling here

- `gh issue view <N>` errors on this repo (Projects classic); use `gh issue view <N> --json <fields>`.
- `gh pr edit --add-label` returns 0 but silently applies nothing; use
  `gh api repos/<owner>/<repo>/issues/<N>/labels -f labels[]=<label>`.
- Merges use a merge commit, not squash — house style is `Merge pull request #NNN …`. (Relevant
  only when the user merges; noted so a squash isn't suggested.)
- Never build the `build/` (msvc) and `build-clang/` trees concurrently, and always cap build
  parallelism — see `CLAUDE.md` → *Build & run*.
