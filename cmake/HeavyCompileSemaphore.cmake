# HeavyCompileSemaphore.cmake
# The `olo_heavy` compile bound for trees where the Ninja pool does not exist (issue #1473).
#
# The root CMakeLists creates the `olo_heavy` job pool only under CMake >= 4.4 with a Ninja
# generator, because binding a pool to a handful of sources needs a per-file-set
# JOB_POOL_COMPILE. Anywhere else the heavy TUs compile at the full -j width, side by side,
# since they sit next to each other in their targets' source lists. On the olo-ci box that
# is CMake 3.31.8, and it OOM-killed gpu-conformance-amd.yml every night from 2026-09-09:
# six heavy GCC compiles held 13.66 GiB of the job's 14 GiB runner unit when the kernel
# fired. The measurement is in scripts/heavy-compile-semaphore.py's header.
#
# ON, this puts scripts/heavy-compile-semaphore.py in front of the C++ compiler launcher
# (ccache, or the #1460 PCH launcher in front of it). A compile of a manifest TU takes one
# of OLO_HEAVY_COMPILE_JOBS flock permits first; every other compile runs at once. With
# OLO_COMPILE_RSS_LOG it also records each compile's peak RSS, the GCC counterpart of
# OLO_PROC_STAT_REPORT (clang only).
#
# OFF BY DEFAULT: it costs a Python start per compile, and where the pool exists (every
# Windows dev tree, CMake 4.4 + Ninja) the pool already does this. Where both are available
# the pool wins and this says so, rather than stacking two throttles.
#
# Must be included AFTER CompilerCache.cmake (it wraps the launcher that module chose) and
# BEFORE the add_subdirectory() calls (a launcher is read when a target is created). The
# manifest itself is written at the end of the root CMakeLists, once every target has
# called olo_bind_heavy_compile_pool(); the launcher only reads it at build time.

option(OLO_HEAVY_COMPILE_SEMAPHORE
       "Bound concurrent compiles of the heavy TU set with a flock launcher where the olo_heavy Ninja pool is unavailable (issue #1473)"
       OFF)
option(OLO_COMPILE_RSS_LOG
       "With OLO_HEAVY_COMPILE_SEMAPHORE: record every C++ compile's peak RSS to <build>/olo-compile-rss.tsv"
       OFF)

set(OLO_HEAVY_COMPILE_MANIFEST "${CMAKE_BINARY_DIR}/olo-heavy-compile-sources.txt")
set(OLO_HEAVY_COMPILE_SEMAPHORE_ACTIVE FALSE)

if(OLO_HEAVY_COMPILE_SEMAPHORE)
    if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
        message(STATUS
            "Heavy-compile semaphore: not needed, the olo_heavy Ninja pool is active "
            "(depth ${OLO_HEAVY_COMPILE_JOBS})")
    elseif(WIN32)
        message(WARNING
            "Heavy-compile semaphore: POSIX only (flock). The heavy TUs compile UNBOUNDED "
            "on this tree; use CMake >= 4.4 with Ninja for the olo_heavy pool instead.")
    elseif(CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
        message(WARNING
            "Heavy-compile semaphore: the ${CMAKE_GENERATOR} generator ignores "
            "CMAKE_CXX_COMPILER_LAUNCHER, so the heavy TUs compile UNBOUNDED.")
    else()
        find_program(OLO_HEAVY_COMPILE_PYTHON NAMES python3 python)
        if(NOT OLO_HEAVY_COMPILE_PYTHON)
            message(WARNING
                "Heavy-compile semaphore: no python3 on PATH, so the heavy TUs compile "
                "UNBOUNDED. This is the state that OOM-killed gpu-conformance-amd.yml (#1473).")
        else()
            set(_olo_heavy_launcher
                "${OLO_HEAVY_COMPILE_PYTHON}"
                "${CMAKE_SOURCE_DIR}/scripts/heavy-compile-semaphore.py"
                "--slots=${OLO_HEAVY_COMPILE_JOBS}"
                "--manifest=${OLO_HEAVY_COMPILE_MANIFEST}")
            if(OLO_COMPILE_RSS_LOG)
                list(APPEND _olo_heavy_launcher "--rss-log=${CMAKE_BINARY_DIR}/olo-compile-rss.tsv")
            endif()
            # The bare `--` ends the script's own options; everything after it is the
            # compile, whatever launcher chain CompilerCache.cmake or -D put there.
            set(CMAKE_CXX_COMPILER_LAUNCHER ${_olo_heavy_launcher} "--" ${CMAKE_CXX_COMPILER_LAUNCHER})
            set(OLO_HEAVY_COMPILE_SEMAPHORE_ACTIVE TRUE)
            message(STATUS
                "Heavy-compile semaphore: ON, ${OLO_HEAVY_COMPILE_JOBS} permit(s) for the "
                "manifest TUs (olo_heavy pool unavailable: CMake ${CMAKE_VERSION}, ${CMAKE_GENERATOR})")
            if(OLO_COMPILE_RSS_LOG)
                message(STATUS "Heavy-compile semaphore: per-compile peak RSS -> ${CMAKE_BINARY_DIR}/olo-compile-rss.tsv")
            endif()
            unset(_olo_heavy_launcher)
        endif()
    endif()
elseif(OLO_COMPILE_RSS_LOG)
    message(WARNING "OLO_COMPILE_RSS_LOG needs OLO_HEAVY_COMPILE_SEMAPHORE=ON; nothing will be recorded.")
endif()

# Called once at the end of the root CMakeLists, after every olo_bind_heavy_compile_pool().
function(olo_write_heavy_compile_manifest)
    get_property(_olo_heavy_sources GLOBAL PROPERTY OLO_HEAVY_COMPILE_SOURCES)
    list(REMOVE_DUPLICATES _olo_heavy_sources)
    list(JOIN _olo_heavy_sources "\n" _olo_heavy_text)
    file(WRITE "${OLO_HEAVY_COMPILE_MANIFEST}" "${_olo_heavy_text}\n")
    if(OLO_HEAVY_COMPILE_SEMAPHORE_ACTIVE)
        list(LENGTH _olo_heavy_sources _olo_heavy_count)
        if(_olo_heavy_count EQUAL 0)
            # An empty manifest bounds nothing and would look exactly like a working one.
            message(FATAL_ERROR
                "Heavy-compile semaphore: no TU was handed to olo_bind_heavy_compile_pool(), "
                "so the manifest is empty and the semaphore would bound nothing.")
        endif()
        message(STATUS "Heavy-compile semaphore: ${_olo_heavy_count} TU(s) in ${OLO_HEAVY_COMPILE_MANIFEST}")
    endif()
endfunction()
