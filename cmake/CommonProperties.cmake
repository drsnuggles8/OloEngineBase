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
        message(STATUS "Enabled Link-Time Optimization (LTO) for ${target_name} (Release/Dist only)")
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
            /wd4324   # 'X': structure padded due to alignment specifier — alignas() is intentional.
                      # Fires across Task/, Containers/, Audio/ for every cache-line-aligned struct
                      # we deliberately request; the padding is the point, not a defect.
            /MP       # Multi-processor compilation (parallel file compilation)
            /FS       # Force synchronous PDB writes (fixes PDB contention with /MP)
            /utf-8    # Enable UTF-8 encoding for source files
            /Zc:preprocessor  # Enable conforming preprocessor (required for __VA_OPT__)
            /Zc:inline        # Remove unreferenced COMDAT functions (reduces linker work)
            /bigobj           # Increase COFF section limit for large translation units
        )
        # Use multi-threaded DLL runtime library.
        # When ASan or fuzzing is on, force the release CRT (/MD) for ALL
        # configurations — MSVC ASan's runtime links against release CRT,
        # and mixing /MDd with release-only third-party static libs (Vulkan
        # SDK spirv-cross on CI without debug libs, etc.) causes
        # _ITERATOR_DEBUG_LEVEL mismatches. Fuzzing uses dynamic ASan +
        # builds libFuzzer from source so it can also use /MD (stock LLVM
        # ships only /MT-built clang_rt.fuzzer-x86_64.lib).
        # Sanitizers.cmake / Fuzzing.cmake also set CMAKE_MSVC_RUNTIME_LIBRARY
        # globally, but this per-target property would otherwise override it.
        if(OLO_ENABLE_ASAN OR OLO_ENABLE_FUZZING)
            set_target_properties(${target_name} PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        else()
            set_target_properties(${target_name} PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    else()
        target_compile_options(${target_name} PRIVATE 
            -Wall 
            -Wextra 
            -Wno-cast-function-type 
            -Wno-error=deprecated-declarations
            -Wno-error=delete-incomplete # Forward-declared types in Ref<T> smart pointers
        )
    endif()
endfunction()

# Set common compiler definitions for OloEngine-based projects
function(olo_set_common_definitions target_name)
    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:OLO_DEBUG>
        $<$<CONFIG:Release>:OLO_RELEASE>
        $<$<CONFIG:Dist>:OLO_DIST>
    )
    # Tracy: only define the macro when the CMake option is on. This lets the
    # TSan preset turn Tracy off (-DTRACY_ENABLE=OFF) — Tracy's rpmalloc has a
    # known static-init-order race that TSan flags, halting test discovery.
    if(TRACY_ENABLE)
        target_compile_definitions(${target_name} PRIVATE
            $<$<CONFIG:Release>:TRACY_ENABLE>
            $<$<CONFIG:Release>:TRACY_ON_DEMAND>
        )
    endif()
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
            $<$<CONFIG:Dist>:/INCREMENTAL:NO>
            $<$<CONFIG:Dist>:/OPT:REF>
            $<$<CONFIG:Dist>:/OPT:ICF>
            # Suppress LNK4099 warnings for missing PDB files from third-party libraries
            # This is common when linking against precompiled libraries (like Mono) that
            # don't include debug information, and there's no way for us to fix it
            /IGNORE:4099
        )
    endif()
endfunction()

# Copy the FFmpeg runtime libs (Windows DLLs / Linux .so) next to an executable
# target's output so a build with OLO_VIDEO_FFMPEG=ON can load them at runtime.
# No-op when FFmpeg is off (OLO_FFMPEG_RUNTIME_DIR is only set by cmake/ffmpeg.cmake).
# Must be called from the directory that created the target (add_custom_command TARGET).
function(olo_copy_ffmpeg_runtime target_name)
    if(DEFINED OLO_FFMPEG_RUNTIME_DIR)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${OLO_FFMPEG_RUNTIME_DIR}" "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying FFmpeg runtime libs to ${target_name} output dir"
            VERBATIM)
        if(NOT WIN32)
            # Resolve the FFmpeg .so copied next to the executable at run time.
            set_target_properties(${target_name} PROPERTIES
                BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
        endif()
    endif()
endfunction()

# Stage steam_api64.dll next to an executable (#644). No-op unless OLO_WITH_STEAM is on with the
# real SDK — a stub-SDK build has no DLL to stage (the symbols are compiled in), and an OFF build
# has no Steam at all.
#
# The DLL is copied from STEAMWORKS_SDK_ROOT at build time rather than committed. It IS the one
# redistributable piece of the SDK — that is what redistributable_bin/ means, Valve intends it to
# ship with your game — but copying sidesteps having to reason about what "redistribution" means
# in a PUBLIC repo, and costs nothing.
#
# Same directory-scope rule as olo_copy_ffmpeg_runtime above: add_custom_command(TARGET ...) must
# run in the directory that created the target, so call this next to each add_executable.
#
# The DEFINED check used to be part of the `if` condition, so an unset variable meant this
# function quietly staged nothing — issue #828: OLO_STEAM_RUNTIME_DLL was resolved BELOW
# add_subdirectory(tests), so on a fresh configure OloEngine-Tests silently shipped without the
# DLL and died hours later at gtest discovery with 0xC0000135, an error naming nothing about
# Steam. It is now a FATAL_ERROR: if Steam is on with the real SDK, there is no legitimate way to
# reach here without the path, and a configure-time sentence naming Steam beats an opaque runtime
# status code. Safe in CI, which is Steam-OFF (or stub) and never enters this branch at all.
function(olo_copy_steam_runtime target_name)
    if(OLO_WITH_STEAM AND NOT OLO_WITH_STEAM_STUB_SDK)
        if(NOT OLO_STEAM_RUNTIME_DLL)
            message(FATAL_ERROR
                "olo_copy_steam_runtime(${target_name}): OLO_WITH_STEAM is ON with the real SDK, "
                "but OLO_STEAM_RUNTIME_DLL is not set, so steam_api64.dll would not be staged "
                "next to ${target_name} and the executable would fail to start with 0xC0000135 "
                "(STATUS_DLL_NOT_FOUND). This is an ordering bug: cmake/Steamworks.cmake resolves "
                "that variable and MUST be included from the root CMakeLists before the "
                "add_subdirectory() that created ${target_name}. See issue #828.")
        endif()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OLO_STEAM_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Staging steam_api64.dll next to ${target_name}"
            VERBATIM)
    endif()
endfunction()

# Complete setup for an application target (combines all the above).
# Pass PCH_HEADER <path/to/pch.h> to opt-in to PCH; omit it to skip PCH entirely.
function(olo_configure_app target_name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PCH_HEADER" "")
    olo_set_output_directories(${target_name})
    olo_set_debugger_directory(${target_name})
    olo_enable_lto(${target_name})
    olo_set_compiler_options(${target_name})
    olo_set_common_definitions(${target_name})
    olo_set_link_options(${target_name})
    # NOTE: olo_copy_ffmpeg_runtime() is NOT called here — add_custom_command(TARGET ...)
    # must run in the directory that created the target, and olo_configure_app is invoked
    # from each app's parent dir. Call it explicitly next to each add_executable instead.

    if(DEFINED ARG_PCH_HEADER)
        olo_enable_pch(${target_name} ${ARG_PCH_HEADER})
    endif()
endfunction()

# Setup common include directories for OloEngine projects
function(olo_set_common_include_directories target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/OloEngine/src
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/filewatch-src
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/imgui-src
        ${imguizmo_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/OloEngine/vendor/sol2-src/include
    )
endfunction()

# Bind FILES to a JOB_POOL_COMPILE-bound SOURCES file set on target_name (CMake 4.4+, Ninja
# only — issue #822). No-op unless OLO_HEAVY_COMPILE_POOL_AVAILABLE (set once in the root
# CMakeLists.txt, alongside the olo_heavy pool itself and its OLO_HEAVY_COMPILE_JOBS validation).
#
# Each FILE must already be REMOVED from target_name's plain source list by the caller — a file
# added via a FILE_SET SOURCES set is a distinct code path from the ordinary SOURCES target
# property (CMake's own docs on prop_tgt:SOURCES: it "does not include File Sets"), so leaving
# the same path in both schedules two build edges for one output file.
#
# FILES may be relative to base_dir OR already absolute — resolved explicitly below rather than
# left to target_sources()'s own relative-path handling, which resolves against the CALLER's
# CMAKE_CURRENT_SOURCE_DIR (this function's call site), not base_dir. That mismatch is exactly how
# this function first failed: OloEngine/tests/CMakeLists.txt calling with base_dir pointing at
# OloEditor/src still had CMake look for the file under OloEngine/tests/, a hard configure error
# ("must be in one of the file set's base directories") rather than a silent misconfiguration.
#
# TYPE SOURCE (not SOURCES) in set_property(FILE_SET ...)'s own example is a documented-but-wrong
# CMake 4.4 doc snippet — verified against a throwaway project: it errors "set_property required
# TARGET option is missing". The working form is set_property(FILE_SET <name> TARGET <target> ...).
function(olo_bind_heavy_compile_pool target_name base_dir)
    if(NOT OLO_HEAVY_COMPILE_POOL_AVAILABLE)
        return()
    endif()
    set(_olo_heavy_files "")
    foreach(_olo_heavy_file ${ARGN})
        if(IS_ABSOLUTE "${_olo_heavy_file}")
            list(APPEND _olo_heavy_files "${_olo_heavy_file}")
        else()
            list(APPEND _olo_heavy_files "${base_dir}/${_olo_heavy_file}")
        endif()
    endforeach()
    target_sources(${target_name} PRIVATE
        FILE_SET olo_heavy_compile TYPE SOURCES
        BASE_DIRS ${base_dir}
        FILES ${_olo_heavy_files}
    )
    set_property(FILE_SET olo_heavy_compile TARGET ${target_name} PROPERTY JOB_POOL_COMPILE olo_heavy)
endfunction()

# Exclude specific translation units from whole-program optimization / LTO (issue #762).
#
# olo_enable_lto() turns INTERPROCEDURAL_OPTIMIZATION on for Release and Dist, which makes MSVC
# compile every TU with /GL: the object then holds the compiler's intermediate representation
# instead of machine code, and codegen is deferred to the /LTCG link. For ordinary engine code
# that costs a few extra MB. For a TU that is one enormous wall of template instantiation it is
# catastrophic — OloEngine's Sol2 binding glue produced a 348 MiB object this way. A single
# OloEngine.lib would have had to hold 4.98 GB of members, making a ~5.10 GB archive: 119% of the
# COFF format's hard 4 GiB ceiling, and every Dist link failed with LNK1248 (see
# cmake/CheckArchiveSize.cmake).
#
# Excluding TUs is NOT what fixed that, and the arithmetic is worth keeping in view: the six
# heaviest objects come to only ~14% of the mass, which does not close a 19% overshoot, let alone
# leave headroom — and one of those six (Scene.cpp) is hot, so it is not a candidate here at all.
# The engine is split across three archives (OloEngine/src/CMakeLists.txt); this function is the
# cheap extra margin on top, worth a measured ~449 MiB across the five TUs it is applied to.
#
# The trade is only worth making where whole-program optimization has nothing to optimize: these
# are one-shot registration and (de)serialization TUs that run at startup or at scene load, never
# in a per-frame loop. Do NOT add a hot TU here to buy headroom — split the archive instead.
#
# Directory-scoped, like every source-file property: call it from the CMakeLists.txt that adds
# the sources to the target. FILES are relative to that directory (or absolute).
function(olo_exclude_from_whole_program_optimization)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # cl.exe takes the LAST of /GL and /GL-, and source-level COMPILE_OPTIONS land after the
        # target's flags on the command line (MSBuild appends them via AdditionalOptions), so
        # this reliably wins over the /GL that INTERPROCEDURAL_OPTIMIZATION adds.
        set(_olo_no_wpo "/GL-")
    else()
        # clang-cl, clang and GCC all spell it the same way, and all three ignore it harmlessly
        # in a build that never enabled LTO in the first place (Debug, or -DOLO_ENABLE_LTO=OFF).
        set(_olo_no_wpo "-fno-lto")
    endif()

    foreach(_olo_wpo_file ${ARGN})
        if(IS_ABSOLUTE "${_olo_wpo_file}")
            set(_olo_wpo_path "${_olo_wpo_file}")
        else()
            set(_olo_wpo_path "${CMAKE_CURRENT_SOURCE_DIR}/${_olo_wpo_file}")
        endif()
        if(NOT EXISTS "${_olo_wpo_path}")
            # A renamed or deleted TU must not silently stop being excluded — that would put the
            # archive back on course for the 4 GiB ceiling with nothing in the log to say so.
            message(FATAL_ERROR
                "olo_exclude_from_whole_program_optimization: no such source file "
                "'${_olo_wpo_path}'. Update the list in the caller, or drop the entry.")
        endif()
        # Absolute paths deliberately: a file moved into a FILE_SET (olo_bind_heavy_compile_pool)
        # is stored absolute, and matching the property key to it keeps the exclusion attached
        # under every generator rather than only the ones without the heavy-compile pool.
        set_source_files_properties("${_olo_wpo_path}" PROPERTIES COMPILE_OPTIONS "${_olo_no_wpo}")
    endforeach()
endfunction()

# Report how close a static archive is to the 4 GiB COFF ceiling after every build, and fail
# before lib.exe would (issue #762). No-op off MSVC — the limit is a property of the .lib format,
# and ELF/Mach-O archives have no comparable ceiling within reach.
#
# Must be called from the directory that created the target: add_custom_command(TARGET ...).
function(olo_check_archive_size target_name)
    if(NOT MSVC)
        return()
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DOLO_ARCHIVE=$<TARGET_FILE:${target_name}>
                -DOLO_ARCHIVE_WARN_BYTES=${OLO_ARCHIVE_WARN_BYTES}
                -DOLO_ARCHIVE_FAIL_BYTES=${OLO_ARCHIVE_FAIL_BYTES}
                -P ${CMAKE_SOURCE_DIR}/cmake/CheckArchiveSize.cmake
        COMMENT "Checking ${target_name} archive size against the 4 GiB COFF limit"
        VERBATIM)
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
