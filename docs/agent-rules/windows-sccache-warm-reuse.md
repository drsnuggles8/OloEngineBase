# Measure compiler reuse after a warm restore

Require a complete build and parse native sccache JSON after a hosted Windows restore;
a successful cache download does not prove that any compiler request reused an object.
Issue [#1584](https://github.com/drsnuggles8/OloEngineBase/issues/1584) follows several
earlier fixes with the same zero-hit symptom and different causes.

## What the earlier fixes established

| Work | Finding and change | Limit of that evidence |
|---|---|---|
| #1009 | Split restore/save, preserve default-branch producers and stop cancelling them. A six-day-old snapshot was not a warm control. | Source drift and producer cadence remain independent of cache capacity. |
| #1073 / #1075 | The Windows compiler cache entry did not exist: the store refused saves because its budget was exhausted. Added confirmed saves and pruning; removed PR-private large saves. | A snapshot that exists and restores can still contain no usable objects. |
| #1082 | Windows ASan cache support and server idle-timeout handling were repaired. | clang-cl ASan is a control, not proof that MSVC Release reuses objects. |
| #1118 | Linux sanitizer object sets exceeded the 900M cap; measured 1300M caps. Stamped Windows ASan caches with the Vulkan SDK. | These measurements did not size the MSVC Release object set. |
| #1084 / #1170 | Warmed master per merge and stamped Release caches with the Vulkan SDK. Measured warm Release hit rates of 94.8-98.5%. | `windows-2025` is a runner label, not an immutable compiler/image version. The Release cache was already at its 2G cap. |

Do not retry a previous fix based only on the symptom. Check restore lineage, source and
toolchain identity, exact resident bytes, and actual compiler reuse.

## Matched controls collected on 2026-10-11

Release producer [38086179377](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38086179377)
and following nightly [38104242016](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38104242016)
both built commit `402f1ce55`, image `20260925.250.1`, MSVC `19.51.36260.0`.
The nightly restored the producer's snapshot. Both returned **0 hits / 2060 misses**, with
zero read/write errors and zero uncacheable calls, at the 2G cap.

The same-commit Windows ASan nightly
[38103213360](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38103213360)
used the same image, clang-cl `23.1.0`, and its previous producer's cache. It returned
**2015 hits / 2 misses (99.90%)**. The cache service and producer cadence were working.

The September transition also occurred within one image/compiler generation:
Release run [34797475126](https://github.com/drsnuggles8/OloEngineBase/actions/runs/34797475126)
returned 1438/1772 hits; producer
[34811731975](https://github.com/drsnuggles8/OloEngineBase/actions/runs/34811731975)
returned 448/1777; its successor
[34812956055](https://github.com/drsnuggles8/OloEngineBase/actions/runs/34812956055)
returned 1/1783. All used MSVC `19.51.36256.0`. These different source revisions locate
the degradation; they do not isolate its cause.

## Hosted capacity contrast identifies eviction

Fresh master run [38166502504](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38166502504)
restored the same `38104242016` snapshot and returned **0/2070 hits** at 2G.
The successful build took 135m33s and confirmed its cache save. The no-save 8G probe
[38168947158](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38168947158)
compiled the same C++ tree (`ada41984c`; only the localized BuildInfo commit input
differs) from that same restored snapshot. Its complete capture recorded **586 hits /
1484 misses**: all **586 keys resident at restore** were reused. There were no cache
read/write errors or failed/non-cacheable compilations.

The original snapshot held 1235 objects / 2,146,612,421 bytes. The probe finished with
2719 objects / 4,548,515,240 bytes. Its 1484 new objects added **2,401,902,819 bytes**,
already exceeding the 2,147,483,648-byte production cap without counting the 586
reused objects. At 2G, early misses evict matching objects before their turn; the
enlarged arm preserves them. This establishes capacity/LRU eviction, rather than a
globally changing hash input, as the cause of the zero-hit regression.

The final snapshot also retains obsolete source keys. It is an upper bound on the
current object set, not its exact size. The refined probe measures unique requested
keys separately and requires every requested key to remain resident before claiming
a complete object-set measurement.

Following no-save probe
[38177455638](https://github.com/drsnuggles8/OloEngineBase/actions/runs/38177455638)
restored fresh master `38166502504` at the same image/compiler. It reused all **1208
keys resident at restore**, with 862 misses and no errors/non-cacheable calls. Its
complete successful build took **61m57s**. All **2070 unique requested objects** remained
resident, totaling **2,818,896,940 bytes (2688.31 MiB)**, with no obsolete objects left
in this snapshot. The native JSON and complete probe are in `windows-cache-health`
and `windows-cache-probe` artifacts. This is sizing evidence; 58.36% reuse from the
truncated 2G snapshot is not the recovery criterion.

Production uses **3200M (3,355,443,200 bytes)**, reserving **511.69 MiB / 19.03%** above
the measured current set. Keep measuring as the compiled graph grows. The shared-store
arithmetic, including overlapping snapshots and dependency rotations, is in
[actions-cache-budget.md](actions-cache-budget-details.md#2026-10-11-msvc-release-capacity).

## Controlled eviction reproduction

A local control used sccache 0.15.0 and five deterministic MSVC translation units,
each exporting 65,536 random unsigned integers, with `/O2 /Z7 /MD`. Inputs, paths and
compile order stayed identical; the daemon restarted between every pass. Builds used
the build lock and a separate cache/server port. Native JSON recorded:

| Pass | Cap | Hits / requests | Resident bytes |
|---|---|---|---|
| Cold | 1MiB | 0/5 | 791,015 |
| Warm, same order | 1MiB | 0/5 | 791,015 |
| First enlarged pass | 8MiB | 3/5 | 1,318,365 |
| Warm, same order | 8MiB | 5/5 | 1,318,365 |

The smaller cache retained three objects, but earlier misses evicted them before their
turn. Enlarging it preserved all three restored objects and then the entire set. This
isolates the zero-hit eviction mechanism; it does not measure the engine's object set
or establish the hosted regression's cause.

## Probe and health contract

`Windows.yml`'s opt-in `cache_probe` dispatch uses an 8G temporary cap and **does not save
that compiler cache**. It explicitly restores the original pre-fingerprint lineage for
diagnosis; normal production restores have no such fallback. Independent probes do not
share a concurrency lane with each other's test shards. It snapshots object keys before configure, captures generated keys,
and reports which were resident at restore. Raw daemon logs stay on the disposable runner;
only derived records and native statistics are uploaded. A missing log or incomplete capture
cannot be reported as a complete probe. `windows_cache_probe.py` scans object entries, not
the separate preprocessor cache. Object basenames can collide, so lookup outcomes are
aggregated rather than attributed to individual basenames.

The production guard reads `cache-matched-key`, which is populated by a prefix restore even
though `cache-hit` is false. It records counts, hit rate, restore key, build outcome and exact
cache/cap bytes. A successful build with at least 500 cacheable requests is required. Missing,
malformed or inconsistent statistics fail; skipped/failed builds and smaller samples fail.
A complete cold run passes without requiring hits. A warm result below 5% fails with an
Actions annotation and a machine-readable artifact. The threshold detects this regression;
passing it alone is not the 94.8-98.5% recovery criterion.

Hosted cache prefixes include a fingerprint of compiler bytes/path, MSVC and Windows SDK
versions, the actual image revision and sccache version. A toolchain roll starts a cold lineage;
there is no fallback to another identity. The fingerprint follows the Vulkan stamp so the
existing janitor strips run/attempt, then fingerprint, then SDK, grouping superseded lineages.
Cache saves require a successful build and remain disabled on PRs and oversized probes.

Recovery still requires named hosted master producer and following PR consumer runs at a
matched toolchain. Branch dispatches and local helper tests cannot replace that evidence.
