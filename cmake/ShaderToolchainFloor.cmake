# =============================================================================
# ShaderToolchainFloor.cmake — fail the CONFIGURE, by name, when the shader
# toolchain cannot compile what production shaders declare.
#
# Issue #1139, ADR 0011 amendment (97). Amendments (95)/(96) put `#extension
# GL_EXT_descriptor_heap : require` in seven production shaders; a toolchain
# older than Vulkan SDK 1.4.357.0 rejects it. Before this file the symptom was
# two GoogleTest failures ~4,000 cases into a two-hour sanitizer run, reported
# as `'descriptor_heap' : unrecognized layout identifier` against a line in an
# include file — a shader bug's diagnostic for a toolchain problem.
#
# WHAT MAKES THIS SKEW-FREE. The probe links THE SAME shaderc the engine links,
# passed in by the caller. Asking `glslc` on PATH instead would have reproduced
# #1139 exactly: hosted CI had an SDK whose glslc accepted the extension while
# the engine linked Ubuntu's libshaderc 2023.8, which did not.
#
# WHAT IT DOES NOT DO. It does not replace the runtime refusal in
# ShaderToolchainFloor.cpp. A build that cannot COMPILE or RUN the probe at all
# (a cross-compile, a toolchain whose shaderc headers moved) gets a WARNING that
# names why, and the engine still refuses per shader at compile time — the
# guarantee is never dropped, only the earliness is.
# =============================================================================

# olo_verify_shader_toolchain_floor(INCLUDE_DIRS <dirs...> LIBRARIES <libs...>)
#
# INCLUDE_DIRS  where <shaderc/shaderc.hpp> lives, plus OloEngine/src for the
#               shared floor header.
# LIBRARIES     exactly what the engine links for shaderc.
function(olo_verify_shader_toolchain_floor)
	cmake_parse_arguments(OLO_STF "" "" "INCLUDE_DIRS;LIBRARIES" ${ARGN})

	set(_probe "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ShaderToolchainProbe.cpp")
	set(_bindir "${CMAKE_BINARY_DIR}/olo-shader-toolchain-floor")

	# NATIVE SEPARATORS MUST GO. try_run writes these paths into a GENERATED
	# CMakeLists, where `C:\VulkanSDK\1.4.357.0/Lib/shaderc_shared.lib` is not a
	# path at all but "Invalid character escape '\V'" — the Windows SDK libraries
	# arrive that way because $ENV{VULKAN_SDK} is native. Silent otherwise: the
	# generated project fails to configure and the probe reports UNVERIFIED.
	set(_libs "")
	foreach(_lib IN LISTS OLO_STF_LIBRARIES)
		if(_lib STREQUAL "debug" OR _lib STREQUAL "optimized" OR _lib STREQUAL "general")
			# find_library keywords, not paths — pass them through untouched.
			list(APPEND _libs "${_lib}")
		else()
			file(TO_CMAKE_PATH "${_lib}" _cmake_lib)
			list(APPEND _libs "${_cmake_lib}")
		endif()
	endforeach()
	set(_includes "")
	foreach(_inc IN LISTS OLO_STF_INCLUDE_DIRS)
		file(TO_CMAKE_PATH "${_inc}" _cmake_inc)
		list(APPEND _includes "${_cmake_inc}")
	endforeach()

	# LSan runs at exit and would otherwise report shaderc's own allocations from
	# a probe that deliberately never tears the compiler down; the verdict is on
	# stdout, but a leak report interleaved with it makes the output unreadable
	# and the run slow. TSan gets the same treatment for the same reason.
	set(ENV{ASAN_OPTIONS} "detect_leaks=0")
	set(ENV{LSAN_OPTIONS} "detect_leaks=0")

	try_run(_run_result _compile_result
		"${_bindir}" SOURCES "${_probe}"
		LINK_LIBRARIES ${_libs}
		CMAKE_FLAGS
			"-DINCLUDE_DIRECTORIES=${_includes}"
			"-DCMAKE_CXX_STANDARD=23"
		COMPILE_OUTPUT_VARIABLE _compile_output
		RUN_OUTPUT_VARIABLE _run_output)

	if(NOT _compile_result)
		message(WARNING
			"Could not build the shader-toolchain floor probe, so the floor is UNVERIFIED at configure "
			"time. The engine still refuses per shader at compile time (ShaderToolchainFloor.cpp) — this "
			"only costs you the early, named failure. Probe output:\n${_compile_output}")
		return()
	endif()

	if(_run_result STREQUAL "FAILED_TO_RUN" OR NOT _run_output MATCHES "OLO_SHADER_TOOLCHAIN_FLOOR:")
		message(WARNING
			"The shader-toolchain floor probe built but produced no verdict, so the floor is UNVERIFIED "
			"at configure time (cross-compiling, or the probe could not load its shaderc runtime). The "
			"engine still refuses per shader at compile time. Probe output:\n${_run_output}")
		return()
	endif()

	if(_run_output MATCHES "OLO_SHADER_TOOLCHAIN_FLOOR: ok")
		message(STATUS "Shader toolchain floor: satisfied (GL_EXT_descriptor_heap compiles)")
		return()
	endif()

	string(STRIP "${_run_output}" _verdict)
	message(FATAL_ERROR
		"Shader toolchain is BELOW the floor this engine requires.\n"
		"${_verdict}\n"
		"Seven production shaders declare `#extension GL_EXT_descriptor_heap : require` "
		"(ADR 0011 amendments (95)/(96)); there is no non-heap fallback, by decision — see "
		"amendment (97). Install Vulkan SDK 1.4.357.0 or newer (equivalently a shader toolchain "
		"built from shaderc v2026.3, whose glslang pin is glslang's own vulkan-sdk-1.4.357.0 tag), "
		"and configure a fresh build tree so the shaderc paths are re-resolved.\n"
		"Ubuntu 24.04's libshaderc-dev (2023.8) and glslang-dev (15.1.0) are both below the floor; "
		"this is what CI hits without .github/actions/setup-shader-toolchain-linux.")
endfunction()
