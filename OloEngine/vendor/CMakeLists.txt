include(FetchContent)
set(FETCHCONTENT_QUIET off)

# Set CMake policies to suppress warnings in vendor libraries
if(POLICY CMP0194)  
    cmake_policy(SET CMP0194 NEW)
endif()

message(STATUS "Downloading third party libraries using CMake's FetchContent and CPM")

# --- Vendor dependency pinning (#294: reproducible builds + supply-chain integrity) ---
# EVERY dependency below is pinned to a full commit SHA -- never a bare tag. A git tag
# (v3.1.1, etc.) is a MOVABLE pointer: the upstream maintainer, or anyone who compromises
# the account, can force-move it to a different commit, so a tag pin does not guarantee
# the bytes you fetch. A 40-char commit SHA is content-addressed and immutable, so it is
# both reproducible AND tamper-evident. A floating branch (GIT_TAG master/main/docking) is
# worse still -- it lets CI silently fetch a newer version than any dev with zero repo
# changes (exactly how the #281 flaky physics crash hid: CI and local ran different Jolt
# commits) and defeats build caching. Two comment conventions, both machine-read by
# Renovate (.github/renovate.json5) so a deliberate bump stays a one-line, reviewable diff:
#   * branch-tracked digest pins carry "# <branch> @ <date>" (SHA = that branch tip);
#   * release pins carry "# <tag>" (SHA = that tag's commit) -- the gold-standard form.
# NOTE: GIT_SHALLOW must be FALSE for a commit-SHA pin -- a shallow fetch only pulls
# branch tips and cannot resolve an arbitrary historical commit (see Jolt below).
FetchContent_Declare(
	assimp
	GIT_REPOSITORY	https://github.com/assimp/assimp.git
	GIT_TAG			f1de01d68eb224cf9f97ac3a3b1cad9aa92ec7ac  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	atomic_queue
	GIT_REPOSITORY	https://github.com/max0x7ba/atomic_queue.git
	GIT_TAG			e9724223814e59a5f2d1aa46ed3ff00754191ace  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	box2d
	GIT_REPOSITORY	https://github.com/erincatto/box2d.git
	GIT_TAG			8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3  # v3.1.1
	GIT_SHALLOW		FALSE
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
	GIT_TAG			73db193f853e2ee079bf3ca8a64aa2eaf6459043  # v2.0.8
	GIT_SHALLOW		FALSE
	SOURCE_SUBDIR	cmake
)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY	https://github.com/glfw/glfw.git
	GIT_TAG			7b6aead9fb88b3623e3b3725ebb42670cbe4c579  # 3.4
	GIT_SHALLOW		FALSE
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
	GIT_TAG f132c893119698e10daef8525d0ad7a3f05176f2  # main @ 2026-06-05
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
	GIT_TAG			9d9890c73011d75920af614485296d1e03e95448  # v1.2
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	recastnavigation
	GIT_REPOSITORY	https://github.com/recastnavigation/recastnavigation.git
	GIT_TAG			6dc1667f580357e8a2154c28b7867bea7e8ad3a7  # v1.6.0
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	spdlog
	GIT_REPOSITORY	https://github.com/gabime/spdlog.git
	GIT_TAG			79524ddd08a4ec981b7fea76afd08ee05f83755d  # v1.17.0
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
	tracy
	GIT_REPOSITORY	https://github.com/wolfpld/tracy.git
	GIT_TAG			05cceee0df3b8d7c6fa87e9638af311dbabc63cb  # v0.13.1
	GIT_SHALLOW		FALSE
	GIT_PROGRESS    TRUE
)
FetchContent_Declare(
	yaml-cpp
	GIT_REPOSITORY	https://github.com/jbeder/yaml-cpp.git
	GIT_TAG			881f6108e5c8d290d33acbba2b01a803b1142df5  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG da607da739fa6047df13e66a2af6b8bec7c2a498  # v1.3.2
	GIT_SHALLOW		FALSE
)

# --- libsodium (crypto backend for GameNetworkingSockets) ---
FetchContent_Declare(
	libsodium
	GIT_REPOSITORY	https://github.com/jedisct1/libsodium.git
	GIT_TAG			77e1ce5d6dee871c49ef211222ba18ef0c486bda  # 1.0.22
	GIT_SHALLOW		FALSE
)

# --- Protobuf (required by GameNetworkingSockets) ---
FetchContent_Declare(
	protobuf
	GIT_REPOSITORY	https://github.com/protocolbuffers/protobuf.git
	GIT_TAG			35cd01f9fe9afbeea38cc7b979a3b6bfcde82c03  # v35.1
	GIT_SHALLOW		FALSE
	EXCLUDE_FROM_ALL
)

# --- GameNetworkingSockets (Valve networking library) ---
FetchContent_Declare(
	GameNetworkingSockets
	GIT_REPOSITORY	https://github.com/ValveSoftware/GameNetworkingSockets.git
	GIT_TAG			f4525e39ee10f6b45181fd92d01fee75f7d71756  # master @ 2026-06-05
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

# Make MSVC vendor compiles cacheable when compiler caching is on (OLO_ENABLE_COMPILER_CACHE,
# cmake/CompilerCache.cmake). sccache/ccache cannot cache a /Zi compile that writes a separate
# PDB — it aborts with "failed to zip up compiler outputs ... failed to open <name>.pdb". These
# three libs hardcode /Zi themselves, so the global Embedded (/Z7) default doesn't reach them.
# Turn their debug-symbol/PDB generation off (only while caching) so their objects cache cleanly;
# CI/test Release binaries don't need vendor symbols. Local non-cache builds are unchanged.
if(OLO_ENABLE_COMPILER_CACHE AND MSVC)
	# assimp gates its `/Zi` on ASSIMP_INSTALL_PDB (assimp-src/CMakeLists.txt); Jolt on
	# GENERATE_DEBUG_SYMBOLS (joltphysics-src/Build/CMakeLists.txt). Disabling Jolt's also turns
	# its large object set from non-cacheable into cacheable — a real repeat-run win.
	set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "Disabled: separate PDB defeats compiler caching" FORCE)
	set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "Jolt: disabled, /Zi PDB defeats compiler caching" FORCE)
	# filewatch hardcodes `/Zi` with no option to disable it (filewatch-src/CMakeLists.txt), but
	# only its `filewatch_unit` test compiles — and that builds solely because the repo's global
	# BUILD_TESTS=ON collides with filewatch's own BUILD_TESTS option. Shadow it OFF with a normal
	# (non-cache) var so it applies to the FetchContent children below but not to OloEngine's own
	# BUILD_TESTS in the parent scope — the same trick used for GameNetworkingSockets below.
	set(BUILD_TESTS OFF)
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
	GIT_TAG 6029ee3789a2b7898f6423ec0c88cc4e5425f5a9  # docking @ 2026-06-05
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
	GIT_TAG fd138a7ce089cfcbfee16da8e8692f37a0e9481e  # master @ 2026-06-05
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
  GIT_TAG 62d899feac3cf9215a55f2b43da250fdd98d2156  # v0.50.1
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(cpp_httplib_ADDED)
	add_library(cpp_httplib INTERFACE)
	target_include_directories(cpp_httplib SYSTEM INTERFACE ${cpp_httplib_SOURCE_DIR})
endif()

# bc7enc_rdo: Rich Geldreich's BC1-7 / BC6H block-compression encoders + decoders.
# Used by the offline texture-compression cook (Renderer/TextureCompression.cpp) to
# encode base-color -> BC7 and normal maps -> BC5 (#440). Dual MIT / public-domain
# licensed for the core sources we use (bc7enc / rgbcx / bc7decomp); we deliberately
# skip bc7e.ispc (Apache + needs the ISPC compiler), lodepng, miniz, and the RDO
# post-processor — none are needed for a straight CPU encode. DOWNLOAD_ONLY because
# the repo's own CMakeLists builds a test exe that requires ISPC; we compile just the
# three self-contained TUs into our own static lib, mirroring the imgui / lua pattern.
# Pinned to a fixed commit SHA for reproducible builds (#294 convention).
CPMAddPackage(NAME bc7enc_rdo
  GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
  GIT_TAG dbe416d28a5530b4e8cc45b14bf034dc6b96bbde  # master @ 2026-02-27
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(bc7enc_rdo_ADDED)
	add_library(bc7enc STATIC
		"${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
		"${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp"
		"${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp")
	# SYSTEM so the encoder's own warnings don't count against OloEngine's /W4-ish flags.
	target_include_directories(bc7enc SYSTEM PUBLIC "${bc7enc_rdo_SOURCE_DIR}")
	set_target_properties(bc7enc PROPERTIES FOLDER "Utilities/bc7enc"
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/bc7enc-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/bc7enc-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/bc7enc-build")
	# clang-cl maps our /WX onto -Werror; the vendored encoder trips a raft of
	# Clang-only warnings (unused, sign-compare, ...). Suppress -Werror on it only,
	# same treatment already applied to Jolt / assimp / box2d below.
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(bc7enc PRIVATE -Wno-error -Wno-everything)
	endif()
endif()

# bcdec: iOrange's single-header BC1-7 + BC6H block DECODER. Dual MIT / Unlicense
# (public domain) — same permissive footing as stb. bc7enc_rdo ships no BC6H encoder,
# so the offline cook (Renderer/TextureCompression.cpp) gained a from-scratch BC6H
# mode-11 encoder (#440); bcdec is used to (a) INDEPENDENTLY validate that encoder in
# CI — a matched encoder/decoder bug can't hide behind a shared codepath — and (b) act
# as the CPU HDR fallback that decodes BC6H to RGBA16F on a context lacking BPTC.
# Header-only: the implementation is compiled exactly once, via BCDEC_IMPLEMENTATION in
# TextureCompression.cpp. DOWNLOAD_ONLY + INTERFACE, mirroring the choc/stb pattern.
# Pinned to a fixed commit SHA for reproducible builds (#294 convention).
CPMAddPackage(NAME bcdec
  GIT_REPOSITORY https://github.com/iOrange/bcdec.git
  GIT_TAG 93628fe5627102fe5187b7eeb99122dec6612c36  # main @ 2025-10-23
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(bcdec_ADDED)
	add_library(bcdec INTERFACE)
	# SYSTEM so the header's own warnings — and the BCDEC_IMPLEMENTATION expansion it
	# emits into TextureCompression.cpp — don't count against OloEngine's /W4 /WX flags,
	# exactly as stb_image's IMPLEMENTATION is handled.
	target_include_directories(bcdec SYSTEM INTERFACE "${bcdec_SOURCE_DIR}")
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
				"#define SODIUM_VERSION_STRING \"1.0.22\"\n"
				"#define SODIUM_LIBRARY_VERSION_MAJOR 26\n"
				"#define SODIUM_LIBRARY_VERSION_MINOR 3\n"
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

# ClangCL + ASan: do NOT instrument the vendored protobuf tool-chain. protoc is a
# build-time code generator (it runs during the build to emit GNS's .pb.cc), not
# something we test. Under clang's ASan an instrumented protoc aborts mid-build on
# findings inside vendored code we can't patch: an ODR false positive / global-buffer-
# overflow in protobuf itself, and — after protobuf pulled in abseil + upb — a SEGV
# inside abseil's swiss-table (absl/container/internal/container_memory.h) while
# building the descriptor pool. protoc must be instrumentation-CONSISTENT, so it's not
# enough to de-instrument the four core protobuf targets (as we first did): every
# target it links must match, or the mismatch crashes it. De-instrument protoc's WHOLE
# dependency closure by source tree — protobuf (which also contains upb + utf8_range)
# and abseil — so a protobuf bump that adds targets stays covered automatically. The
# only cost is the test exe links a non-instrumented protobuf/abseil — third-party code
# out of our ASan scope anyway (the engine, GNS, and the tests stay instrumented).
# Target-level -fno-sanitize=address overrides the directory-inherited /fsanitize=address
# that cmake/Sanitizers.cmake applies (for clang the last -f{no-,}sanitize flag wins).
if(OLO_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	# Recursively gather every target defined under a directory subtree.
	function(olo_collect_subtree_targets _dir _out)
		get_property(_acc DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		get_property(_subs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
		foreach(_sub ${_subs})
			olo_collect_subtree_targets("${_sub}" _sub_out)
			list(APPEND _acc ${_sub_out})
		endforeach()
		set(${_out} "${_acc}" PARENT_SCOPE)
	endfunction()

	set(_asan_exclude_targets "")
	foreach(_tree_dir "${protobuf_SOURCE_DIR}" "${absl_SOURCE_DIR}")
		if(_tree_dir AND IS_DIRECTORY "${_tree_dir}")
			olo_collect_subtree_targets("${_tree_dir}" _tree_targets)
			list(APPEND _asan_exclude_targets ${_tree_targets})
		endif()
	endforeach()
	if(_asan_exclude_targets)
		list(REMOVE_DUPLICATES _asan_exclude_targets)
	else()
		# The collector found nothing — protobuf/abseil would stay instrumented and
		# protoc would abort mid-build (the very failure this block exists to prevent).
		# Fail loudly at configure rather than let that recur silently.
		message(FATAL_ERROR
			"ASan de-instrumentation found no protobuf/abseil targets under "
			"protobuf_SOURCE_DIR='${protobuf_SOURCE_DIR}' / absl_SOURCE_DIR='${absl_SOURCE_DIR}'. "
			"protoc would be built instrumented and crash generating .pb files.")
	endif()

	foreach(_t ${_asan_exclude_targets})
		get_target_property(_t_type ${_t} TYPE)
		# Compile options only apply to targets that actually compile sources.
		if(_t_type AND NOT _t_type STREQUAL "INTERFACE_LIBRARY" AND NOT _t_type STREQUAL "UTILITY")
			target_compile_options(${_t} PRIVATE -fno-sanitize=address)
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
# box2d dropped its bundled `simde` SIMD-portability target in v3.1 (it now carries
# its own SIMD abstraction), so guard the IDE-folder assignment.
if(TARGET simde)
	set_target_properties(simde PROPERTIES FOLDER "Utilities/box2d")
endif()
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
# zlib v1.3.2 gates its `example`/`minigzip` test programs behind ZLIB_BUILD_EXAMPLES
# (and no longer defines them in our static-only config), so guard those two.
if(TARGET example)
	set_target_properties(example PROPERTIES FOLDER "Utilities/zlib")
endif()
if(TARGET minigzip)
	set_target_properties(minigzip PROPERTIES FOLDER "Utilities/zlib")
endif()
if(TARGET zlib)
	set_target_properties(zlib PROPERTIES FOLDER "Utilities/zlib")
endif()
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

	# ClangCL: assimp ships a version resource (code/res/assimp.rc). Building it
	# routes through cmcldeps, which runs clang-cl to scan the .rc's #includes and
	# passes the RC flags along — including the toolchain's `/C 1252` code-page flag
	# (ClangCLToolchain.cmake). clang-cl reads `/C` as its own "keep comments" option
	# (which takes no argument) and then treats `1252` as an input file, failing with
	# "no such file or directory: '1252'". assimp is a static lib and doesn't need a
	# version resource, so strip the .rc source — the same fix already applied to the
	# protobuf targets above.
	if(TARGET assimp)
		get_target_property(_assimp_srcs assimp SOURCES)
		if(_assimp_srcs)
			list(FILTER _assimp_srcs EXCLUDE REGEX "\\.rc$")
			set_target_properties(assimp PROPERTIES SOURCES "${_assimp_srcs}")
		endif()
	endif()
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
