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
# It covers LINKING for free, which is why no linker launcher is involved (the issue's
# step 2 suggested CMAKE_CXX_LINKER_LAUNCHER): `clang++ ... -o app` runs `ld.lld` as a
# driver subprocess, so the same flag on the LINK command line reports the linker's own
# peak RSS. Verified locally — a clang-cl link emitted `"lld-link","a.obj",0,0,25812`
# alongside the compile record. That also keeps LinkSemaphore.cmake's launcher chain
# completely untouched.
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

if(NOT OLO_BUILD_INSTRUMENTATION)
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
        "OLO_BUILD_INSTRUMENTATION is ON but per-invocation peak-RSS reporting needs clang: "
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
else()
    set(_olo_proc_stat_flag "-fproc-stat-report=${OLO_PROC_STAT_FILE}")
endif()

# Directory-scoped rather than per-target so it reaches every TU this project compiles,
# including the in-tree vendor subdirectories — a heavy vendor TU is as much a candidate
# for the peak as an engine one, and excluding it by construction would pre-judge the
# ranking this exists to produce. FetchContent/vcpkg ports build outside these calls and
# are out of reach regardless, the same blind spot #759's trace had.
add_compile_options("${_olo_proc_stat_flag}")

# Links go through the same flag, which is the whole of the issue's step 2: ld.lld at
# 8.7 GB is the other half of the ceiling and OLO_LINK_JOBS=2 rests on it. Static
# ARCHIVING is not covered — it goes through CMAKE_<LANG>_ARCHIVE_* rules, which take
# neither a launcher nor the compiler's own flags. Acceptable, and the same scope note
# LinkSemaphore.cmake makes: the measured spike is the linker, not the archiver.
add_link_options("${_olo_proc_stat_flag}")

message(STATUS
    "Per-invocation peak RSS: ON, appending to '${OLO_PROC_STAT_FILE}' in each build directory "
    "the build tool runs from. Rank with: python scripts/analyze_proc_stat.py ${CMAKE_BINARY_DIR}")

unset(_olo_proc_stat_flag)
