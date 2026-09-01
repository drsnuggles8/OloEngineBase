# A CI cache that restores is not a CI cache that works

Issue [#1009](https://github.com/drsnuggles8/OloEngineBase/issues/1009).

Every job in this repo restored a cache, logged a hit, and rebuilt everything
anyway. It had been doing that for at least six days, across three independent
defects, while every run stayed green and every log line read as success.

**What stayed green:** all of it. A cold cache is not a failure — it is a slow
success. There is no red, no warning, and no assertion anywhere that a cache
achieved anything. The only tell is a number nobody was reading.

The measurement, from one representative Windows run (33502248619):

```
Compile requests   1516
Cache hits            0
Cache misses       1516
Cache hits rate    0.00 %
```

and, from the same run's configure step:

```
Restored 3 package(s) from .vcpkg-binary-cache in 139 ms
Installing 4/116 utfcpp... Building...
```

Three packages restored out of 116, and zero cache hits out of 1516 compiles.
Configure took 40 minutes, the build 102. Neither step failed.

---

## 1. A post-job step does not run when the job is cancelled

`actions/cache` — the combined action — saves in a **post-job** step. GitHub
skips post-job steps when a job is cancelled.

That is a footnote until you look at how runs here actually end. Over the 36-hour
window #1009 measured, `cancel-in-progress` killed **17 of 27** Windows PR runs
and **15 of 24** Sanitizer PR runs. An agent pushing a follow-up commit cancels
the in-flight run; that is normal and desirable. But every one of those runs
rebuilt the world and banked none of it, so the next run was exactly as cold,
took exactly as long, and had a correspondingly wider window in which to be
cancelled itself. The loop is self-reinforcing and it has no visible symptom.

The fix is `actions/cache/restore` + `actions/cache/save` with `if: always()`,
because `always()` **does** fire on a cancel. Place the save immediately after
the step that produced the expensive artefact, not at the end of the job: the
vcpkg save goes after *configure*, so a cancellation anywhere in the build or
test tail has already banked the 40-minute half.

**This repo already knew.** `asan.yml`'s tsan job had been split this way, with a
comment explaining exactly this, and nothing else had followed. The evidence that
it worked was sitting in the cache listing the whole time — one line of
`gh api repos/<owner>/<repo>/actions/caches`:

```
created_at            key
2026-08-26T08:25:59Z  sccache-tsan-linux-32942319314-1     <- restore/save, if: always()
2026-08-19T09:01:04Z  sccache-asan-lsan-linux-32225872289  <- combined action
2026-08-12T09:33:11Z  sccache-windows-release-31571109867  <- combined action
```

Same repo, same kind of job, one week and two weeks older respectively. A working
example beside a broken one is not a fix; somebody has to notice.

Read `created_at`, not `last_accessed_at` — on that field all three look recent,
because a restore touches an entry without replacing it. Section 3b is the reason
that distinction turned out to matter far more than it looks here.

## 2. Only default-branch cache entries are readable fleet-wide

This is the one that inverts an obvious optimisation into a regression.

GitHub scopes a cache entry to the ref that created it. An entry written by a PR
run is visible to **that PR only**. An entry written on the default branch is
visible to every branch. So of the 15 active entries in this repo, all 15 were
`ref=refs/heads/master`, and **every PR in the repo was warming from a master
run's snapshot**.

The optimisation on the table was "drop `push: master` for Windows and
Sanitizers — CI already tested the merge commit as the PR, so this is ~1,400
runner-minutes a day for a second opinion on identical code." That reasoning is
correct as far as it goes. It also happens to delete the only cache writer in the
system, which would have made every PR permanently cold — a change that reads as
a pure saving and is precisely the opposite.

The master trigger was replaced with a **nightly cron** rather than deleted, so
the warmer survives.

Second-order, and it is what made the snapshots *stale* rather than merely
infrequent: the concurrency expression was

```yaml
cancel-in-progress: ${{ github.event_name == 'pull_request' || github.event_name == 'push' }}
```

so a burst of merges cancelled the master runs too — combining defect 1 with
defect 2. The master sccache snapshot was six days old while every PR dutifully
restored it. `cancel-in-progress` is now `pull_request`-only everywhere a cache
is written.

**The rule: before removing a trigger, ask what else that trigger was doing.** A
scheduled build is often load-bearing for something that is not the build.

## 3. A rolling runner image is a hash input

`windows-latest` is an alias. When GitHub rolls it, `cl.exe` changes, and sccache
hashes the compiler binary into every cache key. So an image roll invalidates the
entire compiler cache in one step — and it does it *silently*, because the cache
still restores (the key prefix still matches), still downloads its two gigabytes,
still occupies its slice of the 10 GB cap, and hits on nothing.

Zero hits out of 1516 is not source churn. A source change cannot invalidate
every translation unit; if the number is *exactly* zero, something upstream of
the source changed. Pin the image (`windows-2025`), and put the image name in the
cache key, so that the next deliberate bump ages the dead entries out instead of
restoring them forever.

## 3b. The cache can be READ-ONLY, and only one line in one log says so

The three defects above are real and each was worth fixing. None of them was the
largest one, and the largest one was invisible until a job tried to *write*:

```
##[warning]Cache reservation failed: You have reached your configured budget,
your cache is now read only to prevent additional charges.
Failed to save: Unable to reserve cache with key vcpkg-Linux-…, another job
may be creating this cache.
```

GitHub bills Actions cache storage above the included allowance, and a
configured spending budget of zero does not fail loudly — it flips the whole
repository's cache to **read only**. Every restore keeps working. Every save
fails. The job stays green, because a cache save is not allowed to fail a job.

Two things about that message make it hard to find. It is a `warning`, not an
`error`, so it does not colour a check red or appear in any summary. And the line
*underneath* it — the one that looks like the failure — says `another job may be
creating this cache`, which is the action's generic 409 text and sends you
hunting for a key collision that does not exist. The cause is the line above.

**The measurement that names it in one command**, and the reason this went six
days unnoticed:

```console
$ gh api "repos/<owner>/<repo>/actions/caches?per_page=100" \n    --jq '.actions_caches[]|"\(.created_at)  \(.key)"' | sort -r | head -3
2026-08-26T08:25:59Z  sccache-tsan-linux-32942319314-1
2026-08-26T08:10:55Z  vcpkg-Linux-x64-linux-9704e01b…-32942319409
2026-08-26T08:09:36Z  vcpkg-Linux-x64-linux-9704e01b…-32942319336
```

**Not one entry written in six days**, across hundreds of runs of a dozen
workflows. `created_at` is the field that matters; `last_accessed_at` keeps
ticking forward on every restore and makes a frozen store look busy.

The store sat at 11.6 GB against a 10 GB included allowance, which is what
tripped the budget. So the *quota* item on the issue's list — the one that reads
like housekeeping — was in fact the gate on everything else: until the active
size is back under the allowance (or the budget is raised), no fix to restore/save
splitting, key design or trigger cadence can be observed at all, because nothing
can be written.

**The rule: when a cache is not warming, check that it is WRITABLE before
redesigning anything.** `created_at` on the newest entry answers it, and a
frozen `created_at` across every prefix at once is not a bug in your keys.

## 4. LRU eviction cannot tell current from superseded

The repo held 11.6 GB against a 10 GB cap, so GitHub was already evicting. Its
policy is least-recently-used, and LRU has no idea which entry is the live
snapshot: it will happily evict the newest 2 GB Windows sccache to make room,
having just seen a superseded 786 MB one get touched by a restore-keys prefix
match.

If you are over the cap, the eviction policy is already yours whether you wrote
one or not. `.github/workflows/cache-prune.yml` keeps one entry per key prefix.

---

## The counter-move

**Every cache needs a number in the log, and somebody has to read it.** Not "did
the restore step succeed" — it always does. The three that matter here:

| Cache | The line | A working value |
|---|---|---|
| sccache | `sccache --show-stats` → `Cache hits rate` | high, and never exactly 0.00% |
| vcpkg | `Restored N package(s)` in the configure step | N near the manifest's port count (116) |
| ccache | `ccache -s` → direct/preprocessed hits | non-zero after the first run |
| the store itself | `created_at` of the newest entry (above) | today, not six days ago |

A cache is one of the purest instances of the "your instrument is lying to you"
archetype: it has no wrong-looking failure state. It fails by being slow, and
slow is what everyone already expects CI to be. When you change anything that
touches one — the key, the runner image, a trigger, a compiler flag, a toolchain
version — the acceptance criterion is a **stats line from a real run**, not a
green check.

And when you are about to conclude a cache is fine because it restored: read
`gh api repos/<owner>/<repo>/actions/caches` and look at the dates. It costs one
command, and in this case it would have named two of the three defects on its
own.

## See also

- [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §6 — the
  same genre one level down: a ccache hit that restored the object but not its
  dependency file, so 699 of 701 objects had no header dependencies recorded and
  a header edit rebuilt nothing. The better the cache worked, the more of the
  tree was frozen.
- [docs/ops/self-hosted-gpu-runner.md](../ops/self-hosted-gpu-runner.md) — the
  structural escape from defects 1, 2 and 4 at once: a cache on local disk has no
  save step to skip, no ref scoping and no cap.
