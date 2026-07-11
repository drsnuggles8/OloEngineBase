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

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    project(OloEngineBase
            VERSION 0.0.1
            LANGUAGES CXX C CSharp)
else()
    project(OloEngineBase
            VERSION 0.0.1
            LANGUAGES CXX C)
endif()

# Build optimization options
option(OLO_ENABLE_LTO "Enable Link Time Optimization" ON)
option(OLO_ENABLE_PCH "Enable Precompiled Headers" ON)
option(OLO_ENABLE_UNITY_BUILD "Enable Unity (Jumbo) Builds for faster compilation" OFF)

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
