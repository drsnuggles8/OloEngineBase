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
4. **Before redesigning a cache, add up what the fleet already stores.** The cap was
   ~9537 MiB until 2026-09-09 and is **25 GB** now (see *The cap moved* below); the
   binding number today is `cache-prune.yml`'s 20000 MiB working ceiling, not the cap. If
   the steady set does not leave room for the largest single snapshot, no key design
   will help.
5. **The store exists to speed up GITHUB-HOSTED jobs on a PULL REQUEST. Nothing else has
   a claim on it.** Everything else either has a better cache or has no one waiting:
   a self-hosted job caches on local disk, which has no save step to refuse, no ref
   scoping and no cap; a nightly is not on anyone's critical path. Spending the cap on
   those is spending it on latency nobody experiences — and it is how the Windows ASan
   job came to have no compiler cache at all while 3741 MiB sat in entries returning
   0.00 % (see *A cache can be worth removing*). Work out which jobs are hosted **and**
   PR-triggered before allocating anything; that list is much shorter than the workflow
   list.
6. **Set the cap ABOVE the object set, and measure the object set with `du`.** A cap a
   little under what a build produces does not cost you a little. `SCCACHE_CACHE_SIZE:
   900M` against object sets measured at 945 / 994 / 1084 MiB returned **93.88 / 69.89 / 30.62 %**
   hit rates on identical source, and the arm that was 20 % over the cap was the one
   that built for **36.7 minutes**; clearing all three took them to **99.94 %** and
   1.4-1.7 min. sccache evicts LRU while a build reads its objects in a broadly stable
   order, so a set that does not fit evicts entries this run is about to need, each miss
   writes an object and evicts another, and the shortfall compounds. Measure the set
   with `du -sm "$SCCACHE_DIR"` at a deliberately oversized probe cap — `--show-stats`
   **rounds**, and printed "Cache size 1 GiB" for the 1084 MiB arm, a range that covers
   1024 to 2047 MiB and therefore cannot pick a cap. Then set the cap from the store's
   worst case, not from the set. Both halves, measured, with the arithmetic:
   [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md).

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

### The cap moved: 25 GB from 2026-09-09

The 10 GB cap is no longer the constraint. It is [configurable since
2025-11-20](https://github.blog/changelog/2025-11-20-github-actions-cache-size-can-now-exceed-10-gb-per-repository/)
for a repository with a payment method on file — **not** a plan upgrade, which is what
the changelog's "requires a valid Pro, Team, or Enterprise account" reads like and the
reference docs contradict ("only available to users with a payment method on file who
opt in by configuring cache settings"). Set at Settings → Actions → General → Cache
settings; user-owned repos go to 10 TB. Billing is hourly **peak** above 10 GiB at
$0.07/GiB-month, so the ceiling itself is free — this repo's measured peak bills about
$0.04/month.

This repo: **25 GB**, with an Actions budget, from 2026-09-09.

Two things that do NOT follow from the raise:

* **The read-only failure above is still real, and the budget is now a second way to
  reach it.** The warning this file quotes says "you have reached your **configured
  budget**" — that is the budget path, which goes read-only. Plain overflow of the free
  10 GB evicts LRU instead. A budget of $0 against a raised cap reproduces the original
  bug exactly, which is why the budget is set rather than merely the cap.
* **`cache-prune.yml`'s working ceiling had to move with it**, and did not at first — for
  a few hours the raise was inert, because six sweeps a day went on enforcing the old
  limit. It is 20000 MiB as of 2026-09-09. See the note on it below.

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

Re-measured 2026-09-09 (#1084). Sizes are the **compressed tarball**, which is what
counts against the cap — not `SCCACHE_CACHE_SIZE`, which bounds the local directory
before compression:

| Entry | Size |
|---|---|
| `sccache-asan-lsan-linux-llvm-23.1.0` / `-ubsan-` / `-tsan-` | **1114 + 1137 + 1189 = 3440 MiB measured** 2026-09-09, the first full-size banking at the **1300M** cap #1118 raised them to (from 900M, which measured under all three object sets — `du` put those at 945 / 994 / 1084 MiB and returned 93.88 / 69.89 / 30.62 % hit rates). They land at 86–91 % of the cap, as rule 6 predicts. The 2231 MiB this row carried on 2026-09-08 was the 900M-era set and is not comparable. Both halves of the cap decision: [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md). Removed entirely 2026-09-06 by #1082 at 0.00 % (see *A cache can be worth removing*), re-added by #1095 once the compiler underneath the key stopped rolling. |
| `vcpkg-Windows-x64-windows-static-md` | **706 MiB measured** 2026-09-09 (692 MiB on 2026-09-06) |
| `vcpkg-Linux-x64-linux` | **327 MiB measured** 2026-09-09 |
| `Linux-vulkan-prebuilt-sdk-true-1.4.321.0` | **291 MiB.** No 1.4.357.0 Linux entry exists; only `video-ffmpeg.yml`'s linux arm creates this, and it is weekly. |
| `Windows-vulkan-prebuilt-sdk-true-1.4.357.0` **and** `-1.4.321.0` | **256 + 229 MiB measured** 2026-09-09 — **one dependency, two live entries.** This is the version-in-the-key case rule 6's bullet list warns about: `cache-prune.yml` strips a trailing *hex* hash, and `1.4.321.0` is not hex, so the bump superseded nothing and the old entry sits until the 30-day age-out collects it (created 2026-07-07, last read 2026-09-08). 485 MiB for one SDK is the measured price of that key shape. |
| `ffmpeg-Windows-n7.1-637be53f…` | **4 MiB, and dead.** Its hash no longer matches any workflow, so nothing will ever restore it; the age-out collects it. The FFmpeg cache had in fact **never been saved on any run** — the path cached was not the path CMake installs into, so every save exited zero on a `Path Validation Error` (#1141). The working lineage that replaces it is keyed `ffmpeg-<os>-<hash of build-ffmpeg.sh + cmake/ffmpeg.cmake>` and banks ~19 MB uncompressed. |
| **measured subtotal** | **1813 MiB** (4 + 229 + 256 + 291 + 327 + 706), measured 2026-09-09 |
| `sccache-windows-2025-release` | **2044 MiB** — measured 2026-09-08 and again 2026-09-09. At its 2 G cap and evicting. Warm PR runs return 94.77 / 95.01 / 98.49 %, so the cap is not currently costing much, but it has no headroom left either. |
| `sccache-asan-windows-2025-vk1.4.357.0` | **667 MiB measured** 2026-09-09 — down from **1230 MiB** on 2026-09-08, and the drop is the useful part. #1118 renamed the key to carry the SDK version, which started a **fresh lineage**: this entry holds one build's objects, where the old one had accumulated objects from many source generations in a single dir. **So an object set measured off a long-lived entry overstates what one build needs** — the `du -sm` 1241 MiB that rule 6's arithmetic used for this job was such a measurement. Its 1500M cap now has ~830 MiB spare. |
| **steady set** | **7968 MiB measured** 2026-09-09 by summing `size_in_bytes` over the API listing, 11 entries (the floored rows above sum to 7964). Against a ~9537 MiB wall and `cache-prune.yml`'s 8800 MiB working ceiling that is **832 MiB of headroom to the ceiling** — down from 1738 MiB on 2026-09-08, because #1118's 1300M cap cost the sanitizer trio ~1200 MiB and the Vulkan SDK bump stranded another 229. **Against the 25 GB cap set the same day it is ~17 GB** (see *The cap moved*): the measurement stands, the "nothing sizeable can be added" conclusion it carried does not. What binds is `cache-prune.yml`'s working ceiling, **20000 MiB**, leaving this set **12032 MiB** of room. That headroom, not any object set, is what a cap has to be sized against — see [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md). |

`SCCACHE_CACHE_SIZE` bounds the local directory **before** compression, so it is not the
entry size — but do not read that as "the entry will be much smaller". Every sccache entry
here that **reached** its cap landed at **82–91 % of it**: 2 G → 1825 MiB, 1500M →
1231–1258 MiB. sccache already stores each object compressed, so the tarball has almost
nothing left to squeeze. **Treat the cap as the bill for any cache that fills it.**

The corollary, which the ASan Windows job demonstrates: a cache that does **not** reach its
cap costs what it actually holds, and the only way to know which case you are in is to
measure the directory. Its cap was set to 3 G deliberately so the first successful run
would report an *uncapped* footprint — 656 MiB — instead of being clipped to whatever had
been reserved. Set the provisional cap high, measure, then set it for real. And measure it
with `du`, not `--show-stats`: if the sccache server has exited, the client silently starts
a fresh one and reports its zeros (#1082).

That is why `cache-prune.yml` now **fails** when
the post-sweep store is over its working ceiling: everything it deletes is provably superseded or
unread, so if what remains is still that close to the wall, the fleet has outgrown the
cap and a human has to shrink something. Raising the ceiling to silence the alarm
re-creates this bug.

**That ceiling is the binding constraint, so it tracks the cap.** It was 8800 MiB against
a ~9537 MiB wall; the cap became 25 GB on 2026-09-09 and the constant is **20000 MiB**
since the same day. Moving it to follow a cap that really moved is not the mistake the paragraph
above warns about — that mistake is raising it *under a fixed cap* to quiet an alarm that
is telling the truth.

**Size the margin off the largest single entry, not off a percentage.** That is what the
old number got wrong: 8800 against 9537 left 737 MiB, which is less than
`sccache-windows-2025-release` alone (2044 MiB), so one save landing after a sweep could
clear the wall before the next sweep looked. 20000 against ~23841 leaves 3841 MiB, ~1.9x
that entry. A margin smaller than what can arrive inside one sweep interval is not a
margin.

**And a budget can bind before the cap does.** The read-only failure quoted above says
"you have reached your configured **budget**"; storage over 10 GiB bills hourly, so a
budget set too low goes read-only with the cap nowhere in sight. If the budget is ever
the tighter of the two, the ceiling has to be derived from it instead.

## A cache can be worth removing, and this one was worth 3741 MiB

Measured 2026-09-06 while looking for room for a fourth large Windows entry (#1082).
The three Linux sanitizer sccache entries were **3741 MiB — 55 % of the steady set** —
and on the nightly, the only runs that read them, all three reported:

```
Compile requests   1558      Cache hits          0
Cache misses       1558      Cache hits rate     0.00 %
Cache read errors     0      Cache write errors  0
```

The restore was healthy. `Cache restored from key: sccache-ubsan-linux-32225872289`,
1232 MB at 180 MB/s. It restored perfectly and hit nothing, three jobs out of three.

**The compiler was rolling underneath the key.** sccache hashes the compiler binary into
every cache key, and the hosted arm builds with apt.llvm.org's *snapshot* clang-23:

```
nightly 09-04   Ubuntu clang 23.1.1 (++20260901122056+5340f7cc8814)
nightly 09-05   Ubuntu clang 23.1.1 (++20260903063729+47bafb752202)
```

so every entry died at the next apt refresh — while still restoring, still downloading
1.2 GB, and still holding quota. This is the same failure `Windows.yml`'s header
documents for `windows-latest` image rolls (§ *the key carries the runner IMAGE*),
reproduced on Linux by an unpinned rolling compiler. **A pinned image is only half the
rule: pin whatever the key is hashed over, and on Linux that is the compiler package.**

Two things made it invisible. The hit rate was printed on every run and nobody read it —
which `ci-cache-that-looks-alive.md` already warns about. And almost nothing read the
entries at all: `OLO_LINUX_SELF_HOSTED` is true, so a same-repo PR routes these jobs to
the box, where they use local-disk ccache and skip every sccache step. Only the nightly
(a `schedule` run forces hosted) and fork PRs ever touched the store, and neither is on
the per-PR critical path.

So the store was spending 55 % of its steady set on jobs nobody waits for, for a measured
zero, while the Windows ASan job that *is* on the critical path had no cache at all for
want of room. The entries were removed and deleted.

**What was given up, stated honestly:** at 1558 compilations × 6.283 s average, a
*working* cache there would remove most of ~2 h 43 m of compiler time per job. That value
is real and currently unreachable, because the key cannot be stable while the compiler is
a rolling snapshot. Recovering it means pinning the hosted clang — the self-hosted box
already pins `/opt/llvm-23.1.0` by absolute path — or keying the entry on the clang build
id. That is issue [#1095](https://github.com/drsnuggles8/OloEngineBase/issues/1095).
Re-adding the restore/save without doing one of those reproduces the 0.00 %.

**#1095 did the first one, and the entries are back.** `setup-linux-build` now installs
the official LLVM 23.1.0 release tarball at `/opt/llvm-23.1.0` on the hosted arm — the
same build, at the same path, as the self-hosted box — so the compiler the key is hashed
over holds still, and the version is in the cache key so a bump is a reviewable commit
rather than a silent 0 % run. Two things came back smaller than they went out: the cap is
900M rather than 1500M, because rule 5 says these jobs have no claim on the cap and a
2400 MiB share leaves the fleet 2.7 GiB of headroom rather than 1.4; and the save is
gated on `github.event_name != 'pull_request'`, so **hosted non-PR runs write** — in
practice the nightly, plus a `workflow_dispatch` when someone is measuring a cold/warm
A/B on a branch, which is deliberately kept open because that is how #1073's fix was
verified and how this one has to be. It is *not* nightly-only, and the difference
matters when you are counting who can fill the store. **The
acceptance is the "Cache hits rate" line on the second nightly after it lands, not a
green check** — this section is what happens when that distinction is not made.

**The rule:** before adding a cache, check what the existing ones actually return. A
0 %-hit entry is not neutral; it costs quota, download time, and the room a working cache
needed. Read `created_at`, `last_accessed_at` **and the hit rate** — the first two only
tell you the entry is being written and read, not that it is worth anything.

## Who actually has a claim on the cap

Measured 2026-09-06 by reading every workflow's triggers and `runs-on`. Rule 5 above only
means something once this table exists, and it is much shorter than the 17 workflow files
suggest:

| Job | Runner | Runs on a PR when | Cache it justifies |
|---|---|---|---|
| `Windows.yml / build` | windows-2025 | every native change | `sccache-windows-2025-release`, `vcpkg-Windows`, `Windows-vulkan-prebuilt-sdk`, `ffmpeg-Windows` |
| `asan.yml / asan-windows` | windows-2025 | every native change | `sccache-asan-windows-2025` (#1082); shares the vcpkg and Vulkan entries above |
| `dist-archive.yml / archive` | windows-2025 | only 4 paths (`CMakeLists.txt`, two `cmake/*.cmake`, its own file) | vcpkg only. A full Dist build with **no compiler cache**, and that is correct — a job that runs on a few percent of PRs should not hold ~2 GiB. |
| `steam-stub.yml / single-valve-tu` | ubuntu-24.04 | only Steam-seam changes | none; it compiles one TU |
| `pre-commit`, `detect-changes`, `cancel-merged-pr-runs` | ubuntu-latest | every PR | none; seconds |

The two `windows-2025` rows are conditional as of #1076: `Windows.yml` and `asan.yml` carry the
routing to send them to a self-hosted runner behind `vars.OLO_WINDOWS_SELF_HOSTED`, and every
Windows sccache step is gated on `runner.environment == 'github-hosted'`. The variable is unset and
no Windows runner is registered, so today they are hosted and the entries above are real. **If that
switch is ever flipped, `sccache-windows-2025-release` (1825 MiB) and `sccache-asan-windows-2025`
(~660 MiB) leave the store on the PR path, and this table has to be redone** — see
[ADR 0019](../adr/0019-windows-ci-self-hosted-routing-lands-switched-off.md).

Everything else that runs on a PR is **self-hosted** — `asan.yml`'s three Linux sanitizer
jobs, `vulkan-off.yml`, `steam-stub.yml / stub-build` — and caches on the box's local disk.
Note the trap: a self-hosted runner still talks to the **remote** Actions cache service, so
an `actions/cache` step there spends the shared cap exactly like a hosted one does. Being
self-hosted does not make a cache free; using local disk does.

And everything else in the fleet — `SonarCloud`, `cross-vendor`, `fuzz`, `video-ffmpeg`,
`gpu-*`, `flaky-repro-281`, `release` — is `schedule` or `workflow_dispatch` only. It has no
claim. `Linux-vulkan-prebuilt-sdk` (291 MiB) is the one survivor of that category: it is
created solely by `video-ffmpeg.yml`'s linux matrix arm, which never runs on a PR. Left in
place at 3 % of the cap because it works and removing it only slows a nightly — but it is
the first thing to cut if the store gets tight again.

## Adding a cache to this repo

- Add its steady size to the table above and check the total still clears the wall.
- Save through `save-cache-pruned`, with `prefix` set to the key minus its
  `-<run_id>-<attempt>` tail, and grant the job `actions: write`.
- Gate the save on `github.event_name != 'pull_request'` unless the entry is small
  (a few MiB) or genuinely PR-specific. "PR-specific" means the run produced something the
  default-branch snapshot does not already hold — **not** merely that a PR ran. vcpkg is
  the worked example: a PR that does not touch `vcpkg.json` restores master's snapshot and
  would re-bank a byte-identical copy, so `setup-vcpkg` now emits an empty `cache-key`
  when the restore already matched the hash-specific prefix. Measured on PR #1075 before
  the fix: eight entries on one PR ref in two hours (two per push — Windows.yml and
  asan.yml each save one), ~3.0 GiB of a ~9.3 GiB store, every one a copy of what master
  already had. Count `saves per push × open PRs`, not `saves per PR`.
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

## The other half of #1073

The sccache entry did not exist because the store was read-only. The **vcpkg** entries did
exist, in quantity, and had never once been read — a separate fault entirely: the restore
and the save spelled the same directory two different ways, and `actions/cache` hashes the
path string into each entry's version. See
[cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md).
Do not assume one cause explains every cold cache in a repo; check each prefix's
`created_at` (is anything written?) and `created_at == last_accessed_at` (is anything
read?) separately.

## See also

- [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md) — how to pick a
  `SCCACHE_CACHE_SIZE`, and what a cap 20 % under its object set actually costs.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) — the same store, one
  layer up: four ways a cache restores, logs a hit, and rebuilds everything anyway.
  Read §3b first; this file is what happened when only §3b's *rule* was written down and
  not its arithmetic.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md) — where the manifest
  hash in the vcpkg cache key comes from.
- [docs/ops/self-hosted-gpu-runner.md](../ops/self-hosted-gpu-runner.md) — the structural
  escape: a cache on local disk has no save step to refuse, no ref scoping and no cap.
