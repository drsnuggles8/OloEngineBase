# =============================================================================
# Third-party dependencies.
#
# Since issue #773 most dependencies are PREBUILT by vcpkg from the manifest at
# the repo root (vcpkg.json) and are resolved here with find_package(). The
# machine-global, ABI-hash-keyed binary cache means the first worktree to build a
# given (port, features, triplet, toolchain) populates it and every other
# worktree restores prebuilt archives in seconds.
#
# What that bought, and what it cost, is written up in
# docs/agent-rules/vcpkg-dependency-management.md. Read it before adding,
# removing or version-bumping anything here.
#
# WHAT IS STILL FETCHED, AND WHY. Each of these has a concrete reason it cannot
# come from the registry; none of them is "we didn't get round to it". Re-check
# the reason before moving one — do not just try it and see.
#
#   sol2        Header-only, so a port buys no compile caching, while the port is
#               3.5.0 + a Lua-5.5 patch and we track a newer develop pin. The
#               SOL_ALL_INTEGER_VALUES_FIT u64 contract (issue #643) is load-bearing
#               and hard-won. (Declared in OloEngine/CMakeLists.txt.)
#   lua         The registry baseline is 5.5.1; we pin 5.4.7 because that is what
#               our sol2 pin is tested against. A version override to 5.4.7 needs
#               registry git history that a shallow vcpkg clone does not carry.
#   stb         The registry port is stb@2024-07-29; our pin is ~2 years newer.
#               stb_image is an image DECODER fed untrusted files — silently
#               reverting two years of fixes there is not a build-system decision.
#               (vcpkg installs its own stb as an assimp dependency, but flat as
#               <stb_image.h>; we include <stb_image/stb_image.h>, so the two do
#               not collide.)
#   imgui       We compile a hand-picked backend set with
#               IMGUI_IMPL_OPENGL_LOADER_GLAD=1, which the port does not offer.
#   imguizmo    The port takes a hard dependency on the imgui PORT. With imgui
#               staying here that would link two different Dear ImGui copies into
#               one binary — a guaranteed ODR/symbol collision.
#   glad        The port is glad 1.x; we use 2.0.8's configure-time generator.
#   filewatch   Our own fork.
#   bc7enc_rdo  We compile 3 of its TUs and deliberately skip the ISPC encoder.
#   bcdec       Single header, no port.
#   xatlas      No port at the pinned registry baseline (only uvatlas exists,
#               which drags directxmath/directx-headers onto Linux). Two files,
#               compiled as a tiny static lib like bc7enc.
#   pl_mpeg     Single header, no port.
#   choc        Single header, no port. (Declared in OloEngine/CMakeLists.txt.)
#   FFmpeg      Optional, off on Linux, and already has its own install cache
#               (vendor/ffmpeg-install + scripts/build-ffmpeg.sh). The vcpkg port
#               is one of the registry's longest builds and would additionally
#               flip the runtime from DLLs to static, changing the exe staging
#               contract. See cmake/ffmpeg.cmake.
#   OpenUSD     Structurally impossible: the port opens with
#               vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY) and we need the
#               static-monolithic usd_m + /WHOLEARCHIVE. See cmake/vendor/OpenUSD.cmake.
#
# --- Pinning discipline for what is still fetched (#294) -----------------------
# EVERY FetchContent/CPM dependency below is pinned to a full commit SHA -- never a
# bare tag. A git tag (v3.1.1, etc.) is a MOVABLE pointer: the upstream maintainer,
# or anyone who compromises the account, can force-move it to a different commit, so
# a tag pin does not guarantee the bytes you fetch. A 40-char commit SHA is
# content-addressed and immutable, so it is both reproducible AND tamper-evident. A
# floating branch (GIT_TAG master/main/docking) is worse still -- it lets CI silently
# fetch a newer version than any dev with zero repo changes (exactly how the #281
# flaky physics crash hid: CI and local ran different Jolt commits) and defeats build
# caching. Two comment conventions, both machine-read by Renovate
# (.github/renovate.json5) so a deliberate bump stays a one-line, reviewable diff:
#   * branch-tracked digest pins carry "# <branch> @ <date>" (SHA = that branch tip);
#   * release pins carry "# <tag>" (SHA = that tag's commit) -- the gold-standard form.
# NOTE: GIT_SHALLOW must be FALSE for a commit-SHA pin -- a shallow fetch only pulls
# branch tips and cannot resolve an arbitrary historical commit.
#
# The vcpkg side gets its reproducibility from vcpkg.json's "builtin-baseline",
# which pins the whole registry to one commit — the same lock, one line instead
# of thirty. Renovate tracks it via its own manager (see .github/renovate.json5).
# =============================================================================

include(FetchContent)
set(FETCHCONTENT_QUIET off)

# Set CMake policies to suppress warnings in vendor libraries
if(POLICY CMP0194)
    cmake_policy(SET CMP0194 NEW)
endif()

message(STATUS "Resolving vcpkg-provided dependencies")

# -----------------------------------------------------------------------------
# vcpkg-provided packages.
#
# REQUIRED throughout: a missing package here means the manifest install did not
# run or did not include the feature, and the useful failure is at configure —
# not two minutes later at link with an unresolved external.
#
# CMAKE_FIND_PACKAGE_TARGETS_GLOBAL is load-bearing, not tidiness. find_package()
# creates IMPORTED targets scoped to the CALLING directory and its children — and
# this file is a CHILD of OloEngine/, which is where the targets are consumed.
# Without this, every `target_link_libraries(OloEngine glm::glm)` one directory up
# fails to resolve (CMake treats the unknown name as a raw library and the error
# surfaces at LINK as "cannot open glm::glm.lib"). CMake 3.24+.
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

find_package(assimp CONFIG REQUIRED)
find_package(atomic_queue CONFIG REQUIRED)
find_package(box2d CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# miniaudio is header-only and the port installs a bare miniaudio.h with no CMake
# config, so wrap it the way we wrap our other single-header deps.
#
# NOTE (issue #773, open question 3): the port ships ONLY miniaudio.h — none of
# the five extras/nodes targets (reverb, vocoder, ltrim, channel combiner and
# separator) that miniaudio's own CMakeLists builds. That costs us nothing: those
# five were built by every configure and linked by nothing (verified by grep
# across the whole tree), so this move deletes five dead build targets rather
# than losing a capability.
find_path(OLO_MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE "${OLO_MINIAUDIO_INCLUDE_DIR}")

if(BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# cpp-httplib: single-header HTTP/1.1 server+client behind OloEditor's read-only
# MCP diagnostics server (#285). Linked from OloEditor, not OloEngine, so
# <winsock2.h> stays off the whole engine's include path.
find_package(httplib CONFIG REQUIRED)

# Drop CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO from the port's interface
# definitions (issue #787). It is upstream's CMake default, so the vcpkg port
# propagates it — but before #773 we consumed cpp-httplib as a DOWNLOAD_ONLY
# header drop with NO compile definitions at all, so the move to the port
# switched this on silently. It is not a knob anyone here asked for: it makes
# getaddrinfo_with_timeout() bound DNS resolution by the client's connection
# timeout, and nothing in this repo sets a connection timeout (grep
# set_connection_timeout — there are no call sites).
#
# On glibc it routes every resolve through getaddrinfo_a(), which runs the
# lookup on a thread glibc creates INSIDE libc (resolv/gai_misc.c
# handle_requests). That thread never goes through the PLT, so TSan's
# pthread_create interceptor never sees it and it has no __tsan::ThreadState —
# its first intercepted malloc() then faults inside TSan's own allocator
# (__tsan::user_alloc_internal), killing the process with a bare SIGSEGV and no
# report, because TSan cannot report from a thread it does not know about. That
# is the McpProgressHttpTest crash, and it would equally kill ANY future TSan
# run that resolves a hostname. ASan is unaffected, which is why the ASan job
# stayed green and made this look like an httplib teardown bug.
#
# Upstream's own comment on this path (their #2431) notes the glibc cancel path
# can hold the caller for the system resolver timeout anyway, so the option
# largely does not deliver its advertised benefit here. Turning it off restores
# the pre-#773 behaviour — a plain synchronous getaddrinfo() — on every
# platform, which keeps DNS semantics uniform rather than per-OS.
#
# Filtered rather than overwritten so the port's other definitions (the
# NO_EXCEPTIONS / *_SUPPORT / Darwin entries) survive a port update untouched.
get_target_property(OLO_HTTPLIB_DEFS httplib::httplib INTERFACE_COMPILE_DEFINITIONS)
if(OLO_HTTPLIB_DEFS)
    list(FILTER OLO_HTTPLIB_DEFS EXCLUDE REGEX "CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO")
    set_target_properties(httplib::httplib PROPERTIES
                          INTERFACE_COMPILE_DEFINITIONS "${OLO_HTTPLIB_DEFS}")
endif()

if(OLO_WITH_ALEMBIC)
    # Alembic::Alembic propagates Imath::Imath and its include dirs, so both the
    # IMATH_INSTALL export-validation hack and the $<BUILD_INTERFACE:> wrapper
    # around Imath_INCLUDE_DIRS that the in-tree build needed are gone.
    find_package(Alembic CONFIG REQUIRED)
    message(STATUS "OLO_WITH_ALEMBIC: Alembic (.abc) import enabled")
endif()

if(OLO_WITH_MATERIALX)
    # The port's DEFAULT feature set is exactly our configuration: MaterialXCore +
    # MaterialXFormat only, with render and every GEN_* shader backend off. That is
    # why this needs no options at all — see vcpkg.json's `materialx` feature.
    find_package(MaterialX CONFIG REQUIRED)
    message(STATUS "OLO_WITH_MATERIALX: MaterialX (.mtlx) import enabled")
endif()

if(OLO_WITH_VULKAN)
    # All three are pinned at the SDK-1.4.357.0 tag family — ADR 0010's tooling floor:
    # the first SDK whose validation layers can GPU-validate VK_EXT_descriptor_heap, and
    # the version whose vulkan_core.h the backend's device-selection gate compiles
    # against. The vcpkg baseline happens to carry exactly 1.4.357.0 for all three;
    # VulkanContext.cpp's static_assert on VK_HEADER_VERSION is what actually enforces
    # it, so a baseline bump that moves them will fail loudly at compile, not silently.
    #
    # The vendored headers deliberately shadow whatever SDK is installed: the engine's
    # find_package(Vulkan) (OloEngine/CMakeLists.txt, for the shaderc toolchain)
    # contributes the SDK's include dir too, and /I order is what decides.
    find_package(VulkanHeaders CONFIG REQUIRED)
    find_package(volk CONFIG REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)

    # STAGE the pinned headers into a private directory and export THAT. Neither
    # Vulkan::Headers' interface nor the shared vcpkg include root works here, for two
    # independent reasons — and the second cannot be fixed by ordering at all:
    #
    #   1. `find_package(VulkanHeaders CONFIG)` is not authoritative. The LunarG SDK
    #      ships its own VulkanHeadersConfig.cmake, so on a machine whose SDK differs
    #      from the pinned version the imported target can resolve to the SDK and
    #      quietly hand back the WRONG headers.
    #   2. `<vcpkg_installed>/<triplet>/include` is the SHARED include root that every
    #      vcpkg imported target also contributes, marked SYSTEM. CMake dedupes our
    #      entry against theirs and the SYSTEM marking wins, so the path is emitted
    #      once as `-external:I`. MSVC searches EVERY `/I` before ANY `-external:I`,
    #      so the SDK's plain `/I` beats it no matter where we place ours in the list.
    #      Verified in the generated project: pointing at the shared root, the path
    #      appeared ONLY as `-external:I` and never as `/I`.
    #
    # Copying `vulkan/` (and `vk_video/`, which vulkan_core.h includes) into a
    # directory nothing else contributes keeps it a normal `/I` in the position we
    # choose. Same staging idiom as stb_image further down.
    #
    # Both failure modes are invisible on a dev box whose SDK matches the pinned
    # version (mine is 1.4.357, exactly what we pin) — which is how each of them
    # reached CI, where setup-vulkan installs 1.4.321, as:
    #
    #   error C2065: 'VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME': undeclared identifier
    #
    # VulkanContext.cpp's static_assert on VK_HEADER_VERSION is the backstop if the
    # pinned version itself ever drifts.
    set(_olo_vk_src "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    if(NOT EXISTS "${_olo_vk_src}/vulkan/vulkan_core.h")
        message(FATAL_ERROR
            "Pinned Vulkan headers not found at '${_olo_vk_src}/vulkan/vulkan_core.h'. "
            "Without them the installed SDK wins the <vulkan/vulkan_core.h> lookup and the "
            "backend compiles against the wrong version.")
    endif()
    # Staged into the BUILD tree, not the source tree: build/ and build-clang/ must not
    # share it (see the "never build both trees concurrently" rule in
    # docs/agent-rules/build-trees-and-windows-asan.md).
    set(OLO_VULKAN_HEADERS_INCLUDE_DIR "${CMAKE_BINARY_DIR}/vulkan-headers-pinned/include")
    file(COPY "${_olo_vk_src}/vulkan" DESTINATION "${OLO_VULKAN_HEADERS_INCLUDE_DIR}")
    if(EXISTS "${_olo_vk_src}/vk_video")
        file(COPY "${_olo_vk_src}/vk_video" DESTINATION "${OLO_VULKAN_HEADERS_INCLUDE_DIR}")
    endif()
    set(OLO_VULKAN_HEADERS_INCLUDE_DIR "${OLO_VULKAN_HEADERS_INCLUDE_DIR}" PARENT_SCOPE)

    message(STATUS "OLO_WITH_VULKAN: Vulkan backend deps enabled (Vulkan-Headers + volk + VMA from vcpkg)")
endif()

# -----------------------------------------------------------------------------
# Still fetched in-tree. See the header comment for why each one is here.
# -----------------------------------------------------------------------------
message(STATUS "Downloading the remaining in-tree third party libraries (FetchContent / CPM)")

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

# pl_mpeg: single-file, public-domain MPEG-1 video + MP2 audio decoder.
# Header-only (one plmpeg.h; PL_MPEG_IMPLEMENTATION is defined in Video/PlMpeg.cpp).
# The repo ships no CMakeLists.txt, so MakeAvailable just populates the source dir
# and we add it to OloEngine's include path manually.
FetchContent_Declare(
	pl_mpeg
	GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
	GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023  # master @ 2026-06-05
	GIT_SHALLOW		FALSE
)

# Tracy: profiling enabled by default. The option is still the source of truth for
# whether the engine LINKS Tracy::TracyClient at all and whether the consumer-side
# TRACY_ENABLE/TRACY_ON_DEMAND macros are defined (cmake/CommonProperties.cmake);
# the TSan preset turns it off (cmake/Sanitizers.cmake). The port itself is built
# with profiling on and, via the `on-demand` feature, with TRACY_ON_DEMAND — so the
# two sides agree without the per-config target_compile_definitions the in-tree
# build needed.
option(TRACY_ENABLE "Enable profiling" ON)

# filewatch hardcodes `/Zi` with no option to disable it (filewatch-src/CMakeLists.txt),
# which sccache/ccache cannot cache (it aborts with "failed to zip up compiler outputs
# ... failed to open <name>.pdb"). Only its `filewatch_unit` test compiles — and that
# builds solely because the repo's global BUILD_TESTS=ON collides with filewatch's own
# BUILD_TESTS option. Shadow it OFF with a normal (non-cache) var so it applies to the
# FetchContent children below but not to OloEngine's own BUILD_TESTS in the parent scope.
if(OLO_ENABLE_COMPILER_CACHE AND MSVC)
	set(BUILD_TESTS OFF)
endif()

FetchContent_MakeAvailable(filewatch glad pl_mpeg)

# REPRODUCIBLE (issue #824) makes glad generate from its OWN bundled spec instead of fetching
# https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml at configure
# time. That fetch is unauthenticated, uncached and un-retried, and it has already cost a
# 15+ minute HTTP 429 on a fresh build tree. Every new worktree and every CI runner configures
# a fresh tree, so every one of them pays that risk exactly once. We pin a single fixed API
# surface (gl:core=4.6, stable for years), so generating from the bundled spec costs us
# nothing but the very latest registry revision.
glad_add_library(glad STATIC REPRODUCIBLE API gl:core=4.6 LOCATION ${PROJECT_SOURCE_DIR}/vendor/glad-build/${TARGET})

FetchContent_Declare(stb_image
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20  # master @ 2026-06-05
	GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(stb_image)
add_library(stb_image INTERFACE)
file(COPY ${stb_image_SOURCE_DIR}/stb_image.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
file(COPY ${stb_image_SOURCE_DIR}/stb_image_write.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
file(COPY ${stb_image_SOURCE_DIR}/stb_truetype.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
set(STBIMAGE_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/stb_image-build")

CPMAddPackage(NAME imgui
	GITHUB_REPOSITORY ocornut/imgui
	GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5  # docking @ 2026-06-05
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
	target_include_directories(imgui PUBLIC ${IMGUI_DIR} "glad-build/include")
	target_link_libraries(imgui PUBLIC glfw)
	set_target_properties(imgui PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build")
endif()

CPMAddPackage(NAME imguizmo
	GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
	GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1  # master @ 2026-06-05
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

# bc7enc_rdo: Rich Geldreich's BC1-7 / BC6H block-compression encoders + decoders.
# Used by the offline texture-compression cook (Renderer/TextureCompression.cpp) to
# encode base-color -> BC7 and normal maps -> BC5 (#440). Dual MIT / public-domain
# licensed for the core sources we use (bc7enc / rgbcx / bc7decomp); we deliberately
# skip bc7e.ispc (Apache + needs the ISPC compiler), lodepng, miniz, and the RDO
# post-processor — none are needed for a straight CPU encode. DOWNLOAD_ONLY because
# the repo's own CMakeLists builds a test exe that requires ISPC; we compile just the
# three self-contained TUs into our own static lib, mirroring the imgui / lua pattern.
CPMAddPackage(NAME bc7enc_rdo
  GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
  GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0  # master @ 2026-02-27
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
	# Clang-only warnings (unused, sign-compare, ...). Suppress -Werror on it only.
	# This is now the ONLY -Wno-error block left in this file: every other library
	# that needed one is prebuilt by vcpkg, and a prebuilt binary has no warnings
	# to suppress (issue #773).
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
# TextureCompression.cpp.
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

# xatlas: jpcy's UV-unwrapping / chart-packing library (MIT). Generates the UV2
# lightmap parameterization for the baked-GI path (issue #439): a static mesh
# without an authored second UV set gets its charts + per-mesh atlas from here at
# bake time (Renderer/Baking/LightmapUnwrap.cpp). No port exists at the pinned
# registry baseline (only uvatlas, which pulls directxmath + directx-headers on
# Linux), so it is fetched and compiled as a tiny static lib, mirroring bc7enc.
# Upstream has been stable/complete since 2022 — the pin is the master tip.
CPMAddPackage(NAME xatlas
  GIT_REPOSITORY https://github.com/jpcy/xatlas.git
  GIT_TAG f700c7790aaa030e794b52ba7791a05c085faf0c  # master @ 2022-07-25
  GIT_SHALLOW FALSE
  DOWNLOAD_ONLY YES)
if(xatlas_ADDED)
	add_library(xatlas STATIC "${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp")
	# SYSTEM so the library's own warnings don't count against OloEngine's /W4-ish flags.
	target_include_directories(xatlas SYSTEM PUBLIC "${xatlas_SOURCE_DIR}/source/xatlas")
	# Single-threaded on purpose: the lightmap bake's determinism contract (same
	# scene -> bit-identical unwrap -> bit-identical bake) must not depend on
	# xatlas's internal task scheduling, and the unwrap is not on a hot path.
	target_compile_definitions(xatlas PRIVATE XA_MULTITHREADED=0)
	set_target_properties(xatlas PROPERTIES FOLDER "Utilities/xatlas"
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/xatlas-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/xatlas-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/xatlas-build")
	# clang-cl maps our /WX onto -Werror; suppress it for the vendored TU only,
	# same rationale as the bc7enc block above.
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(xatlas PRIVATE -Wno-error -Wno-everything)
	endif()
endif()

# -----------------------------------------------------------------------------
# IDE folders for the targets we still build ourselves. The vcpkg-provided
# packages are IMPORTED targets and never appear in the solution, so every
# folder assignment they used to need is gone.
# -----------------------------------------------------------------------------
set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(lua PROPERTIES FOLDER "Utilities/Lua")

#======================================
# Propagate FetchContent _SOURCE_DIR / _BINARY_DIR variables to parent scope
# so OloEngine/CMakeLists.txt can use them in target_include_directories.
foreach(_dep filewatch glad pl_mpeg stb_image imgui imguizmo lua)
	if(DEFINED ${_dep}_SOURCE_DIR)
		set(${_dep}_SOURCE_DIR "${${_dep}_SOURCE_DIR}" PARENT_SCOPE)
	endif()
	if(DEFINED ${_dep}_BINARY_DIR)
		set(${_dep}_BINARY_DIR "${${_dep}_BINARY_DIR}" PARENT_SCOPE)
	endif()
endforeach()

# FFmpeg (optional) — built from source for the H.264/MP4 video backend.
# (Lives in the tracked cmake/ dir because OloEngine/vendor/* is gitignored.)
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)

# FSR2 (optional, Windows + OpenGL only) — AMD's temporal upscaler, issue #684.
# Same reason for living in cmake/: OloEngine/vendor/* is gitignored.
include(${CMAKE_SOURCE_DIR}/cmake/fsr2.cmake)
set(OLO_WITH_FSR2 "${OLO_WITH_FSR2}" PARENT_SCOPE)

message(STATUS "Third-party dependency resolution done")
