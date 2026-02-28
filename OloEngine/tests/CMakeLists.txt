enable_testing()
add_executable(OloEngine-Tests
		OloEngineTest.cpp
		FastRandomTest.cpp
		ContainerTest.cpp
		SnowSettingsTest.cpp
		WindSettingsTest.cpp

		Async/ParkingLotTest.cpp
		Async/MutexTest.cpp
		Async/MutexLockRateTest.cpp
		Async/SemaphoreTest.cpp
		Async/ManualResetEventTest.cpp
		Async/SharedMutexTest.cpp
		Async/SharedRecursiveMutexTest.cpp
		Async/ExternalMutexTest.cpp
		Tasks/TaskSystemTest.cpp
		Containers/ConcurrentQueuesTest.cpp
		Memory/MemoryViewTest.cpp
		Templates/FunctionWithContextTest.cpp
		Templates/TypeTraitsTest.cpp
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
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/atomic_queue-src/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
)
