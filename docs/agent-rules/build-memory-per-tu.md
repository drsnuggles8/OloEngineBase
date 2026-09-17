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

**`OLO_BUILD_INSTRUMENTATION=ON` cannot build this project on Windows (issue #1306), and
that is not this measurement's fault.** `cmake_instrumentation()` wraps every custom command in
`ctest --instrument -- <argv>`, which *executes* argv rather than handing it to a shell.
Vendored glad's generator rule is four `COMMAND echo …` lines, one of them
`COMMAND echo ${GLAD_ARGS} > ${GLAD_ARGS_PATH}` — under `cmd.exe` `echo` is a builtin with
no executable and `>` is shell redirection, so the build dies at `glad-generate` with
`Batch file failed at line 3 with errorcode 1` and no diagnostic. Reproduced in isolation
on CMake 4.4.2: the same wrapped `echo` succeeds under a POSIX shell (where `echo` is a
real binary) and fails under `cmd.exe`, which is why Linux CI is unaffected and why §5d's
numbers were obtainable at the time. `OLO_PROC_STAT_REPORT` is the per-TU-memory half on
its own: a compiler flag, no launcher, no CMake floor, no generator restriction. The
umbrella still turns it on, so nothing about the issue's "one switch" contract changed.

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
`base_dir` does **not** rewrite it: base_dir relativises arguments ccache recognises as
paths to existing files, and this one names a file that does not exist yet at hash time.
So an absolute path makes the hash tree-specific and silently ends cross-tree sharing —
including between the two `olo-ci` runner slots, which share one `CCACHE_DIR` and have
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
correct — a hit costs no memory — but it means a ranking taken from a warm build covers
only whatever missed. Two consequences:

- A measurement run wants the cache out of the way. `build-memory.yml` uses
  `CCACHE_DISABLE=1`. Locally, prefer **`CCACHE_RECACHE=1`**: it also forces every TU
  through the compiler, but it *populates* the shared cache with the work instead of
  throwing it away.
- The analyser always prints coverage (records vs. object files actually in the tree, not
  vs. `compile_commands.json` — that counts the whole project while CI builds one target)
  and `--fail-under-coverage` fails the job rather than publishing a lower bound as a
  census.

Under ccache's preprocessor mode a **miss** also emits a second record for the `-E` pass,
written into ccache's own tmp dir at ~30 MiB. The analyser classifies those as
`intermediate` and counts them separately, rather than dropping them silently or letting
them pad the record count.

## Trap 4 — compare only against a build with the same PCH and unity settings

`cmake/CompilerCache.cmake` **forces** `OLO_ENABLE_PCH=OFF` and
`OLO_ENABLE_UNITY_BUILD=OFF` whenever the cache is on, which is what every cached CI job
builds with. Turning the cache off to get a clean measurement therefore silently
re-enables PCH (default `ON`) — and a unity build would merge 16 TUs into one object and
destroy the per-TU ranking outright. Pass `-DOLO_ENABLE_PCH=OFF
-DOLO_ENABLE_UNITY_BUILD=OFF` explicitly on any cache-off measurement run, as
`build-memory.yml` does, or the numbers are not comparable to CI's.

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
| `-gsplit-dwarf` + `--gdb-index` | **ccache handles it.** Measured on ccache 4.13.6: a cold compile produced `.o` + `.dwo`, and after deleting both, a cache **hit** restored both at identical sizes. The `.o` names its companion as a bare `t.dwo`, with the directory coming from `DW_AT_comp_dir` — which this repo already rewrites to `/olo` via `-ffile-prefix-map`, so the object stays path-independent and cross-slot sharing is unaffected. **Unverified risk before adopting:** that same `/olo` mapping is what ASan's symbolizer would have to resolve the `.dwo` through. Confirm a sanitizer stack still symbolises before turning this on for the sanitizer jobs. |
| `-g1` on sanitizer CI only | Keeps line tables and function names, which is what ASan needs to symbolise, and drops variable/type DWARF. Price it against the ranking on the specific TUs the ranking names, not against the tree average — the whole point of #1305 is that the peak is set by a handful of TUs. |

## History

- **#759** (2026-08) — wired the instrumentation API; found compilation, not linking, sets
  the build's memory ceiling, and that a handful of TUs set it rather than the `-j` width.
  Identified that handful by compile time.
- **#822** — native CMake 4.3/4.4 instrumentation and the `olo_heavy` per-file-set compile
  job pool, populated from the same time-based ranking.
- **#1305** — this file. Added per-invocation peak RSS for compiles and links, and the
  weekly artifact that keeps it from decaying again.
