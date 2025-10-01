include(CMakePrintHelpers)
include(CheckIPOSupported)

project(OloEngine
        VERSION 0.0.1
        LANGUAGES CXX)

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

# CHOC - for SoundGraph audio processing (Value system, FIFOs, real-time utilities)
CPMAddPackage(NAME choc
  GIT_REPOSITORY https://github.com/Tracktion/choc.git
  GIT_TAG main
  DOWNLOAD_ONLY YES)
if(choc_ADDED)
  add_library(choc INTERFACE IMPORTED)
  target_include_directories(choc SYSTEM INTERFACE ${choc_SOURCE_DIR})
endif()

# nlohmann/json - for JSON serialization
CPMAddPackage(NAME nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  VERSION 3.11.3
  OPTIONS
    "JSON_BuildTests OFF"
    "JSON_Install OFF")

# Main targets built by this project
add_subdirectory(src)
if(BUILD_TESTS)
	add_subdirectory(tests)
	set_target_properties(OloEngine-Tests PROPERTIES FOLDER "Test")
endif()

# Vulkan stuff
find_package(Vulkan REQUIRED COMPONENTS glslc glslang SPIRV-Tools)

# Configure the OloEngine target using our common properties
olo_set_output_directories(OloEngine)
olo_enable_lto(OloEngine)
olo_enable_pch(OloEngine src/OloEnginePCH.h)
olo_set_compiler_options(OloEngine)

# Additional engine-specific compile definitions
target_compile_definitions(OloEngine PRIVATE
						_CRT_SECURE_NO_WARNINGS
						GLFW_INCLUDE_NONE
						YAML_CPP_STATIC_DEFINE
)
olo_set_common_definitions(OloEngine)

target_include_directories(OloEngine PRIVATE
	src
	vendor/assimp-src/include
	vendor/box2d-src/include/box2d
	vendor/box2d-src/src
	vendor/entt-src/single_include/entt
	vendor/filewatch-src
	vendor/glad-build/include
	vendor/glfw-src/include
	vendor/glm-src
	vendor/joltphysics-src
	vendor/miniaudio-src
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

target_link_libraries(OloEngine
	assimp
	box2d
	choc
	glad
	glfw
	imgui
	imguizmo
	Jolt
	msdf-atlas-gen
	nlohmann_json::nlohmann_json
	opengl32.lib
	sol2
	stb_image
	TracyClient
	yaml-cpp
	Vulkan::Vulkan
	Vulkan::glslang
	Vulkan::SPIRV-Tools
	debug $ENV{VULKAN_SDK}/Lib/shaderc_sharedd.lib
	debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib
	debug $ENV{VULKAN_SDK}/Lib/spirv-cross-glsld.lib
	optimized $ENV{VULKAN_SDK}/Lib/shaderc_shared.lib
	optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
	optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-glsl.lib
	debug ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Debug/libmono-static-sgen.lib
	optimized ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Release/libmono-static-sgen.lib
	BCrypt
	Version
	ws2_32
	WinMM
	png_shared
    $<$<CONFIG:Debug>:${ZLIB_LIBRARY_DEBUG}>
    $<$<CONFIG:Release>:${ZLIB_LIBRARY_RELEASE}>
    $<$<CONFIG:Dist>:${ZLIB_LIBRARY_DIST}>
)