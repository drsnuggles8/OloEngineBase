# SanitizerOptimize.cmake
# Compile the sanitizer jobs at `-O1` instead of `Debug`'s implicit `-O0`.
#
# WHY IT IS A CANDIDATE. Upstream recommends it: the AddressSanitizer documentation says to
# build with `-O1` or higher, because at `-O0` the instrumented code keeps every redundant
# check the optimizer would fold, which costs runtime. Every sanitizer job here is
# `CMAKE_BUILD_TYPE=Debug`, and this repo never sets `CMAKE_CXX_FLAGS_DEBUG` for a GNU or
# Clang frontend, so it is CMake's default `-g` with NO `-O` flag at all — `-O0`.
#
# WHY IT IS NOT OBVIOUSLY A WIN, and why this ships OFF.
#
# 1. IT IS AIMED AT THE WRONG NUMBER. #1305 measured the ceiling as the LINK (11.17 GiB
#    under Debug + ASan), not a compile: the median compile is 0.32 GiB and p95 is 0.85 GiB.
#    Optimization level moves compile memory, in a direction that is not even signed in
#    advance — `-O1` runs passes `-O0` skips (more peak) while shrinking the IR those passes
#    carry (less peak). Whatever it does to a 0.32 GiB median does not touch the 11.17 GiB
#    that sets the cap. Wall-clock is the plausible win here, not memory.
#
# 2. IT CHANGES WHAT THE JOBS TEST. Inlining changes which frames a report shows, and
#    ASan's stack-use-after-scope detection depends on the lifetime markers the optimizer
#    is free to move. UBSan is the sharper case: `-O1` can prove a check unreachable and
#    delete it, so a suite that passes at `-O1` has not necessarily exercised what the same
#    suite exercised at `-O0`. A sanitizer job that finds less is not a faster sanitizer job.
#
# 3. IT IS UNPRICED. No cell has ever run it. `.github/workflows/build-memory.yml` gains a
#    `Debug + ASan + -O1` cell with this commit; that cell decides whether the default moves,
#    the same way `OLO_SPLIT_DWARF`'s did.
#
# So this exists to be MEASURED, and the measurement has to report the suite result beside
# the memory and wall figures — a green suite is part of the evidence, not a precondition
# for reading the numbers. If it is adopted, adopt it per job: the ASan and TSan suites can
# take it on different evidence than UBSan, whose checks are the ones optimization can remove.
#
# Ordering: CMake emits `CMAKE_CXX_FLAGS_<CONFIG>` before a directory's COMPILE_OPTIONS and
# the compilers take the LAST `-O` on the line, so this `-O1` wins over the build type's
# (absent) one. Verified the same way as cmake/MinimalDebugInfo.cmake. This file must stay
# above the add_subdirectory() calls for the usual reason: add_compile_options() is read
# when a target is created.

option(OLO_SANITIZER_OPTIMIZE
    "Compile sanitizer builds at -O1 rather than Debug's -O0 (upstream ASan's recommendation). OFF by default and unpriced: see build-memory.yml's -O1 cell and cmake/SanitizerOptimize.cmake."
    OFF)

if(NOT OLO_SANITIZER_OPTIMIZE)
    return()
endif()

# Only meaningful when something is actually instrumented. Asking for it on a plain build is
# a request to change the Debug configuration's optimization level, which is a different
# decision with different consequences — name it rather than quietly doing it.
if(NOT (OLO_ENABLE_ASAN OR OLO_ENABLE_UBSAN OR OLO_ENABLE_TSAN OR OLO_ENABLE_LSAN))
    message(WARNING
        "OLO_SANITIZER_OPTIMIZE is ON but no sanitizer is enabled — ignoring it. This option "
        "exists to take ASan's own -O1 recommendation on the sanitizer jobs, not to re-optimize "
        "a plain Debug build; set CMAKE_CXX_FLAGS_DEBUG for that.")
    return()
endif()

# The MSVC frontend spells it /O1 and means something different by it (favour size, and it
# implies optimizations clang-cl's ASan pairs badly with). The Windows ASan job is not the
# host whose caps are in question, so this stays a GNU-frontend option rather than growing
# a second spelling nobody has measured.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$")
    message(WARNING
        "OLO_SANITIZER_OPTIMIZE is ON but this toolchain uses the MSVC frontend or an "
        "unsupported compiler ('${CMAKE_CXX_COMPILER_ID}') — ignoring it.")
    return()
endif()

set(_olo_so_langs "")
foreach(_olo_so_lang C CXX)
    if(CMAKE_${_olo_so_lang}_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$"
       AND NOT CMAKE_${_olo_so_lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options("$<$<COMPILE_LANGUAGE:${_olo_so_lang}>:-O1>")
        list(APPEND _olo_so_langs ${_olo_so_lang})
    endif()
endforeach()
unset(_olo_so_lang)

if(NOT _olo_so_langs)
    message(WARNING "OLO_SANITIZER_OPTIMIZE is ON but no C or C++ compiler accepts -O1.")
    return()
endif()

list(JOIN _olo_so_langs "/" _olo_so_note)
message(STATUS
    "Sanitizer optimization: -O1 for ${_olo_so_note} instead of Debug's -O0. Inlining now "
    "occurs, so reports show different frames and UBSan may optimize away checks it made at "
    "-O0 — compare suite results, not just timings, before treating this as free.")
unset(_olo_so_note)
