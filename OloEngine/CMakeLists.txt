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
  VERSION 3.12.0
  OPTIONS
    "JSON_BuildTests OFF"
    "JSON_Install OFF")

# Main targets built by this project
add_subdirectory(src)
if(BUILD_TESTS)
	add_subdirectory(tests)
	set_target_properties(OloEngine-Tests PROPERTIES FOLDER "Test")
endif()

# Vulkan SDK
if(MSVC)
	find_package(Vulkan REQUIRED COMPONENTS glslc glslang SPIRV-Tools)
else()
	# On Linux with system packages, glslang sub-libraries (OGLCompiler etc.)
	# may not match what CMake's FindVulkan expects. Find the core Vulkan
	# library via find_package and locate glslang/SPIRV-Tools manually.
	find_package(Vulkan REQUIRED)
endif()

# Configure the OloEngine target using our common properties
olo_set_output_directories(OloEngine)
olo_enable_lto(OloEngine)
olo_enable_pch(OloEngine src/OloEnginePCH.h)
olo_set_compiler_options(OloEngine)

# Unity Build: combines multiple .cpp files per translation unit for faster compilation
if(OLO_ENABLE_UNITY_BUILD)
    set_target_properties(OloEngine PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 16
    )
endif()

# Additional engine-specific compile definitions
target_compile_definitions(OloEngine PRIVATE
						_CRT_SECURE_NO_WARNINGS
						GLFW_INCLUDE_NONE
						IMGUI_DEFINE_MATH_OPERATORS
						YAML_CPP_STATIC_DEFINE
						OLO_ENGINE_VERSION="${CMAKE_PROJECT_VERSION}"
)
olo_set_common_definitions(OloEngine)

target_include_directories(OloEngine PUBLIC
	src
	vendor/assimp-src/include
	vendor/atomic_queue-src/include
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
	${imguizmo_SOURCE_DIR}
	vendor/spdlog-src/include
	vendor/tracy-src/public
	vendor/sol2-src/include
	vendor/stb_image-build/include
	vendor/recastnavigation-src/Recast/Include
	vendor/recastnavigation-src/Detour/Include
	vendor/recastnavigation-src/DetourCrowd/Include
	vendor/recastnavigation-src/DetourTileCache/Include
	mono/include
	${Vulkan_INCLUDE_DIRS}
	vendor/GameNetworkingSockets-src/include
)

target_include_directories(OloEngine PRIVATE
	vendor/zlib-src
	vendor/zlib-build
)

# Cross-platform link libraries
target_link_libraries(OloEngine
	assimp
	box2d
	choc
	Detour
	DetourCrowd
	glad
	glfw
	imgui
	imguizmo
	Jolt
	nlohmann_json::nlohmann_json
	Recast
	sol2
	stb_image
	TracyClient
	yaml-cpp
	GameNetworkingSockets_s
	Vulkan::Vulkan
	png_shared
	$<$<CONFIG:Debug>:${ZLIB_LIBRARY_DEBUG}>
	$<$<CONFIG:Release>:${ZLIB_LIBRARY_RELEASE}>
	$<$<CONFIG:Dist>:${ZLIB_LIBRARY_DIST}>
)

# OpenGL — platform-specific library name
find_package(OpenGL REQUIRED)
target_link_libraries(OloEngine OpenGL::GL)

# Shader compilation libraries (shaderc, spirv-cross, glslang, SPIRV-Tools)
if(MSVC)
	if(NOT DEFINED ENV{VULKAN_SDK})
		message(FATAL_ERROR
			"VULKAN_SDK environment variable is not set.\n"
			"Install the Vulkan SDK from https://vulkan.lunarg.com/sdk/home and "
			"ensure the VULKAN_SDK environment variable points to the installation directory.")
	endif()
	# Windows: link from Vulkan SDK + CMake imported targets
	target_link_libraries(OloEngine
		Vulkan::glslang
		Vulkan::SPIRV-Tools
		debug $ENV{VULKAN_SDK}/Lib/shaderc_sharedd.lib
		debug $ENV{VULKAN_SDK}/Lib/spirv-cross-cored.lib
		debug $ENV{VULKAN_SDK}/Lib/spirv-cross-glsld.lib
		optimized $ENV{VULKAN_SDK}/Lib/shaderc_shared.lib
		optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-core.lib
		optimized $ENV{VULKAN_SDK}/Lib/spirv-cross-glsl.lib
	)
else()
	# Linux: find all shader-related libraries from system packages
	find_library(SHADERC_LIB NAMES shaderc_shared shaderc)
	find_library(SPIRV_CROSS_CORE_LIB NAMES spirv-cross-core)
	find_library(SPIRV_CROSS_GLSL_LIB NAMES spirv-cross-glsl)
	find_library(SPIRV_CROSS_C_LIB NAMES spirv-cross-c-shared)
	find_library(GLSLANG_LIB NAMES glslang)
	find_library(SPIRV_LIB NAMES SPIRV)
	find_library(SPIRV_TOOLS_LIB NAMES SPIRV-Tools)
	find_library(MACHINE_INDEPENDENT_LIB NAMES MachineIndependent)
	find_library(OS_DEPENDENT_LIB NAMES OSDependent)
	find_library(GENERIC_CODEGEN_LIB NAMES GenericCodeGen)
	target_link_libraries(OloEngine
		${SHADERC_LIB}
		${SPIRV_CROSS_CORE_LIB}
		${SPIRV_CROSS_GLSL_LIB}
		${GLSLANG_LIB}
		${SPIRV_LIB}
		${SPIRV_TOOLS_LIB}
		${MACHINE_INDEPENDENT_LIB}
		${OS_DEPENDENT_LIB}
		${GENERIC_CODEGEN_LIB}
	)
	if(SPIRV_CROSS_C_LIB)
		target_link_libraries(OloEngine ${SPIRV_CROSS_C_LIB})
	endif()
endif()

# Mono (C# scripting) — Windows-only for now
if(MSVC)
	target_link_libraries(OloEngine
		debug ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Debug/libmono-static-sgen.lib
		optimized ${CMAKE_CURRENT_SOURCE_DIR}/mono/lib/Release/libmono-static-sgen.lib
	)
	target_compile_definitions(OloEngine PRIVATE OLO_ENABLE_CSHARP_SCRIPTING=1)
else()
	target_compile_definitions(OloEngine PRIVATE OLO_ENABLE_CSHARP_SCRIPTING=0)
endif()

# Platform-specific system libraries
if(MSVC)
	target_link_libraries(OloEngine
		BCrypt
		DbgHelp.lib
		dwmapi
		Synchronization
		Version
		ws2_32
		WinMM
		$<$<CONFIG:Debug>:${ZLIB_LIBRARY_DEBUG}>
		$<$<CONFIG:Release>:${ZLIB_LIBRARY_RELEASE}>
		$<$<CONFIG:Dist>:${ZLIB_LIBRARY_DIST}>
	)
else()
	target_link_libraries(OloEngine
		pthread
		dl
		${CMAKE_DL_LIBS}
	)
endif()

# Ensure zlib is built before OloEngine links against it (raw file paths above
# don't create an implicit build dependency).
add_dependencies(OloEngine zlib)
