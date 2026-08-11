if(NOT SET_UP_CONFIGURATIONS_DONE)
    set(SET_UP_CONFIGURATIONS_DONE TRUE)

    # No reason to set CMAKE_CONFIGURATION_TYPES if it's not a multiconfig generator
    # Also no reason to mess with CMAKE_BUILD_TYPE if it's a multiconfig generator.
    get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(isMultiConfig)
        set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "" FORCE) 
    else()
        set(allowableBuildTypes Debug Release Dist)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${allowableBuildTypes}")
        if(NOT CMAKE_BUILD_TYPE)
            message("Defaulting to Debug build.")
            set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
        elseif(NOT CMAKE_BUILD_TYPE IN_LIST allowableBuildTypes)
            message(FATAL_ERROR "Invalid build type: ${CMAKE_BUILD_TYPE}")
        endif()
    endif()
    
    # `Dist` is a project-local configuration that no IMPORTED target knows about.
    # Every vcpkg package installs exactly two configurations (Debug + Release), so
    # without an explicit mapping CMake falls back to the FIRST entry of the imported
    # target's IMPORTED_CONFIGURATIONS list — in practice Debug — and a Dist build
    # would silently link debug third-party libraries against release engine code.
    # That is a CRT/iterator-debug-level mismatch, i.e. a runtime heap fault rather
    # than a link error, so nothing in the build output would flag it (issue #773).
    # The trailing empty entry lets a config-less IMPORTED_LOCATION satisfy the
    # lookup as a last resort. Must be set before the first consuming target is
    # created — this file is included well ahead of the add_subdirectory() calls.
    set(CMAKE_MAP_IMPORTED_CONFIG_DIST "Release;")

    # Set up Dist configuration
    set(CMAKE_C_FLAGS_DIST "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "")
    set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "")
    set(CMAKE_EXE_LINKER_FLAGS_DIST "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "")
    set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "")
    set(CMAKE_STATIC_LINKER_FLAGS_DIST "${CMAKE_STATIC_LINKER_FLAGS_RELEASE}" CACHE STRING "")
    set(CMAKE_MODULE_LINKER_FLAGS_DIST "${CMAKE_MODULE_LINKER_FLAGS_RELEASE}" CACHE STRING "")

    if(MSVC)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,Release>:ProgramDatabase>")
    endif()
endif()
