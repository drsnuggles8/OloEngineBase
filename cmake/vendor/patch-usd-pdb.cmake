# Patch OpenUSD's static-monolithic PDB-install bug (invoked as an ExternalProject PATCH_COMMAND
# with -DUSD_SRC=<source dir>). cmake/macros/Public.cmake unconditionally does
#   install(FILES $<TARGET_PDB_FILE:usd_m> ...)
# on WIN32, but in the static-monolithic build usd_m is a STATIC archive with no linker-created
# PDB, so the generator expression errors at generate time:
#   "TARGET_PDB_FILE is allowed only for targets with linker created artifacts".
# Guard it with BUILD_SHARED_LIBS so the PDB install only applies to the shared (usd_ms) build.
# Idempotent: re-running finds the already-guarded form and no-ops.

set(_pub "${USD_SRC}/cmake/macros/Public.cmake")
if(NOT EXISTS "${_pub}")
    message(FATAL_ERROR "patch-usd-pdb: Public.cmake not found at ${_pub}")
endif()

file(READ "${_pub}" _content)

set(_needle "            if(WIN32)\n                install(\n                    FILES $<TARGET_PDB_FILE:usd_m>")
set(_replacement "            if(WIN32 AND BUILD_SHARED_LIBS)\n                install(\n                    FILES $<TARGET_PDB_FILE:usd_m>")

string(FIND "${_content}" "${_replacement}" _already)
if(NOT _already EQUAL -1)
    message(STATUS "patch-usd-pdb: already patched")
    return()
endif()

string(REPLACE "${_needle}" "${_replacement}" _patched "${_content}")
string(FIND "${_patched}" "${_replacement}" _ok)
if(_ok EQUAL -1)
    message(FATAL_ERROR
        "patch-usd-pdb: could not find the usd_m PDB-install block to patch in ${_pub}. "
        "The pinned OpenUSD version likely changed this macro — update the needle in "
        "cmake/vendor/patch-usd-pdb.cmake.")
endif()

file(WRITE "${_pub}" "${_patched}")
message(STATUS "patch-usd-pdb: applied static-lib PDB-install guard")
