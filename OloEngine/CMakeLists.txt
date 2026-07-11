include(CMakePrintHelpers)
include(CheckIPOSupported)

project(OloEngine
        VERSION 0.0.1
        LANGUAGES CXX)

# Externally provided content
add_subdirectory(vendor)

# Vendor deps below are pinned to fixed commits for reproducible builds (#294); see the
# rationale in vendor/CMakeLists.txt. GIT_SHALLOW must be FALSE for a commit-SHA pin.
# Sol2 - for Lua scripting
CPMAddPackage(NAME sol2
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG c1f95a773c6f8f4fde8ca3efe872e7286afe4444  # develop @ 2026-06-05
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(sol2_ADDED)
  add_library(sol2 INTERFACE IMPORTED)
  target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
  target_link_libraries(sol2 INTERFACE lua)
endif()

# CHOC - for SoundGraph audio processing (Value system, FIFOs, real-time utilities)
CPMAddPackage(NAME choc
  GIT_REPOSITORY https://github.com/Tracktion/choc.git
  GIT_TAG 5685fb59db9e60d31a35df7bff0ce7967beaaf25  # main @ 2026-06-05
  GIT_SHALLOW FALSE
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

# Fuzz harnesses (Layer 11 / §11) — Clang only, opt-in via OLO_ENABLE_FUZZING.
add_subdirectory(tests/Fuzzing)

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

# Propagate C++23 as a transitive requirement so anything that links OloEngine
# (apps, tests, scripting bindings) inherits the standard rather than relying
# on the global CMAKE_CXX_STANDARD=23. Belt-and-braces with the global setting.
target_compile_features(OloEngine PUBLIC cxx_std_23)

# UE5.7-ported code (Containers/, Templates/, Serialization/, HAL/) triggers
# warnings that don't apply to our codebase.  PUBLIC so consumers that
# transitively include these headers (Scene.h -> Map.h, etc.) also compile
# without noise.  Revisit as UE-ported code is cleaned up or replaced.
if(NOT MSVC)
	target_compile_options(OloEngine PUBLIC
		-Wno-undef              # #if MACRO without prior #define (UE convention)
		-Wno-pedantic           # Zero-size arrays, anonymous structs
		-Wno-shadow             # Constructor params shadowing members
		$<$<CXX_COMPILER_ID:GNU>:-Wno-changes-meaning>  # GCC name lookup differs in class scope
		-Wno-unused-function    # Static helpers in UE trait headers
		-Wno-unknown-pragmas    # MSVC-specific #pragma warning push/pop
		-Wno-ignored-qualifiers # const on return-by-value (UE accessor pattern)
		-Wno-unused-local-typedefs  # Type aliases used only in static_assert (UE Tuple.h)
	)
endif()

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
# EnTT's type_index family counter is a plain integer unless ENTT_USE_ATOMIC is
# defined; gameplay systems marked Parallelizable (issue #453) first-touch
# component types from worker tasks, and a racing non-atomic counter can hand two
# types the same id — corrupting every registry's pool mapping process-wide (the
# entt "Unexpected type" assert). PUBLIC: every TU that includes entt.hpp must
# agree on ENTT_MAYBE_ATOMIC or the inline definitions violate ODR.
target_compile_definitions(OloEngine PUBLIC ENTT_USE_ATOMIC)
olo_set_common_definitions(OloEngine)

target_include_directories(OloEngine PUBLIC
	src
	${assimp_SOURCE_DIR}/include
	${atomic_queue_SOURCE_DIR}/include
	${box2d_SOURCE_DIR}/include/box2d
	${box2d_SOURCE_DIR}/src
	${entt_SOURCE_DIR}/single_include/entt
	${filewatch_SOURCE_DIR}
	vendor/glad-build/include
	${glfw_SOURCE_DIR}/include
	${glm_SOURCE_DIR}
	${joltphysics_SOURCE_DIR}
	${meshoptimizer_SOURCE_DIR}/src
	${miniaudio_SOURCE_DIR}
	${pl_mpeg_SOURCE_DIR}
	${lua_SOURCE_DIR}
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends
	${imgui_SOURCE_DIR}/examples
	${imguizmo_SOURCE_DIR}
	${spdlog_SOURCE_DIR}/include
	${tracy_SOURCE_DIR}/public
	${sol2_SOURCE_DIR}/include
	vendor/stb_image-build/include
	${recastnavigation_SOURCE_DIR}/Recast/Include
	${recastnavigation_SOURCE_DIR}/Detour/Include
	${recastnavigation_SOURCE_DIR}/DetourCrowd/Include
	${recastnavigation_SOURCE_DIR}/DetourTileCache/Include
	mono/include
	${Vulkan_INCLUDE_DIRS}
	${GameNetworkingSockets_SOURCE_DIR}/include
)

target_include_directories(OloEngine PRIVATE
	${zlib_SOURCE_DIR}
	${zlib_BINARY_DIR}
)

# Cross-platform link libraries
target_link_libraries(OloEngine
	assimp
	bc7enc
	bcdec
	box2d
	choc
	Detour
	DetourCrowd
	glad
	glfw
	imgui
	imguizmo
	Jolt
	meshoptimizer
	nlohmann_json::nlohmann_json
	Recast
	sol2
	spdlog
	stb_image
	yaml-cpp
	GameNetworkingSockets_s
	Vulkan::Vulkan
	zlibstatic
)

# FFmpeg video backend (optional). vendor/ffmpeg.cmake defines the `ffmpeg` INTERFACE
# target (includes + import libs) and builds FFmpeg from source when OLO_VIDEO_FFMPEG=ON;
# the compile def flips on FFmpegBackend.cpp so VideoDecoder can decode H.264/MP4/etc.
if(TARGET ffmpeg)
	target_link_libraries(OloEngine ffmpeg)
	target_compile_definitions(OloEngine PUBLIC OLO_VIDEO_FFMPEG=1)
endif()

# Tracy — only link the client library when profiling is on. Tracy's TracyClient.cpp
# contains file-scope globals (Profiler, _memory_span_cache) whose constructors fire
# unconditionally if the lib is linked, regardless of the TRACY_ENABLE compile def.
# The TSan preset turns the option off so those globals never make it into the binary
# (avoids tracy_rpmalloc.cpp's static-init-order race that TSan halts on).
if(TRACY_ENABLE)
	target_link_libraries(OloEngine TracyClient)
endif()

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

	# Resolve debug/release library paths — CI headless SDK installs may lack debug libs.
	set(_vk_lib "$ENV{VULKAN_SDK}/Lib")
	foreach(_base shaderc_shared spirv-cross-core spirv-cross-glsl)
		set(_dbg "${_vk_lib}/${_base}d.lib")
		set(_rel "${_vk_lib}/${_base}.lib")
		if(NOT EXISTS "${_rel}")
			message(FATAL_ERROR
				"Vulkan SDK release library not found at '${_rel}'. Install / repair "
				"the Vulkan SDK from https://vulkan.lunarg.com/sdk/home and make sure "
				"the shaderc_shared + spirv-cross-{core,glsl} components are present.")
		endif()
		if(EXISTS "${_dbg}")
			list(APPEND _vk_shader_libs debug "${_dbg}" optimized "${_rel}")
		else()
			message(STATUS "Vulkan SDK debug lib not found (${_base}d.lib), using release for all configs")
			list(APPEND _vk_shader_libs "${_rel}")
		endif()
	endforeach()

	# Windows: link from Vulkan SDK + CMake imported targets
	target_link_libraries(OloEngine
		Vulkan::glslang
		Vulkan::SPIRV-Tools
		${_vk_shader_libs}
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
	foreach(_lib SHADERC_LIB SPIRV_CROSS_CORE_LIB SPIRV_CROSS_GLSL_LIB GLSLANG_LIB SPIRV_LIB SPIRV_TOOLS_LIB
			MACHINE_INDEPENDENT_LIB OS_DEPENDENT_LIB GENERIC_CODEGEN_LIB)
		if(NOT ${_lib})
			message(FATAL_ERROR "Required shader library not found: ${_lib}. "
				"Install the Vulkan SDK / shader development packages.")
		endif()
	endforeach()
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
	target_compile_definitions(OloEngine PUBLIC OLO_ENABLE_CSHARP_SCRIPTING=1)
else()
	target_compile_definitions(OloEngine PUBLIC OLO_ENABLE_CSHARP_SCRIPTING=0)
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
	)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(OloEngine
		Threads::Threads
		${CMAKE_DL_LIBS}
	)

	# std::stacktrace runtime. Platform/OpenGL/OpenGLDebug.cpp calls
	# std::stacktrace::current() behind __has_include(<stacktrace>). On GCC's
	# libstdc++ (GCC 12+) that implementation lives in a SEPARATE archive,
	# libstdc++exp.a, which is NOT linked by default — so on Linux/Clang (the
	# sanitizer CI) and GCC the OpenGLDebug.cpp.o references are undefined at
	# every executable link:
	#   ld.lld: error: undefined symbol: std::__stacktrace_impl::_S_current(...)
	#   ld.lld: error: undefined symbol: std::stacktrace_entry::_Info::_M_populate(...)
	# MSVC's STL resolves std::stacktrace via DbgHelp (linked in the MSVC branch
	# above), and libc++ gates the header out, so this is a libstdc++-only need.
	# check_linker_flag adds the lib only where it actually exists (a no-op on
	# libc++ / toolchains without it), and as an OloEngine link dep CMake orders
	# it after libOloEngine.a so the static-archive references resolve.
	include(CheckLinkerFlag)
	check_linker_flag(CXX "-lstdc++exp" OLO_HAVE_LIBSTDCXXEXP)
	if(OLO_HAVE_LIBSTDCXXEXP)
		target_link_libraries(OloEngine stdc++exp)
	endif()
endif()

# When fuzzing is enabled, instrument OloEngine with ASan/UBSan. Helper is
# defined in cmake/Fuzzing.cmake and is a no-op when OLO_ENABLE_FUZZING=OFF
# (so this is safe to call unconditionally). PUBLIC linkage of the sanitiser
# runtime libs propagates to downstream consumers (OloEditor / OloRuntime /
# OloServer / fuzz harnesses), which need the asan_dynamic.lib import lib at
# link time but don't need to be instrumented themselves.
olo_apply_fuzz_sanitizer(OloEngine)
