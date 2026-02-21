enable_testing()
add_executable(OloEngine-Tests
		OloEngineTest.cpp
		#MathTest.cpp
		#LoggerTest.cpp
		#AssetCreationTest.cpp
		#CoreUtilitiesTest.cpp
		#SoundGraphBasicTest.cpp
		#AudioEventQueueTest.cpp
		FastRandomTest.cpp
		ContainerTest.cpp
		ComputeShaderAndSSBOTest.cpp

		# ============================================================================
		# Async - Threading primitives (ported from UE5.7 Tests/Async/)
		# ============================================================================
		Async/ParkingLotTest.cpp            # ParkingLot wait queue tests
		Async/MutexTest.cpp                 # FMutex and TUniqueLock
		Async/MutexLockRateTest.cpp         # Mutex lock rate benchmarks
		Async/SemaphoreTest.cpp             # FSemaphore counting semaphore
		Async/ManualResetEventTest.cpp      # FManualResetEvent sync primitive
		Async/SharedMutexTest.cpp           # FSharedMutex reader-writer lock
		Async/SharedRecursiveMutexTest.cpp  # FSharedRecursiveMutex recursive reader-writer lock
		Async/ExternalMutexTest.cpp         # TExternalMutex external state mutex

		# ============================================================================
		# Tasks - Task system tests (ported from UE5.7 Tests/Tasks/)
		# ============================================================================
		Tasks/TaskSystemTest.cpp            # Launch, Wait, FTaskEvent, FPipe, FTaskConcurrencyLimiter, etc.

		# ============================================================================
		# Containers - Container tests (ported from UE5.7 Tests/Containers/)
		# ============================================================================
		Containers/ConcurrentQueuesTest.cpp # TMpscQueue, TMpmcQueue, TSpscQueue, TSpmcQueue

		# ============================================================================
		# Memory - Memory utilities (ported from UE5.7 Tests/Memory/)
		# ============================================================================
		Memory/MemoryViewTest.cpp           # FMemoryView, FMutableMemoryView

		# ============================================================================
		# Templates - Template utilities (ported from UE5.7 Tests/Templates/)
		# ============================================================================
		Templates/FunctionWithContextTest.cpp # TFunctionWithContext callable adapter
		Templates/TypeTraitsTest.cpp          # Type traits and concepts
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
