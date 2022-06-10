enable_testing()
add_executable(OloEngine_Test
		OloEngineTest.cpp
		LogTest.cpp
)
target_link_libraries(
	OloEngine_Test
	OloEngine
	gtest_main
)

target_include_directories(OloEngine_Test PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/googletest-src/googletest/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
)

include(GoogleTest)
gtest_discover_tests(OloEngine_Test)