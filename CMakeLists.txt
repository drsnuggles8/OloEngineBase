cmake_minimum_required(VERSION 3.16)

message(STATUS "CMake version: ${CMAKE_VERSION}")

project(OloEngineBase
        LANGUAGES CXX)

set(OloEngineBase_VERSION_MAJOR 0)
set(OloEngineBase_VERSION_MINOR 0)
set(OloEngineBase_VERSION_PATCH 1)
set(OloEngineBase_VERSION ${OloEngineBase_VERSION_MAJOR}.${OloEngineBase_VERSION_MINOR}.${OloEngineBase_VERSION_PATCH})

# sets some variables
#include(cmake/preamble.cmake)

# sets build option
#include(cmake/buildOptions.cmake)

# find all the dependancies.
#include(cmake/findDependancies.cmake)

# Project wide setup
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEditor)
add_subdirectory(Sandbox)
