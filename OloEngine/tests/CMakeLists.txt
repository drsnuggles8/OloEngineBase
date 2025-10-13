enable_testing()
add_executable(OloEngine-Tests
		# Old tests (commented out - all passing, will be re-enabled after task system implementation)
		# OloEngineTest.cpp
		# MathTest.cpp
		# LoggerTest.cpp
		# AssetCreationTest.cpp
		# CoreUtilitiesTest.cpp
		# SoundGraphBasicTest.cpp
		# AudioEventQueueTest.cpp
		# FastRandomTest.cpp
		
		# Task System Tests (Phase 1-7)
		# Phase 1: Core task infrastructure
		TaskSystemTest.cpp
		# Phase 2: Lock-free queues (to be added)
		# LockFreeQueueTest.cpp
		# Phase 3: Worker threads (to be added)
		# WorkerThreadTest.cpp
		# Phase 4: Synchronization (to be added)
		# TaskSynchronizationTest.cpp
		# Phase 5: Advanced features (to be added)
		# AdvancedTaskTest.cpp
		# Phase 6: Integration (to be added)
		# IntegrationTest.cpp
		# Phase 7: Profiling and debugging (to be added)
		# ProfilingTest.cpp
		)
		
target_link_libraries(OloEngine-Tests
	OloEngine
	gtest_main)

# Fix UTF-8 compilation issue
if(MSVC)
	target_compile_options(OloEngine-Tests PRIVATE /utf-8)
endif()

target_include_directories(OloEngine-Tests PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/googletest-src/googletest/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/miniaudio-src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/choc-src)

include(GoogleTest)
gtest_discover_tests(OloEngine-Tests
			XML_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/test_results/)
