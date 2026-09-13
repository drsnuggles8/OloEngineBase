# A self-hosted compiler cache is sized by configurations x runner slots

Before setting the cap on a local-disk compiler cache, count the object set as
*configurations x the runner slots that can build them*, then check whether anything
makes those slots share. Two slots that do not share hold the same object twice, so the
cap you need is double the one the configuration list implies — and the statistics will
not tell you, because a cumulative hit rate averages the warm-up era with the thrash.

Companion to [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md), which owns cap
sizing for the **hosted** store, where the Actions cache budget overrules the number. This
file owns the self-hosted case, where the constraints are different: no store budget, a
filesystem instead, and a multiplier that file never has to think about.

## What it looked like

The `olo-ci` ccache on the box, measured 2026-09-13 before the fix:

```
Cacheable calls:   1254946 / 1254961 (100.0%)
  Hits:             958813 / 1254946 (76.40%)
Uncacheable calls:      15 / 1254961 ( 0.00%)
Cache size (GB):      30.0 /    30.0 (99.95%)
Files:               63889
Cleanups:             5884
```

Read those in the right order. **`Uncacheable calls: 15`** says eligibility is fine — the
two defects in
[compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md) are
fixed, so nothing is being refused. **`30.0 / 30.0` with 5,884 cleanups** says the cache
is pinned at its cap and evicting continuously. Capacity is the entire constraint.

**`76.40%` is the lying instrument.** It is a lifetime counter over a directory shared by
every job on the box, so it averages a warm 2026 era with the present. It says nothing
about a run. That is the same trap as
[ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md): the cache is present, warm,
growing and healthy, and half the tree recompiles anyway.

## The multiplier: two runner slots, one object cached twice

`olo-ci-1` and `olo-ci-2` run under one Unix account and share one `CCACHE_DIR`, but
their `_work` trees are different directories. ccache hashes the compile CWD by default
(`hash_dir`) whenever debug info is on, so the same object was stored **once per slot**.
Jobs do not pin to a slot, so which copy a job found was a coin flip — roughly half the
potential reuse was discarded for no benefit, before capacity even entered into it.

That was a deliberate choice, and the reason was sound: turning `hash_dir` off makes the
slots share, and then a slot-2 job gets an object whose debug info names slot 1 — and
these jobs exist to produce readable ASan/UBSan/TSan stacks. The fix is to remove the
hazard rather than accept it. Measured on the box, clang 23.1.0 / ccache 4.11.3, two trees
under different `actions-runner-ci-N/_work` roots, Debug + ASan, with a PCH:

| configuration | slot 2 | `DW_AT_comp_dir` in the shared object |
|---|---|---|
| neither variable (was) | MISS, 0 of 4 hits | each slot names itself |
| `CCACHE_BASEDIR` + `CCACHE_NOHASHDIR` | HIT | **names slot 1** |
| the above + `-ffile-prefix-map` | HIT | `/olo/build`, byte-identical objects |

The PCH hits too, so it does not poison the compiles that consume it. Through real CMake
with the launcher wired up, slot 2 hits slot 1's object: 1 miss + 1 hit of 2 calls.

### base_dir is not optional, for a non-obvious reason

`-ffile-prefix-map=<absolute source root>=/olo` **itself contains the tree-specific
path**, so with the flag alone the two slots' command lines still differ. `base_dir` is
what rewrites that argument to a relative form before hashing. Dropping it while keeping
the flag measured **0 of 2 hits** — no sharing at all. The flag and the variable are one
mechanism, not two independent improvements.

### The placeholder must be a hardcoded literal

ccache does **not** distinguish two different prefix-map *targets* once `base_dir` has
rewritten the argument. Two trees passing `/olo1` and `/olo2` still **hit each other**,
and the object served carried `/olo1`. So a placeholder derived from anything
tree-specific — a workspace path, a job name, a matrix arm — would stamp every object
with some other tree's value and **never miss to reveal it**. One constant, shared by
every consumer of the cache.

Note the division of labour: `base_dir` already relativises `DW_AT_name` as a side effect
of rewriting the command line. `DW_AT_comp_dir` is the one that survives, and the one
`-ffile-prefix-map` exists here to fix.

## Sizing the cap

One generation of the object set, measured the same day:

- **1,653 TUs** per Linux sanitizer build.
- **~944 KB per cache entry** (30.0 GB / 63,593 files — use the file count, not a guess).
- **7-9 distinct flag sets** can land on this box: `asan.yml`'s three Linux jobs,
  `gpu-sanitizers-amd.yml`'s three arms, `gpu-conformance-amd.yml`, `steam-stub.yml`,
  `vulkan-off.yml`. Enumerate them by **runner label**, not by symptom — the scoping
  lesson from the `--parallel` work applies here unchanged.

That is **~20-26 GB to hold one generation of everything**, so a 30G cap held barely one
and any two consecutive commits evicted each other. 120G is ~5 generations, ~10 now that
the slots share.

**Then the filesystem decides, the way the store decides on hosted.** The cache is on
`/home` (NVMe): 218G used of 389G, 171G free, of which this cache was 28 GiB. `120G` is
111.8 GiB — ccache's `G` is a power of 1000 — so worst-case growth is ~84 GiB and ~87 GiB
stays free. The other three runner accounts (88G) and every `_work` tree are on the same
filesystem, and a full `/home` takes out all four runner accounts and the login. Measure
before raising it again.

**Relocating is not the easy answer it looks like.** `/mnt/SSD` has 79G free and already
carries the foxguard target cache; `/mnt/2TB-A` and `/mnt/2TB-B` are **SMR spinning
disks**, which is the wrong medium for a cache doing 2.5M reads and 666K writes.

## The cap is set in exactly one place

`.github/actions/setup-linux-build/action.yml` re-asserts `max_size` into `ccache.conf`
**on every self-hosted job**. A hand-run `ccache -M`, or a bump in the box's provisioning
script, is silently reverted by the next run. There is no second setter — verify with
`git grep -nE 'max-size|max_size|CCACHE_MAXSIZE' .github/` before believing otherwise.

Print `--show-stats -v`, not `--show-stats`: `Files` and `Cleanups` appear only under
`-v`, and they are what separate eviction from invalidation. At or above the cap with
cleanups climbing is eviction; comfortably below it is a key problem.

## See also

- [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md) — the same cap question on
  the hosted store, where the Actions cache budget overrules the measurement.
- [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md) — read
  the uncacheable line *first*; capacity arithmetic is meaningless while compiles are
  being refused.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) — the other ways a cache
  restores, reports health and rebuilds everything anyway.
