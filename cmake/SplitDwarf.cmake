# SplitDwarf.cmake
# Split debug info out of the objects so the LINKER stops loading it — issue #1313.
#
# WHY THIS IS A CANDIDATE AT ALL. #1305 measured the build's memory ceiling and it is
# not a compile: one `ld.lld` link of OloEngine-Tests peaks at 11.17 GiB under
# Debug + ASan and 10.03 GiB with no sanitizer, against a heaviest compile of 8.09 /
# 4.65 GiB. The evidence that the link is DEBUG-INFO bound rather than instrumentation
# bound is the ratio between those two cells: ASan adds +74% to the heaviest compile but
# only +11% to the link, so whatever dominates those 10-11 GiB is present without any
# sanitizer. `-gsplit-dwarf` puts `.debug_info` in a side-car `.dwo` the linker never
# reads, which is the most direct attack on that number available.
#
# DEFAULT OFF, AND THAT IS A MEASURED POSITION, NOT CAUTION.
#
# This repo maps `DW_AT_comp_dir` to `/olo` via `-ffile-prefix-map` whenever the compiler
# cache is on — which is every CI job — so that objects are path-independent and the two
# olo-ci runner slots can share one ccache (see cmake/CompilerCache.cmake). Split DWARF
# resolves its side-car through exactly that field: the skeleton records
# `DW_AT_comp_dir=/olo` plus a bare `DW_AT_dwo_name`, so a symbolizer looks for
# `/olo/<name>.dwo`, which does not exist.
#
# Measured with llvm-symbolizer, clang 23.1.0, cross-compiling to x86_64-linux, one
# control per variable:
#
#     -gsplit-dwarf alone, .dwo present          inline frames RESOLVE
#     -ffile-prefix-map=/olo alone               inline frames RESOLVE
#     both together, .dwo present                inline frames LOST
#
# So it is the COMBINATION that breaks, not either half. What survives in every arm is
# the function name (from `.symtab`) and file:line (from `.debug_line`, which stays in the
# object) — so an ASan report keeps its frames and their locations, and loses the inlined
# ones. Packaging the side-cars into a `.dwp` did not restore it in that probe either, but
# that probe symbolized a `.o` rather than an executable, where the lookup differs — treat
# the `.dwp` route as UNPROVEN rather than ruled out.
#
# Mitigating, and the reason this is a small loss rather than a blocker: every sanitizer
# job is `CMAKE_BUILD_TYPE=Debug`, where there is very little inlining to lose.
#
# `--gdb-index` IS DELIBERATELY NOT PAIRED WITH THIS, though the issue's table pairs them.
# That pairing is the standard *debugging* recipe: it builds a `.gdb_index` section so GDB
# starts quickly. For a *memory* goal it works against us — lld constructs that index by
# reading the debug info it would otherwise skip, which is the very work this option exists
# to avoid. If it is ever wanted, price it in the same measurement cell rather than
# assuming it is free.
#
# HOW TO PRICE IT: `.github/workflows/build-memory.yml` carries a
# `Debug + ASan + split DWARF` cell on its scheduled and on-demand runs. Compare its
# reported link peak against the plain `debug-asan` cell from the same run; both are
# published as artifacts with their full build configuration attached.

option(OLO_SPLIT_DWARF
    "Emit debug info into side-car .dwo files so the linker never loads it (issue #1313). OFF by default: it costs inlined frames in sanitizer stacks while this repo maps DW_AT_comp_dir to /olo — see the measurement in cmake/SplitDwarf.cmake."
    OFF)

if(NOT OLO_SPLIT_DWARF)
    return()
endif()

# DWARF only. The MSVC frontend (cl.exe and clang-cl alike) emits CodeView, where this
# flag has no meaning at all — a request for it there is a configuration mistake worth
# naming rather than a no-op to discover later.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$")
    message(WARNING
        "OLO_SPLIT_DWARF is ON but this toolchain does not produce DWARF "
        "('${CMAKE_CXX_COMPILER_ID}', frontend '${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}') — "
        "ignoring it. Split debug info is a DWARF feature; the MSVC frontend emits CodeView "
        "and already keeps debug info out of the objects via a separate .pdb.")
    return()
endif()

# Per language, and only for the languages whose compiler actually takes the flag — the
# same rule cmake/ProcStatReport.cmake follows, and for the same reason: C and C++ are
# separate cache entries that a toolchain file can point at different compilers, so
# validating one and flagging both hands an unsupported option to the other.
set(_olo_split_dwarf_langs "")
foreach(_olo_sd_lang C CXX)
    if(CMAKE_${_olo_sd_lang}_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$"
       AND NOT CMAKE_${_olo_sd_lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options("$<$<COMPILE_LANGUAGE:${_olo_sd_lang}>:-gsplit-dwarf>")
        list(APPEND _olo_split_dwarf_langs ${_olo_sd_lang})
    endif()
endforeach()
unset(_olo_sd_lang)

if(NOT _olo_split_dwarf_langs)
    message(WARNING "OLO_SPLIT_DWARF is ON but no C or C++ compiler accepts -gsplit-dwarf.")
    return()
endif()

list(JOIN _olo_split_dwarf_langs "/" _olo_sd_note)
message(STATUS
    "Split DWARF: ON for ${_olo_sd_note} — debug info goes to .dwo side-cars the linker "
    "does not read. Inlined frames will NOT symbolize while -ffile-prefix-map is also in "
    "effect (issue #1313); function names and file:line still will.")
unset(_olo_sd_note)
unset(_olo_split_dwarf_langs)
