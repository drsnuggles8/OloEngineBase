# A CI cache that restores is not a CI cache that works

Issue [#1009](https://github.com/drsnuggles8/OloEngineBase/issues/1009). Read before changing any CI cache, cache key, runner image, workflow trigger or concurrency setting. A cold cache is a slow success: no red, no warning, so verify with a number from a real run.

## Rules

1. **Accept a cache change only on a stats line from a real run, not a green check.** sccache: `sccache --show-stats` `Cache hits rate`, high and never exactly 0.00%. vcpkg: `Restored N package(s)` in the configure step, N near the port count (116). ccache: `ccache -s` direct/preprocessed hits, non-zero after the first run. The store: `created_at` of the newest entry, today and not six days ago. [Details](ci-cache-that-looks-alive-details.md#the-counter-move)
2. **When a cache is not warming, check that it is writable first.** The signal is a `warning`, not an error: `Cache reservation failed: You have reached your configured budget, your cache is now read only`; the line below it ("another job may be creating this cache") is generic 409 text and misleads. A frozen newest `created_at` across every key prefix is not a key bug. [Details](ci-cache-that-looks-alive-details.md#3b-the-cache-can-be-read-only-and-only-one-line-in-one-log-says-so)
3. **Read `created_at`, not `last_accessed_at`.** A restore touches an entry without replacing it, so `last_accessed_at` makes a frozen store look busy. [Details](ci-cache-that-looks-alive-details.md#3b-the-cache-can-be-read-only-and-only-one-line-in-one-log-says-so)
4. **Remember that over the cap nothing is evicted; the store goes read-only.** GitHub does not do LRU eviction (#1073), so you must delete something; the eviction policy is `.github/workflows/cache-prune.yml` (one entry per key prefix per ref, necessary but not sufficient). Budget arithmetic: [actions-cache-budget.md](actions-cache-budget.md). [Details](ci-cache-that-looks-alive-details.md#4-over-the-cap-nothing-is-evicted--the-store-stops-accepting-writes)
5. **Split `actions/cache` into `actions/cache/restore` + `actions/cache/save` with `if: always()`.** The combined action saves in a post-job step, which GitHub skips on cancel; `cancel-in-progress` killed 17 of 27 Windows PR runs and 15 of 24 Sanitizer PR runs. Place the save right after the step producing the expensive artefact (the vcpkg save goes after configure). `asan.yml`'s tsan job is the working example. [Details](ci-cache-that-looks-alive-details.md#1-a-post-job-step-does-not-run-when-the-job-is-cancelled)
6. **Keep a default-branch cache writer.** Cache entries are visible only to the ref that created them, except default-branch entries, which every branch reads; all active entries were `refs/heads/master`. Dropping `push: master` would make every PR permanently cold, so a nightly cron replaced it. Before removing a trigger, ask what else it was doing. [Details](ci-cache-that-looks-alive-details.md#2-only-default-branch-cache-entries-are-readable-fleet-wide)
7. **Set `cancel-in-progress` to `pull_request`-only wherever a cache is written.** Cancelling master runs left a six-day-old snapshot that every PR restored. [Details](ci-cache-that-looks-alive-details.md#2-only-default-branch-cache-entries-are-readable-fleet-wide)
8. **Pin the runner image and put its name in the cache key.** `windows-latest` is an alias; a roll changes `cl.exe`, sccache hashes the compiler binary, and the cache still restores with zero hits. Use `windows-2025`. Exactly 0 hits out of 1516 is something upstream of the source, not source churn. [Details](ci-cache-that-looks-alive-details.md#3-a-rolling-runner-image-is-a-hash-input)

## Commands and checks

```
gh api "repos/<owner>/<repo>/actions/caches?per_page=100" --jq '.actions_caches[]|"\(.created_at)  \(.key)"' | sort -r | head -3
```

Lists the newest cache entries by creation time; the dates should be recent.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[ci-cache-that-looks-alive-details.md](ci-cache-that-looks-alive-details.md).
