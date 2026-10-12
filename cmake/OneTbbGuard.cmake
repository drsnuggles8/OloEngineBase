# One oneTBB per linked binary (#1575).
#
# Fails the build if any executable or shared library links more than one oneTBB archive
# (tbb12.lib / tbb12_debug.lib / libtbb12.a ...). Two static oneTBB runtimes in one process is an
# ODR hazard nothing else catches: the linker takes every TBB symbol from whichever archive comes
# first, /FORCE:MULTIPLE (on for the assimp/MaterialX pugixml clash) hides any duplicate, and code
# compiled against one version's headers runs on the other's internals. It happened: OpenUSD built
# its own oneTBB 2022.2 beside vcpkg's 2023.1, and OpenVDB ran on USD's.
#
# The check reads the link line CMake itself generated, per configuration, from the File API
# codemodel (cmake-file-api(7)), so it sees an archive whatever brought it in: an imported target,
# a raw path in a variable, a generator expression, a target defined in another directory. It runs
# as a custom command OloEngine depends on, before anything compiles, and again after every
# reconfigure (the codemodel is rewritten by every generate; olo_add_onetbb_guard explains how a
# fresh tree gets its first one). It cannot see a library named only
# by a `#pragma comment(lib)` inside an object file; those resolve through the link line's own
# library search, which this check already covers.
#
# Two modes in one file:
#   include(cmake/OneTbbGuard.cmake) + olo_add_onetbb_guard(<target>) — configure time, top level,
#       after every target exists.
#   cmake -DOLO_ONETBB_REPLY_DIR=... -DOLO_ONETBB_CONFIG=... -DOLO_ONETBB_TARGETS=... -P <this file>
#       — the check itself.

# A oneTBB core archive by file name: tbb12 / tbb12_debug, any platform prefix and suffix. The
# separate tbbmalloc / tbbmalloc_proxy libraries are a different runtime and are not counted.
set(_OLO_ONETBB_ARCHIVE_REGEX "(^|/)(lib)?tbb12(_debug)?\\.(lib|a|so|dylib)(\\.[0-9.]+)?$")

if(CMAKE_SCRIPT_MODE_FILE)
    foreach(_var OLO_ONETBB_REPLY_DIR OLO_ONETBB_CONFIG OLO_ONETBB_TARGETS)
        if(NOT DEFINED ${_var})
            message(FATAL_ERROR "OneTbbGuard: ${_var} is not set")
        endif()
    endforeach()
    string(REPLACE "," ";" OLO_ONETBB_TARGETS "${OLO_ONETBB_TARGETS}")
    file(GLOB _indices "${OLO_ONETBB_REPLY_DIR}/index-*.json")
    if(NOT _indices)
        message(FATAL_ERROR "OneTbbGuard: no File API reply under ${OLO_ONETBB_REPLY_DIR}. "
                            "Reconfigure the tree; the oneTBB check (#1575) cannot run without it.")
    endif()
    # Index files are named by generation time, so the lexically last is the newest.
    list(SORT _indices)
    list(GET _indices -1 _index)
    file(READ "${_index}" _json)
    string(JSON _codemodel_file ERROR_VARIABLE _err GET "${_json}" reply client-oloengine codemodel-v2 jsonFile)
    if(_err)
        message(FATAL_ERROR "OneTbbGuard: ${_index} has no codemodel-v2 reply for client-oloengine: ${_err}")
    endif()
    file(READ "${OLO_ONETBB_REPLY_DIR}/${_codemodel_file}" _json)
    string(JSON _config_count LENGTH "${_json}" configurations)
    set(_config_index -1)
    math(EXPR _last "${_config_count} - 1")
    foreach(_i RANGE 0 ${_last})
        string(JSON _name GET "${_json}" configurations ${_i} name)
        if(_name STREQUAL OLO_ONETBB_CONFIG)
            set(_config_index ${_i})
        endif()
    endforeach()
    if(_config_index EQUAL -1)
        message(FATAL_ERROR "OneTbbGuard: configuration '${OLO_ONETBB_CONFIG}' is not in ${_codemodel_file}")
    endif()

    # Target name -> its reply file, for this configuration.
    string(JSON _target_count LENGTH "${_json}" configurations ${_config_index} targets)
    math(EXPR _last "${_target_count} - 1")
    set(_checked 0)
    set(_failures "")
    foreach(_i RANGE 0 ${_last})
        string(JSON _name GET "${_json}" configurations ${_config_index} targets ${_i} name)
        if(NOT _name IN_LIST OLO_ONETBB_TARGETS)
            continue()
        endif()
        string(JSON _target_file GET "${_json}" configurations ${_config_index} targets ${_i} jsonFile)
        file(READ "${OLO_ONETBB_REPLY_DIR}/${_target_file}" _target)
        string(JSON _fragments ERROR_VARIABLE _err GET "${_target}" link commandFragments)
        if(_err)
            continue()  # nothing linked
        endif()
        math(EXPR _checked "${_checked} + 1")
        string(JSON _fragment_count LENGTH "${_fragments}")
        math(EXPR _last_fragment "${_fragment_count} - 1")
        set(_archives "")
        foreach(_f RANGE 0 ${_last_fragment})
            string(JSON _role GET "${_fragments}" ${_f} role)
            if(NOT _role STREQUAL "libraries")
                continue()
            endif()
            string(JSON _fragment GET "${_fragments}" ${_f} fragment)
            string(STRIP "${_fragment}" _fragment)
            string(REPLACE "\"" "" _fragment "${_fragment}")
            string(REPLACE "\\" "/" _fragment "${_fragment}")
            string(TOLOWER "${_fragment}" _fragment)
            if(_fragment MATCHES "${_OLO_ONETBB_ARCHIVE_REGEX}" OR _fragment MATCHES "^-ltbb12(_debug)?$")
                list(APPEND _archives "${_fragment}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _archives)
        list(LENGTH _archives _archive_count)
        if(_archive_count GREATER 1)
            list(JOIN _archives "\n      " _shown)
            string(APPEND _failures "  ${_name} (${OLO_ONETBB_CONFIG}) links ${_archive_count} oneTBB archives:\n      ${_shown}\n")
        endif()
    endforeach()
    if(_checked EQUAL 0)
        message(FATAL_ERROR "OneTbbGuard: none of the targets [${OLO_ONETBB_TARGETS}] is in the "
                            "'${OLO_ONETBB_CONFIG}' codemodel, so nothing was checked")
    endif()
    if(_failures)
        message(FATAL_ERROR "More than one oneTBB reaches a link (#1575):\n${_failures}"
            "Every binary must link exactly one oneTBB: vcpkg's TBB::tbb. Link that target instead of a "
            "second archive, and build any dependency that bundles its own oneTBB against vcpkg's "
            "(cmake/vendor/OpenUSD.cmake shows how).")
    endif()
    message(STATUS "OneTbbGuard: ${_checked} binaries checked for ${OLO_ONETBB_CONFIG}, each links at most one oneTBB")
    return()
endif()

# Every executable and shared/module library defined in `dir` and below.
function(_olo_onetbb_binaries dir out)
    set(_found "")
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            list(APPEND _found ${_t})
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _olo_onetbb_binaries("${_sub}" _sub_found)
        list(APPEND _found ${_sub_found})
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# Wires the check in front of `gate_target` (OloEngine: every binary that could link oneTBB links
# it, so the check runs before the first object compiles). Call at the end of the top-level
# CMakeLists, after every add_subdirectory().
function(olo_add_onetbb_guard gate_target)
    # The File API query. CMake reads queries BEFORE it configures, so a query first written here
    # is answered only by the next run: on a fresh tree this run's generate writes no reply. The
    # marker below exists only once a run started with the query in place. It is a configure
    # dependency, and a missing one makes the build system regenerate before it builds anything,
    # so a fresh tree's first build regenerates once and that run answers the query.
    set(_query "${CMAKE_BINARY_DIR}/.cmake/api/v1/query/client-oloengine/codemodel-v2")
    set(_answered "${CMAKE_BINARY_DIR}/olo_onetbb_guard.answered")
    if(EXISTS "${_query}")
        file(WRITE "${_answered}" "this run's generate answers the File API query\n")
    else()
        file(WRITE "${_query}" "")
        file(REMOVE "${_answered}")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_answered}")
    _olo_onetbb_binaries("${CMAKE_SOURCE_DIR}" _binaries)
    list(JOIN _binaries "," _binaries_arg)
    # Rewritten on every configure, so the check re-runs once after each regenerate and is a
    # no-op stamp check on every other build.
    string(TIMESTAMP _now "%Y-%m-%dT%H:%M:%S")
    set(_trigger "${CMAKE_BINARY_DIR}/olo_onetbb_guard.trigger")
    file(WRITE "${_trigger}" "${_now}\n${_binaries_arg}\n")
    set(_stamp "${CMAKE_BINARY_DIR}/olo_onetbb_guard_$<CONFIG>.stamp")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND}
                "-DOLO_ONETBB_REPLY_DIR=${CMAKE_BINARY_DIR}/.cmake/api/v1/reply"
                "-DOLO_ONETBB_CONFIG=$<CONFIG>"
                "-DOLO_ONETBB_TARGETS=${_binaries_arg}"
                -P "${CMAKE_SOURCE_DIR}/cmake/OneTbbGuard.cmake"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS "${_trigger}" "${CMAKE_SOURCE_DIR}/cmake/OneTbbGuard.cmake"
        COMMENT "Checking that every binary links one oneTBB (#1575)"
        VERBATIM)
    add_custom_target(olo_onetbb_guard DEPENDS "${_stamp}")
    add_dependencies(${gate_target} olo_onetbb_guard)
endfunction()
