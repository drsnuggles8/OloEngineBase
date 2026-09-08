# A compiler cache capped below its object set is far worse than the shortfall

Issue [#1084](https://github.com/drsnuggles8/OloEngineBase/issues/1084). Companion to
[actions-cache-budget.md](actions-cache-budget.md), which owns the store's arithmetic;
this file owns the one number that file does not: how big to make a cap.

Two numbers, in this order, and the second one wins.

**1. The object set.** Set the cap deliberately too high, run once with the save step
**disabled** (a multi-GiB entry banked against 2.5 GiB of headroom is how the store goes
read-only), and read `du -sm "$SCCACHE_DIR"`. Not `--show-stats`: it rounds, and printed
`Cache size 1 GiB` for a directory `du` put at **1084 MiB** — a range covering 1024 to
2047 MiB, which cannot pick anything. Every sccache job in `asan.yml` now prints both.

Measured at a 3G probe cap, hosted, identical source, 2026-09-08:

| arm | object set | at `900M` | at `3G` |
|---|---|---|---|
| `tsan-linux` | 945 MiB | 93.88 % / 4.0 min | 99.94 % / 1.7 min |
| `asan-lsan-linux` | 994 MiB | 69.89 % / 17.1 min | 99.94 % / 1.5 min |
| `ubsan-linux` | **1084 MiB** | **30.62 % / 36.7 min** | 99.94 % / 1.4 min |

`900M` is 5 / 10 / 20 % under the three sets, and the arm 20 % over loses **69 points of hit
rate and 35 minutes**: a cap under the object set is not a proportional saving. Run the control
before believing it — master's entries are re-banked nightly, so "a fresher entry" explains the
jump just as well until you re-run the OLD cap on the same commit.
Note what the old comment on those lines believed — that the sets "filled 1500M", from
entry sizes of 1252 / 1231 / 1258 MiB measured in the #1082 era. They did not: a
*rolling* compiler was banking a fresh object set into one directory nightly (see the
next section). **An entry size measured while the cache key was unstable says nothing
about the object set.**

**2. The store's worst case, which decides it.** Entries land at 82–91 % of a cap they
reach, so size the cap such that `0.91 × cap × copies` still clears `cache-prune.yml`'s
8800 MiB failure threshold. From the 7062 MiB steady set measured 2026-09-08, of which
these three are 2231 MiB (so 4831 MiB is everything else):

| Linux cap | worst case, three entries | fleet worst case | clears 8800? |
|---|---|---|---|
| `900M` (today) | ~2455 MiB | ~7286 MiB | yes |
| `1300M` | ~3549 MiB | ~8380 MiB | yes, 420 MiB spare |
| `1500M` | ~4095 MiB | ~8926 MiB | **no** |

So "just use the workflow-level 1500M" is not available. **Do this arithmetic before
raising any cap** — the number that constrains you is rarely the one you set out to
measure.

`1300M` is what landed. **Then check who is waiting** — rule 5 decides between two caps
that both fit, and these three entries are read only by the nightly and by fork PRs
(same-repo PRs route those jobs to the box and its local ccache). The apparent competing
claim was `sccache-asan-windows-2025`, on the PR critical path, at 1230 MiB against its
own 1500M cap. **It was not competing, and the reason is this file's subject.** Its stats
print `Cache size 1 GiB / Max cache size 1 GiB`, which reads as a full cache; `du -sm`
puts its object set at 1241 MiB, and dispatches at 1500M and at 3G on identical source
both returned **99.87 %** and 12–13 min. The cap has ~259 MiB spare.

## The other way a cache misses on nothing: the key does not cover what changed

That job *did* return 42.17 % once — 923 misses of 1588, a 2 h 06 build — on PR #1112,
which bumps the Vulkan SDK. The entry restored cleanly, and the stats showed the same
`1 GiB / 1 GiB` pair, so it read as capacity. It was not. sccache hashes **preprocessed
output**, so every TU reaching a Vulkan header changed, while the key
`sccache-asan-windows-2025-<run_id>-<attempt>` carried no SDK version and the
`restore-keys` prefix therefore kept finding the pre-bump entry. The tell is in that
run's own log: `Found Vulkan ... found version "1.4.357"` under a `-1.4.321.0` cache key.

This is the rule the Linux keys already follow with `llvm-<version>` (see *A cache can be
worth removing*, where a **rolling clang** produced 0.00 % for weeks): **pin whatever the
key is hashed over.** The Windows ASan key now carries `-vk<version>` in both `key` and
`restore-keys`, so a bump costs one visible cold build instead of a mystery 42 %, and
`cache-prune.yml` strips that segment when grouping so the bump supersedes its
predecessor rather than stranding ~1230 MiB for thirty days.

**Two failure modes, one symptom.** A cache that misses on everything looks the same
whether the cap is too small or the key is too coarse. `du -sm` against the cap separates
them: at or above the cap it is eviction, comfortably below it is invalidation.

## See also

- [actions-cache-budget.md](actions-cache-budget.md) — the store, its ~9537 MiB wall and
  what every key in it costs. Rules 4, 5 and 6 there are the ones this file applies.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) — four ways a cache
  restores, logs a hit and rebuilds everything anyway. A cap under the object set is a
  fifth: the restore is healthy, the entry is current, and the hit rate is still 32 %.
