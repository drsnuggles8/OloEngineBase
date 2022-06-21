enable_testing()
add_executable(OloEngine_Tests
		OloEngineTest.cpp
		MathTest.cpp
		LoggerTest.cpp)
		
target_link_libraries(OloEngine_Tests
	OloEngine
	gtest_main)

target_include_directories(OloEngine_Tests PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/googletest-src/googletest/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include)

include(GoogleTest)
gtest_discover_tests(OloEngine_Tests
			XML_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/test_results/)
