# =============================================================================
# AMD FidelityFX Super Resolution 2 (FSR2) — OpenGL backend (issue #684).
#
# Upstream is JuanDiegoMontoya/FidelityFX-FSR2-OpenGL: AMD's official FSR 2.2.1
# sources plus a maintained OpenGL 4.6 backend. MIT licensed.
#
# WHY IT IS FETCHED RATHER THAN TAKEN FROM vcpkg: there is no port, for either
# the AMD SDK or this fork. (Add it to the "still fetched" roster in
# OloEngine/vendor/CMakeLists.txt if that ever changes.)
#
# WHY IT IS WINDOWS-ONLY. Two independent reasons, and neither is "we didn't get
# round to it" — see docs/agent-rules/notes-renderer.md:
#
#   1. `src/ffx-fsr2-api/gl/ffx_fsr2_gl.cpp` is written against the Win32 CRT and
#      Win32 API: `wcstombs_s` (MSVC's C11 Annex K), `GetModuleHandleA` for its
#      RenderDoc detection, and `<Windows.h>`. Porting that is upstream's job,
#      not a fork we would then have to carry.
#   2. The shader permutations are compiled ahead of time by `tools/sc/
#      FidelityFX_SC.exe`, a PREBUILT Win32 binary vendored in that repository.
#      It is a permutation-expanding wrapper around glslang that emits the
#      SPIR-V blobs plus the reflection tables `ffx_fsr2_shaders_gl.cpp`
#      indexes. There is no Linux build of it upstream.
#
# So `OLO_WITH_FSR2` auto-detects to ON only on Windows, and only when the
# fetched tree actually carries the compiler. Everywhere else the engine builds
# and runs normally with the FSR2 upscaler reporting itself unsupported — the
# render pass and its settings still compile on every platform, because only the
# thin backend adapter (Platform/OpenGL/OpenGLTemporalUpscaler.cpp) is guarded.
#
# The generated permutation headers land in the BUILD tree, never the source
# tree, so build/ and build-clang/ cannot share (or race on) them — the same
# rule as the pinned Vulkan headers next door.
# =============================================================================

include_guard(GLOBAL)

option(OLO_WITH_FSR2 "Build the FSR2 temporal upscaler (OpenGL backend, Windows only)" ON)

if(NOT OLO_WITH_FSR2)
	message(STATUS "OLO_WITH_FSR2: disabled by option — the FSR2 upscaler will report itself unsupported")
	return()
endif()

if(NOT WIN32)
	set(OLO_WITH_FSR2 OFF CACHE BOOL "" FORCE)
	message(STATUS "OLO_WITH_FSR2: OFF (the upstream OpenGL backend and its shader compiler are Win32-only)")
	return()
endif()

include(FetchContent)

# Pinned to a full commit SHA, never a tag — see the pinning discipline note in
# OloEngine/vendor/CMakeLists.txt. GIT_SHALLOW must be FALSE for a SHA pin.
#
# GIT_SUBMODULES "" is load-bearing, not tidiness. Upstream registers two
# submodules for its sample app — GPUOpen's Cauldron framework and, far worse,
# `media/cauldron-media`, a multi-gigabyte art repository. Neither contributes a
# single byte to the FSR2 API we compile below, and cloning them turns a ~60 MB
# fetch into a download nobody would accept on a fresh worktree or CI runner.
#
# SOURCE_SUBDIR points at a directory that deliberately does not exist. That is
# the documented way to make FetchContent_MakeAvailable populate the tree WITHOUT
# running add_subdirectory on it: upstream's top-level CMakeLists configures the
# Cauldron sample framework plus the DX12 and Vulkan sample apps, all of which
# would fail (or worse, succeed) inside our build. We want the sources only.
FetchContent_Declare(fsr2gl
	GIT_REPOSITORY https://github.com/JuanDiegoMontoya/FidelityFX-FSR2-OpenGL.git
	GIT_TAG 7fb8c92d18e300b84975f2f609b58713b8bde4a7  # main @ 2026-05-04
	GIT_SHALLOW FALSE
	GIT_SUBMODULES ""
	SOURCE_SUBDIR olo-does-not-build-upstream-cmake)

FetchContent_MakeAvailable(fsr2gl)

set(OLO_FSR2_API_DIR "${fsr2gl_SOURCE_DIR}/src/ffx-fsr2-api")
set(OLO_FSR2_SC "${fsr2gl_SOURCE_DIR}/tools/sc/FidelityFX_SC.exe")

if(NOT EXISTS "${OLO_FSR2_SC}")
	set(OLO_WITH_FSR2 OFF CACHE BOOL "" FORCE)
	message(WARNING "OLO_WITH_FSR2: OFF — FidelityFX_SC.exe missing from the fetched tree at '${OLO_FSR2_SC}'")
	return()
endif()

# -----------------------------------------------------------------------------
# Ahead-of-time shader permutations.
#
# These argument lists are transcribed from upstream's
# src/ffx-fsr2-api/CMakeLists.txt and gl/CMakeLists.txt. They are NOT free
# parameters: the option set and its ORDER define the permutation key bitfield
# that the vendored ffx_fsr2_shaders_gl.cpp indexes at runtime, so dropping an
# option (even one our integration always pins) shifts every key and hands the
# driver the wrong SPIR-V blob. Change them only in lockstep with a pin bump.
# -----------------------------------------------------------------------------
set(OLO_FSR2_SC_BASE_ARGS
	-reflection -deps=gcc -DFFX_GPU=1
	-DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
	-DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2)

# Sampler/texture bindings start at 8 so the generated image bindings keep low
# indices; --target-env opengl + spirv1.3 is what GL_ARB_gl_spirv accepts.
set(OLO_FSR2_SC_GL_ARGS
	-compiler=glslang -e main --target-env opengl --target-env spirv1.3
	--amb --stb comp 8 --ssb comp 8 --sib comp 0 --suavb comp 0
	-Os -S comp -DFFX_GLSL=1)

set(OLO_FSR2_SC_PERMUTATION_ARGS
	"-DFFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}"
	"-DFFX_FSR2_OPTION_HDR_COLOR_INPUT={0,1}"
	"-DFFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
	"-DFFX_FSR2_OPTION_JITTERED_MOTION_VECTORS={0,1}"
	"-DFFX_FSR2_OPTION_INVERTED_DEPTH={0,1}"
	"-DFFX_FSR2_OPTION_APPLY_SHARPENING={0,1}")

set(OLO_FSR2_PASSES
	ffx_fsr2_tcr_autogen_pass
	ffx_fsr2_autogen_reactive_pass
	ffx_fsr2_accumulate_pass
	ffx_fsr2_compute_luminance_pyramid_pass
	ffx_fsr2_depth_clip_pass
	ffx_fsr2_lock_pass
	ffx_fsr2_reconstruct_previous_depth_pass
	ffx_fsr2_rcas_pass)

set(OLO_FSR2_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fsr2-gl-shaders")
file(MAKE_DIRECTORY "${OLO_FSR2_SHADER_OUTPUT_DIR}")

# Every shared header a pass source can pull in. CONFIGURE_DEPENDS so adding one
# upstream (or in a patch) re-globs instead of going unnoticed.
file(GLOB OLO_FSR2_SHADER_INCLUDES CONFIGURE_DEPENDS
	"${OLO_FSR2_API_DIR}/shaders/*.h"
	"${OLO_FSR2_API_DIR}/gl/shaders/*.h")

set(OLO_FSR2_PERMUTATION_HEADERS "")
foreach(_pass IN LISTS OLO_FSR2_PASSES)
	set(_header "${OLO_FSR2_SHADER_OUTPUT_DIR}/${_pass}_permutations.h")

	# The luminance-pyramid pass has no 16-bit variant upstream; generating one
	# would add a key bit the runtime never sets.
	if(_pass STREQUAL "ffx_fsr2_compute_luminance_pyramid_pass")
		set(_half_arg -DFFX_HALF=0)
	else()
		set(_half_arg "-DFFX_HALF={0,1}")
	endif()

	# DEPENDS on the pass source AND on every shared shader header it can
	# include. The glob is deliberately coarse — a few unnecessary regenerations
	# cost seconds, while a MISSED one costs a debugging session against shaders
	# that are not the ones you edited.
	#
	# This is not hypothetical and is not only about upstream pin bumps: patching
	# the fetched tree (a fork, or a .patch applied here) usually means editing a
	# shared ffx_*.h, which is exactly the case the old "pass source only"
	# dependency missed. It cost hours — a deliberate write-constant-red control
	# silently did nothing — before it was caught.
	add_custom_command(
		OUTPUT "${_header}"
		COMMAND "${OLO_FSR2_SC}"
			${OLO_FSR2_SC_BASE_ARGS} ${OLO_FSR2_SC_GL_ARGS}
			${OLO_FSR2_SC_PERMUTATION_ARGS} ${_half_arg}
			"-name=${_pass}"
			"-I${OLO_FSR2_API_DIR}/gl/shaders"
			"-output=${OLO_FSR2_SHADER_OUTPUT_DIR}"
			"${OLO_FSR2_API_DIR}/shaders/${_pass}.glsl2"
		DEPENDS "${OLO_FSR2_API_DIR}/shaders/${_pass}.glsl2" ${OLO_FSR2_SHADER_INCLUDES}
		WORKING_DIRECTORY "${fsr2gl_SOURCE_DIR}"
		COMMENT "FSR2: compiling ${_pass} SPIR-V permutations"
		VERBATIM)

	list(APPEND OLO_FSR2_PERMUTATION_HEADERS "${_header}")
endforeach()

add_custom_target(ffx_fsr2_gl_shaders DEPENDS ${OLO_FSR2_PERMUTATION_HEADERS})
set_target_properties(ffx_fsr2_gl_shaders PROPERTIES FOLDER "Utilities/FSR2")

# -----------------------------------------------------------------------------
# The library itself: the platform-agnostic FSR2 context + the GL backend.
# -----------------------------------------------------------------------------
add_library(ffx_fsr2_gl STATIC
	"${OLO_FSR2_API_DIR}/ffx_fsr2.cpp"
	"${OLO_FSR2_API_DIR}/ffx_assert.cpp"
	"${OLO_FSR2_API_DIR}/gl/ffx_fsr2_gl.cpp"
	"${OLO_FSR2_API_DIR}/gl/shaders/ffx_fsr2_shaders_gl.cpp")

add_dependencies(ffx_fsr2_gl ffx_fsr2_gl_shaders)

# add_dependencies ORDERS the targets; it does NOT make the object that embeds
# the blobs depend on the header files. Without OBJECT_DEPENDS, regenerating the
# permutations relinks the archive while ffx_fsr2_shaders_gl.cpp.obj keeps the
# OLD SPIR-V — so the build reports success and the GPU runs the previous
# shaders. Measured: two different shader-source edits produced bit-identical
# frames (74.576736802867472 mean luma to 15 digits), and a deliberate
# write-constant-red control did nothing at all, until this object was deleted
# by hand. Nothing warns; you simply debug the wrong binary.
#
# This only bites when the SHADER SOURCES change under a fixed pin (a local
# investigation, or a patch applied to the fetched tree), which is exactly when
# you are least able to afford a stale build.
set_source_files_properties("${OLO_FSR2_API_DIR}/gl/shaders/ffx_fsr2_shaders_gl.cpp"
	PROPERTIES OBJECT_DEPENDS "${OLO_FSR2_PERMUTATION_HEADERS}")

# The backend calls GL exclusively through its own function table, loaded from
# the `getProcAddress` we hand it — it links no loader. Upstream's bundled
# glad/gl.h is therefore used for nothing but the PFNGL* typedefs and enums, and
# our own glad (2.0.8, gl:core=4.6) does not carry the GL_KHR_shader_subgroup
# enums it also needs. So its own TUs compile against the bundled header: two
# glad *headers* in different TUs are fine precisely because neither library
# resolves a symbol from the other's.
#
# THE GLAD DIRECTORY MUST STAY PRIVATE. OloEngine links this target, so a PUBLIC
# include directory here lands on the include path of EVERY engine TU — and
# upstream's copy is glad 2.0.4 while ours is 2.0.8, both spelled <glad/gl.h>.
# Whichever directory the compiler searched first would win, silently, for the
# whole engine. PRIVATE keeps that impossible. The one engine TU that includes
# the FSR2 headers (Platform/OpenGL/OpenGLTemporalUpscaler.cpp) includes OUR
# <glad/gl.h> first, and both generations use the same GLAD_GL_H_ guard, so the
# copy ffx_fsr2_gl.h pulls in is a no-op there.
target_include_directories(ffx_fsr2_gl SYSTEM PRIVATE
	"${OLO_FSR2_API_DIR}/gl/external/glad/include")

target_include_directories(ffx_fsr2_gl SYSTEM PUBLIC
	"${OLO_FSR2_API_DIR}"
	"${OLO_FSR2_API_DIR}/gl")

# Generated permutation headers: only ffx_fsr2_shaders_gl.cpp includes them.
target_include_directories(ffx_fsr2_gl SYSTEM PRIVATE
	"${OLO_FSR2_SHADER_OUTPUT_DIR}")

# std::wstring_convert / std::codecvt_utf8_utf16 are deprecated since C++17 and
# upstream still uses them for its debug resource names.
target_compile_definitions(ffx_fsr2_gl PRIVATE
	_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
	_CRT_SECURE_NO_WARNINGS
	NOMINMAX
	WIN32_LEAN_AND_MEAN)

# Vendored code does not answer to OloEngine's /W4 /WX, exactly as the bc7enc
# and xatlas blocks in OloEngine/vendor/CMakeLists.txt handle theirs.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(ffx_fsr2_gl PRIVATE -Wno-error -Wno-everything)
elseif(MSVC)
	target_compile_options(ffx_fsr2_gl PRIVATE /W0)
endif()

set_target_properties(ffx_fsr2_gl PROPERTIES FOLDER "Utilities/FSR2")

message(STATUS "OLO_WITH_FSR2: ON (FSR2 2.2.1 OpenGL backend from ${fsr2gl_SOURCE_DIR})")
