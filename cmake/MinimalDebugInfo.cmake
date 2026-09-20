# MinimalDebugInfo.cmake
# Drop variable/type DWARF from the objects so the LINKER loads less of it — the third
# lever docs/agent-rules/build-memory-per-tu.md priced but left unmeasured.
#
# WHY THIS IS THE LEVER THAT IS LEFT. #1305 established that the Linux ceiling is one
# `ld.lld` link of OloEngine-Tests at 11.17 GiB under Debug + ASan, above the heaviest
# compile (8.09 GiB), and that the link is DEBUG-INFO bound rather than instrumentation
# bound: ASan adds +74% to the heaviest compile but only +11% to the link, so whatever
# fills those gigabytes is present with no sanitizer at all. Two levers attack that. The
# first, `-gsplit-dwarf`, is measured at -2.47 GiB (-22%) and is OFF because it costs
# inlined frames (cmake/SplitDwarf.cmake). This is the second, and it attacks strictly
# more of the same quantity.
#
# `-g1` (clang's `-gline-tables-only` by another name) keeps `.debug_line` and the symbol
# table — file, line and function name, which is exactly what a sanitizer report prints —
# and drops `.debug_info`'s types, variables and lexical scopes, which only a debugger
# reads. CI never attaches one.
#
# WHAT IT BUYS, measured 2026-09-20 on this box, clang 23.1.0, one header-heavy C++23 TU
# (STL containers, a variant, a 24-deep recursive template) compiled `-O0 -fsanitize=address
# -ffile-prefix-map=...=/olo`, i.e. the CI compile line. The figure that matters is the
# `.debug*` bytes left IN THE OBJECT, because that is what the linker has to load:
#
#                            object     .debug in object    vs -g
#     -g  (what CI does now)  842,168        446,195         100%
#     -g1                     328,800         30,177           7%
#     -g -gsplit-dwarf        452,680        153,746          34%
#     -g1 -gsplit-dwarf       326,200         28,927           6%
#
# So `-g1` removes 93% of the linker-visible debug info where `-gsplit-dwarf` removes 66%.
# Since split DWARF's 66% was worth -22% on the real link, this lever plausibly lands
# larger — but ONE SYNTHETIC TU IS NOT THE TREE, the real mix decides, and the ratio does
# not transfer linearly to a peak. Treat the table as the reason to measure, not as the
# measurement: `.github/workflows/build-memory.yml` has a `Debug + ASan + -g1` cell and
# that is the number to quote.
#
# IT KEEPS INLINED FRAMES, which is the whole reason split DWARF stays off. Same probe
# methodology as cmake/SplitDwarf.cmake but on an EXECUTABLE rather than a `.o` (the
# lookup differs, which is what made that file's `.dwp` result unproven), clang 23.1.0,
# an `always_inline` callee overflowing a heap buffer under ASan, every arm carrying
# `-ffile-prefix-map=<src>=/olo` as every CI job does:
#
#     -g                     (baseline)        inlined frame RESOLVES
#     -g -gsplit-dwarf                         inlined frame LOST      <- the known trade
#     -g -gsplit-dwarf  + .dwp beside the exe  inlined frame RESOLVES  <- see SplitDwarf.cmake
#     -g1                                      inlined frame RESOLVES  <- this lever
#     -g1 -gsplit-dwarf                        inlined frame LOST
#
# Function name and file:line survived in every arm. So `-g1` gives a larger reduction
# than split DWARF at no symbolization cost, and needs no side-car file to do it.
#
# DO NOT COMBINE IT WITH `OLO_SPLIT_DWARF`. The last row is why: the combination inherits
# split DWARF's lost frames for 1% more reduction than `-g1` alone. The two are alternatives,
# not a stack, and the configure step below says so rather than leaving it to be rediscovered.
#
# DEFAULT OFF, for the same reason OLO_SPLIT_DWARF is: this repo decides build-configuration
# defaults from the published ranking, not from a plausible mechanism. Flip it when the
# `-g1` cell has a number.
#
# WHAT YOU GIVE UP, stated plainly: a core dump from a CI binary loses variable inspection,
# and `gdb` on one shows no locals. Frames, function names and file:line — everything a
# sanitizer report, a gtest failure or a stack trace prints — are unaffected. If a job ever
# needs full DWARF, leave this OFF for that job rather than for the tree.
#
# Ordering is load-bearing and verified: CMake emits `CMAKE_CXX_FLAGS_<CONFIG>` before a
# directory's COMPILE_OPTIONS, so `Debug`'s `-g` lands first and this `-g1` lands second.
# Confirmed by generating a project and reading the rule (`-g -g1 -c`) and by compiling both
# orders — clang takes the LAST `-g*` on the line, so the downgrade holds. That is also why
# this file MUST be included above the add_subdirectory() calls, like its two siblings.

option(OLO_MINIMAL_DEBUG_INFO
    "Compile with -g1 (line tables, no variable/type DWARF) so the linker loads less debug info. OFF by default: price it in build-memory.yml's -g1 cell first — see cmake/MinimalDebugInfo.cmake."
    OFF)

if(NOT OLO_MINIMAL_DEBUG_INFO)
    return()
endif()

# DWARF only. The MSVC frontend (cl.exe and clang-cl alike) emits CodeView, where `-g1`
# has no meaning — same rule, and same reason, as cmake/SplitDwarf.cmake states: a request
# for it there is a configuration mistake worth naming rather than a no-op to discover later.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$")
    message(WARNING
        "OLO_MINIMAL_DEBUG_INFO is ON but this toolchain does not produce DWARF "
        "('${CMAKE_CXX_COMPILER_ID}', frontend '${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}') — "
        "ignoring it. Debug-info level is a DWARF concern here; the MSVC frontend emits "
        "CodeView and already keeps it out of the objects via a separate .pdb.")
    return()
endif()

# The combination is strictly worse than this option alone: it buys ~1% more reduction and
# inherits split DWARF's lost inlined frames. Refuse to configure it silently.
if(OLO_SPLIT_DWARF)
    message(WARNING
        "OLO_MINIMAL_DEBUG_INFO and OLO_SPLIT_DWARF are both ON. They are alternatives, not "
        "a stack: -g1 alone removes 93% of the linker-visible debug info and KEEPS inlined "
        "frames, while the pair removes 94% and LOSES them (cmake/MinimalDebugInfo.cmake). "
        "Prefer -g1 alone unless you are deliberately measuring the combination.")
endif()

# Per language, and only where the compiler takes the flag — the same rule
# cmake/SplitDwarf.cmake and cmake/ProcStatReport.cmake follow, and for the same reason:
# C and C++ are separate cache entries a toolchain file can point at different compilers,
# so validating one and flagging both hands an unsupported option to the other.
set(_olo_mdi_langs "")
foreach(_olo_mdi_lang C CXX)
    if(CMAKE_${_olo_mdi_lang}_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$"
       AND NOT CMAKE_${_olo_mdi_lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options("$<$<COMPILE_LANGUAGE:${_olo_mdi_lang}>:-g1>")
        list(APPEND _olo_mdi_langs ${_olo_mdi_lang})
    endif()
endforeach()
unset(_olo_mdi_lang)

if(NOT _olo_mdi_langs)
    message(WARNING "OLO_MINIMAL_DEBUG_INFO is ON but no C or C++ compiler accepts -g1.")
    return()
endif()

list(JOIN _olo_mdi_langs "/" _olo_mdi_note)
message(STATUS
    "Minimal debug info: ON for ${_olo_mdi_note} — -g1 keeps line tables and function "
    "names (so sanitizer reports, including inlined frames, still symbolize) and drops "
    "variable/type DWARF. A debugger on these binaries will show no locals.")
unset(_olo_mdi_note)
