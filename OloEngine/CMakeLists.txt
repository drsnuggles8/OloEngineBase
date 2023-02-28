include(CMakePrintHelpers)
include(CheckIPOSupported)

project(OloEngine
		LANGUAGES CXX)

set(OloEngine_VERSION_MAJOR 0)
set(OloEngine_VERSION_MINOR 0)
set(OloEngine_VERSION_PATCH 1)
set(OloEngine_VERSION ${OloEngine_VERSION_MAJOR}.${OloEngine_VERSION_MINOR}.${OloEngine_VERSION_PATCH})

# Externally provided content
add_subdirectory(vendor)

# Sol2 - for Lua scripting
CPMAddPackage(NAME sol2
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  VERSION 3.3.0
  DOWNLOAD_ONLY YES)
if(sol2_ADDED)
  add_library(sol2 INTERFACE IMPORTED)
  target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
  target_link_libraries(sol2 INTERFACE lua)
endif()

# Main targets built by this project
add_subdirectory(src)
add_subdirectory(tests)

# Vulkan stuff
message(STATUS "Using module to find Vulkan")
find_package(Vulkan)

set_target_properties(OloEngine PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG 		${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE 	${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					ARCHIVE_OUTPUT_DIRECTORY_DIST 		${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_DEBUG 		${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_RELEASE 	${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_DIST 		${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_DEBUG 		${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_RELEASE 	${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_DIST 		${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
)

check_ipo_supported(RESULT LTO_SUPPORT OUTPUT output)
if(LTO_SUPPORT)
	message(STATUS "-- Enabled Link-Time Optimization (LTO)!")
	set_target_properties(OloEngine PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
	message(WARNING "IPO is not supported: ${output}")
endif()

target_precompile_headers(OloEngine PUBLIC src/OloEnginePCH.h)

target_include_directories(OloEngine PRIVATE
	src
	vendor/assimp-src/include
	vendor/box2d-src/include/box2d
	vendor/entt-src/single_include/entt
	vendor/filewatch-src
	vendor/glad-build/include
	vendor/glfw-src/include
	vendor/glm-src
	vendor/miniaudio-build/include
	vendor/lua-src
	vendor/imgui-src
	vendor/imgui-src/backends
	vendor/imgui-src/examples
	vendor/imguizmo-src
	vendor/spdlog-src/include
	vendor/tracy-src/public
	vendor/sol2-src/include
	vendor/stb_image-build/include
	mono/include
	${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(OloEngine PRIVATE
							_CRT_SECURE_NO_WARNINGS
							GLFW_INCLUDE_NONE
							YAML_CPP_STATIC_DEFINE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Release>:TRACY_ENABLE>
							$<$<CONFIG:Release>:TRACY_ON_DEMAND>
							$<$<CONFIG:Dist>:OLO_DIST>
)

target_link_libraries(OloEngine
					assimp
					box2d
					glad
					glfw
					imgui
					imguizmo
					opengl32.lib
					sol2
					stb_image
					TracyClient
					yaml-cpp
					${Vulkan_LIBRARIES}
					debug $ENV{VULKAN_SDK}/Lib/shaderc_sharedd.lib
					debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib
					debug $ENV{VULKAN_SDK}/Lib/spirv-cross-glsld.lib
					debug $ENV{VULKAN_SDK}/Lib/SPIRV-Toolsd.lib
					optimized $ENV{VULKAN_SDK}/Lib/shaderc_shared.lib
					optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
					optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-glsl.lib
					debug ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Debug/libmono-static-sgen.lib
					optimized ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Release/libmono-static-sgen.lib
					BCrypt
					Version
					ws2_32
					WinMM
)

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	target_compile_options("OloEngine" PRIVATE /W4)
else()
	target_compile_options("OloEngine" PRIVATE -Wall -Wextra -Wundef -Wno-cast-function-type -pedantic -Wno-long-long -Wshadow -Werror -Wno-error=deprecated-declarations)
endif()