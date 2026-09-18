# Per-TU build memory: measuring it, and the four traps

**Set `--parallel`, `OLO_HEAVY_COMPILE_JOBS`, `OLO_LINK_JOBS` and any cgroup cap from the
published ranking, not from a remembered number.** Configure with clang and
`-DOLO_PROC_STAT_REPORT=ON` (or `-DOLO_BUILD_INSTRUMENTATION=ON`, the umbrella, on Linux),
build, then hand `scripts/analyze_proc_stat.py` the **build tree**:

```powershell
# On WINDOWS use OLO_PROC_STAT_REPORT, not the umbrella — see the note below.
cmake --preset dev-cached -DOLO_PROC_STAT_REPORT=ON
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEditor --target OloEngine-Tests --config Debug --parallel 6'
python scripts/analyze_proc_stat.py build-cached --cap-gib 14
```

**`OLO_BUILD_INSTRUMENTATION=ON` cannot build this project on Windows — issue #1306, which
has the reproduction.** `cmake_instrumentation()` wraps every custom command in
`ctest --instrument -- <argv>`, which *executes* argv instead of handing it to a shell, and
vendored glad's generator rule needs a shell for both `echo` and `>`. The build dies at
`glad-generate` with `Batch file failed at line 3 with errorcode 1` and no diagnostic.
Linux is unaffected because `echo` is a real binary there. `OLO_PROC_STAT_REPORT` is the
per-TU-memory half alone — a compiler flag, no launcher, no CMake floor, no generator
restriction — and the umbrella still turns it on.

CI publishes the same report weekly as the `build-memory-*` artifact
(`.github/workflows/build-memory.yml`), for `Debug` and `Debug + ASan`. That artifact
exists because the alternative already failed: #759 measured this once by hand in 2026-08,
the number was never re-measured, and by 2026-09 five sources disagreed about it by 3x
while `--parallel 2`, a 14 GiB unit cap and a 19 GiB slice cap all rested on it (#1305).

The mechanism is clang's `-fproc-stat-report=<file>`: the driver appends one CSV record per
subprocess it runs, carrying wall time, user time and **that process's peak RSS in KiB**.

    "clang++","CMakeFiles/OloEngine.dir/src/.../Foo.cpp.o",203125,187500,132552
     tool      output file                                  wall   user    peak KiB

## Why not the instrumentation API from #759/#822

It cannot answer this question, and it looks like it can. Its per-command memory field is
`args.dynamicSystemInformation.afterHostMemoryUsed` — **system-wide** host memory in KiB,
sampled when that command finished. It cannot attribute memory to a TU at all. Everything
before #1305 therefore ranked "heavy TUs" by compile **time**, a proxy. Use the API for
time and the flag for memory; they are complementary, and `OLO_BUILD_INSTRUMENTATION`
turns on both — `OLO_PROC_STAT_REPORT` is the memory half alone, for the Windows case
above.

## Trap 1 — the records path must stay RELATIVE, or every other job loses its cache

The flag's value is part of the command line, so the compiler cache hashes it. ccache's
`base_dir` does **not** rewrite it — base_dir relativises arguments ccache recognises as
paths to *existing* files, and this one names a file that does not exist yet at hash time.
An absolute path therefore makes the hash tree-specific and silently ends cross-tree
sharing, including between the two `olo-ci` slots, which share one `CCACHE_DIR` under
different `GITHUB_WORKSPACE` paths.

Measured, ccache 4.13.6 / clang-cl 23.1.0, two source-identical trees under one `base_dir`,
the second expected to hit:

| command line | hit rate |
|---|---|
| no flag at all (baseline) | 1/2 — **50%** |
| `-fproc-stat-report=<absolute path>` | 0/2 — **0%** |
| `-fproc-stat-report=<relative path>` | 1/2 — **50%**, identical to baseline |

`cmake/ProcStatReport.cmake` rejects an absolute `OLO_PROC_STAT_FILE` with a
`FATAL_ERROR`, and `build-memory.yml` greps the generated build system for one. Both
guards are there because the failure is invisible: the cache stays *alive*, just never
warm.

## Trap 2 — there is one records file per SUBDIRECTORY on Linux

A relative path is resolved against the **build tool's** working directory, and that is
not the same directory for every generator. Confirmed by generating both and reading the
emitted compile rule:

| generator | rule | result |
|---|---|---|
| Ninja | runs every command from the top build dir | one `build/olo-proc-stat.csv` |
| Unix Makefiles | `cd <build>/<subdir> && clang++ … -c …` | **one file per subdirectory** |

Every Linux job in this repo configures with no `-G`, so it gets Unix Makefiles
([build-trees-and-windows-asan.md §5e](build-trees-and-windows-asan.md#5e-both-memory-pools-are-ninja-only--so-on-linux-ci---parallel-n-is-the-only-cap-issue-796)).
**Always pass the build tree, never a single file** — the analyser walks it and gathers
every records file, re-rooting the outputs so two targets cannot collide. A reader that
opened only the top-level file on a Linux build would rank a fraction of the TUs, and the
fraction it omits is the engine and test subdirectories: the heavy ones. A Ninja-only
check does not catch this.

## Trap 3 — a warm compiler cache makes a partial ranking look like a cheap build

On a cache **hit** ccache does not run the compiler, so it appends no record. That is
correct — the record measures compiler RSS and no compiler ran — but it is **not** the same
as "a hit is free": ccache still does the lookup and decompresses the result, and that costs
memory this instrument cannot see. Either way a warm build ranks only whatever missed. So: CI uses
`CCACHE_DISABLE=1`; locally prefer **`CCACHE_RECACHE=1`**, which also forces every TU
through the compiler but *populates* the shared cache instead of discarding the work. The
analyser always prints coverage (records vs. objects in the tree — not
`compile_commands.json`, which counts the whole project while CI builds one target) and
`--fail-under-coverage` fails rather than publishing a lower bound as a census.

Under ccache's preprocessor mode a **miss** emits a second record for the `-E` pass (~30 MiB,
in ccache's tmp dir). Those are classified `intermediate` and counted separately, not
dropped silently.

**Measuring the hit RATE needs `CCACHE_STATSLOG`, never before/after `ccache -s`:** every
worktree here shares one `CCACHE_DIR`, so the global counters move with the siblings' builds
(a 28-step build of mine showed a 2378-call delta). The statslog writes one line per
invocation but is **build-scoped, not self-resetting**: it appends, so create or truncate a
unique file per build or you will read the previous build's results back.

## Trap 4 — compare only against a build with the same PCH and unity settings

`cmake/CompilerCache.cmake` **forces** `OLO_ENABLE_PCH=OFF` and
`OLO_ENABLE_UNITY_BUILD=OFF` whenever the cache is on, which is what every cached CI job
builds with. Turning the cache off to get a clean measurement therefore silently
re-enables PCH (default `ON`) — and a unity build would merge 16 TUs into one object and
destroy the per-TU ranking outright. Pass `-DOLO_ENABLE_PCH=OFF
-DOLO_ENABLE_UNITY_BUILD=OFF` explicitly on any cache-off measurement run, as
`build-memory.yml` does, or the numbers are not comparable to CI's.

## The measurement (2026-09-17): the LINK is the ceiling, not any compile

Linux, `/opt/llvm-23.1.0/bin/clang++`, `olo-ci-1`, cache off, PCH and unity off, target
`OloEngine-Tests`, **1761 compiles at 99.9% coverage** in each cell. This is the
configuration the caps were derived from, so these are the numbers to use.

| | plain `Debug` | `Debug + ASan` |
|---|---:|---:|
| median compile | 0.31 GiB | 0.32 GiB |
| p95 compile | 0.84 GiB | 0.85 GiB |
| compiles over 2 GiB | 10 | 12 |
| **heaviest compile** | 4.65 GiB | **8.09 GiB** |
| **heaviest link (`ld.lld`)** | **10.03 GiB** | **11.17 GiB** |
| link ÷ heaviest compile | **2.16x** | **1.38x** |

**One `ld.lld` link of `OloEngine-Tests` uses 11.17 GiB — 80% of the runner unit's 14 GiB
`memory.max`, and more than any translation unit in the build.** The belief it replaces was
8.7 GB; it is 38% higher than that, and it is 10.03 GiB even with no sanitizer. The heaviest
compile, meanwhile, is 8.09 GiB — the folklore "~9.0 GB for one `clang++` TU" was close, and
the "one TU hit 13.9 GB" claim does **not** reproduce.

This does not contradict #759, it completes it. #759 found `OLO_LINK_JOBS=2` is not the
**wall-clock** serialisation point — still true, links are off the critical path. For
**memory** the link is the single largest consumer in both configurations.

**The consequence that matters: nothing bounds it on Linux.** `OLO_LINK_JOBS` and
`olo_heavy` are Ninja job pools and every Linux CI job gets Unix Makefiles
([§5e](build-trees-and-windows-asan.md#5e-both-memory-pools-are-ninja-only--so-on-linux-ci---parallel-n-is-the-only-cap-issue-796)),
so `make -j2` is free to run that 11.17 GiB link alongside a compile. Link + the heaviest
compile is **19.26 GiB**, past the 14 GiB unit cap and at the 19 GiB slice cap. In practice
the tests link happens at the end when little else is left, which is why this has not been
failing constantly — but it is unguarded, not safe by construction.

### What to do with `--parallel`, stated as a recommendation not a change

- **Sanitizer jobs: keep `--parallel 2`.** Three lanes is a 15.24 GiB worst case, past the
  cap. The pin is correct and now has evidence.
- **Non-sanitizer Linux Debug: 4 lanes fits on compile grounds** (the derivation's largest
  fitting N), but the gain is bounded by the same link, so raise it only if the queue
  actually needs it.
- **The lever worth having is a link bound that works under Makefiles.** A 14 GiB unit cap
  leaves 2.8 GiB of headroom over a single link; that, not `--parallel`, is what is tight.

None of these were applied here — changing a live runner's configuration is out of #1305's
scope and is the maintainer's call.

### The `olo_heavy` pool is mispopulated in both directions

Confirmed independently on Linux+ASan and on a Windows clang-cl census — same names, same
direction, so it is not an artefact of either host. On the Linux+ASan ranking it holds
`AbilityComponentRoundTripTest.cpp` (#281), `RenderGraphTest.cpp` (#34),
`LuaScriptGlue_EngineApi.cpp` (#21), `Prefab.cpp` (#18) and `McpToolsRender.cpp` (#17),
while `Scene.cpp` (**#3**), `ComponentFieldRegistry.cpp` (**#5**), `SceneSerializer.cpp`
(#7), `LuaScriptGlue.cpp` (#9) and `SaveGameSerializer.cpp` (#11) sit outside it.

`LuaScriptGlue.cpp` was deliberately excluded when the glue was split, on the recorded
grounds that "the dispatcher left behind is an ordinary small TU"
(`OloEngine/src/CMakeLists.txt`). It ranks **9th**. What the pool gets right:
`McpFieldRegistry.cpp` is #1 in every cell and the `LuaScriptGlue_*` parts cluster in the
top 15 — #822's split worked. Re-populating it is **issue #1307**; the pool is Ninja-only,
so this costs nothing on Linux CI and everything locally.

## Scope: what this cannot see

- **MSVC.** `-fproc-stat-report` is clang-only and there is no equivalent; the `msvc`
  preset is not the host whose caps are in question. The option must still *configure and
  build cleanly* under it, which it does — `cmake/ProcStatReport.cmake` warns and returns.
- **vcpkg ports and FetchContent dependencies.** They build outside this project's
  `add_compile_options()` reach — the same blind spot #759's trace had.
- **Static archiving.** `CMAKE_<LANG>_ARCHIVE_*` rules take neither a launcher nor the
  compiler's own flags. Same scope note `cmake/LinkSemaphore.cmake` makes: the measured
  spike is the linker, not the archiver.
- **Links, but only on Windows.** With a GNU-frontend clang, `CMAKE_CXX_LINK_EXECUTABLE`
  runs the *compiler* as the link driver, which spawns `ld.lld` as a subprocess — so the
  flag on the link line reports the linker's own peak RSS, no linker launcher needed, and
  Linux gets the link half of #1305 for free. Under **clang-cl** CMake invokes `lld-link`
  **directly**: there is no clang driver in the link step to pass the option to, and
  `/clang:…` is a compiler option, so lld-link takes it as an input file and the link
  fails outright with `could not open '/clang:-fproc-stat-report=…'`. The module therefore
  adds it to compiles only there, and *says* so at configure time — "no link records on
  Windows" would otherwise read as a measurement that found links to be free. For link
  peak RSS, use a Linux clang build, which is the host whose caps are in question anyway.

## The cheap config levers, priced

The three levers #1305 listed, checked rather than assumed:

| lever | verdict |
|---|---|
| ThinLTO instead of full LTO | **Cannot help the sanitizer jobs.** `cmake/CommonProperties.cmake` sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE`/`_DIST` only, and every sanitizer job is `CMAKE_BUILD_TYPE=Debug`. No LTO is enabled there to convert. |
| `-gsplit-dwarf` + `--gdb-index` | **ccache handles it**, which #1305 asked to verify rather than assume. Measured on ccache 4.13.6: a cold compile produced `.o` + `.dwo`; with both deleted, a cache **hit** restored both at identical sizes. The `.o` names its companion as a bare `t.dwo` and takes the directory from `DW_AT_comp_dir`, which this repo already rewrites to `/olo`, so objects stay path-independent and cross-slot sharing is unaffected. **Unverified risk:** that same `/olo` mapping is what ASan's symbolizer must resolve the `.dwo` through. Confirm a sanitizer stack still symbolises before adopting. |
| `-g1` on sanitizer CI only | Keeps line tables and function names — what ASan needs to symbolise — and drops variable/type DWARF. Price it on the TUs the ranking names, not on the tree average: the peak is set by a handful. |

### Appendix: the committed per-TU figures were wrong when written, and three methods now agree

`OloEngine/src/CMakeLists.txt`, `OloEditor/src/CMakeLists.txt` and issue #1113 carry per-TU
figures up to **8.7x too high** — a recipe bug, not decay. That survey ran
`cmake --build <dir> --target X -- '<obj>'`, which passes *both* the target and the object to
ninja and so builds the **whole target**, then credited the largest of a dozen concurrent
unrelated compiles to whichever TU was named. An isolated re-measurement on 2026-09-08
(per-PID `PeakWorkingSet64`, one `clang-cl` asserted) corrected it; this census is a third,
independent method and agrees with that correction within **1.11-1.23x** on all five
spot-checked TUs. Three of them, as committed → isolated → census: `Scene.cpp`
2,498 → 2,499 → 2,785 MB; `ComponentRoundTripTest.cpp` 12,389 → 1,415 → 1,599 MB;
`Prefab.cpp` 6,400 → 1,282 → 1,488 MB. The other two were `McpToolsRender.cpp`
(7,838 → 1,369 → 1,680 MB) and `RenderGraphTest.cpp` (8,619 → 949 → 1,053 MB).

`Scene.cpp` is the control — the one row that reproduced exactly under the buggy recipe —
and it lands at the same 1.11x, so the residual is the parallel-build environment rather
than a systematic error in either method. **Census for ranking, the isolated recipe for one
TU's absolute figure.** Treat every number in those comments as void until re-measured.

## History

- **#759** (2026-08) — wired the instrumentation API; found compilation, not linking, sets
  the build's memory ceiling, and that a handful of TUs set it rather than the `-j` width.
  Identified that handful by compile time.
- **#822** — native CMake 4.3/4.4 instrumentation and the `olo_heavy` per-file-set compile
  job pool, populated from the same time-based ranking.
- **#1305** — this file. Added per-invocation peak RSS for compiles and links, and the
  weekly artifact that keeps it from decaying again.
