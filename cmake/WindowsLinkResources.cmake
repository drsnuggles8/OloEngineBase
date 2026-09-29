# Windows/CodeView resource controls (issue #1386). Keep Linux/DWARF policy in
# its existing modules. MSVC link.exe has neither /threads nor /DEBUG:GHASH.
if(NOT MSVC)
    return()
endif()
get_filename_component(_olo_linker_name "${CMAKE_LINKER}" NAME_WE)
set(OLO_USING_LLD_LINK FALSE)
if(_olo_linker_name STREQUAL "lld-link")
    set(OLO_USING_LLD_LINK TRUE)
endif()
unset(_olo_linker_name)

# Half the logical CPUs remain available for compiler jobs and the desktop.
# Divide the remainder by MACHINE-WIDE permits, not the per-tree Ninja pool.
# 0 is the explicit unbounded control used for baseline measurements.
set(OLO_LINK_THREADS "auto" CACHE STRING "lld-link threads: auto, 0 (linker default), or a positive integer")
if(NOT OLO_LINK_THREADS STREQUAL "auto" AND NOT OLO_LINK_THREADS MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "OLO_LINK_THREADS must be auto or a nonnegative integer")
endif()
if(OLO_USING_LLD_LINK)
    if(OLO_LINK_THREADS STREQUAL "auto")
        cmake_host_system_information(RESULT _olo_cpus QUERY NUMBER_OF_LOGICAL_CORES)
        if(NOT OLO_LINK_SEMAPHORE_SLOTS MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "Automatic linker threads require a positive OLO_LINK_SEMAPHORE_SLOTS")
        endif()
        math(EXPR _olo_threads "${_olo_cpus} / (2 * ${OLO_LINK_SEMAPHORE_SLOTS})")
        if(_olo_threads LESS 1)
            set(_olo_threads 1)
        endif()
    else()
        set(_olo_threads "${OLO_LINK_THREADS}")
    endif()
    if(_olo_threads GREATER 0)
        add_link_options("/threads:${_olo_threads}")
        message(STATUS "lld-link: ${_olo_threads} worker threads per linker")
    endif()

    # CMake's Windows defaults carry this independently of target options.
    # Normal variables shadow the cache, so changing linkers later does not
    # permanently strip MSVC's useful incremental setting from that cache.
    foreach(_olo_kind EXE SHARED MODULE)
        string(REGEX REPLACE "(^|[ \t])/[Ii][Nn][Cc][Rr][Ee][Mm][Ee][Nn][Tt][Aa][Ll]([ \t]|$)" "\\1\\2"
               CMAKE_${_olo_kind}_LINKER_FLAGS_DEBUG "${CMAKE_${_olo_kind}_LINKER_FLAGS_DEBUG}")
    endforeach()
    unset(_olo_kind)
    unset(_olo_threads)
    unset(_olo_cpus)
elseif(NOT OLO_LINK_THREADS STREQUAL "auto" AND NOT OLO_LINK_THREADS STREQUAL "0")
    message(FATAL_ERROR "OLO_LINK_THREADS requires lld-link; link.exe does not support /threads")
endif()
