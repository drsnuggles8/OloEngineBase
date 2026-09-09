# =============================================================================
# FetchContent PATCH_COMMAND for the FSR2 OpenGL tree.
#
# Run in script mode (`cmake -P`) with the source tree as the working directory,
# which is what FetchContent's patch step gives us.
#
# THE CONTRACT IS "TREE == PIN + cmake/fsr2-patches/*.patch", and it is enforced
# by RESTORING the tracked files to the pin before applying anything. Three
# things fall out of that, all of which an apply-only script gets wrong:
#
#   * It is idempotent, which it has to be -- FetchContent re-runs its patch step
#     on every configure while FETCHCONTENT_UPDATES_DISCONNECTED is OFF, so a
#     plain `git apply` would fail the second time and take the configure with it.
#   * DELETING a patch file actually un-patches the tree. Without the restore,
#     removing a patch whose fix has landed upstream leaves the tree carrying
#     shader edits that exist in no tracked file in this repo -- the worst
#     possible state, because nothing reports it.
#   * A hand edit to the fetched tree is reverted rather than colliding. Debug in
#     the patch file; edits under OloEngine/vendor/clang/fsr2gl-src do not
#     survive a configure.
#
# The restore only covers TRACKED files, so a patch that ADDS a file would leave
# it behind on removal. None of ours do; if one ever does, extend this.
#
# The price is that restore-then-apply rewrites the patched headers on EVERY
# configure, so their mtimes move and the permutations they feed regenerate --
# about two minutes of shader compiles on a configure that changed nothing.
# That is deliberate: a configure is rare, and the alternative (skip the work
# when the tree looks already-patched) is precisely the apply-only behaviour
# whose failure modes are listed above.
# =============================================================================

if(NOT DEFINED OLO_FSR2_PATCH_DIR)
	message(FATAL_ERROR "OLO_FSR2_PATCH_DIR must be set")
endif()

find_package(Git QUIET REQUIRED)

# Back to the pin, whatever the tree currently holds.
execute_process(
	COMMAND "${GIT_EXECUTABLE}" checkout -- .
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	RESULT_VARIABLE _restore
	ERROR_VARIABLE _restore_err)

if(NOT _restore EQUAL 0)
	message(FATAL_ERROR
		"FSR2: could not restore the fetched tree to its pin\n"
		"Tree: ${CMAKE_CURRENT_SOURCE_DIR}\n"
		"${_restore_err}")
endif()

file(GLOB OLO_FSR2_PATCHES "${OLO_FSR2_PATCH_DIR}/*.patch")
list(SORT OLO_FSR2_PATCHES)

foreach(_patch IN LISTS OLO_FSR2_PATCHES)
	get_filename_component(_name "${_patch}" NAME)

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
			"If the pin in cmake/fsr2.cmake just moved, the fix is probably upstream now — "
			"delete the patch file, or rebase it onto the new pin.")
	endif()

	message(STATUS "FSR2 patch applied: ${_name}")
endforeach()

list(LENGTH OLO_FSR2_PATCHES _count)
if(_count EQUAL 0)
	message(STATUS "FSR2: no local patches; the fetched tree is the pin as-is")
endif()
