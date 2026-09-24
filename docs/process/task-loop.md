# The task loop — from `HANDOVER.md` to a green, reviewed PR

The contract for a **worker session**: a Claude session running in a task worktree, started by
`/start-work`, kicked off with *"Read ./HANDOVER.md and continue the task it describes."*

`/start-work` picks the work and scaffolds the worktree. This document owns everything after that.
The session runs the whole way to a green, self-reviewed, review-clean PR without being re-prompted
at each step, and it **fixes the bugs it finds on the way** instead of filing them (Phase 1a).

**Where it stops: merging.** The loop ends at "PR is green, reviewed, all threads resolved."
Merging the PR and closing the issue stay explicit user opt-ins. `/cleanup-worktree` reclaims the
worktree afterwards.

The stories behind the rules are in the [appendix](#appendix--why-these-rules-exist).

---

## How turns end

This session runs unattended, so a message with no tool call ends the work until someone notices.
Do not end a turn in any of these four ways while work the task owes is still open:

1. a summary of what was done that closes by announcing the next step, with no tool call;
2. an offer to carry on "unless you'd prefer otherwise";
3. a list of decisions for the user when, by your own account, none of them blocks the rest;
4. stopping to report because the turn has been long or a milestone is done.

Status notes and recommendations are welcome: put them in the same message as your next tool call
and carry on with whatever does not depend on an answer. The stops that are wanted: nothing can move
without the user; the next step is a gated action (see `CLAUDE.md` → *Committing and publishing*);
or the Phase 6 exit gate has passed.

## Phase 0 — Orient

Read `HANDOVER.md`, then `CLAUDE.md` (especially *Definition of done*) and any `docs/agent-rules/`
file the task touches; [agent-rules/README.md](../agent-rules/README.md) finds them by failure mode.
Set the model and effort `HANDOVER.md` recommends.

Confirm you are in the worktree, not the base repo:

```bash
git rev-parse --path-format=absolute --git-common-dir --show-toplevel
```

If `--show-toplevel` equals the base repo, **stop**: you would put branch work on `master`.

## Phase 1 — Implement

Work the plan in `HANDOVER.md`, covering its **full** scope. If the plan is wrong, say so and adapt;
the handover is a brief, not a spec. If the task is already done, stop and report that instead of
manufacturing work.

Honour the *Definition of done* cross-binding checks as you go: an ECS component change has
touch-points the pre-commit hook cannot catch.

### 1a. Bugs you find on the way — fix them here

**A bug you find while working is fixed in this worktree, on this branch.** You have just proven it,
you have the context loaded, and you have a build and a verification setup running. Filing it
instead costs a future session all of that from cold. The measured pattern before this rule was
several issues a day, each opening with "Found while verifying #N", many of them a one-line fix.

The fix goes in **its own commit** (`fix(<scope>): …`, not folded into the task's commits) with
its own test or evidence, sized like the neighbouring tests, and it is listed in the PR body under
*Found and fixed* (Phase 4). It goes through the same verification matrix as the task where it
reaches a rendering path.

**File an issue instead only when one of these is true**, and name which:

| reason | what it means |
|---|---|
| **needs a decision** | the fix needs a design, scope or trade-off call from the user |
| **needs access** | hardware, an asset, a licensed SDK or a credential this session does not have |
| **too big** | the fix is larger than the task itself, or is a redesign of another subsystem |
| **owned elsewhere** | a sibling worktree (`HANDOVER.md` registry snapshot, `git worktree list`) is changing those files |

"It widens the diff", "it is out of scope", "it is a different subsystem" and "it needs its own
verification" are not reasons. They describe the fix, not something that blocks it.

A filed issue carries, in its body:

```markdown
Found while working on #<task issue> (or the task's source, for a non-issue task).
Not fixed in #<PR> because: <needs a decision | needs access | too big | owned elsewhere> — <the fact>.
```

Before the PR exists, write the branch name in place of `#<PR>`. Add an `olo-score` block ([issue-scoring.md §5](issue-scoring.md), scored against the §4
anchors): `gh issue create --body` bypasses the issue templates, and an unscored issue is invisible
to `/start-work`. For **owned elsewhere**, also message the owning session if it is live
(`ListAgents`, then `SendMessage`) so the fix lands there instead of waiting in the backlog.

A partial fix is still a fix: make the part you can, commit it, and file only the remainder with the
reason the remainder is blocked.

## Phase 2 — Verify locally, before anyone else sees it

Build and run the suite via the `run-oloengine` skill.

**Trust the build tool's own exit code, nothing else.** A trailing pipe returns the pipe's status; a
PowerShell truncating filter (`Select-Object -First N`) masks the code *and* stops the build early;
a trailing `echo` becomes the block's status. Test the build's status directly, with nothing after
it, then confirm the artifact exists:

```bash
if ! cmake --build build --target OloEngine-Tests --config Debug --parallel 6 > /tmp/b.log 2>&1; then
    echo "BUILD FAILED"; tail -40 /tmp/b.log; exit 1
fi
test -f build/OloEngine/tests/Debug/OloEngine-Tests.exe || { echo "no artifact despite exit 0"; exit 1; }
```

A new or renamed test `.cpp` needs its `// OLO_TEST_LAYER:` classification, or pre-commit blocks
the commit.

### 2a. The verification matrix — run every cell, or give a fact for the one you didn't

**A change with more than one execution path is verified on every path a user can reach it
through.** `HANDOVER.md` names the axes (`/start-work` §5a); if it does not, derive them and say so.

| subsystem | always a cell | additionally, when the change reaches it |
|---|---|---|
| renderer / anything visual | `{OpenGL, Vulkan}` × `{Forward, Forward+, Deferred}` | MSAA on/off; upscale mode; a non-native resolution |
| serialization | `{scene YAML, asset pack, save-game}` | a prior on-disk version, if the change is versioned |
| scripting | `{C#, Lua}` | — |
| physics | `{Box2D 2D, Jolt 3D}` | — |
| assets | `{loose, cooked}` | — |

A second-column axis is a cell only if the change can reach it, and that is decided once, in the
HANDOVER, with the reason written down: "MSAA: not a cell, this pass runs after the resolve" is a
decision; leaving MSAA unmentioned is not.

A cell is evidenced in one of two ways, fixed by the cell:

- **Artefact-backed**: a headless evidence test captured it, so the proof is a committed file named
  after the cell: `<Feature>_<Backend>_<Path>[_<Angle>].png`, with `<Feature>Off_…` as the A/B
  control. An unrun cell is then a file missing from the diff.
- **Live-only**: no test can produce it, so the proof is a measurement plus a log check from a real
  editor session. **Every Vulkan cell is live-only**: the headless fixtures need a GL 4.6 context.

The PR body carries the matrix, one row per cell, both kinds (Phase 6, item 5). An abbreviated
table says so above itself.

### 2b. Live-editor verification

CPU or contract tests are not sufficient for a visual change. Capture from several angles and **look
at the images**: an assertion that passes on an empty frame is the normal failure here.

The `run-oloengine` skill's `attach` action starts the editor with the MCP server; `olo_screenshot`,
`olo_camera_*`, `olo_render_capture_target` and `olo_shader_errors` inspect the real frame. Write
access needs both `OLO_MCP_AUTOSTART=1` and `OLO_MCP_ALLOW_WRITES=1` set *before* launch.

- **Vulkan needs `-Rhi vulkan`**, and `[RHI] Backend: Vulkan (source: --rhi flag)` in
  `OloEditor/OloEngine.log` is the only proof it took. A Vulkan pass-suite test passing is not this.
- **`attach -Rhi vulkan` can time out and still succeed** on a cold shader cache: check for a live
  `OloEditor` process and poll `%TEMP%\oloengine-mcp-<port>.json` before relaunching.
- **`olo_shader_errors` answers `notInitialized` on Vulkan.** Grep the log for `[error]` and `VUID`.
- **Prefer a measured A/B to an eyeball.** `olo_render_toggle_pass` flips a feature in place;
  diff the two frames. "61 000 pixels differ, max delta 89/255" is a result; "looks the same" is not.
- **Attribute a failure in a pass you touched.** Shaders are runtime assets: restore the base
  commit's copy and re-run, no rebuild. For C++, build a probe with the suspect line reverted.
- Before concluding "it drew nothing", read
  [live-verification-noise-floor.md](../agent-rules/live-verification-noise-floor.md): an iconified
  editor answers every read tool with a stale frame.

## Phase 3 — Self-review your own diff

Required before the PR exists.

```bash
git fetch origin && git diff origin/master...HEAD --stat
```

Run `/code-review` at `medium` for a small focused change, `high` for a substantive or multi-file
one. (`ultra` is user-triggered and billed: recommend it in your report if warranted, don't launch
it.) Fold every real finding into a fix. A finding you judge wrong gets a one-line dismissal, never
a silent drop. A real bug the review finds in code you did not write is a Phase 1a bug: fix it here.
Re-run Phase 2 afterwards.

## Phase 4 — Commit, push, open the PR

**Close the loop in the repo first**, so it ships in this push and passes the same review and gate:
tick the `docs/` checkbox, delete the resolved `// TODO`, and write any engine lesson to
`docs/agent-rules/` (Phase 7, item 2, says where). A repo edit you only think of after Phase 6
goes back through commit, push and the exit gate.

Pre-authorized on a `feature/*` branch in a task worktree (`CLAUDE.md` → *Committing and
publishing*), and nowhere else.

**Never `git add -A` after a test run.** A full `OloEngine-Tests` run rewrites ~10 tracked PNGs under
`OloEditor/assets/tests/visual/` even when nothing changed. Stage deliberately:

```bash
git status --short
git add <the files you actually changed>
git checkout -- OloEditor/assets/tests/visual/   # unless a golden legitimately moved
git commit -F - <<'MSG'
<type>(<scope>): <summary>

<why, not what>
MSG
```

**Always pass `-m` or `-F`**: a bare `git commit` opens `$EDITOR` and hangs. A golden that
legitimately moved is explained in the diff; see
[procedural-generator-golden-coupling.md](../agent-rules/procedural-generator-golden-coupling.md).
If the `Stop` hook reformats anything, re-stage and commit again.

**Never a bare `git push`:**

```bash
git push -u origin feature/<slug>
gh pr create --title "<type>(<scope>): <summary>" --body-file <file>
```

The body states what changed and why, the verification (named evidence: test names, screenshot
paths), and `Closes #N` only when the PR completes the issue. For one item of an umbrella tracker,
reference it as a bare `#N`.

End the body with these sections:

```markdown
## Found and fixed
- <commit sha> fix(<scope>): <bug> — <how it was found, the test or evidence that pins it>
  (or "None.")

## Filed, not fixed
- #<N> <title> — <reason from Phase 1a> (or "None.")

## Review guide

**Where I'd look hardest**
1. `<file:line>` — <why this is the riskiest part>
2. …  (2–3 entries, ranked; not a file list)

**What I verified, and how** — <named evidence; the check that would have failed if this were wrong>

**Least confident about** — <what you'd want a second opinion on, or "nothing" and why>
```

"Least confident" is for uncertainty about work that is done. Unrun work belongs in the matrix with
a factual reason, or it gets run.

## Phase 5 — Drive the PR to green

A round costs 2–3 hours: Actions runs ~137–201 min and CodeRabbit reviews about once an hour. A push
into a running CI run cancels it and throws that time away. So while CI is running: **gather
everything, decide once, push once.** A push does **not** use up a CodeRabbit review: if its window
is closed, it posts when the next one is available. Once every check is terminal, push finished work
without waiting for CodeRabbit.

### 5a. Wait for a complete picture

One failed check does not end the run. Poll until every check that can reveal a code problem is
terminal (`SUCCESS`/`FAILURE`/`CANCELLED`/`TIMED_OUT`), then decide. Never push into a healthy
in-flight run; a CodeRabbit nit at 30 minutes waits for the rest. Poll with background execution
at a cadence matched to what is left.

```bash
gh pr view <#> --repo <owner/repo> --json mergeable,statusCheckRollup
```

### 5b. Classify every failure before you touch code

Get the log first; `gh run view --log` is empty in this repo, so use
`gh api repos/<owner>/<repo>/actions/jobs/<job-id>/logs`.

- **Infrastructure** (vcpkg setup disconnect, `packages.microsoft.com` apt 403, runner drop,
  SonarCloud 6 h cap): `gh run rerun --failed`. No code change, no push.
- **Known flake**: check the flake memories first. Re-run, and say so with evidence.
- **Real** (compile error, genuine test failure, sanitizer report): the only bucket that earns code.

A retry, a loosened assertion, a disabled test or a widened tolerance is acceptable only for a
confirmed infra or known-flake failure, stated with evidence. "It passed on re-run" is not a
diagnosis.

### 5c. Batch, then push once

All real failures across all jobs plus all review findings, fixed together, pushed once.

### 5d. CI mechanics

- **`mergeable == CONFLICTING`**: merge master in (`git fetch origin && git merge origin/master`),
  never rebase or force-push. If either side touched an ECS component, re-check the cross-binding
  touch-points. Rebuild after. **Resolve within the same turn**: the `Stop` hook clang-formats a
  file left mid-conflict and mangles the markers (`== == == =`).
- SonarCloud (~201 min) and the Linux sanitizer jobs (~170 min, once 499) are usually last. That is
  normal; don't cancel or re-dispatch them.

### 5e. Review threads

Use **unresolved review threads** as the signal, never the comment count:

```bash
gh api graphql --paginate   -f query='query($o:String!,$r:String!,$n:Int!,$endCursor:String){repository(owner:$o,name:$r){pullRequest(number:$n){reviewThreads(first:100,after:$endCursor){pageInfo{hasNextPage endCursor} nodes{id isResolved path line comments(first:1){nodes{author{login} body}}}}}}}'   -F o=<owner> -F r=<repo> -F n=<#>   --jq '.data.repository.pullRequest.reviewThreads.nodes[]|select(.isResolved==false)|"\(.id)  \(.path):\(.line)  [\(.comments.nodes[0].author.login)]"'
```

Every thread ends as **Fix** (it is right; the change ships with the next push) or **Rebut** (reply
with the reasoning). A real bug a reviewer points at outside your diff is a Phase 1a bug.

```bash
gh api graphql -f query='mutation($t:ID!,$b:String!){addPullRequestReviewThreadReply(input:{pullRequestReviewThreadId:$t,body:$b}){comment{id}}}' -F t=<threadId> -F b="False positive: <one-line reason>."
gh api graphql -f query='mutation($t:ID!){resolveReviewThread(input:{threadId:$t}){thread{isResolved}}}' -F t=<threadId>
```

**Check the premise of each finding against the code; a severity label is not evidence.**
CodeRabbit's sandbox often has no git history, and a tiny `Length of output:` in its analysis chain
means it inferred history that isn't there. CodeRabbit usually resolves its own thread within a
minute or two of a substantive reply, so reply, wait, re-query. Because a reply alone can resolve a
thread, a zero count never substitutes for the fix being **pushed**.

## Phase 6 — Exit gate

You may **not** report the task done while any of these is false:

1. `mergeable == MERGEABLE`.
2. Every check `SUCCESS`.
3. Unresolved review-thread count is **0**:

   ```bash
   gh api graphql --paginate      -f query='query($o:String!,$r:String!,$n:Int!,$endCursor:String){repository(owner:$o,name:$r){pullRequest(number:$n){reviewThreads(first:100,after:$endCursor){pageInfo{hasNextPage endCursor} nodes{isResolved}}}}}'      -F o=<owner> -F r=<repo> -F n=<#>      --jq '[.data.repository.pullRequest.reviewThreads.nodes[]|select(.isResolved==false)]|length' | paste -sd+ | bc
   ```

4. The self-review covers the **current** head; if you pushed after it, re-review the new commits.
5. **The verification matrix in the PR body has no blank rows.** Each cell is run, citing an
   artefact filename or a measurement, or explicitly not run with a reason that is a fact about the
   world (no hardware, unreachable on that path, backend does not implement it). "Ran out of time",
   "assumed equivalent" and "the other path covers it" are not reasons.
6. **Every bug you found is accounted for.** Each is either a commit listed under *Found and fixed*,
   or an issue listed under *Filed, not fixed* whose body carries the `Not fixed in #<PR> because:`
   line with one of the four Phase 1a reasons. A bug mentioned only in prose fails this.

A matrix looks like this; the last row is the one the gate exists for:

```
Complete matrix — 6 cells from {GL, Vulkan} x {Forward, Forward+, Deferred}.

| backend | path      | kind     | evidence                           | result             |
|---------|-----------|----------|------------------------------------|--------------------|
| GL      | Forward   | artefact | SkinDiffusion_GL_Forward.png       | 54 258 px, max 86  |
| GL      | Forward+  | artefact | SkinDiffusion_GL_ForwardPlus.png   | test passes        |
| GL      | Deferred  | artefact | SkinDiffusion_GL_Deferred.png      | 31 002 px, max 84  |
| Vulkan  | Forward   | live     | A/B + log: 0 errors, 0 VUIDs       | 52 301 px, max 88  |
| Vulkan  | Deferred  | live     | A/B + log: 10 VUIDs, pre-existing  | 29 856 px, max 41  |
| Vulkan  | Forward+  | live     | NOT RUN — no reason                | <- gate fails here |
```

The one legitimate open thread is one CodeRabbit posted against your final push that hasn't landed
yet: name it as outstanding.

Post one self-review summary comment (additive, no ask):

```bash
gh pr comment <#> --repo <owner/repo> --body "## 🤖 Self-review @ \`$(git rev-parse HEAD)\`
Reviewed the PR diff at <effort> effort.
- **Findings:** <n> · **Fixed:** <one-liner per fix>
- **Dismissed:** <finding> — <reason>"
```

Do **not** submit a formal approval (`gh pr review --approve`): it can satisfy branch protection.

## Phase 7 — Close the loop, then report and stop

1. **Confirm the source is marked done.** The checkbox and `// TODO` edits went in before Phase 4. If
   the PR only advanced an umbrella tracker, comment which item landed (a comment, not a repo edit).
2. **Capture any reusable lesson, repo first.** Repo lessons belong in the PR (Phase 4); if one
   only surfaces now, it goes back through commit, push and the exit gate. A non-obvious engine gotcha goes to
   `docs/agent-rules/`: a failure story as its own postmortem file, an incremental fact appended to
   the relevant `notes-*.md`. Link a new file from both parts of
   [agent-rules/README.md](../agent-rules/README.md) (subsystem index and failure-mode table). Rule
   first, story second, under about 10 KB. `CLAUDE.md` gets nothing.

   A machine, tool or CI fact goes to persistent memory with one line in `MEMORY.md`. Your memory
   dir is a junction to the base repo's shared store (`/start-work` §4): you start with every
   durable fact the project has, and `MEMORY.md` is shared, so **append** your line rather than
   rewriting the file. If `~/.claude/projects/<this-slug>/memory` is a real directory rather than a
   junction, say so in your report so `/cleanup-worktree` salvages it.

Then **report and stop**: the PR number and link, what changed, how it was verified (named
evidence), the bugs found and fixed, any issues filed and why, what CI and review needed, and what
is left for the user, which is normally just *merge it*. Do not merge, and do not close the issue.

---

## Notes on the tooling here

- `gh issue view <N>` errors on this repo (Projects classic); use `--json <fields>`.
- `gh pr edit --add-label` returns 0 and applies nothing; use
  `gh api repos/<owner>/<repo>/issues/<N>/labels -f labels[]=<label>`.
- Merges use a merge commit, not squash.
- Never build `build/` and `build-clang/` concurrently; always cap parallelism (`CLAUDE.md` →
  *Build & run*).

## Appendix — why these rules exist

- **Fix found bugs here (1a).** In the two days before this rule, worker sessions filed #1421,
  #1422, #1431, #1439, #1440 and #1441 as side findings. #1431 was a per-frame log line needing a
  rate limit; #1441 was one wrong argument (`glTextureStorage2D(…, 1, …)`) that the issue itself
  pinpointed. Each then needed its own `/start-work` slot, worktree and cold re-onboarding. The old
  rule allowed "report it as a follow-up if it would widen the diff a lot", and a filed issue reads
  as rigour, so filing always won.
- **Every matrix cell (2a).** #1241 shipped its first PR with OpenGL verified and Vulkan not run,
  although the rule was written in `CLAUDE.md`, in memory and in that task's HANDOVER. The gap was
  disclosed in prose as "not yet verified". The same correction had been made on #708. Hence the
  two forcing functions: a missing file and a blank table row.
- **One push per round (5).** PR #785's checks show `CANCELLED` 37–38 minutes into a 137-minute
  build, thrown away by a push. The measured median is 3 commits per merged PR.
- **No `git add -A` after a test run (4).** Two runs of the identical binary moved
  `VirtualGeometry_Debug_ClusterId` by 158/255 and `Fluid_Waterline` by 80/255. Committing that
  noise on #732 implied a visual change that did not happen and needed a revert commit.
- **Bare `git push` (4).** Under `push.default` it has pushed a real branch straight to `master`.
- **Check the premise (5e).** PR #373's "🔴 Critical" `operator==` finding was a false positive (the
  helper memcmps a byte range against `o` at the same offset). PR #400's "add a protocol-version
  guard" assumed `Scale` had been added to the wire; the diff only moved an existing
  `ar << Scale.x/y/z` above the load branch. CodeRabbit's history commands had returned nothing.
- **How turns end.** Adapted from Anthropic's *Prompting Claude Opus 5.5* guide, section
  *Unattended agentic runs*, which names these four early stops as the ones that halt an unattended
  run while work is owed.
