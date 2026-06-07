include(FetchContent)
set(FETCHCONTENT_QUIET off)

# Set CMake policies to suppress warnings in vendor libraries
if(POLICY CMP0194)  
    cmake_policy(SET CMP0194 NEW)
endif()

message(STATUS "Downloading third party libraries using CMake's FetchContent and CPM")

# --- Vendor dependency pinning (#294: reproducible builds) ---
# Every dependency below is pinned to a fixed commit. A floating branch (GIT_TAG
# master/main/docking) lets CI silently fetch a newer version than any dev with zero
# repo changes -- exactly how the #281 flaky physics crash hid (CI and local ran
# different Jolt commits) -- and it defeats build caching. The SHAs are the branch
# tips as of 2026-06-05 (the known-good versions built against today); each keeps a
# trailing "# <branch> @ <date>" so a deliberate bump is a one-line, reviewable change.
# NOTE: GIT_SHALLOW must be FALSE for a commit-SHA pin -- a shallow fetch only pulls
# branch tips and cannot resolve an arbitrary historical commit (see Jolt below).
FetchContent_Declare(
	assimp
	GIT_REPOSITORY	https://github.com/assimp/assimp.git
	GIT_TAG			ace8c310f5d7452d7ef1e19e81957513326ccd07  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	atomic_queue
	GIT_REPOSITORY	https://github.com/max0x7ba/atomic_queue.git
	GIT_TAG			705fe1acf5a0207ad0b2a24ac59d211b91e151a7  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	box2d
	GIT_REPOSITORY	https://github.com/erincatto/box2d.git
	GIT_TAG			v3.0.0
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	entt
	GIT_REPOSITORY	https://github.com/skypjack/entt.git
	GIT_TAG			1333fa53129e7cfded5a9640c4336a254049917b  # main @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	filewatch
	GIT_REPOSITORY	https://github.com/drsnuggles8/filewatch.git
	GIT_TAG			2b24bd3bd513f7a6c9c9c566858a48a6124ad5af  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)

FetchContent_Declare(
	glad
	GIT_REPOSITORY	https://github.com/Dav1dde/glad.git
	GIT_TAG			v2.0.8
	GIT_SHALLOW		TRUE
	SOURCE_SUBDIR	cmake
)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY	https://github.com/glfw/glfw.git
	GIT_TAG			3.4
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 6f14f4792a0cde5d0cf2c910506724d61cb95834  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG 7140cd416cecd7462a8aae488024abeee55598e4  # main @ 2026-06-05
	GIT_SHALLOW		FALSE
)
# Pin Jolt to a fixed commit (not a floating branch). A floating `GIT_TAG master`
# makes builds non-deterministic — CI can fetch a newer Jolt than any dev with zero
# repo changes, which is exactly how the #281 flaky physics crash hid (CI and local
# ran different Jolt commits). We can't pin to a release tag: the engine uses
# JPH::CharacterContact (JoltCharacterController.h), a CharacterVirtual contact-listener
# type added after the latest release (v5.5.0). 6cef2a22 is the master HEAD as of
# 2026-06-01 that 100/100 Release runs pass against locally. Bump deliberately after
# re-testing the JoltScene/JoltJobSystemAdapter integration.
# NOTE: GIT_SHALLOW must be FALSE when GIT_TAG is a commit SHA — a shallow fetch only
# pulls branch tips, so it can't resolve an arbitrary historical commit.
FetchContent_Declare(
	joltphysics
	GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
	GIT_TAG 6cef2a2214741a77fff8771e233c84da6b875ed5
	GIT_SHALLOW		FALSE
	SOURCE_SUBDIR Build
)
FetchContent_Declare(
	miniaudio
	GIT_REPOSITORY https://github.com/mackron/miniaudio.git
	GIT_TAG 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)

# pl_mpeg: single-file, public-domain MPEG-1 video + MP2 audio decoder.
# Header-only (one plmpeg.h; PL_MPEG_IMPLEMENTATION is defined in Video/PlMpeg.cpp).
# The repo ships no CMakeLists.txt, so MakeAvailable just populates the source dir
# and we add it to OloEngine's include path manually (mirrors the miniaudio pattern).
FetchContent_Declare(
	pl_mpeg
	GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
	GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)

FetchContent_Declare(
	meshoptimizer
	GIT_REPOSITORY	https://github.com/zeux/meshoptimizer.git
	GIT_TAG			v1.1
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	recastnavigation
	GIT_REPOSITORY	https://github.com/recastnavigation/recastnavigation.git
	GIT_TAG			v1.6.0
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	spdlog
	GIT_REPOSITORY	https://github.com/gabime/spdlog.git
	GIT_TAG			v1.15.1
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	tracy
	GIT_REPOSITORY	https://github.com/wolfpld/tracy.git
	GIT_TAG			v0.11.1
	GIT_SHALLOW		TRUE
	GIT_PROGRESS    TRUE
)
FetchContent_Declare(
	yaml-cpp
	GIT_REPOSITORY	https://github.com/jbeder/yaml-cpp.git
	GIT_TAG			2decf96e915d2b0c26c68c1659665789dfef2633  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
	GIT_SHALLOW		TRUE
)

# --- libsodium (crypto backend for GameNetworkingSockets) ---
FetchContent_Declare(
	libsodium
	GIT_REPOSITORY	https://github.com/jedisct1/libsodium.git
	GIT_TAG			1.0.20-RELEASE
	GIT_SHALLOW		TRUE
)

# --- Protobuf (required by GameNetworkingSockets) ---
FetchContent_Declare(
	protobuf
	GIT_REPOSITORY	https://github.com/protocolbuffers/protobuf.git
	GIT_TAG			v3.21.12
	GIT_SHALLOW		TRUE
	EXCLUDE_FROM_ALL
)

# --- GameNetworkingSockets (Valve networking library) ---
FetchContent_Declare(
	GameNetworkingSockets
	GIT_REPOSITORY	https://github.com/ValveSoftware/GameNetworkingSockets.git
	GIT_TAG			d67d1673abb1a3c00dfa9e62344860e15c741c1d  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
	EXCLUDE_FROM_ALL
)

FetchContent_GetProperties(zlib)
FetchContent_MakeAvailable(zlib)

# Tell assimp (and any other consumer using find_package(ZLIB)) to use our
# fetched zlibstatic target instead of searching system paths.
set(ZLIB_FOUND TRUE)
set(ZLIB_LIBRARIES zlibstatic)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_BUILD_ZLIB OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES Off CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS Off CACHE BOOL "" FORCE)
set(GLFW_INSTALL Off CACHE BOOL "" FORCE)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BuildTests OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
# Tracy: profiling enabled by default. The leading `set(... OFF)` would have
# established a cache var that the following `option()` would then no-op on,
# leaving Tracy disabled — the single `option()` below is the source of truth.
option(TRACY_ENABLE "Enable profiling" ON)
set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

# Recast/Detour configuration - build as static library, disable demos/tests
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Jolt Physics configuration - enable static linking and cross-platform determinism
set(JPH_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(JPH_CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
# Enable C++ RTTI in Jolt — our engine uses dynamic_cast/typeid on Jolt shape types
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
# Disable Jolt Vulkan compute shaders on Linux — requires dxc which is not readily available
if(NOT MSVC)
	set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
endif()


FetchContent_MakeAvailable(assimp atomic_queue box2d entt filewatch glad glfw glm googletest joltphysics meshoptimizer miniaudio recastnavigation spdlog tracy yaml-cpp)

# Fix Jolt's INTERFACE_INCLUDE_DIRECTORIES to remove absolute Vulkan SDK paths
# This is a workaround for CMake's strict policy against source-dir absolute paths in interface directories
if(TARGET Jolt)
    get_target_property(jolt_interface_dirs Jolt INTERFACE_INCLUDE_DIRECTORIES)
    if(jolt_interface_dirs)
        list(FILTER jolt_interface_dirs EXCLUDE REGEX "VULKAN_SDK|Vulkan")
        set_target_properties(Jolt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${jolt_interface_dirs}")
    endif()
endif()

# atomic_queue is header-only - create an interface target for easy include
add_library(atomic_queue_interface INTERFACE)
target_include_directories(atomic_queue_interface INTERFACE ${atomic_queue_SOURCE_DIR}/include)
set(ATOMIC_QUEUE_INCLUDE_DIR "${atomic_queue_SOURCE_DIR}/include" CACHE INTERNAL "")

# Tracy's own macros — only define when the CMake option is on. With this off
# the entire Profiler ctor (and its worker threads) compile out, which avoids
# the static-init race that TSan trips on (see CMakePresets.json linux-tsan).
if(TRACY_ENABLE)
    target_compile_definitions("TracyClient" PRIVATE $<$<CONFIG:Release>:TRACY_ENABLE>)
endif()

glad_add_library(glad STATIC API gl:core=4.6 LOCATION ${PROJECT_SOURCE_DIR}/vendor/glad-build/${TARGET})

FetchContent_Declare(stb_image
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4  # master @ 2026-06-05
	GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(stb_image)
add_library(stb_image INTERFACE)

# pl_mpeg has no CMakeLists, so MakeAvailable only populates ${pl_mpeg_SOURCE_DIR}.
FetchContent_MakeAvailable(pl_mpeg)
file(COPY ${stb_image_SOURCE_DIR}/stb_image.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
file(COPY ${stb_image_SOURCE_DIR}/stb_image_write.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
file(COPY ${stb_image_SOURCE_DIR}/stb_truetype.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
set(STBIMAGE_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/stb_image-build")

CPMAddPackage(NAME imgui
	GITHUB_REPOSITORY ocornut/imgui
	GIT_TAG 2af6dd9694288e6befe1edb7ce25510911693c22  # docking @ 2026-06-05
	GIT_SHALLOW FALSE
	DOWNLOAD_ONLY YES)
if(imgui_ADDED)
	FILE(GLOB imgui_sources ${imgui_SOURCE_DIR}/*.cpp)
	file(GLOB IMGUI_HEADERS ${IMGUI_PATH}/*.h)
	FILE(GLOB imgui_sources_cpp ${imgui_SOURCE_DIR}/misc/cpp/*.cpp)

	set(IMGUI_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_INCLUDE_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_SOURCES ${imgui_sources} ${imgui_sources_cpp})

	add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS} "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp" "${IMGUI_DIR}/backends/imgui_impl_opengl3.h" "${IMGUI_DIR}/backends/imgui_impl_opengl3_loader.h")
	target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD=1)
	target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${glfw_SOURCE_DIR}/include "glad-build/include")
	set_target_properties(imgui PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build")
endif()

CPMAddPackage(NAME imguizmo
	GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
	GIT_TAG be8aa4aeab86b402701c8c1df011bd8cd776760b  # master @ 2026-06-05
	GIT_SHALLOW FALSE
	DOWNLOAD_ONLY YES)
if (imguizmo_ADDED)
	# As of tag 1.9 the repository moved sources into a src/ subdirectory.
	# Fall back to the root for older checkouts to stay forward-compatible.
	if (EXISTS "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
		set(IMGUIZMO_SRC_DIR "${imguizmo_SOURCE_DIR}/src")
	else()
		set(IMGUIZMO_SRC_DIR "${imguizmo_SOURCE_DIR}")
	endif()
	file(GLOB IMGUIZMO_SOURCES ${IMGUIZMO_SRC_DIR}/*.cpp)
	file(GLOB IMGUIZMO_HEADERS ${IMGUIZMO_SRC_DIR}/*.h)
	add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
	target_include_directories(imguizmo PUBLIC ${IMGUIZMO_SRC_DIR} ${imgui_SOURCE_DIR})
	set_target_properties(imguizmo	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build")
endif()

CPMAddPackage(NAME lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  VERSION 5.4.7
  DOWNLOAD_ONLY YES)  
if(lua_ADDED)
  file(GLOB lua_sources ${lua_SOURCE_DIR}/*.c)
  list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c" "${lua_SOURCE_DIR}/onelua.c" "${lua_SOURCE_DIR}/ltests.c")
  add_library(lua STATIC ${lua_sources})
  target_include_directories(lua SYSTEM PUBLIC $<BUILD_INTERFACE:${lua_SOURCE_DIR}>)
  set_target_properties(lua	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build")
endif()

# cpp-httplib: single-header HTTP/1.1 server+client. Used by OloEditor's read-only
# MCP diagnostics server (#285) for the localhost Streamable-HTTP transport. Header-only,
# so DOWNLOAD_ONLY + an INTERFACE target whose include dir propagates. Non-imported
# INTERFACE (not INTERFACE IMPORTED) so the target is global and can be linked from
# OloEditor (the actual consumer; see OloEditor/CMakeLists.txt) and any other target.
# Pinned to a fixed tag SHA for reproducible builds (#294 convention).
CPMAddPackage(NAME cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG 5304464a53f793f8e3e4777ea5941d60cf8c91e3  # v0.29.0
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(cpp_httplib_ADDED)
	add_library(cpp_httplib INTERFACE)
	target_include_directories(cpp_httplib SYSTEM INTERFACE ${cpp_httplib_SOURCE_DIR})
endif()

set_target_properties(assimp PROPERTIES FOLDER "Utilities/assimp")
if(TARGET UpdateAssimpLibsDebugSymbolsAndDLLs)
	set_target_properties(UpdateAssimpLibsDebugSymbolsAndDLLs PROPERTIES FOLDER "Utilities/assimp")
endif()

# --- Build libsodium (must be before GNS) ---
FetchContent_GetProperties(libsodium)
if(NOT libsodium_POPULATED)
	FetchContent_Populate(libsodium)
	# libsodium doesn't have a root CMakeLists.txt; build manually from sources
	file(GLOB_RECURSE SODIUM_SOURCES "${libsodium_SOURCE_DIR}/src/libsodium/*.c")
	add_library(sodium STATIC ${SODIUM_SOURCES})
	target_include_directories(sodium PUBLIC
		"${libsodium_SOURCE_DIR}/src/libsodium/include"
		"${libsodium_SOURCE_DIR}/src/libsodium/include/sodium"
	)
	# Platform-specific configuration header
	if(MSVC)
		target_compile_definitions(sodium PRIVATE
			SODIUM_STATIC=1
			SODIUM_EXPORT=
			CONFIGURED=1
			NATIVE_LITTLE_ENDIAN=1
			_CRT_SECURE_NO_WARNINGS
		)
		target_include_directories(sodium PRIVATE
			"${libsodium_SOURCE_DIR}/builds/msvc"
		)
		# Copy the MSVC version.h to where libsodium expects it
		configure_file(
			"${libsodium_SOURCE_DIR}/builds/msvc/version.h"
			"${libsodium_SOURCE_DIR}/src/libsodium/include/sodium/version.h"
			COPYONLY
		)
	else()
		target_compile_definitions(sodium PRIVATE
			SODIUM_STATIC=1
			SODIUM_EXPORT=
			CONFIGURED=1
			HAVE_TI_MODE=1
			NATIVE_LITTLE_ENDIAN=1
		)
		# Generate version.h for non-MSVC builds (autotools would normally do this)
		if(NOT EXISTS "${libsodium_SOURCE_DIR}/src/libsodium/include/sodium/version.h")
			file(WRITE "${libsodium_SOURCE_DIR}/src/libsodium/include/sodium/version.h"
				"#ifndef sodium_version_H\n"
				"#define sodium_version_H\n"
				"#define SODIUM_VERSION_STRING \"1.0.20\"\n"
				"#define SODIUM_LIBRARY_VERSION_MAJOR 26\n"
				"#define SODIUM_LIBRARY_VERSION_MINOR 2\n"
				"#define SODIUM_LIBRARY_MINIMAL 0\n"
				"#endif\n"
			)
		endif()
	endif()
	target_compile_definitions(sodium PUBLIC SODIUM_STATIC=1)
endif()
# Bypass GNS's Findsodium.cmake: mark sodium as found and point at our target.
# GNS's module unsets cached variables and runs find_library which fails at
# configure time (the .a doesn't exist yet). Instead, we provide our own
# minimal Findsodium that recognises the already-defined 'sodium' target.
set(sodium_FOUND TRUE CACHE BOOL "" FORCE)
set(sodium_INCLUDE_DIR "${libsodium_SOURCE_DIR}/src/libsodium/include" CACHE PATH "" FORCE)
# Write a tiny Findsodium override into the build tree so find_package(sodium)
# short-circuits when GNS calls it.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cmake_override/Findsodium.cmake"
"# Auto-generated override — the 'sodium' target was already built by FetchContent.\n"
"if(TARGET sodium)\n"
"  set(sodium_FOUND TRUE)\n"
"  set(sodium_INCLUDE_DIR \"${libsodium_SOURCE_DIR}/src/libsodium/include\")\n"
"  return()\n"
"endif()\n"
)
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_BINARY_DIR}/cmake_override")

# --- Build Protobuf (must be before GNS) ---
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(protobuf)

# ClangCL: Protobuf leaks C++ flags (/utf-8, /wd4xxx, /bigobj) into RC
# compilation via $FLAGS, which rc.exe doesn't understand.  Static libs
# don't need version resources, so just strip the .rc sources.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	foreach(_proto_target libprotobuf libprotobuf-lite libprotoc protoc)
		if(TARGET ${_proto_target})
			get_target_property(_srcs ${_proto_target} SOURCES)
			list(FILTER _srcs EXCLUDE REGEX "\\.rc$")
			set_target_properties(${_proto_target} PROPERTIES SOURCES "${_srcs}")
		endif()
	endforeach()
endif()

# Make protobuf_generate_cpp available (from CMake's built-in FindProtobuf)
# GNS calls find_package(Protobuf) which needs this function
set(Protobuf_FOUND TRUE CACHE BOOL "" FORCE)
set(PROTOBUF_FOUND TRUE CACHE BOOL "" FORCE)
if(TARGET protobuf::protoc)
	get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
	if(NOT Protobuf_PROTOC_EXECUTABLE)
		set(Protobuf_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>")
	endif()
endif()
set(Protobuf_PROTOC_EXECUTABLE "${Protobuf_PROTOC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
set(Protobuf_INCLUDE_DIRS "${protobuf_SOURCE_DIR}/src" CACHE PATH "" FORCE)

# --- Build GameNetworkingSockets ---
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)

# Use normal (non-cache) variable to shadow BUILD_TESTS for GNS only.
# GNS checks BUILD_TESTS; a normal var shadows the cache in child scope
# without corrupting the root-level option.
set(BUILD_TESTS OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(USE_CRYPTO "libsodium" CACHE STRING "" FORCE)
set(USE_CRYPTO25519 "libsodium" CACHE STRING "" FORCE)
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
# Skip GNS install rules to avoid export-set errors with FetchContent targets
set(_OLD_SKIP_INSTALL_RULES ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES TRUE)
FetchContent_MakeAvailable(GameNetworkingSockets)
set(CMAKE_SKIP_INSTALL_RULES ${_OLD_SKIP_INSTALL_RULES})

# Organize new vendor targets into IDE folders
if(TARGET sodium)
	set_target_properties(sodium PROPERTIES FOLDER "Utilities/libsodium")
endif()
if(TARGET protobuf::libprotobuf OR TARGET libprotobuf)
	if(TARGET libprotobuf)
		set_target_properties(libprotobuf PROPERTIES FOLDER "Utilities/protobuf")
	endif()
	if(TARGET protoc)
		set_target_properties(protoc PROPERTIES FOLDER "Utilities/protobuf")
	endif()
	if(TARGET libprotobuf-lite)
		set_target_properties(libprotobuf-lite PROPERTIES FOLDER "Utilities/protobuf")
	endif()
	if(TARGET libprotoc)
		set_target_properties(libprotoc PROPERTIES FOLDER "Utilities/protobuf")
	endif()
endif()
if(TARGET GameNetworkingSockets_s)
	set_target_properties(GameNetworkingSockets_s PROPERTIES FOLDER "Utilities/GNS")
endif()
if(TARGET GameNetworkingSockets)
	set_target_properties(GameNetworkingSockets PROPERTIES FOLDER "Utilities/GNS")
endif()

set_target_properties(assimp PROPERTIES FOLDER "Utilities/assimp")
set_target_properties(box2d PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(simde PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(glfw PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(glm PROPERTIES FOLDER "Utilities")
set_target_properties(update_mappings PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(gmock PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gmock_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(Jolt PROPERTIES FOLDER "Utilities/JoltPhysics")
set_target_properties(miniaudio PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(meshoptimizer PROPERTIES FOLDER "Utilities/meshoptimizer")
set_target_properties(miniaudio_channel_combiner_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_channel_separator_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_ltrim_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_reverb_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_vocoder_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(lua PROPERTIES FOLDER "Utilities/Lua")
set_target_properties(spdlog PROPERTIES FOLDER "Utilities")
set_target_properties(TracyClient PROPERTIES FOLDER "Utilities")
set_target_properties(yaml-cpp PROPERTIES FOLDER "Utilities")
set_target_properties(example PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(minigzip PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(zlib PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(zlibstatic PROPERTIES FOLDER "Utilities/zlib")

# ClangCL compatibility: Vendor libraries (Jolt, assimp, box2d) use /Wall /WX,
# which maps to -Weverything -Werror on Clang—triggering dozens of Clang-only
# warnings (padded, nrvo, unique-object-duplication, etc.) absent from MSVC's /Wall.
# Append -Wno-error AFTER target options so it overrides /WX.
# Also suppress -Wpadded to avoid thousands of noise warnings that flood output.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	foreach(_vendor_target Jolt assimp box2d)
		if(TARGET ${_vendor_target})
			target_compile_options(${_vendor_target} PRIVATE -Wno-error -Wno-padded)
		endif()
	endforeach()
endif()

#======================================
# Propagate FetchContent _SOURCE_DIR / _BINARY_DIR variables to parent scope
# so OloEngine/CMakeLists.txt can use them in target_include_directories.
foreach(_dep assimp atomic_queue box2d entt filewatch glad glfw glm googletest
             joltphysics meshoptimizer miniaudio pl_mpeg recastnavigation spdlog tracy yaml-cpp
             zlib stb_image imgui imguizmo lua GameNetworkingSockets)
	if(DEFINED ${_dep}_SOURCE_DIR)
		set(${_dep}_SOURCE_DIR "${${_dep}_SOURCE_DIR}" PARENT_SCOPE)
	endif()
	if(DEFINED ${_dep}_BINARY_DIR)
		set(${_dep}_BINARY_DIR "${${_dep}_BINARY_DIR}" PARENT_SCOPE)
	endif()
endforeach()

# FFmpeg (optional) — built from source for the H.264/MP4 video backend. Off by default.
# (Lives in the tracked cmake/ dir because OloEngine/vendor/* is gitignored.)
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)

message(STATUS "Fetching thirdparty libraries done")
