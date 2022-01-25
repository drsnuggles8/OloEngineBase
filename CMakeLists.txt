# Preamble
cmake_minimum_required(VERSION 3.16)

message(STATUS "CMake version: ${CMAKE_VERSION}")

project(OloEngineBase
        LANGUAGES CXX)

set(OloEngine_VERSION_MAJOR 0)
set(OloEngine_VERSION_MINOR 0)
set(OloEngine_VERSION_PATCH 1)
set(OloEngine_VERSION
${OloEngine_VERSION_MAJOR}.${OloEngine_VERSION_MINOR}.${OloEngine_VERSION_PATCH}
)

# Project wide setup
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Main targets built by this project
add_subdirectory(OloEngine)
add_subdirectory(OloEditor)
add_subdirectory(Sandbox)
