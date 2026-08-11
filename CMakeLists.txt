cmake_minimum_required(VERSION 3.25)

message(STATUS "CMake version: ${CMAKE_VERSION}")

# Performance optimizations for CMake itself
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
set(CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY ON)

# Set CMake policies to suppress warnings
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)  # Empty commands warning
endif()
if(POLICY CMP0194)
    cmake_policy(SET CMP0194 NEW)  # MSVC assembler policy
endif()

# --------------------------------------------------------------------------
# vcpkg manifest features (issue #773).
#
# MUST be set BEFORE project() — vcpkg's toolchain file runs the manifest
# install during the first project() call, so a feature list appended after it
# is simply ignored (silently: the ports just never get installed, and the
# failure surfaces much later as a find_package error).
#
# Each entry mirrors a CMake option declared further down. The defaults are
# duplicated here on purpose: option() has not run yet at this point, so the
# only thing available is whatever the user put in the cache / on the command
# line. Keep the two in sync — a mismatch means either an unused port build
# (harmless, slow) or a missing one (a find_package failure at configure).
foreach(_olo_feat alembic materialx vulkan)
    string(TOUPPER "${_olo_feat}" _olo_feat_uc)
    if(NOT DEFINED OLO_WITH_${_olo_feat_uc} OR OLO_WITH_${_olo_feat_uc})
        list(APPEND VCPKG_MANIFEST_FEATURES "${_olo_feat}")
    endif()
endforeach()
if(NOT DEFINED BUILD_TESTS OR BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    project(OloEngineBase
            VERSION 0.0.1
            LANGUAGES CXX C CSharp)
else()
    project(OloEngineBase
            VERSION 0.0.1
            LANGUAGES CXX C)
endif()

# Fail fast and helpfully when the vcpkg toolchain is absent. Without it every
# find_package() below resolves against system paths (or not at all), and the
# first symptom is a wall of "Could not find a package configuration file" —
# which reads like a broken checkout rather than a missing one-time setup step.
if(NOT DEFINED VCPKG_TOOLCHAIN)
    message(FATAL_ERROR
        "This project resolves its third-party dependencies through a vcpkg manifest "
        "(vcpkg.json) since issue #773, but the vcpkg toolchain file was not loaded.\n"
        "Use a CMake preset (`cmake --preset msvc` / `clangcl` / `linux-gcc`) with the "
        "VCPKG_ROOT environment variable pointing at a bootstrapped vcpkg clone, or pass "
        "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake yourself.\n"
        "Setup instructions: docs/ops/build.md.")
endif()

# Build optimization options
option(OLO_ENABLE_LTO "Enable Link Time Optimization" ON)
option(OLO_ENABLE_PCH "Enable Precompiled Headers" ON)
option(OLO_ENABLE_UNITY_BUILD "Enable Unity (Jumbo) Builds for faster compilation" OFF)

# Asset-import format options (#655). All default ON — importing USD / Alembic / MaterialX works
# out of the box, matching UE5's USD-by-default posture. Alembic + MaterialX are small static
# FetchContent builds. OpenUSD is large + install-centric: it is built from source once via
# ExternalProject (oneTBB + static-monolithic USD, ~10-15 min + ~2 GB on the FIRST build, then
# cached), OR reused from a prebuilt install via -DOLO_USD_INSTALL_DIR=<prefix> (CI cache / a
# shared build). CI that doesn't need USD should pass -DOLO_WITH_USD=OFF to skip the heavy build.
# See docs/agent-rules/asset-import-usd-alembic.md.
option(OLO_WITH_ALEMBIC "Import Alembic (.abc) geometry caches (vendors Imath + Alembic)" ON)
option(OLO_WITH_MATERIALX "Import MaterialX (.mtlx) materials (vendors MaterialX)" ON)
option(OLO_WITH_USD "Import OpenUSD stages (.usd/.usda/.usdc/.usdz); builds USD from source unless OLO_USD_INSTALL_DIR is set" ON)

# Vulkan RHI backend (#691 Phase 4, ADR 0011 §2). This is the AVAILABILITY axis only —
# "is the backend compiled in at all" — deliberately separate from the runtime SELECTION
# axis (`--rhi=opengl|vulkan` at process start; default OpenGL). There is no `vulkan`
# CMake preset and must never be one: one binary carries every compiled-in backend, so a
# machine below Vulkan's hardware floor (ADR 0010) can fall back to GL at runtime.
# The backend vendors Vulkan-Headers + volk + VMA pinned at SDK 1.4.357.0 (the ADR 0010
# tooling floor) and volk dlopens the loader, so turning this ON adds no link-time
# dependency on an installed Vulkan SDK beyond what the shaderc toolchain already needs.
option(OLO_WITH_VULKAN "Compile the Vulkan RHI backend (vendors Vulkan-Headers + volk + VMA; runtime selection stays --rhi=)" ON)

# Includes
include(cmake/SetupConfigurations.cmake)
include(cmake/CommonProperties.cmake)
include(cmake/Sanitizers.cmake)
# Must come AFTER SetupConfigurations.cmake (it overrides that file's MSVC
# debug-info default) and BEFORE the target subdirectories below (it sets the
# compiler-launcher variables that initialize each target's property).
include(cmake/CompilerCache.cmake)

# Default FetchContent downloads to PROJECT_SOURCE_DIR/OloEngine/vendor so vendor
# libraries live in a predictable location alongside the checked-in vendor tree.
# Must be set BEFORE include(cmake/CPM.cmake) — CPM internally does
# include(FetchContent) which caches the default (_deps/) and would prevent a
# later override.
# Presets (e.g. clangcl) can still override via -DFETCHCONTENT_BASE_DIR=…
if(NOT DEFINED CACHE{FETCHCONTENT_BASE_DIR})
	set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/OloEngine/vendor" CACHE PATH "" FORCE)
endif()

# Fuzzing.cmake must be included BEFORE add_subdirectory(OloEngine) so its
# add_compile_definitions(_DISABLE_VECTOR_ANNOTATION …) propagates to the
# OloEngine static lib. Otherwise the harnesses fail to link with a /failif
# mismatch: annotate_string ABI error because the lib was compiled with one
# STL annotation flag and the harness was compiled with another. Must also
# be included AFTER the FETCHCONTENT_BASE_DIR setting above — Fuzzing.cmake's
# file(DOWNLOAD) call for the compiler-rt tarball builds its destination path
# from FETCHCONTENT_BASE_DIR.
include(cmake/Fuzzing.cmake)

include(cmake/CPM.cmake)

# Project wide setup - Configure module paths
list(APPEND CMAKE_MODULE_PATH 
    ${CMAKE_CURRENT_LIST_DIR}/cmake
    ${CMAKE_CURRENT_BINARY_DIR}
)

# --------------------------------------------------------------------------
# Link concurrency (Ninja only).
#
# Linking is this project's memory spike, not compiling: OloEngine-Tests and
# OloEditor pull in the whole static engine plus its vendored dependencies, and
# each concurrent link holds that peak at once. Ninja's default width is
# `cores + 2` (18 on a 16-core host), so an unqualified `ninja` will happily
# start several of those together and take the machine out — which is exactly
# how this host was OOM-killed while three other CI runners shared it.
#
# A pool is the right tool rather than a smaller `-j`: it throttles ONLY the
# link step, leaving compilation at full width. It also lives in the generated
# build.ninja, so it protects a bare `ninja` invocation too — not just
# `cmake --build`, which is the only thing CMAKE_BUILD_PARALLEL_LEVEL reaches.
#
# Raise it on a machine with memory to spare: -DOLO_LINK_JOBS=4.
if(CMAKE_GENERATOR MATCHES "Ninja")
    set(OLO_LINK_JOBS "2" CACHE STRING "Maximum concurrent link steps (Ninja generators)")
    # THIS PROJECT requires a finite positive depth — that is a stricter rule
    # than the toolchain's, deliberately.
    #
    # Measured, rather than assumed: CMake already rejects a non-numeric depth
    # at configure time (build.ninja is never generated), so that case needs no
    # help here. But `depth = 0` passes straight through CMake AND ninja, and
    # the build then runs happily with the cap doing nothing. Since the entire
    # point of this pool is to bound peak memory during linking, a value that
    # quietly disables it is worse than one that fails — so reject it here and
    # say why, instead of leaving a green build that is not actually capped.
    if(NOT OLO_LINK_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "OLO_LINK_JOBS must be a positive integer (got '${OLO_LINK_JOBS}'). "
            "It bounds concurrent link steps to limit peak memory; 0 or empty "
            "would leave links effectively uncapped.")
    endif()
    set_property(GLOBAL PROPERTY JOB_POOLS olo_link=${OLO_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK olo_link)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "v4.8.1")
    set(CMAKE_CSharp_FLAGS "/langversion:8")
endif()
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Utilities/cmake")

# Build options
option(BUILD_TESTS "Build tests" ON)

# Tools
add_subdirectory(tools/OloHeaderTool)

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEngine-LuaScriptCore)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_subdirectory(OloEngine-ScriptCore)
    add_subdirectory(OloEditor/SandboxProject/Assets/Scripts)
    set_target_properties(OloEngine-ScriptCore PROPERTIES FOLDER "Scripting")
    set_target_properties(Sandbox-Scripting PROPERTIES FOLDER "Scripting")
endif()

add_subdirectory(OloEditor)
add_subdirectory(OloRuntime)
set_target_properties(OloEditor PROPERTIES FOLDER "Applications")
set_target_properties(OloRuntime PROPERTIES FOLDER "Applications")
# Ensure OloRuntime is always up-to-date when building OloEditor.
# Both link against OloEngine, and the editor's BuildGamePanel copies
# OloRuntime.exe into game builds — a stale binary would silently ship
# without the latest engine fixes.
add_dependencies(OloEditor OloRuntime)

# Ensure a targeted `--target OloEditor` build also produces a working C#
# scripting setup. OloEngine-ScriptCore.dll builds straight into
# OloEditor/Resources/Scripts/ (see olo_configure_csharp_project in
# cmake/CommonProperties.cmake) and Sandbox-Scripting.dll is the sample
# project's game-script assembly the editor loads at startup; without this
# edge, ScriptEngine::Init silently disables C# scripting because neither
# DLL exists on disk yet. C# targets only exist under the Visual Studio
# generator (see the guard above), so this edge must live inside the same
# guard or clang/ninja configure would fail on an unknown target.
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_dependencies(OloEditor OloEngine-ScriptCore Sandbox-Scripting)
endif()

add_subdirectory(OloServer)

set_target_properties(OloEngine-LuaScriptCore PROPERTIES FOLDER "Scripting")
set_target_properties(OloServer PROPERTIES FOLDER "Applications")

# App-launch smoke test wiring (issue #303). Hand OloEngine-Tests the on-disk
# paths of the three shipped executables so AppLaunchSmokeTest can spawn each one
# with `--smoke-test` and assert a clean exit. Done here — after every app target
# exists — because OloEngine/tests is configured before the app subdirectories,
# so an `if(TARGET ...)` check inside tests/CMakeLists.txt would be premature.
# $<PATH:CMAKE_PATH,...> normalises the native path to forward slashes so it
# stays a valid C string literal on Windows. A target absent on this platform
# simply leaves its macro undefined and the matching test SKIPs at runtime.
if(TARGET OloEngine-Tests)
    foreach(_olo_app OloServer OloEditor OloRuntime)
        if(TARGET ${_olo_app})
            string(TOUPPER ${_olo_app} _olo_app_uc)
            target_compile_definitions(OloEngine-Tests PRIVATE
                "OLO_TEST_${_olo_app_uc}_EXE=\"$<PATH:CMAKE_PATH,$<TARGET_FILE:${_olo_app}>>\"")
        endif()
    endforeach()
endif()

# Auto-run OloHeaderTool before compiling targets that consume its output.
# OloEngine includes the generated .inl files; OloEngine-ScriptCore includes
# the generated .cs files.
add_dependencies(OloEngine GenerateBindings)
if(CMAKE_GENERATOR MATCHES "Visual Studio")
	add_dependencies(OloEngine-ScriptCore GenerateBindings)
endif()

if(${CMAKE_GENERATOR} MATCHES "Visual Studio")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "OloEditor")
endif()
