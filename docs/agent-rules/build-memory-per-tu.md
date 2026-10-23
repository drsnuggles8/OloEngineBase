# Per-TU build memory: measuring it, and the four traps

Rules for measuring per-translation-unit build memory and for setting `--parallel`, `OLO_HEAVY_COMPILE_JOBS`, `OLO_LINK_JOBS` and any cgroup cap (issues #759, #822, #1305, #1313). Read before changing a build-memory cap or listing a TU as heavy.

## Rules

1. **Set job caps from the published ranking, not a remembered number.** Configure with clang and `-DOLO_PROC_STAT_REPORT=ON` (memory only; `-DOLO_BUILD_INSTRUMENTATION=ON` is the umbrella adding timing and tracing), build, then run `scripts/analyze_proc_stat.py` on the **build tree**. CI publishes the same report weekly as the `build-memory-*` artifact (`.github/workflows/build-memory.yml`). [Details](build-memory-per-tu-details.md#per-tu-build-memory-measuring-it-and-the-four-traps)
2. **Use the instrumentation API for time and `-fproc-stat-report` for memory.** The API's memory field is system-wide host memory and cannot attribute memory to a TU. [Details](build-memory-per-tu-details.md#why-not-the-instrumentation-api-from-759822)
3. **Keep the records path RELATIVE.** An absolute `OLO_PROC_STAT_FILE` is hashed by the compiler cache and ends cross-tree sharing (0% hit vs 50%). `cmake/ProcStatReport.cmake` rejects it with `FATAL_ERROR` and `build-memory.yml` greps for one. [Details](build-memory-per-tu-details.md#trap-1--the-records-path-must-stay-relative-or-every-other-job-loses-its-cache)
4. **Always pass the build tree to the analyser, never one file.** Unix Makefiles (every Linux CI job) writes one records file per subdirectory; Ninja writes one. A single file ranks a fraction, and the omitted part is the heavy engine and test directories. [Details](build-memory-per-tu-details.md#trap-2--there-is-one-records-file-per-subdirectory-on-linux)
5. **Measure with the compiler cache forced through the compiler.** A cache hit appends no record, so a warm build ranks only the misses. CI uses `CCACHE_DISABLE=1`; locally prefer `CCACHE_RECACHE=1`. Check the analyser's coverage line, and use `--fail-under-coverage` so a lower bound is never published as a census. `intermediate` records (the `-E` pass) are counted separately. [Details](build-memory-per-tu-details.md#trap-3--a-warm-compiler-cache-makes-a-partial-ranking-look-like-a-cheap-build)
6. **Measure cache hit rate with `CCACHE_STATSLOG`, never before/after `ccache -s`.** All worktrees share one `CCACHE_DIR`. The statslog appends, so create or truncate a unique file per build. [Details](build-memory-per-tu-details.md#trap-3--a-warm-compiler-cache-makes-a-partial-ranking-look-like-a-cheap-build)
7. **Pass `-DOLO_ENABLE_PCH=OFF -DOLO_ENABLE_UNITY_BUILD=OFF` explicitly on any cache-off measurement.** `cmake/CompilerCache.cmake` forces them off only when the cache is on; unity would merge 16 TUs into one and destroy the per-TU ranking. [Details](build-memory-per-tu-details.md#trap-4--compare-only-against-a-build-with-the-same-pch-and-unity-settings)
8. **Treat the link as the memory ceiling on Linux.** One `ld.lld` link of `OloEngine-Tests` is 10.03 GiB (plain Debug) and 11.17 GiB (Debug + ASan), more than any compile (heaviest 4.65 / 8.09 GiB). Weigh the 14 GiB unit cap and 19 GiB slice cap against link plus a compile, and two concurrent links. [Details](build-memory-per-tu-details.md#the-measurement-2026-09-17-the-link-is-the-ceiling-not-any-compile)
9. **Do not assume `OLO_LINK_JOBS` or `olo_heavy` bound anything under Makefiles.** They are Ninja job pools. On Linux the bound is `scripts/link-semaphore.py` (N permits via `flock`, fails open), with the permit count from `OLO_LINK_SEMAPHORE_SLOTS` passed through `cmake -E env`. [Details](build-memory-per-tu-details.md#the-measurement-2026-09-17-the-link-is-the-ceiling-not-any-compile)
10. **Keep sanitizer jobs at `--parallel 2`.** Three lanes is a 15.24 GiB worst case, past the cap. Non-sanitizer Linux Debug fits 4 lanes on compile grounds, but the link bounds the gain. Changing a live runner configuration is the maintainer's call. [Details](build-memory-per-tu-details.md#what-to-do-with---parallel-stated-as-a-recommendation-not-a-change)
11. **Do not trust the `olo_heavy` pool's membership; re-populate from the census (issue #1307).** It holds TUs that rank low (`AbilityComponentRoundTripTest.cpp` #281, `RenderGraphTest.cpp` #34) and omits ones that rank high (`Scene.cpp` #3, `ComponentFieldRegistry.cpp` #5). It is Ninja-only. [Details](build-memory-per-tu-details.md#the-olo_heavy-pool-is-mispopulated-in-both-directions)
12. **Do not read "no link records on Windows" as free links.** MSVC has no equivalent; clang-cl invokes `lld-link` directly, so the module adds the flag to compiles only and says so at configure time. Use a Linux clang build for link peak RSS. vcpkg, FetchContent and static archiving are also invisible. [Details](build-memory-per-tu-details.md#scope-what-this-cannot-see)
13. **Know the debug-info levers before proposing one.** ThinLTO cannot help sanitizer jobs (no LTO in Debug). `OLO_SPLIT_DWARF` (`-gsplit-dwarf`) cuts the link 11.23 to 8.75 GiB but loses inlined frames with `-ffile-prefix-map`, so it is OFF; do not pair with `--gdb-index`. `OLO_MINIMAL_DEBUG_INFO` (`-g1`, `cmake/MinimalDebugInfo.cmake`) keeps inlined frames and is not to be combined with `OLO_SPLIT_DWARF`. `OLO_SANITIZER_OPTIMIZE` (`-O1`, `cmake/SanitizerOptimize.cmake`) changes what the jobs test, so read the suite result. All default OFF. [Details](build-memory-per-tu-details.md#the-cheap-config-levers-priced)
14. **Use `.dwp`, not `-fdebug-compilation-dir`, to recover frames under `-gsplit-dwarf`.** A real absolute `DW_AT_comp_dir` re-introduces tree-specific paths and ends cross-slot ccache sharing. [Details](build-memory-per-tu-details.md#the-cheap-config-levers-priced)
15. **Treat committed per-TU figures in `OloEngine/src/CMakeLists.txt`, `OloEditor/src/CMakeLists.txt` and #1113 as void until re-measured.** They are up to 8.7x too high: `cmake --build <dir> --target X -- '<obj>'` builds the whole target. Use the census for ranking and an isolated per-PID `PeakWorkingSet64` recipe for one TU's absolute figure. [Details](build-memory-per-tu-details.md#appendix-the-committed-per-tu-figures-were-wrong-when-written-and-three-methods-now-agree)
16. **Do not use line count as a memory lever.** `RenderGraphTest.cpp` (13.2k lines) ranks #34; registry and serializer TUs lead (`McpFieldRegistry.cpp` #1). Propose a file split against the published ranking or not at all. [Details](build-memory-per-tu-details.md#appendix-the-committed-per-tu-figures-were-wrong-when-written-and-three-methods-now-agree)

## Commands and checks

Measure per-TU memory (the memory half only):

```powershell
cmake --preset dev-cached -DOLO_PROC_STAT_REPORT=ON
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEditor --target OloEngine-Tests --config Debug --parallel 6'
python scripts/analyze_proc_stat.py build-cached --cap-gib 14
```

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[build-memory-per-tu-details.md](build-memory-per-tu-details.md).
