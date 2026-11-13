# Git LFS bandwidth in CI: details

The rules are in [ci-lfs-bandwidth.md](ci-lfs-bandwidth.md). This file holds the measurements and
the incident behind them (#1682).

## The numbers

- **Quota:** 10 GiB of Git LFS bandwidth a month on the account's free plan. It was at 100 % on
  2026-11-12, twelve days into the month. Reading per-day usage needs the billing API with the
  `user` scope (`gh auth refresh -h github.com -s user`) or Settings → Billing → Usage.
- **One full pull:** 12 objects, 1 024 MB on the wire, as measured by `git lfs ls-files --size` on
  2026-11-12 (Samoyed, Bernese and Dog models, grooms, bindings and authoring sources, added
  2026-10-09 to 10-23). As a compressed Actions cache entry it is 589 MiB.
- **What the quota does not cover:** the remaining ~2 GiB of November's usage was not attributable
  from CI logs. Candidates: five self-hosted jobs whose `git lfs pull` took 17–43 s (silent before
  this change), local clones, and outside clones (178 unique cloners in 14 days; a clone with
  git-lfs installed pulls ~1 GB on the owner's quota).
- **Blocked or not:** a `download` request to
  `https://github.com/<owner>/<repo>.git/info/lfs/objects/batch` returns an `href` while downloads
  are allowed and costs no bandwidth itself. It returned 200 on 2026-11-12 despite the 100 %
  reading.

## How CI gets the objects

`.github/actions/restore-lfs-assets`, in order:

1. Computes the digest of the current object set (`git lfs ls-files --long`, sorted, hashed). The
   cache key is `olo-lfs-v1-<digest>`; restore-keys `olo-lfs-v1-`.
2. Hosted: restores that key from the cache, or, with `restore-artifact: 'true'`, downloads the
   run's artifact. Self-hosted: points `lfs.storage` at `$HOME/.cache/oloengine-lfs`, a persistent
   store shared by the account's checkout slots, and restores nothing.
3. `git lfs pull` and `git lfs fsck`, with the object store's file count and byte total taken
   before and after. `git lfs pull` prints its progress only to a terminal, so the store size is
   the measurement. Every job prints `LFS fetched: …`; a non-zero fetch is a `::warning::` and a
   step-summary line. On a self-hosted box a concurrent slot's pull can land in the window, so the
   count is an upper bound there.
4. Without `restore-artifact`, and not on `push`, publishes exactly the current objects as a
   three-day artifact for the rest of the run (three, so "Re-run failed jobs" a day later still
   finds it: that does not re-run the job that published it).
5. On a hosted `refs/heads/master` run without an exact hit, drops every object the tree does not
   reference and saves the entry.

Entries saved from a PR run are readable only by that PR, so the master entry is the only one that
saves bandwidth across PRs. Single-flight therefore uses artifacts, not PR-scoped caches.

## 2026-11-02: four builds, four pulls

From the jobs API and job logs for 2026-11-01 to 11-12: 223 hosted build jobs restored the entry
(`git lfs pull` → `fsck OK` in 3–12 s, nothing fetched), ~540 test shards took a build's artifact,
and **8 build jobs found no entry** (`Cache not found for input keys: olo-lfs-v1-…`) and pulled the
full set each:

- 2026-11-02, the `renovate/vcpkg-registry-baseline` PR #1673: Windows `build`, ASan
  (Windows/clang-cl), UBSan and TSan (Linux/Clang).
- 2026-11-03 ~01:26, the master Sanitizers nightly: TSan, ASan+LSan, UBSan, ASan (Windows). That
  run saved the replacement entry (`olo-lfs-v1-61ea3a70…`, created 01:26:50Z).

asan.yml's four builds each restored the cache on their own, so one missing entry cost four pulls
per run. Since #1682 its `lfs-objects` job makes the run's only restore-or-fetch and publishes
`sanitizers-lfs`. The builds `needs:` it and every build and shard takes that artifact. It runs
after `detect-changes` and only when a build will, because ungated it would pull the full set for
a PR that builds nothing. The builds run under `!cancelled()`, so a failed `lfs-objects` turns
them red at the artifact download rather than skipped (a skipped check reads as green).
On `push` the master warm runs only the ASan Windows build, so `lfs-objects` is skipped and that
build restores, fetches on a miss and saves the entry itself: one fetch, even when the save fails.
Windows.yml and vulkan-software.yml have one fetching job per run and were already single-flight.
A PR push that finds no master entry now costs one pull per workflow that runs: Windows,
Sanitizers and lavapipe, three at most.

Measured on #1682's forced-miss dispatch (run 43178737320): `lfs-objects` fetched 12 objects,
1024410138 bytes, and the four builds and sixteen test shards each reported
`LFS fetched: 0 object(s) … objects from: artifact sanitizers-lfs`. The job takes 54–58 s and
runs after `detect-changes`, so the builds start about a minute later than before.

## Why the master entry disappeared

`cache-prune.yml` did not delete it. Its sweep logs list the entry as `keep` on every run from
2026-11-01 01:13 to 11-02 21:12, at 1402 MiB, and the prefix strip groups the key as `olo-lfs-v1`
(the newest per (ref, prefix) is always kept). Its 30-day `last_accessed_at` age-out never binds
either, because GitHub itself deletes an entry unread for 7 days, and every hosted build reads this
one.

GitHub's cap eviction did. The repository cap is 25 GB (`gh api
repos/<owner>/<repo>/actions/cache/storage-limit` → `{"max_cache_size_gb":25}`), ~23841 MiB.
The sweep listings:

| sweep (UTC) | listed after sweep | what changed |
|---|---|---|
| 11-01 13:10 | 19424 MiB | PR #1673 holds a 5104 MiB vcpkg-Windows entry |
| 11-01 17:09 | 24643 MiB, **failed** | PR #1675 adds 5219 MiB; over the cap |
| 11-02 05:15 | 19227 MiB | GitHub had evicted `Linux-vulkan-prebuilt` (no DELETE line for it) |
| 11-02 13:15 | 24664 MiB, **failed** | #1673 re-banks 5436 MiB |
| 11-02 21:12 | 23419 MiB, **failed** | `olo-lfs-v1-61ea3a70…` still `keep` |
| 11-03 01:14 | 10711 MiB | master's Windows sccache, both vcpkg sets, both Vulkan SDKs, ffmpeg and `olo-lfs-v1-*` gone; #1673 had banked five more entries in between, 6054 + 6054 + 6033 + 676 + 664 = 19481 MiB |

The ceiling alarm went red five times in 30 hours and could not act. The entries over the line
were live PR entries, which the sweep correctly keeps. The eviction happened at save time, between
two sweeps. So the fix is at save time: `save-vcpkg-cache` lists the store before any save off
`refs/heads/master` and skips it with a warning when the store plus the entry would exceed the
20000 MiB working ceiling, or when the token cannot list. The check alone races, because
Windows.yml and asan.yml bank the same ~6 GB Windows set in parallel under two run ids. So after
saving, the job lists again and deletes its own entry if the store is over the ceiling. GitHub's
eviction is not immediate (the store sat at 24643 MiB for hours on 11-01), which gives the
re-check time to act. The branch pays by rebuilding its changed ports on the next push; master's
entries stay.

## The entry only ever grew

Saving after a restore-keys match banked the restored objects plus the new ones, so every LFS
change on master grew the entry. The evicted entry was 1402 MiB; the one saved from a cold pull on
11-03 is 589 MiB for the same object set. PR runs check out the merge ref, whose set is master's
plus the PR's own, so the older objects were never read. The action now deletes them from the
ephemeral hosted store before saving.

## Verifying a change without spending the quota

- Probe the batch API (above) before any run that might pull cold.
- A **hit** costs nothing: read `LFS fetched: 0 object(s)` in each job.
- A **forced miss** costs one full pull: salt the cache key and its restore-keys prefix in one
  `workflow_dispatch` of asan.yml (`force_hosted: true` so all four builds are hosted), then read one
  `fetched 12 object(s)` in `lfs-objects` and `0` everywhere else. Take the salt from a dispatch
  input, so the PR runs on the same commit (Windows, Sanitizers, lavapipe) stay unsalted; a salt
  in the action itself would make each of them pull too. Remove it before merge.
- A **restore-keys match** costs only the delta: salt the key but not the prefix. The run restores
  master's entry, fetches nothing it already holds, and takes the artifact path.
- **Self-hosted:** dispatch `gpu-conformance-amd.yml` on the branch and read the action's
  `LFS fetched:` line in `amd-hardware`; cancel the run after that step if the rest is not needed.
- Never delete the master entry to force a miss: every PR running before the next master save
  would pay a full pull.
