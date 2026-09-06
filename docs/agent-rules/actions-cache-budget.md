# The Actions cache store is a fixed budget, and every key must fit in it

Issue [#1073](https://github.com/drsnuggles8/OloEngineBase/issues/1073).

**Rules, first:**

1. **A cache key that ends in `-<run_id>` is append-only.** It never replaces the entry
   it supersedes. Something has to delete the predecessor, and a janitor that runs once
   a day cannot do it between two saves thirty minutes apart. Use
   [`.github/actions/save-cache-pruned`](../../.github/actions/save-cache-pruned/action.yml),
   which asks the API whether the save actually landed and, only if it did not, deletes
   this ref's older entries under the same prefix and retries.
2. **Never save a large cache on a `pull_request` run.** The entry is scoped to
   `refs/pull/N/merge`, is readable by that one PR, and is charged to the whole
   repository for as long as the PR stays open.
3. **A key that embeds a content hash needs the hash stripped when you group entries for
   pruning.** Otherwise every dependency bump starts a new group whose single member is
   immortal — and so is the entry it superseded.
4. **Before redesigning a cache, add up what the fleet already stores.** The cap is
   ~9537 MiB. If the steady set does not leave room for the largest single snapshot, no
   key design will help.

---

## What happened

`sccache --show-stats` on the Windows build, every run, for weeks:

```
Compile requests    1630
Cache hits             0
Cache misses        1630
Cache hits rate     0.00 %
Average compiler   24.690 s
```

1630 requests × 24.69 s is **11 h 10 m of compiler time**, against a measured build step
of **2 h 31 m** — the difference is Ninja's parallelism, about 4.4× on a 4-vCPU runner.
Both numbers matter: the wall clock is what a PR waits for, and the compiler time is what
a working cache would have removed almost all of. Every step was green.

The cause was not a hash input, not `SCCACHE_CACHE_SIZE`, not `/Zi` vs `/Z7`. **The cache
entry did not exist.** A full listing of the repository's cache store contained no
`sccache-windows-2025-release-*` at all — not a stale one, not a small one, none. The
restore step said so plainly and nobody was reading it:

```
Cache not found for input keys: sccache-windows-2025-release-33993528874-1,
                                sccache-windows-2025-release-
```

and three and a half hours later, at the save:

```
##[warning]Cache reservation failed: You have reached your configured budget,
           your cache is now read only to prevent additional charges.
Failed to save: Unable to reserve cache with key sccache-windows-2025-release-…,
                another job may be creating this cache.
```

The same pair appeared on the **nightly master run**, which is this workflow's only
fleet-wide cache producer. The warmer could not write. So the restore had nothing to
find, so every compile missed, so the build took four hours, indefinitely and stably.

## Why the existing defences did not hold

[ci-cache-that-looks-alive.md §3b](ci-cache-that-looks-alive.md) had already found the
read-only budget, a year of runs earlier, and written the rule down. `cache-prune.yml`
had already been built to keep the store under the cap. Both were in place. The store
filled anyway, for three reasons that the earlier work did not reach.

### The cap is a wall, not an eviction threshold

`ci-cache-that-looks-alive.md` §4 says GitHub "was already evicting" LRU. It is not, and
that misreading is load-bearing: it makes an over-full store sound self-correcting.
It is not self-correcting. Past the cap the **whole store goes read-only** and every save
in the repository is refused until something is deleted. `actions/cache/save` reports
that as a `warning` and **exits zero**, so the step's own status cannot tell you.

The cap is 10 GB **decimal**, and that is measured rather than read off the docs: saves
were being refused with the store at 9,713,535,876 bytes = 9264 MiB. Had the cap been
10 GiB (10737 MiB) there was 1.4 GiB free and the 706 MiB vcpkg save in the very same run
would have landed. It did not. The wall is ~9537 MiB.

### The janitor could not collect the biggest orphan

`cache-prune.yml` kept the newest entry per `(ref, prefix)`, where the prefix was the key
minus its trailing run id. It said of the leftovers:

> A prefix that has genuinely gone away (a renamed key) therefore leaves one orphan
> behind; one entry per dead prefix is a rounding error against the cap

The vcpkg key is `vcpkg-<os>-<triplet>-<64 hex manifest hash>-<run_id>-<attempt>`. Strip
only the run id and **every `vcpkg.json` bump is a brand-new prefix**, so the bump orphans
a ~2 GB entry permanently. The largest single item in the store was 1988 MiB of a
superseded manifest hash, created three days earlier and never read once — its
`last_accessed_at` had not moved off its creation time, because the current-hash entry
matched first. Not a rounding error: 21 % of the cap, alone.

The prefix now strips a trailing content hash as well as the run id, so a hash bump
supersedes instead of orphaning. That is correct for every key of this shape here,
because a restore reaches at most one entry of a lineage anyway — `ffmpeg-…` restores by
exact key, and `vcpkg-…` falls back to the bare `vcpkg-<os>-<triplet>-` prefix, which
GitHub answers with the newest match. A **30-day `last_accessed_at` age-out** catches
what remains: a genuinely renamed prefix leaves an orphan that matches nothing, so its
read timestamp never moves again, while a live entry keeps being read.

Thirty days, not fourteen, and the difference is the whole safety of the rule. The read
cadence is **not** daily for every prefix: `Linux-vulkan-prebuilt-sdk-true-1.4.321.0` was
created 2026-08-19 and last read 2026-09-02 — a fortnight with no read in it — and went
unread for four days after that while the Sanitizers nightly ran every one of them. An
age-out shorter than a prefix's real read interval deletes live caches. Nothing is lost
by waiting: the orphans this catches are permanent, and the recurring leak is handled by
the hash strip, not by this.

### Six open PRs do not fit in a 10 GB store

Both Windows and the Linux sanitizer jobs saved their sccache dir on `pull_request` runs.
That entry is visible to one pull request. Six concurrent PRs — a routine day here — is
up to 12 GB of PR-private Windows sccache against a 9.5 GiB wall. The PR runs were
buying a marginal delta over the master snapshot they already restore, and paying for it
by holding the store read-only, which stopped the nightly from writing the snapshot every
branch actually restores from. **The PR runs were funding the mechanism that kept them
cold.** They now restore and never save.

## The arithmetic, so the next person does not have to re-derive it

Measured 2026-09-06, after the sweep. Sizes are the **compressed tarball**, which is what
counts against the cap — not `SCCACHE_CACHE_SIZE`, which bounds the local directory
before compression:

| Entry | Size |
|---|---|
| `sccache-asan-lsan-linux` / `sccache-ubsan-linux` / `sccache-tsan-linux` | 1252 / 1231 / 1258 MiB |
| `vcpkg-Windows-x64-windows-static-md` | 692 MiB |
| `Linux-` / `Windows-vulkan-prebuilt-sdk` | 291 + 229 MiB |
| `ffmpeg-Windows-n7.1` | 4 MiB |
| **measured subtotal** | **4957 MiB** |
| `sccache-windows-2025-release` | **not measured — no entry has ever existed to measure.** `SCCACHE_CACHE_SIZE` caps the local dir at 2 GiB; `sccache-flaky-281`, same cap and the same build, compressed to 886 MiB. Expect 0.9–2.0 GiB; plan against 2.0 until a real one lands. |
| **steady set** | **5.7 GiB likely, 6.8 GiB worst case** (4.84 GiB measured + 0.87–2.0), against a ~9.3 GiB wall — so 2.5 GiB of margin even at the bound |

Do not quote the sccache row as a measurement until an entry has been written and listed.
The number that mattered here was one nobody had ever read.

That is why `cache-prune.yml` now **fails** when
the post-sweep store is over 8800 MiB: everything it deletes is provably superseded or
unread, so if what remains is still that close to the wall, the fleet has outgrown the
cap and a human has to shrink something. Raising the ceiling to silence the alarm
re-creates this bug.

## Adding a cache to this repo

- Add its steady size to the table above and check the total still clears the wall.
- Save through `save-cache-pruned`, with `prefix` set to the key minus its
  `-<run_id>-<attempt>` tail, and grant the job `actions: write`.
- Gate the save on `github.event_name != 'pull_request'` unless the entry is small
  (a few MiB) or genuinely PR-specific.
- **A `workflow_dispatch` run on a `feature/*` branch banks its caches against that
  branch's ref**, and those entries outlive the branch. `cache-prune.yml` now deletes
  entries whose branch is gone, mirroring the closed-PR rule — but if you dispatch a
  heavy workflow on a branch to measure something, expect ~1.7 GiB of branch-scoped
  entries to exist until it sweeps.
- If the key embeds a content hash, confirm `cache-prune.yml`'s prefix regex strips it
  (`-[0-9a-f]{32,}$`). A key whose varying part is **not** hex — the Vulkan SDK entries end
  in a version, `…-prebuilt-sdk-true-1.4.321.0` — is not stripped, so a version bump
  orphans the old entry and only the 30-day age-out collects it. Acceptable at 229 MiB;
  it would not be at 2 GiB. Size the key shape to the entry.
- **Check the read cadence before relying on the age-out** (see the 30-day note above).
- **A workflow that is finished is a cache that is still charged.** The nightly
  `flaky-repro-281` hunt held 886 MiB for an investigation closed months earlier; its
  schedule is gone. Retire the schedule when you retire the question.

## See also

- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) — the same store, one
  layer up: four ways a cache restores, logs a hit, and rebuilds everything anyway.
  Read §3b first; this file is what happened when only §3b's *rule* was written down and
  not its arithmetic.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md) — where the manifest
  hash in the vcpkg cache key comes from.
- [docs/ops/self-hosted-gpu-runner.md](../ops/self-hosted-gpu-runner.md) — the structural
  escape: a cache on local disk has no save step to refuse, no ref scoping and no cap.
