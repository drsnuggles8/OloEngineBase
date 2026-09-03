# The Windows CI critical path is measured on a writable cache before any self-hosted Windows runner is built, and never on the interactive workstation

Issue [#1015](https://github.com/drsnuggles8/OloEngineBase/issues/1015), item F. #1009 named
the Windows workflow the PR critical path (159 min median over 250 runs) and #1010 shipped
the cache fixes meant to bring it down. This ADR records what the runs since then show, why
no Windows runner is built now, and what would make the question worth reopening. The numbers
below come from the Actions REST API (`runs`, `jobs`, per-job step timings) for 273 completed
runs between 2026-08-18 and 2026-09-02. The table below lists the PULL-REQUEST rows only, which
is what the critical-path question is about; the remaining 62 are `push` runs of the same two
workflows (Windows 30, Sanitizers 28, medians 171 and 170), and they are what the nightly
cache-warming argument rests on.

---

## 1. What the post-#1010 runs actually measured

Run-level critical path in minutes (completed runs only; cancelled runs excluded):

| Workflow | Window | n | median | p90 |
|---|---|---|---|---|
| Windows | before #1010 merged, PRs | 107 | 175 | 208 |
| Windows | #1010's own branch, new YAML, warm PR-scoped cache | 4 | 128 | 194 |
| Windows | after the merge, PRs | 1 | 172 | – |
| Sanitizers | before #1010 merged, PRs | 98 | 181 | 299 |
| Sanitizers | after the merge, PRs | 1 | 171 | – |

The Windows `build` job is compile-bound: the "Build with CMake" step is 131 min median
cold (p90 139), configure 37 min cold, ctest 21 min flat, queue time under a minute. The two
#1010 runs that restored a warm sccache entry reported a 66.8 % hit rate (1021 of 1528
compiles) and built in 69 and 94 min, with configure at 3 min because the vcpkg binary
cache restored 115 of 116 ports. That is the best available measurement of what #1010
delivers: a run of roughly 110 to 130 min instead of 171.

The one completed post-merge PR run got none of it: `Cache not found for input keys:
sccache-windows-2025-release-…`, 0 of 1531 hits, a 141-minute build. There is no master
sccache entry for the new key because the Actions cache store has been read-only since
2026-09-01 21:00 (`Cache reservation failed: You have reached your configured budget`,
10.98 GB active against the 10 GB cap), `cache-prune.yml` had never fired (its first cron
slot was one minute after the merge), and all seven other post-merge PR runs were cancelled
by a later push. The vcpkg half of #1010 is proven fleet-wide; the sccache half is proven on
one branch and blocked by the store.

## 2. Decision

1. **No self-hosted Windows runner is built for now.** The only sample that could justify
   one is n=1 and cold by accident; the warm measurement says the hosted path drops to about
   110 to 130 min once the store accepts writes, which is close to the sanitizer jobs' own
   170 min. A runner that removes 40 minutes from one workflow while the sibling stays at 170
   does not move the PR.
2. **Measure again after two preconditions:** the cache store is writable, and one nightly
   has banked a master `sccache-windows-2025-release-*` entry. The first was done on
   2026-09-02 while writing this: `cache-prune.yml` dispatched by hand (run 33641842057)
   reported success and deleted nothing, because the 3.1 GB that mattered were two entries
   scoped to `refs/pull/1010/merge`, a merged PR's ref that no run can read again and that the
   prune does not consider. Deleting those two by hand brought the listed total from 10.98 to
   8.66 GB. The prune now deletes every entry on a closed PR's ref (this PR). That is the
   store brought back UNDER the cap; it is not yet proof that writes resume, which needs one
   observed successful cache save followed by a PR run restoring a
   `sccache-windows-2025-release-*` entry written on `master`. Until both appear in a log, treat
   the precondition as unmet. Then collect at least ten completed post-nightly PR runs and
   recompute the table above the same way. The question is
   reopened only if the warm Windows median is still the ceiling by a margin that a runner
   would close.
3. **If a Windows runner is ever built, it is a separate machine or VM, not the interactive
   workstation**, and it takes the routing shape `vulkan-off.yml` uses: a `vars.` kill switch
   first, then `github.event.pull_request.head.repo.full_name == github.repository`, then the
   label set, with `windows-2025` as the fallback; never a `pull_request_target` that checks
   out the PR head.

## 3. Why not the workstation

Probed 2026-09-02. The workstation has no GitHub runner today (no service, no `Runner.Listener`,
no runner directory on any drive); the "hosts runners for another repo" sentence in the
build notes describes the Linux box. Beyond that:

- **Toolchain parity is a moving target.** The hosted image is VS 2026 with MSVC toolset
  14.51.36231 and the VS-bundled clang-cl 20.1.8; the workstation has the same MSVC toolset
  today, clang-cl 21.1.8 from a separate LLVM install, and Vulkan SDK 1.4.357.0 against the
  hosted 1.4.321.0. The sccache key hashes the compiler binary, so every VS update on the
  workstation becomes a CI-visible event.
- **The builds would run outside the build lock.** `build-lock.ps1` bounds concurrent builds
  for agent shells through a hook; a runner service is not an agent shell. A CI build at
  Ninja's default width on top of interactive builds that already peak near 47 GiB is the
  OOM the lock exists to prevent.
- **The GPU changes what the suite tests.** The workstation's RTX 4090 would execute the
  roughly 200 GL-gated tests that skip on `windows-2025`, the same behaviour change #1010
  met on the Linux box (215 failures on one commit). "Faster Windows CI" would arrive as a new
  golden-baseline problem unless the job passed `--olo-gl-backend=none`, at which point the
  GPU buys nothing.
- **A live editor blocks the relink**, the box sleeps and reboots with its user, and the
  runner account would need to be kept away from the worktrees, the SSH keys and the `gh`
  login on a public repository where the same-repo condition is the only isolation. A user
  account has no runner groups.

## 4. Cheaper levers, in order

1. Free the cache store and keep it under the cap: the prune workflow, and a check that a
   PR-scoped sccache entry is never the only one for its key.
2. `cmake --build` in `Windows.yml` passes no `--parallel`; Ninja therefore runs six lanes on
   four vCPUs. Whether four lanes is faster is unmeasured and cheap to A/B once the cache is
   warm; it is not changed blind here.
3. The ASan (Windows/clang-cl) job is 172 min median regardless of cache state because it
   compiles without sccache (the server crashes on its largest TUs). If Windows drops to 110
   min, this job is the next ceiling and the Linux sanitizers on the self-hosted box the one
   after it. Any runner decision has to account for all three, not the first.

## Considered options

- **Windows runner on the workstation now.** Rejected: see §3, and the sample does not
  support the need.
- **Windows runner on a dedicated machine now.** Deferred: the warm-cache measurement is
  within 40 min of the sibling workflow; buy hardware for a measured gap, not a projected one.
- **Move the ASan (Windows) job to the Linux box.** Rejected: its point is MSVC ABI and the
  Windows-only platform code.
