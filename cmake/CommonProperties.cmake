# CommonProperties.cmake
# Common CMake property settings for OloEngine and related projects
# This file centralizes common settings to avoid duplication across projects

# Prevent multiple inclusion
if(DEFINED OLO_COMMON_PROPERTIES_INCLUDED)
    return()
endif()
set(OLO_COMMON_PROPERTIES_INCLUDED TRUE)

include(CMakePrintHelpers)
include(CheckIPOSupported)

# Configure output directories for a target based on project name
function(olo_set_output_directories target_name)
    set_target_properties(${target_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG     ${CMAKE_SOURCE_DIR}/bin/Debug/${target_name}
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE   ${CMAKE_SOURCE_DIR}/bin/Release/${target_name}
        ARCHIVE_OUTPUT_DIRECTORY_DIST      ${CMAKE_SOURCE_DIR}/bin/Dist/${target_name}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG     ${CMAKE_SOURCE_DIR}/bin/Debug/${target_name}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE   ${CMAKE_SOURCE_DIR}/bin/Release/${target_name}
        LIBRARY_OUTPUT_DIRECTORY_DIST      ${CMAKE_SOURCE_DIR}/bin/Dist/${target_name}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG     ${CMAKE_SOURCE_DIR}/bin/Debug/${target_name}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE   ${CMAKE_SOURCE_DIR}/bin/Release/${target_name}
        RUNTIME_OUTPUT_DIRECTORY_DIST      ${CMAKE_SOURCE_DIR}/bin/Dist/${target_name}
    )
endfunction()

# Set VS debugger working directory
function(olo_set_debugger_directory target_name)
    if(MSVC)
        set_target_properties(${target_name} PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
endfunction()

# Apply Link Time Optimization if supported and enabled (Release/Dist only)
function(olo_enable_lto target_name)
    if(NOT DEFINED OLO_ENABLE_LTO)
        set(OLO_ENABLE_LTO ON)
    endif()
    
    check_ipo_supported(RESULT LTO_SUPPORT OUTPUT output)
    if(OLO_ENABLE_LTO AND LTO_SUPPORT)
        message(STATUS "-- Enabled Link-Time Optimization (LTO) for ${target_name} (Release/Dist only)")
        # Only enable LTO for Release and Dist — it dramatically slows Debug builds
        set_target_properties(${target_name} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            INTERPROCEDURAL_OPTIMIZATION_DIST TRUE
        )
    else()
        if(OLO_ENABLE_LTO AND NOT LTO_SUPPORT)
            message(WARNING "LTO requested but not supported: ${output}")
        endif()
    endif()
endfunction()

# Enable precompiled headers for a target
function(olo_enable_pch target_name pch_header)
    if(NOT DEFINED OLO_ENABLE_PCH)
        set(OLO_ENABLE_PCH ON)
    endif()
    
    if(OLO_ENABLE_PCH)
        target_precompile_headers(${target_name} PUBLIC ${pch_header})
    endif()
endfunction()

# Configure common compiler options
function(olo_set_compiler_options target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE 
            /W4
            /MP       # Multi-processor compilation (parallel file compilation)
            /utf-8    # Enable UTF-8 encoding for source files
            /Zc:preprocessor  # Enable conforming preprocessor (required for __VA_OPT__)
            /Zc:inline        # Remove unreferenced COMDAT functions (reduces linker work)
            /bigobj           # Increase COFF section limit for large translation units
        )
        # Use multi-threaded DLL runtime library
        set_target_properties(${target_name} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(${target_name} PRIVATE 
            -Wall 
            -Wextra 
            -Wundef 
            -Wno-cast-function-type 
            -pedantic 
            -Wno-long-long 
            -Wshadow 
            -Werror 
            -Wno-error=deprecated-declarations
        )
    endif()
endfunction()

# Set common compiler definitions for OloEngine-based projects
function(olo_set_common_definitions target_name)
    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:OLO_DEBUG>
        $<$<CONFIG:Release>:OLO_RELEASE>
        $<$<CONFIG:Release>:TRACY_ENABLE>
        $<$<CONFIG:Release>:TRACY_ON_DEMAND>
        $<$<CONFIG:Dist>:OLO_DIST>
    )
endfunction()

# Configure link options for all builds
function(olo_set_link_options target_name)
    if(MSVC)
        target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/INCREMENTAL>  # Incremental linking for fast Debug iteration
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/OPT:REF> # Remove unreferenced functions and data
            $<$<CONFIG:Release>:/OPT:ICF> # Identical COMDAT folding
            # Suppress LNK4099 warnings for missing PDB files from third-party libraries
            # This is common when linking against precompiled libraries (like Mono) that
            # don't include debug information, and there's no way for us to fix it
            /IGNORE:4099
        )
    endif()
endfunction()

# Complete setup for an application target (combines all the above)
function(olo_configure_app target_name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_PCH" "PCH_HEADER" "")
      olo_set_output_directories(${target_name})
    olo_set_debugger_directory(${target_name})
    olo_enable_lto(${target_name})
    olo_set_compiler_options(${target_name})
    olo_set_common_definitions(${target_name})
    olo_set_link_options(${target_name})
    
    if(NOT ARG_NO_PCH AND DEFINED ARG_PCH_HEADER)
        olo_enable_pch(${target_name} ${ARG_PCH_HEADER})
    endif()
endfunction()

# Setup common include directories for OloEngine projects
function(olo_set_common_include_directories target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/OloEngine/src
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/filewatch-src
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/imgui-src
        ${imguizmo_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/sol2-src/include
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
    )
endfunction()

# Configure C# project properties
function(olo_configure_csharp_project target_name output_dir)
    # Set output directories for C# assemblies
    set_target_properties(${target_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG      ${output_dir}
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE    ${output_dir}
        ARCHIVE_OUTPUT_DIRECTORY_DIST       ${output_dir}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG      ${output_dir}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE    ${output_dir}
        LIBRARY_OUTPUT_DIRECTORY_DIST       ${output_dir}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG      ${output_dir}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE    ${output_dir}
        RUNTIME_OUTPUT_DIRECTORY_DIST       ${output_dir}
    )
    
    # Add common configuration definitions
    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:OLO_DEBUG>
        $<$<CONFIG:Release>:OLO_RELEASE>
        $<$<CONFIG:Dist>:OLO_DIST>
    )
endfunction()
