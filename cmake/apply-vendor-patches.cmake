# =============================================================================
# FetchContent PATCH_COMMAND for a fetched vendor tree.
#
# Shared by every dependency this repo patches (FSR2's OpenGL tree, glad). Each
# caller passes OLO_PATCH_DIR and a human-readable OLO_PATCH_LABEL; the contract
# and every failure mode below are identical regardless of which tree it is, which
# is why there is one script rather than one per dependency.
#
# Run in script mode (`cmake -P`) with the source tree as the working directory,
# which is what FetchContent's patch step gives us.
#
# THE CONTRACT IS "TREE == PIN + ${OLO_PATCH_DIR}/*.patch", and it is enforced
# by RESTORING the tracked files to the pin before applying anything. Three
# things fall out of that, all of which an apply-only script gets wrong:
#
#   * It is idempotent, which it has to be -- FetchContent re-runs its patch step
#     on every configure while FETCHCONTENT_UPDATES_DISCONNECTED is OFF, so a
#     plain `git apply` would fail the second time and take the configure with it.
#   * DELETING a patch file actually un-patches the tree. Without the restore,
#     removing a patch whose fix has landed upstream leaves the tree carrying
#     edits that exist in no tracked file in this repo -- the worst
#     possible state, because nothing reports it.
#   * A hand edit to the fetched tree is reverted rather than colliding. Debug in
#     the patch file; edits inside the fetched source tree do not
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

if(NOT DEFINED OLO_PATCH_DIR)
	message(FATAL_ERROR "OLO_PATCH_DIR must be set")
endif()
if(NOT DEFINED OLO_PATCH_LABEL)
	message(FATAL_ERROR "OLO_PATCH_LABEL must be set — it names the tree in every message below")
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
		"${OLO_PATCH_LABEL}: could not restore the fetched tree to its pin\n"
		"Tree: ${CMAKE_CURRENT_SOURCE_DIR}\n"
		"${_restore_err}")
endif()

file(GLOB OLO_PATCHES "${OLO_PATCH_DIR}/*.patch")
list(SORT OLO_PATCHES)

foreach(_patch IN LISTS OLO_PATCHES)
	get_filename_component(_name "${_patch}" NAME)

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --verbose "${_patch}"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		RESULT_VARIABLE _result
		ERROR_VARIABLE _stderr)

	if(NOT _result EQUAL 0)
		message(FATAL_ERROR
			"${OLO_PATCH_LABEL} patch failed to apply: ${_name}\n"
			"Tree: ${CMAKE_CURRENT_SOURCE_DIR}\n"
			"${_stderr}\n"
			"If the pin just moved, the fix is probably upstream now — "
			"delete the patch file, or rebase it onto the new pin.")
	endif()

	message(STATUS "${OLO_PATCH_LABEL} patch applied: ${_name}")
endforeach()

list(LENGTH OLO_PATCHES _count)
if(_count EQUAL 0)
	message(STATUS "${OLO_PATCH_LABEL}: no local patches; the fetched tree is the pin as-is")
endif()
