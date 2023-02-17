cmake_minimum_required(VERSION 3.23)

message(STATUS "CMake version: ${CMAKE_VERSION}")

project(OloEngineBase
        LANGUAGES CXX C CSharp)

set(OloEngineBase_VERSION_MAJOR 0)
set(OloEngineBase_VERSION_MINOR 0)
set(OloEngineBase_VERSION_PATCH 1)
set(OloEngineBase_VERSION ${OloEngineBase_VERSION_MAJOR}.${OloEngineBase_VERSION_MINOR}.${OloEngineBase_VERSION_PATCH})

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

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEngine-ScriptCore)
add_subdirectory(OloEngine-LuaScriptCore)
add_subdirectory(OloEditor/SandboxProject/Assets/Scripts)
add_subdirectory(OloEditor)
add_subdirectory(Sandbox)

set_target_properties(OloEngine-Tests PROPERTIES FOLDER "Test")
set_target_properties(OloEngine-ScriptCore PROPERTIES FOLDER "Scripting")
set_target_properties(OloEngine-LuaScriptCore PROPERTIES FOLDER "Scripting")
set_target_properties(Sandbox-Scripting PROPERTIES FOLDER "Scripting")
set_target_properties(OloEditor PROPERTIES FOLDER "Applications")
set_target_properties(Sandbox PROPERTIES FOLDER "Applications")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "OloEditor")
