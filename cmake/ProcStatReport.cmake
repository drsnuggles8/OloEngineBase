# ProcStatReport.cmake
# Per-invocation PEAK RSS for every compile and every link — issue #1305.
#
# WHAT THIS ADDS THAT #759/#822 COULD NOT
# ---------------------------------------
# The CMake Instrumentation API wired in the root CMakeLists.txt records per-command
# TIME plus `args.dynamicSystemInformation.afterHostMemoryUsed`, which is SYSTEM-WIDE
# host memory in KiB sampled when the command finished — not the command's own RSS.
# So the existing traces cannot attribute memory to a translation unit at all, and the
# "handful of heavy TUs" #759 named were identified by compile TIME, a proxy. Every
# number that rests on per-TU memory — the Linux `--parallel 2` pins, OLO_LINK_JOBS,
# OLO_HEAVY_COMPILE_JOBS, and the box's 14 GiB unit / 19 GiB slice cgroup caps — was
# therefore derived from an unmeasured figure, and five sources disagreed by 3x.
#
# clang's `-fproc-stat-report=<file>` closes exactly that gap: the driver appends one
# CSV record per SUBPROCESS it executes, carrying wall time, user time and that
# process's PEAK RSS in KiB, at essentially zero overhead.
#
#     "clang++.exe","CMakeFiles/OloEngine.dir/src/.../Foo.cpp.o",203125,187500,132552
#      ^tool         ^output file                                 ^wall ^user  ^peak KiB
#                                                                  (microseconds)
#
# It covers LINKING wherever the COMPILER drives the link, which is why no linker launcher
# is involved (the issue's step 2 suggested CMAKE_CXX_LINKER_LAUNCHER): `clang++ ... -o app`
# runs `ld.lld` as a driver subprocess, so the same flag on the LINK command line reports
# the linker's own peak RSS — verified with a standalone clang-cl link, which emitted
# `"lld-link","a.obj",0,0,25812` alongside the compile record. That keeps
# LinkSemaphore.cmake's launcher chain completely untouched.
#
# CMake's own link rule only drives it that way for a GNU-frontend clang, though; under
# clang-cl it runs lld-link directly and the flag must NOT be added to the link line at
# all. See the `_olo_proc_stat_links` branch below — that difference is measured, and it
# is why the Linux builds (the ones whose caps are in question) get the link half and the
# local Windows trees do not.
#
# WHY THE PATH IS RELATIVE, AND WHY THAT IS NOT A STYLE CHOICE
# ------------------------------------------------------------
# It is what keeps the compiler cache warm, and it was MEASURED, not assumed (the issue
# asked for exactly this check under a different lever). The flag's value is part of the
# command line, so ccache hashes it. ccache's `base_dir` does NOT rewrite it — base_dir
# relativises arguments ccache recognises as paths to existing files, and this one names
# a file that does not exist yet at hash time. So an ABSOLUTE path makes the hash
# tree-specific and silently destroys cross-tree sharing.
#
# Measured on ccache 4.13.6 / clang-cl 23.1.0, two source-identical trees under one
# base_dir, second tree expected to hit:
#
#     no flag at all (baseline)        1 hit  / 2   50%   <- what sharing looks like
#     -fproc-stat-report=<abs path>    0 hits / 2    0%   <- sharing GONE
#     -fproc-stat-report=<rel path>    1 hit  / 2   50%   <- identical to baseline
#
# A relative path is resolved by clang against the BUILD TOOL'S working directory, so each
# build tree still gets its own records file while every tree puts the SAME text on the
# command line. Do not "tidy this up" into ${CMAKE_BINARY_DIR}/...; that is the 0% row.
#
# THAT WORKING DIRECTORY IS NOT THE SAME FOR EVERY GENERATOR, and the difference decides
# how the records are read back. Confirmed by generating both and reading the emitted
# compile rule, not by assumption:
#
#     Ninja           every command runs from the top build dir
#                     -> one  <build>/olo-proc-stat.csv
#     Unix Makefiles  the rule is literally `cd <build>/<subdir> && clang++ ... -c ...`
#                     -> one  <build>/<subdir>/olo-proc-stat.csv  PER SUBDIRECTORY
#
# Every Linux job in this repo configures with no -G and so gets Unix Makefiles (see
# docs/agent-rules/build-trees-and-windows-asan.md §5e). So ALWAYS hand the analyser the
# build TREE, not one file — scripts/analyze_proc_stat.py walks it and gathers every
# records file. Reading only the top-level file on a Linux build would rank a fraction of
# the TUs, and the fraction it omits is the engine and test subdirectories: the heavy ones.
#
# The other half of the same measurement, and the reason the report is honest about
# coverage rather than silent: on a cache HIT ccache does not run the compiler, so NO
# record is appended. A warm build therefore yields a PARTIAL ranking. That is correct
# behaviour — a hit costs no memory — but it means the ranking must come from a cold
# build, and scripts/analyze_proc_stat.py prints the recorded-vs-expected count so a
# partial run can never be mistaken for a complete one. Under ccache's preprocessor mode
# a miss also emits a SECOND record for the `-E` pass (output under ccache's tmp dir,
# ~30 MiB); the analyser classifies and counts those separately instead of dropping them.
#
# The records file is APPENDED to, never truncated, and the records carry no timestamp.
# The analyser therefore reports the MAXIMUM peak RSS seen per output, and CI deletes the
# file before building so a published ranking covers one build.

# TWO SWITCHES, AND THE SECOND ONE IS NOT OPTIONAL POLISH.
#
# OLO_BUILD_INSTRUMENTATION stays the umbrella the issue asked this to ride: turning it
# on turns this on. But it must also be possible to get per-TU memory WITHOUT the CMake
# Instrumentation API, because on Windows the API cannot build this project at all.
#
# cmake_instrumentation() wraps every custom command in `ctest --instrument -- <argv>`,
# which EXECUTES argv directly instead of handing it to a shell. Vendored glad's own
# generator rule (OloEngine/vendor/clang/glad-src/cmake/GladConfig.cmake, a FetchContent
# download that must not be edited) is built from four `COMMAND echo ...` lines, one of
# which is `COMMAND echo ${GLAD_ARGS} > ${GLAD_ARGS_PATH}`. Under cmd.exe `echo` is a
# shell BUILTIN with no executable, and `>` is shell redirection — so the wrapped command
# fails with exit 1 and NO diagnostic, and the build stops at `glad-generate`:
#
#     Batch file failed at line 3 with errorcode 1
#     FAILED: [code=1] .../glad-build/include/glad/gl.h ...
#
# Reproduced in isolation (CMake 4.4.2): the same `ctest --instrument ... -- echo x`
# succeeds under a POSIX shell, where `echo` is a real binary, and fails under cmd.exe.
# That is a pre-existing incompatibility between #822's wiring and an upstream
# dependency, not something this file introduced — and fixing it means restructuring a
# vendored third-party custom command, so it is filed as ISSUE #1306 rather than done here.
#
# So: OLO_PROC_STAT_REPORT defaults to the umbrella but can be set on its own, which is
# the supported way to measure per-TU memory on Windows today:
#
#     cmake --preset dev-cached -DOLO_PROC_STAT_REPORT=ON
#
# It needs no launcher, no CMake version floor and no particular generator, so it has
# none of the API's constraints.
# NOT option() / set(... CACHE ...): that would make the umbrella's value STICKY. The
# first configure with OLO_BUILD_INSTRUMENTATION=ON would write OLO_PROC_STAT_REPORT=ON
# into the cache, and every later configure — including one that turns the umbrella back
# OFF — would keep reading the stale ON and quietly go on instrumenting. That is the
# "set forever after the first configure" trap spelled out in cmake/CompilerCache.cmake's
# OLO_COMPILER_CACHE_TOOL comment, and this is the same shape.
#
# `if(NOT DEFINED ...)` is what makes both paths work: an explicit
# -DOLO_PROC_STAT_REPORT=ON creates a real cache entry, so it is DEFINED and wins; with
# no explicit value the variable is re-derived from the umbrella on EVERY configure and
# can never go stale.
if(NOT DEFINED OLO_PROC_STAT_REPORT)
    set(OLO_PROC_STAT_REPORT ${OLO_BUILD_INSTRUMENTATION})
endif()

if(NOT OLO_PROC_STAT_REPORT)
    return()
endif()

# Clang only, and stated rather than left to be discovered. MSVC has no equivalent flag,
# and the `msvc` preset is not the host whose caps are in question — the self-hosted Linux
# builds are clang, and they are. GCC is not a toolchain this repo builds with.
#
# CMAKE_CXX_COMPILER_FRONTEND_VARIANT splits the two clang spellings: a GNU-frontend
# clang/clang++ takes the flag directly, while clang-cl parses MSVC-style options and
# needs it handed to the clang driver through the `/clang:` pass-through. Getting that
# wrong is not a build failure — clang-cl would treat a bare `-fproc-stat-report=` as an
# input filename and the records file would simply never appear.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    message(WARNING
        "Per-invocation peak-RSS reporting was requested but it needs clang: "
        "'${CMAKE_CXX_COMPILER_ID}' has no equivalent of -fproc-stat-report (issue #1305). "
        "Timing and host-memory instrumentation (if the generator supports it) is unaffected; "
        "only the per-TU memory ranking is unavailable. Configure a clang preset — dev-cached, "
        "clangcl, or a Linux preset — to get it.")
    return()
endif()

set(OLO_PROC_STAT_FILE "olo-proc-stat.csv" CACHE STRING
    "Records file for per-invocation peak RSS (issue #1305). MUST stay RELATIVE — an absolute path is hashed by the compiler cache and drops cross-tree sharing to zero; see the measurement in cmake/ProcStatReport.cmake.")

# Reject a value that would quietly cost the cache, rather than accepting it and letting
# a future cold-cache CI job look like a cache-configuration problem. Same "fail rather
# than silently disable" rule OLO_LINK_JOBS and OLO_HEAVY_COMPILE_JOBS follow.
if(OLO_PROC_STAT_FILE STREQUAL "")
    message(FATAL_ERROR
        "OLO_PROC_STAT_FILE is empty — clang would write the records to a file named '' and the "
        "measurement would silently produce nothing. Unset it to take the default, or name a file.")
endif()
if(IS_ABSOLUTE "${OLO_PROC_STAT_FILE}")
    message(FATAL_ERROR
        "OLO_PROC_STAT_FILE must be a RELATIVE path (got '${OLO_PROC_STAT_FILE}'). An absolute "
        "path goes into the compiler cache's hash and cannot be relativised by ccache's base_dir, "
        "which drops cross-tree cache sharing from the measured 50% baseline to 0%. It is resolved "
        "against the build tool's working directory (CMAKE_BINARY_DIR) either way, so a relative "
        "path already gives this tree its own file.")
endif()

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(_olo_proc_stat_flag "/clang:-fproc-stat-report=${OLO_PROC_STAT_FILE}")
    # LINKS ARE NOT MEASURABLE UNDER clang-cl, and this is a property of CMake's link
    # rule rather than of the flag. With a GNU-frontend clang, CMAKE_CXX_LINK_EXECUTABLE
    # runs the COMPILER as the link driver, which spawns ld.lld as a subprocess — so the
    # flag on the link line reports the linker's own peak RSS, which is exactly what
    # issue #1305's step 2 wanted. Under clang-cl, CMake invokes `lld-link` DIRECTLY;
    # there is no clang driver in the link step, so there is nothing to pass the option
    # to and no subprocess to report on. `/clang:` is a clang-cl COMPILER option, so
    # lld-link takes it as an input filename and the link fails outright:
    #
    #     lld-link: error: could not open '/clang:-fproc-stat-report=olo-proc-stat.csv'
    #
    # Said out loud rather than silently skipped, because "no link records on Windows"
    # otherwise looks like a measurement that found links to be free.
    set(_olo_proc_stat_links FALSE)
else()
    set(_olo_proc_stat_flag "-fproc-stat-report=${OLO_PROC_STAT_FILE}")
    set(_olo_proc_stat_links TRUE)
endif()

# Directory-scoped rather than per-target so it reaches every TU this project compiles,
# including the in-tree vendor subdirectories — a heavy vendor TU is as much a candidate
# for the peak as an engine one, and excluding it by construction would pre-judge the
# ranking this exists to produce. FetchContent/vcpkg ports build outside these calls and
# are out of reach regardless, the same blind spot #759's trace had.
add_compile_options("${_olo_proc_stat_flag}")

# Links go through the same flag wherever the compiler drives them, which is the whole of
# the issue's step 2: ld.lld at 8.7 GB is the other half of the ceiling and OLO_LINK_JOBS=2
# rests on it. Static ARCHIVING is not covered either way — it goes through
# CMAKE_<LANG>_ARCHIVE_* rules, which take neither a launcher nor the compiler's own flags.
# Acceptable, and the same scope note LinkSemaphore.cmake makes: the measured spike is the
# linker, not the archiver.
if(_olo_proc_stat_links)
    add_link_options("${_olo_proc_stat_flag}")
    set(_olo_proc_stat_link_note "compiles and links")
else()
    set(_olo_proc_stat_link_note
        "compiles ONLY - links are not measurable under clang-cl, where CMake invokes lld-link directly instead of through the clang driver (see cmake/ProcStatReport.cmake). Use a Linux clang build for link peak RSS")
endif()

message(STATUS
    "Per-invocation peak RSS: ON for ${_olo_proc_stat_link_note}. Appending to "
    "'${OLO_PROC_STAT_FILE}' in each build directory the build tool runs from. Rank with: "
    "python scripts/analyze_proc_stat.py ${CMAKE_BINARY_DIR}")

unset(_olo_proc_stat_flag)
unset(_olo_proc_stat_links)
unset(_olo_proc_stat_link_note)
