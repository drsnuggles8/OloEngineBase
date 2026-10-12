# Make OpenUSD's work library include oneTBB's version header itself (invoked as an ExternalProject
# PATCH_COMMAND with -DUSD_SRC=<source dir>, #1575).
#
# pxr/base/work/workTBB picks between the oneTBB and the legacy TBB API with
#   #if TBB_INTERFACE_VERSION_MAJOR >= 12
# but never includes the header that defines the macro. oneTBB 2022.2's blocked_range.h included
# version.h, so the macro arrived transitively. vcpkg's oneTBB 2023.1 dropped that include: the
# macro is undefined, reads as 0, and USD compiles its legacy path, which fails with
#   dispatcher_impl.h(73): error C2516: 'tbb::v1::task': is not a valid base class
# Insert the include ahead of the first oneTBB header in the two files that test the macro before
# anything else defines it (detachedTask_impl.h and dispatcher_impl.cpp get it through
# dispatcher_impl.h; taskGraph_impl.h already includes it). Idempotent: a patched file is skipped.

set(_marker "#include <tbb/version.h> // OloEngine #1575")
foreach(_rel "pxr/base/work/workTBB/dispatcher_impl.h" "pxr/base/work/workTBB/threadLimits_impl.cpp")
    set(_file "${USD_SRC}/${_rel}")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "patch-usd-tbb-version: ${_file} not found. The pinned OpenUSD layout "
                            "changed; update cmake/vendor/patch-usd-tbb-version.cmake.")
    endif()
    file(READ "${_file}" _content)
    string(FIND "${_content}" "${_marker}" _already)
    if(NOT _already EQUAL -1)
        message(STATUS "patch-usd-tbb-version: ${_rel} already patched")
        continue()
    endif()
    set(_needle "#include <tbb/blocked_range.h>")
    string(FIND "${_content}" "${_needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "patch-usd-tbb-version: no '${_needle}' in ${_file} to insert ahead of. "
                            "The pinned OpenUSD version changed this file; update the needle.")
    endif()
    string(SUBSTRING "${_content}" 0 ${_at} _head)
    string(SUBSTRING "${_content}" ${_at} -1 _tail)
    file(WRITE "${_file}" "${_head}${_marker}\n${_tail}")
    message(STATUS "patch-usd-tbb-version: ${_rel} now includes tbb/version.h")
endforeach()
