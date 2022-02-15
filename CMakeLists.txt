cmake_minimum_required(VERSION 3.16)

message(STATUS "CMake version: ${CMAKE_VERSION}")

project(OloEngineBase
        LANGUAGES CXX C)

set(OloEngineBase_VERSION_MAJOR 0)
set(OloEngineBase_VERSION_MINOR 0)
set(OloEngineBase_VERSION_PATCH 1)
set(OloEngineBase_VERSION ${OloEngineBase_VERSION_MAJOR}.${OloEngineBase_VERSION_MINOR}.${OloEngineBase_VERSION_PATCH})

# Project wide setup
include(cmake/SetupConfigurations.cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# SET THE BELOW TO OFF IF YOU WANT TO DOWNLOAD ALL DEPENDENCIES
set(FETCHCONTENT_FULLY_DISCONNECTED OFF)

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEditor)
add_subdirectory(Sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "OloEditor")
