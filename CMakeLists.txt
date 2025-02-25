cmake_minimum_required(VERSION 3.25)

message(STATUS "CMake version: ${CMAKE_VERSION}")

project(OloEngineBase
        VERSION 0.0.1
        LANGUAGES CXX C CSharp)

# Includes
include(cmake/SetupConfigurations.cmake)
include(cmake/CPM.cmake)

# Project wide setup
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "v4.8.1")
set(CMAKE_CSharp_FLAGS "/langversion:8")
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Utilities/cmake")

# Build options
option(BUILD_TESTS "Build tests" ON)

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEngine-ScriptCore)
add_subdirectory(OloEngine-LuaScriptCore)
add_subdirectory(OloEditor/SandboxProject/Assets/Scripts)
add_subdirectory(OloEditor)
add_subdirectory(Sandbox2D)

set_target_properties(OloEngine-ScriptCore PROPERTIES FOLDER "Scripting")
set_target_properties(OloEngine-LuaScriptCore PROPERTIES FOLDER "Scripting")
set_target_properties(Sandbox-Scripting PROPERTIES FOLDER "Scripting")
set_target_properties(OloEditor PROPERTIES FOLDER "Applications")
set_target_properties(Sandbox2D PROPERTIES FOLDER "Applications")

if(${CMAKE_GENERATOR} MATCHES "Visual Studio")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "Sandbox2D")
endif()
