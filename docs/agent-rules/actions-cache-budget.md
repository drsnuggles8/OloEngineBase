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

1630 × 24.7 s is essentially the whole 2 h 31 m build step. Every step was green.

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
GitHub answers with the newest match. A **14-day `last_accessed_at` age-out** catches
what remains: a genuinely renamed prefix leaves an orphan that matches nothing, so its
read timestamp never moves again, while a live entry is read daily.

### Six open PRs do not fit in a 10 GB store

Both Windows and the Linux sanitizer jobs saved their sccache dir on `pull_request` runs.
That entry is visible to one pull request. Six concurrent PRs — a routine day here — is
up to 12 GB of PR-private Windows sccache against a 9.5 GiB wall. The PR runs were
buying a marginal delta over the master snapshot they already restore, and paying for it
by holding the store read-only, which stopped the nightly from writing the snapshot every
branch actually restores from. **The PR runs were funding the mechanism that kept them
cold.** They now restore and never save.

## The arithmetic, so the next person does not have to re-derive it

Measured 2026-09-06, after the sweep:

| Entry | Size |
|---|---|
| `sccache-windows-2025-release` | 2.0 GiB |
| `sccache-asan-lsan-linux` / `sccache-ubsan-linux` / `sccache-tsan-linux` | 1.2 GiB each |
| `vcpkg-Windows-x64-windows-static-md` | 0.7 GiB |
| `Windows-` / `Linux-vulkan-prebuilt-sdk` | 0.5 GiB together |
| `ffmpeg-Windows-n7.1` | 4 MiB |
| **steady set** | **~7.9 GiB against a ~9.3 GiB wall** |

The margin is one Windows snapshot wide. That is why `cache-prune.yml` now **fails** when
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
- If the key embeds a content hash, confirm `cache-prune.yml`'s prefix regex strips it.
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
