# Timing baseline for the #774 vcpkg spike — NOT part of the real engine
# build. Cold-builds the SAME three deps (same pinned commits/tags, same
# settings) that OloEngine/vendor/CMakeLists.txt fetches today via
# FetchContent, isolated from the other ~22 vendor deps so the timing is a
# fair apples-to-apples comparison against the vcpkg cold/cache-hit runs.
cmake_minimum_required(VERSION 3.21)
project(olo_fetchcontent_baseline CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 6f14f4792a0cde5d0cf2c910506724d61cb95834  # master @ 2026-06-05 (matches OloEngine/vendor/CMakeLists.txt)
	GIT_SHALLOW FALSE
)
FetchContent_Declare(
	spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d  # v1.17.0 (matches OloEngine/vendor/CMakeLists.txt)
	GIT_SHALLOW FALSE
)
FetchContent_Declare(
	joltphysics
	GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
	GIT_TAG 6cef2a2214741a77fff8771e233c84da6b875ed5  # matches OloEngine/vendor/CMakeLists.txt
	GIT_SHALLOW FALSE
	SOURCE_SUBDIR Build
)

set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

set(JPH_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(JPH_CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glm spdlog joltphysics)

add_executable(fc_baseline_consumer ../main.cpp)
target_link_libraries(fc_baseline_consumer PRIVATE glm::glm spdlog::spdlog Jolt)
target_include_directories(fc_baseline_consumer PRIVATE "${joltphysics_SOURCE_DIR}")
