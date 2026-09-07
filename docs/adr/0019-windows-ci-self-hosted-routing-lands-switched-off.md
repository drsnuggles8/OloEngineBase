# The Windows jobs can be routed to a self-hosted runner, and the switch stays off until a runner exists and both paths are measured

Issue [#1076](https://github.com/drsnuggles8/OloEngineBase/issues/1076), under the umbrella
[#1084](https://github.com/drsnuggles8/OloEngineBase/issues/1084). Extends
[ADR 0017](0017-windows-ci-critical-path-measure-before-a-self-hosted-runner.md), which deferred
the runner and named the preconditions for reopening the question. This ADR records what landed
now — the routing, the gating and the fork guard, all inert — and what has to be true before
`vars.OLO_WINDOWS_SELF_HOSTED` is set to `true`.

**This changes where PR CI *can* execute, which is why it is an ADR. It does not change where PR
CI *does* execute: no Windows runner is registered on this repository and the variable is unset,
so every run today lands on `windows-2025` exactly as before.**

---

## 1. Why route at all

The two Windows jobs are the per-PR critical path on every PR — the Linux sanitizers finish 86
minutes earlier — and almost everything that has gone wrong with their caches is a property of the
**Actions cache store** rather than of caching:

- the store is a **wall, not an eviction threshold**. Past ~9537 MiB the whole store goes
  read-only and every save in the repository is refused; `actions/cache/save` reports that as a
  warning and exits zero, so the step's own status cannot tell you
  ([#1073](https://github.com/drsnuggles8/OloEngineBase/issues/1073)).
- an entry written on a `pull_request` run is scoped to `refs/pull/N/merge` and readable by that
  one PR. Six concurrent PRs is a routine day here and does not fit.
- the save is a post-job step, and a cancelled run skips it. `cancel-in-progress` cancels most PR
  runs here.

A persistent runner has none of those, because **its disk is the cache**: no upload, no download,
no cap, no ref scoping, no post-job step. That is the same move `asan.yml` made for the Linux
sanitizer jobs in #1009, and `setup-vcpkg` already implements the two-sided version of it — on a
self-hosted runner it uses a persistent directory in the runner user's home and skips
`actions/cache` entirely.

It also frees budget. The steady set is ~6.1 GiB of a ~9.3 GiB wall
([actions-cache-budget.md](../agent-rules/actions-cache-budget.md)); the two Windows sccache
entries are 1825 MiB and ~660 MiB of that.

## 2. What landed

In `Windows.yml` (`build`, `tests`) and `asan.yml` (`asan-windows`, `asan-windows-tests`):

```yaml
runs-on: ${{ vars.OLO_WINDOWS_SELF_HOSTED == 'true'
             && github.event_name != 'schedule'
             && (github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository)
             && fromJSON('["self-hosted","windows","x64","olo-ci-win"]') || 'windows-2025' }}
```

Read left to right, because the order is the safety property:

1. **The kill switch comes first**, so unsetting the variable can only ever move a job *off* the
   box, never onto it.
2. **The nightly stays hosted.** It is this workflow's only fleet-wide cache warmer — `actions/cache`
   entries created on a PR branch are scoped to that ref, and only default-branch entries are
   readable by every branch. Route the cron to the box and the hosted fallback goes permanently
   cold. (The Linux sanitizers keep their nightly hosted for the same reason.)
3. **The fork boundary is `head.repo.full_name`**, never a `pull_request_target` that checks out
   the PR head. An untrusted fork PR must never execute on a persistent machine.
4. **`olo-ci-win`, not `olo-ci`.** Runner **groups are unavailable on a user account**, so
   isolation from the existing Linux runners is by label only.

Alongside it: `runner.environment == 'github-hosted'` now gates every Windows sccache restore and
save; `SCCACHE_DIR` is repointed out of the workspace on a self-hosted runner, because
`actions/checkout` wipes the workspace and would otherwise discard the cache on every run while
looking perfectly healthy; `choco install nasm` is hosted-only; and a self-hosted **preflight**
verifies `cmake`, `ninja`, `nasm`, `python`, `cl`, `jinja2` and `VULKAN_SDK` and fails the job
naming what is missing — *verify, never install*, the convention
[`setup-linux-build`](../../.github/actions/setup-linux-build/action.yml) states for the Linux box
after an unconditional `sudo apt-get` once kept the nightly red for days.

## 3. Decision

1. **The routing lands switched off.** It is reviewable now, alongside the sharding and cache work
   it interacts with, rather than as a separate change later against a moved target.
2. **Enabling it is the maintainer's call, not CI's.** Setting `vars.OLO_WINDOWS_SELF_HOSTED` and
   registering a runner are the two acts this ADR does not perform.
3. **Nothing here is measured on a self-hosted Windows runner, and none of it can be until one
   exists.** #1076's acceptance — build-step duration and sccache hit rate on both paths, and the
   Windows cache entries gone from the store — is unmet by construction. The issue stays open.
4. **ADR 0017 §3 still stands: not the interactive workstation.** Its four reasons are unchanged —
   toolchain parity becomes a CI-visible event on every VS update; a runner service is not an
   agent shell and so runs outside `build-lock.ps1`; the RTX 4090 would execute the ~200 GL-gated
   tests that skip on `windows-2025`, which is how routing changed *what was tested* on the Linux
   box (215 failures on one commit, #1010); and a public repo where the same-repo condition is the
   only isolation should not share an account with the worktrees, the SSH keys and the `gh` login.

## 4. What provisioning it costs, stated before anyone agrees to it

- A Windows host that is **not** the workstation, with the VS 2022/2026 build tools, the Windows
  SDK, the Vulkan SDK, nasm and Python + jinja2 — what `Windows.yml` currently provisions per run,
  provisioned once and kept in step with the hosted image by hand.
- **Memory.** The box that would host it already runs the Linux runners and is memory-bound; a
  Windows engine build peaks high enough that concurrency limits have to be set deliberately
  rather than discovered. It has been OOM-killed by an uncapped build before.
- **Runner slots, and this is the one that decides whether #1076 and #1083 can coexist.**
  [#1083](https://github.com/drsnuggles8/OloEngineBase/issues/1083) split the test steps across a
  matrix — 4 shards for ASan, 3 for `Windows / build`. Shards follow the build job's routing
  because `gtest_discover_tests` bakes absolute paths into the generated test list. **On a box with
  fewer runner slots than shards they queue, and the wall-clock the sharding bought is handed
  straight back.** Seven concurrent Windows jobs on one machine is not a small ask; a smaller shard
  count on the self-hosted path is the obvious trade and is unmeasured.
- **One compiling runner instance per Windows account.** sccache is a per-**user daemon**: the
  first client to start it fixes the server's environment and every later client on that account is
  served by it whatever `SCCACHE_DIR` that client set. The Linux box sidesteps this by using
  ccache, which has no daemon; there is no equivalent drop-in for clang-cl here.

## Considered options

- **Enable it in this change.** Rejected: there is no runner, so it would be a switch pointing at
  nothing, and #1076's acceptance is measurements from both paths.
- **Leave `Windows.yml` alone until a runner exists.** Rejected: the same files are being
  restructured for #1083 right now, and writing the routing later means writing it against a tree
  that has moved, in a PR with no other reason to touch these jobs.
- **Route the nightly to the box as well.** Rejected: it is the only producer of the default-branch
  cache entries the hosted fallback and every fork PR restore from.
- **Reuse the `olo-ci` label.** Rejected: no runner groups on a user account, so the label is the
  only isolation there is between a Windows engine build and three Linux sanitizer jobs on a
  memory-bound box.
