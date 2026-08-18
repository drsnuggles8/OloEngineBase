# CheckArchiveSize.cmake — post-build headroom check for a static archive (issue #762).
#
# The classic COFF archive format (.lib) stores each member's offset in the second linker
# member as a 32-bit big-endian value, so an archive can never exceed 4 GiB. There is no
# extended/64-bit variant: lib.exe simply refuses, and the message you get is
#
#     OloEngine.lib : fatal error LNK1248: image size (10007B6EE) exceeds maximum allowable
#     size (FFFFFFFF)
#
# which names neither the cause nor the cure. Worse, it is a *cliff*: the archive grows a few
# MB per feature and then one day the build stops, in whichever config crosses first — for
# #762 that was Dist, whose /GL whole-program-optimization objects carry compiler IL rather
# than machine code and are one to two orders of magnitude larger than ordinary objects.
#
# So this script is run as a POST_BUILD step on the archive and turns the cliff into a slope:
# it reports how close the archive is to the ceiling every time it is rebuilt, warns while
# there is still room to act, and fails with an actionable message before lib.exe would.
#
# Invoked as:
#   cmake -DOLO_ARCHIVE=<path> -DOLO_ARCHIVE_WARN_BYTES=<n> -DOLO_ARCHIVE_FAIL_BYTES=<n>
#         [-DOLO_ARCHIVE_QUIET=ON] -P cmake/CheckArchiveSize.cmake

if(NOT DEFINED OLO_ARCHIVE)
    message(FATAL_ERROR "CheckArchiveSize.cmake: -DOLO_ARCHIVE=<path> is required")
endif()

# A missing archive is not this script's failure to report — the build step that was supposed
# to produce it would already have failed. Staying silent here keeps the real error at the top
# of the log instead of burying it under a second, derived one.
if(NOT EXISTS "${OLO_ARCHIVE}")
    return()
endif()

file(SIZE "${OLO_ARCHIVE}" _olo_archive_bytes)

# 4 GiB exactly — the format limit, not a policy knob, so it is not configurable.
set(_olo_archive_limit 4294967296)

get_filename_component(_olo_archive_name "${OLO_ARCHIVE}" NAME)
math(EXPR _olo_archive_pct "(${_olo_archive_bytes} * 100) / ${_olo_archive_limit}")
math(EXPR _olo_archive_mib "${_olo_archive_bytes} / 1048576")
math(EXPR _olo_archive_headroom_mib "(${_olo_archive_limit} - ${_olo_archive_bytes}) / 1048576")
set(_olo_archive_report
    "${_olo_archive_name} is ${_olo_archive_mib} MiB (${_olo_archive_pct}% of the 4 GiB COFF "
    "archive limit, ${_olo_archive_headroom_mib} MiB of headroom left)")
string(JOIN "" _olo_archive_report ${_olo_archive_report})

set(_olo_archive_advice
    "The .lib format cannot exceed 4 GiB and has no 64-bit variant, so this is a hard ceiling: "
    "once crossed, every app target fails to link with LNK1248. Recover headroom by excluding "
    "more binding-heavy translation units from whole-program optimization (see "
    "olo_exclude_from_whole_program_optimization in cmake/CommonProperties.cmake and its call "
    "site in OloEngine/src/CMakeLists.txt), or take the durable fix and split OloEngine into "
    "several archives. Background: docs/agent-rules/static-archive-4gib-ceiling.md")
string(JOIN "" _olo_archive_advice ${_olo_archive_advice})

if(DEFINED OLO_ARCHIVE_FAIL_BYTES AND _olo_archive_bytes GREATER_EQUAL OLO_ARCHIVE_FAIL_BYTES)
    # DELETE THE ARCHIVE BEFORE FAILING. This runs as a POST_BUILD step, so the .lib is already
    # written and newer than its inputs by the time we get here. Leave it in place and the next
    # build finds the target up to date, skips the project entirely -- post-build event included --
    # and reports success with the oversized archive still there. A gate you can clear by pressing
    # Build twice is not a gate. Removing the output keeps the target permanently out of date, so
    # the check re-runs and re-fails until someone actually recovers headroom.
    file(REMOVE "${OLO_ARCHIVE}")
    message(FATAL_ERROR "${_olo_archive_report}.\n${_olo_archive_advice}")
elseif(DEFINED OLO_ARCHIVE_WARN_BYTES AND _olo_archive_bytes GREATER_EQUAL OLO_ARCHIVE_WARN_BYTES)
    message(WARNING "${_olo_archive_report}.\n${_olo_archive_advice}")
elseif(NOT OLO_ARCHIVE_QUIET)
    message(STATUS "${_olo_archive_report}")
endif()
