# Make OpenUSD's work library include oneTBB's version header itself (invoked as an ExternalProject
# PATCH_COMMAND with -DUSD_SRC=<source dir>, #1575).
#
# pxr/base/work/workTBB picks between the oneTBB and the legacy TBB API with
#   #if TBB_INTERFACE_VERSION_MAJOR >= 12
# but never includes the header that defines the macro. oneTBB 2022.2's blocked_range.h included
# version.h, so the macro arrived transitively. vcpkg's oneTBB 2023.1 dropped that include: the
# macro is undefined, reads as 0, and USD compiles its legacy path, which fails with
#   dispatcher_impl.h(73): error C2516: 'tbb::v1::task': is not a valid base class
# The include goes ahead of the first oneTBB header (<tbb/blocked_range.h>, which under 2023.1 no
# longer brings the macro in) in the two files that test the macro with nothing else defining it:
# dispatcher_impl.h and threadLimits_impl.cpp. The others are covered without a patch:
# detachedTask_impl.h and dispatcher_impl.cpp include dispatcher_impl.h; taskGraph_impl.h falls back
# to <tbb/version.h> when <tbb/tbb_stddef.h> (legacy TBB only) is absent, and taskGraph_impl.cpp
# includes it. Idempotent: a patched file is skipped.

# A -P script starts with every policy unset, and CMake 3.x then applies OLD behaviour; pin them.
cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED USD_SRC OR NOT IS_DIRECTORY "${USD_SRC}")
    message(FATAL_ERROR "patch-usd-tbb-version: USD_SRC ('${USD_SRC}') is not the OpenUSD source directory")
endif()

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
