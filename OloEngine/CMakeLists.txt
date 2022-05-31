include(CMakePrintHelpers)

project(OloEngine
		LANGUAGES CXX)

set(OloEngine_VERSION_MAJOR 0)
set(OloEngine_VERSION_MINOR 0)
set(OloEngine_VERSION_PATCH 1)
set(OloEngine_VERSION
${OloEngine_VERSION_MAJOR}.${OloEngine_VERSION_MINOR}.${OloEngine_VERSION_PATCH}
)

# Externally provided content
add_subdirectory(vendor)
find_package(Vulkan REQUIRED FATAL_ERROR)
message("Vulkan found? " ${Vulkan_FOUND})
message("Alright, where is it? " ${Vulkan_LIBRARIES})
message("And I can include it? " ${Vulkan_INCLUDE_DIRS})
message("Vulkan version: " ${Vulkan_VERSION})
message("Path to GLSL SPIR-V Compiler:" ${Vulkan_GLSLC_EXECUTABLE})

# Main targets built by this project
add_subdirectory(src)

set_target_properties(OloEngine PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					ARCHIVE_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					LIBRARY_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEngine
					RUNTIME_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEngine
)

#add_dependencies(OloEngine
#				glfw
#				glad
#				ryml
#)

target_precompile_headers("OloEngine" PUBLIC src/OloEnginePCH.h)

target_include_directories("OloEngine" PRIVATE
	src
	vendor/spdlog-src/include
	vendor/glfw-src/include
	${GLAD_OUT_DIR}/include
	vendor/imgui-src
	vendor/imgui-src/backends
	vendor/imgui-src/examples
	vendor/imguizmo-src
	vendor/glm-src
	vendor/stb_image-src
	vendor/entt-src/single_include/entt
	vendor/ryml-src/src
	${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions("OloEngine" PRIVATE
							_CRT_SECURE_NO_WARNINGS
							GLFW_INCLUDE_NONE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
							YAML_CPP_STATIC_DEFINE
)

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	target_compile_options("OloEngine" PRIVATE /W4)
else()
	target_compile_options("OloEngine" PRIVATE -Wall -Wextra -pedantic)
endif()

target_link_libraries(OloEngine
					  glad
					  glfw
					  imgui
					  imguizmo
					  opengl32.lib
					  ryml
					  stb_image
					  yaml-cpp
					  ${Vulkan_LIBRARIES}
)
