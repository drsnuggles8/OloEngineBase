# =============================================================================
# FetchContent PATCH_COMMAND for the FSR2 OpenGL tree.
#
# Run in script mode (`cmake -P`) with the source tree as the working directory,
# which is what FetchContent's patch step gives us. Applies every
# cmake/fsr2-patches/*.patch in sorted order.
#
# IT MUST BE IDEMPOTENT. FetchContent re-runs the patch step on every configure
# while FETCHCONTENT_UPDATES_DISCONNECTED is OFF, so a plain `git apply` would
# fail the second time round and take the whole configure with it. Each patch is
# therefore probed with `git apply --reverse --check` first: that succeeds only
# when the patch is ALREADY in the tree, in which case there is nothing to do.
#
# Consequence worth knowing before you go debugging a shader: a hand edit to the
# fetched tree that collides with one of these patches is reverted on the next
# configure. Investigate in the patch file, not in OloEngine/vendor/clang/.
# =============================================================================

if(NOT DEFINED OLO_FSR2_PATCH_DIR)
	message(FATAL_ERROR "OLO_FSR2_PATCH_DIR must be set")
endif()

find_package(Git QUIET REQUIRED)

file(GLOB OLO_FSR2_PATCHES "${OLO_FSR2_PATCH_DIR}/*.patch")
list(SORT OLO_FSR2_PATCHES)

foreach(_patch IN LISTS OLO_FSR2_PATCHES)
	get_filename_component(_name "${_patch}" NAME)

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		RESULT_VARIABLE _already
		OUTPUT_QUIET ERROR_QUIET)

	if(_already EQUAL 0)
		message(STATUS "FSR2 patch already applied: ${_name}")
		continue()
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --verbose "${_patch}"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		RESULT_VARIABLE _result
		ERROR_VARIABLE _stderr)

	if(NOT _result EQUAL 0)
		message(FATAL_ERROR
			"FSR2 patch failed to apply: ${_name}\n"
			"Tree: ${CMAKE_CURRENT_SOURCE_DIR}\n"
			"${_stderr}\n"
			"If the pin in cmake/fsr2.cmake moved, the patch may already be upstream — "
			"delete it, or rebase it onto the new pin.")
	endif()

	message(STATUS "FSR2 patch applied: ${_name}")
endforeach()
