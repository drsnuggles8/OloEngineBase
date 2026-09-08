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
| `tsan-linux` | 945 MiB | 94 % / 2 min | 99.94 % / 1.7 min |
| `asan-lsan-linux` | 994 MiB | 71 % / 17 min | 99.94 % / 1.5 min |
| `ubsan-linux` | **1084 MiB** | **32 % / 38 min** | 99.94 % / 1.4 min |

`900M` is 5 / 10 / 20 % under the three sets, and the arm 20 % over loses 68 points of
hit rate and 36 minutes: **a cap under the object set is not a proportional saving.**
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

**Then check who is waiting.** Rule 5 decides between two caps that both fit. These three
entries are read only by the nightly and by fork PRs (same-repo PRs route those jobs to
the box and its local ccache), while `sccache-asan-windows-2025` measured 1230 MiB
against its own 1500M cap the same day — it fills its cap and evicts too, on a job that
*is* on the PR critical path. Rule 5 does not answer in favour of the arm with the
worse-looking number.

## See also

- [actions-cache-budget.md](actions-cache-budget.md) — the store, its ~9537 MiB wall and
  what every key in it costs. Rules 4, 5 and 6 there are the ones this file applies.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) — four ways a cache
  restores, logs a hit and rebuilds everything anyway. A cap under the object set is a
  fifth: the restore is healthy, the entry is current, and the hit rate is still 32 %.
