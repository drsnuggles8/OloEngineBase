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

# --------------------------------------------------------------------------
# Steamworks platform integration (#644) — achievements, Steam Cloud, rich presence/overlay.
#
# AVAILABILITY axis only ("is the Steam code compiled in at all"), deliberately separate from
# runtime availability: even an ON build degrades when the Steam client isn't running
# (SteamAPI_Init() returns false → warn + disable, NEVER throw), so an ON binary still starts
# fine on a machine with no Steam. Both variants are required; they cover different failures.
#
# The SDK is NOT vendored and NOT a vcpkg dependency, and can never become either: it downloads
# from partner.steamgames.com behind a Valve login, its licence forbids redistribution, and THIS
# REPO IS PUBLIC. It is therefore resolved from an environment variable the developer sets —
# mirroring how the Vulkan SDK is found ($ENV{VULKAN_SDK}, OloEngine/CMakeLists.txt) rather than
# how Mono is vendored in-tree, which is only legal there because Mono is redistributable.
#
# DEVELOPERS MUST OBTAIN THE SDK THEMSELVES. It is free — the Steamworks SDK Access Agreement
# needs only an ordinary Steam account; the $100 Steam Direct fee is for publishing, not for
# building. Setup instructions: docs/ops/build.md § "Steamworks SDK (optional)".
#
# The default is auto-detected rather than a flat OFF: a developer who went to the trouble of
# installing the SDK and exporting the variable almost certainly wants it compiled. CI sets no
# such variable, so every workflow gets OFF for free — which is precisely the problem that
# OLO_WITH_STEAM_STUB_SDK below exists to solve. NOTE the usual CMake cache caveat: this default
# is only consulted on the FIRST configure, so installing the SDK later needs an explicit
# -DOLO_WITH_STEAM=ON (or a wiped cache) to take effect.
set(OLO_STEAM_SDK_DEFAULT OFF)
if(DEFINED ENV{STEAMWORKS_SDK_ROOT} AND NOT "$ENV{STEAMWORKS_SDK_ROOT}" STREQUAL "")
    if(IS_DIRECTORY "$ENV{STEAMWORKS_SDK_ROOT}/public/steam")
        set(OLO_STEAM_SDK_DEFAULT ON)
    else()
        # The variable is set but does not point at an SDK. Do NOT fall through to a silent OFF:
        # a developer who exported the variable clearly wants Steam, and a quiet "Steam does
        # nothing" build is the worst outcome — the FATAL_ERROR in OloEngine/CMakeLists.txt that
        # explains the correct layout only fires when the option is explicitly forced ON, so it
        # would never reach the person who made this mistake. Warn here instead.
        #
        # Pointing one level too high (at the zip's outer folder rather than the inner `sdk/`)
        # is by far the most common way to get here, so detect it and name the exact fix.
        if(IS_DIRECTORY "$ENV{STEAMWORKS_SDK_ROOT}/sdk/public/steam")
            # Native separators: this line exists to be copy-pasted into a setx/export.
            file(TO_NATIVE_PATH "$ENV{STEAMWORKS_SDK_ROOT}/sdk" OLO_STEAM_SUGGESTED_ROOT)
            message(WARNING
                "STEAMWORKS_SDK_ROOT is set one level too high, so Steam support is OFF.\n"
                "  current: $ENV{STEAMWORKS_SDK_ROOT}\n"
                "  correct: ${OLO_STEAM_SUGGESTED_ROOT}\n"
                "It must point at the directory DIRECTLY containing public/ and "
                "redistributable_bin/. See docs/ops/build.md.")
        else()
            message(WARNING
                "STEAMWORKS_SDK_ROOT is set to '$ENV{STEAMWORKS_SDK_ROOT}' but no "
                "public/steam/ directory was found there, so Steam support is OFF. It must point "
                "at the directory DIRECTLY containing public/ and redistributable_bin/ (the inner "
                "'sdk' folder of the Steamworks SDK zip). See docs/ops/build.md.")
        endif()
    endif()
endif()
option(OLO_WITH_STEAM "Compile Steamworks platform integration (requires STEAMWORKS_SDK_ROOT; see docs/ops/build.md)" ${OLO_STEAM_SDK_DEFAULT})

# Build the Steam-enabled path against HAND-WRITTEN STUB headers instead of the real SDK.
#
# This exists because the real SDK can never reach CI (public repo + non-redistributable
# headers), so without it the ON path would be compiled on developer machines only and would
# bitrot silently — exactly Trap 3 in docs/agent-rules/vcpkg-dependency-management.md. A repo
# secret smuggling the SDK onto the runner was considered and rejected: it is a licence question
# we should not answer unilaterally, and secrets are unavailable to fork PRs anyway.
#
# The stubs declare ONLY the entry points this engine actually calls AND provide their symbols,
# so CI compiles, LINKS and RUNS a genuine OLO_WITH_STEAM=ON binary against canned returns —
# including the SteamAPI_Init()-returns-false branch, the one behaviour that must never regress.
# A call the stubs do not declare turns CI red; that is the mechanism working, not a maintenance
# tax, so do not "fix" it by deleting the job.
option(OLO_WITH_STEAM_STUB_SDK "Build the Steam path against hand-written stub headers (CI anti-rot; implies OLO_WITH_STEAM)" OFF)
if(OLO_WITH_STEAM_STUB_SDK AND NOT OLO_WITH_STEAM)
    message(STATUS "OLO_WITH_STEAM_STUB_SDK=ON implies OLO_WITH_STEAM=ON — enabling it.")
    set(OLO_WITH_STEAM ON CACHE BOOL "Compile Steamworks platform integration" FORCE)
endif()

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

# --------------------------------------------------------------------------
# Heavy-TU compile concurrency (CMake 4.4+, Ninja only — issue #822).
#
# The missing middle between the link pool above and the blunt -j width used for
# everything else. #759's instrumented trace (docs/agent-rules/
# build-trees-and-windows-asan.md §5) found compilation, not linking, sets this
# host's build memory ceiling — and it's a HANDFUL of TUs that set it, not the
# lane count. A fresh trace taken while adding this (issue #822) confirms the
# same two outliers, both roughly unchanged in identity from #759: LuaScriptGlue.cpp
# (~198s CPU-time, more than 3x the next-heaviest TU) and McpFieldRegistry.cpp
# (~67s, compiled separately into both OloEditor and OloEngine-Tests). Both wrap
# heavy macro-driven binding-glue generation (Sol2 Lua bindings; the generated MCP
# writable-field registry) the compiler has to instantiate wholesale, unlike
# ordinary engine TUs.
#
# 4.4 lets a SOURCES-type file set carry its own JOB_POOL_COMPILE, which overrides
# both the target- and source-level setting — so these specific TUs can be
# throttled without capping compilation for the rest of the tree. Each target that
# compiles one of the heavy TUs binds it via olo_bind_heavy_compile_pool()
# (cmake/CommonProperties.cmake) after removing it from its own plain source
# list — see that function's comment for why the removal is mandatory.
#
# Raise -DOLO_HEAVY_COMPILE_JOBS=<N> (or set it to the compile -j width) to
# loosen the throttle without reverting the file-set wiring.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.4 AND CMAKE_GENERATOR MATCHES "Ninja")
    set(OLO_HEAVY_COMPILE_JOBS "2" CACHE STRING
        "Maximum concurrent compiles of the handful of memory-heavy TUs (LuaScriptGlue.cpp, McpFieldRegistry.cpp — issue #822). Ninja generators only.")
    # Same rule, and the same reason, as OLO_LINK_JOBS above: depth 0 or empty
    # passes straight through CMake AND Ninja and leaves the pool silently uncapped.
    if(NOT OLO_HEAVY_COMPILE_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "OLO_HEAVY_COMPILE_JOBS must be a positive integer (got '${OLO_HEAVY_COMPILE_JOBS}'). "
            "It bounds concurrent compiles of the memory-heavy TU set; 0 or empty "
            "would leave them effectively uncapped.")
    endif()
    # APPEND — the link pool above already set JOB_POOLS; a plain (non-APPEND) set
    # here would silently clobber olo_link's registration, uncapping links instead
    # of adding a second pool alongside them.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS olo_heavy=${OLO_HEAVY_COMPILE_JOBS})
    set(OLO_HEAVY_COMPILE_POOL_AVAILABLE TRUE)
else()
    set(OLO_HEAVY_COMPILE_POOL_AVAILABLE FALSE)
endif()

# --------------------------------------------------------------------------
# Static-archive headroom against the 4 GiB COFF ceiling (issue #762).
#
# A .lib stores member offsets as 32-bit values and has no 64-bit variant, so 4 GiB is a hard
# format limit rather than a tunable. OloEngine.lib crossed it in the Dist configuration — whose
# /GL objects hold compiler IL rather than machine code — and every app target stopped linking
# with a message that names neither the cause nor the cure:
#
#     OloEngine.lib : fatal error LNK1248: image size (10007B6EE) exceeds maximum allowable size
#
# Nothing was watching, because no CI workflow builds Dist and the ones that build Release pass
# -DOLO_ENABLE_LTO=OFF, so no gate had ever produced a /GL object at all. The thresholds below
# turn that cliff into a slope: cmake/CheckArchiveSize.cmake runs after the archive is written
# and reports its size every time, warns while there is still room to act, and fails with an
# actionable message short of the point lib.exe gives up.
#
# Expressed as a percentage of the format limit, because that is the number that matters — the
# absolute size is meaningless without it. Raise them only to buy time for a real fix; lowering
# WARN is free and gives earlier notice.
set(OLO_ARCHIVE_WARN_PERCENT "75" CACHE STRING
    "Warn when a static archive reaches this percentage of the 4 GiB COFF limit (issue #762)")
set(OLO_ARCHIVE_FAIL_PERCENT "90" CACHE STRING
    "Fail the build when a static archive reaches this percentage of the 4 GiB COFF limit (issue #762)")
foreach(_olo_archive_var OLO_ARCHIVE_WARN_PERCENT OLO_ARCHIVE_FAIL_PERCENT)
    # Same rule and reason as OLO_LINK_JOBS above: a value CMake would accept but that silently
    # disables the check (empty, 0, or past the format limit) is worse than one that fails here.
    #
    # 100 is rejected along with 0, and for the same reason rather than as an off-by-one: an
    # archive can never REACH 100% of 4 GiB, because lib.exe refuses to write one at or past the
    # limit — that refusal is the whole premise of this check. A threshold of 100 is therefore a
    # threshold that can never fire, which is exactly the silently-disabled case above.
    if(NOT ${_olo_archive_var} MATCHES "^[1-9][0-9]?$")
        message(FATAL_ERROR
            "${_olo_archive_var} must be an integer percentage in 1..99 (got "
            "'${${_olo_archive_var}}'). It bounds how close a static archive may get to the "
            "4 GiB COFF limit; an out-of-range value would leave the archive unguarded, and 100 "
            "is unreachable because lib.exe never writes an archive that large.")
    endif()
endforeach()
# GREATER_EQUAL, not GREATER: cmake/CheckArchiveSize.cmake tests the fail threshold first and the
# warn threshold in an elseif, so equal thresholds do not merely make the warning redundant — they
# make it unreachable, which is the same defect this message already describes.
if(OLO_ARCHIVE_WARN_PERCENT GREATER_EQUAL OLO_ARCHIVE_FAIL_PERCENT)
    message(FATAL_ERROR
        "OLO_ARCHIVE_WARN_PERCENT (${OLO_ARCHIVE_WARN_PERCENT}) must be strictly less than "
        "OLO_ARCHIVE_FAIL_PERCENT (${OLO_ARCHIVE_FAIL_PERCENT}) — the build would fail before "
        "ever emitting the warning that is supposed to precede it.")
endif()
math(EXPR OLO_ARCHIVE_WARN_BYTES "(4294967296 / 100) * ${OLO_ARCHIVE_WARN_PERCENT}")
math(EXPR OLO_ARCHIVE_FAIL_BYTES "(4294967296 / 100) * ${OLO_ARCHIVE_FAIL_PERCENT}")

# --------------------------------------------------------------------------
# Build instrumentation (CMake 4.3+, Makefile/Ninja/FASTBuild only — issue #822).
#
# Native successor to the recipe issue #759 used by hand
# (docs/agent-rules/build-trees-and-windows-asan.md §5): the same
# Instrumentation API existed pre-4.3 but only behind an experimental,
# UUID-suffixed query directory undocumented outside Help/dev/experimental.rst
# (not shipped in the binary install). cmake_instrumentation() is the same
# feature, now stable, invoked from the project itself — no gate UUID, no
# manually-written query file, repeatable by any future agent.
#
# Opt-in, default OFF: the API wraps every compile/link/custom command in a
# launcher, and profiling every developer build by default is not wanted.
# Enable with -DOLO_BUILD_INSTRUMENTATION=ON on a Ninja tree (build-clang/, or
# a Linux preset). The API itself only supports Makefile / Ninja / FASTBuild
# generators (cmake-instrumentation(7)) — checked as an ALLOW-list below, not a
# deny-list on just "Visual Studio", so Xcode (and any other generator this
# repo doesn't currently use) also gets the loud message(WARNING) instead of
# silently attempting instrumentation and doing nothing.
#
# stdout/stderr capture (the `captureOutput` option, 4.4+) is deliberately
# left off: it inflates snippet volume considerably for data this project
# doesn't currently consume — timing and host memory before/after (the two
# questions #759 asked) don't need it.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3)
    option(OLO_BUILD_INSTRUMENTATION
        "Emit CMake Instrumentation API trace data (compile/link/custom timing + host memory before/after) for this build tree. Ninja/Makefile/FASTBuild only; no effect under other generators (Visual Studio, Xcode, ...)."
        OFF)
    if(OLO_BUILD_INSTRUMENTATION)
        if(CMAKE_GENERATOR MATCHES "Makefiles|Ninja|FASTBuild")
            cmake_instrumentation(
                API_VERSION 1
                DATA_VERSION 1
                HOOKS postGenerate postCMakeBuild
                OPTIONS staticSystemInformation dynamicSystemInformation trace
            )
        else()
            message(WARNING
                "OLO_BUILD_INSTRUMENTATION has no effect under the '${CMAKE_GENERATOR}' "
                "generator - the CMake Instrumentation API is Makefile/Ninja/"
                "FASTBuild only (cmake-instrumentation(7)). Configure the clangcl "
                "preset (or a Linux preset) instead.")
        endif()
    endif()
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
