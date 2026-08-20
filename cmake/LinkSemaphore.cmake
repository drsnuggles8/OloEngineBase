# LinkSemaphore.cmake
# Cross-BUILD-TREE link throttling.
#
# The `olo_link` Ninja job pool in the root CMakeLists.txt bounds concurrent link
# steps, but it is scoped to ONE BUILD TREE. That is a real gap, not a theoretical
# one — measured 2026-08-19 by running several worktrees' builds concurrently with
# the build mutex bypassed, sampling every 15s:
#
#     1 concurrent linker  -> avg 41.2 GiB in use
#     2 concurrent linkers -> avg 43.4 GiB, max 55.3    peak COMPILERS: 2
#     3 concurrent linkers -> 59.1 GiB, 4.7 GiB free    (+11.3 GiB in one 15s sample)
#
# Three linkers took a 64 GB host to within 4.7 GiB of nothing while the peak
# compiler count was only two. N concurrent trees get up to N x OLO_LINK_JOBS
# linkers, and no per-tree pool can see the others.
#
# This wires every link step through .claude/skills/run-oloengine/link-semaphore.ps1,
# which takes a permit from an OS-named semaphore shared by every tree on the machine.
# The wrapper FAILS OPEN by design (see its header): if the semaphore is unavailable
# the link runs unthrottled rather than failing.
#
# GENERATOR SUPPORT, and why it shapes the whole policy: CMAKE_<LANG>_LINKER_LAUNCHER
# is honoured ONLY by the Ninja and Makefile generators — the same restriction as
# CMAKE_<LANG>_COMPILER_LAUNCHER (see cmake/CompilerCache.cmake). The Visual Studio
# generator ignores BOTH, so a `build/` tree has no compiler cache AND no link
# throttling of any kind. That is why build-lock.ps1 only ever grants a SECOND
# concurrent build slot to a cached (Ninja) tree: concurrency is something a tree
# earns by being throttleable, and the VS tree cannot be.
#
# Scope note: this covers executable and shared-library links. Static archiving goes
# through CMAKE_<LANG>_ARCHIVE_* rules, which take no launcher — acceptable, because
# the measured spike is the linker, not the archiver.

option(OLO_ENABLE_LINK_SEMAPHORE
       "Throttle link steps across ALL build trees via a shared OS semaphore (Ninja/Makefiles only)"
       ON)

set(OLO_LINK_SEMAPHORE_SLOTS "2" CACHE STRING
    "Permits for the cross-tree link semaphore. 2 is the measured safe ceiling on a 64 GB host.")

if(OLO_ENABLE_LINK_SEMAPHORE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        # Not a warning: the VS tree is a legitimate, expected configuration and the
        # build mutex compensates by never granting it a concurrent slot. Saying this
        # at STATUS keeps the log honest without crying wolf on every configure.
        message(STATUS
            "Link semaphore: unavailable under the Visual Studio generator "
            "(it ignores CMAKE_<LANG>_LINKER_LAUNCHER). This tree will only ever build "
            "exclusively — see .claude/skills/run-oloengine/build-lock.ps1.")
    elseif(NOT OLO_LINK_SEMAPHORE_SLOTS MATCHES "^[1-9][0-9]*$")
        # Same rule, and the same reasoning, as OLO_LINK_JOBS in the root CMakeLists:
        # a value that silently disables a memory bound is worse than one that fails.
        message(FATAL_ERROR
            "OLO_LINK_SEMAPHORE_SLOTS must be a positive integer (got "
            "'${OLO_LINK_SEMAPHORE_SLOTS}'). Set OLO_ENABLE_LINK_SEMAPHORE=OFF to turn "
            "the throttle off deliberately, rather than passing a value that disables it "
            "by accident.")
    else()
        find_program(OLO_PWSH NAMES pwsh powershell)
        set(_olo_link_wrapper "${CMAKE_SOURCE_DIR}/.claude/skills/run-oloengine/link-semaphore.ps1")

        if(NOT OLO_PWSH)
            message(WARNING
                "Link semaphore: neither 'pwsh' nor 'powershell' found on PATH — linking "
                "WITHOUT cross-tree throttling. Concurrent builds in other worktrees could "
                "then overlap their link steps.")
        elseif(NOT EXISTS "${_olo_link_wrapper}")
            message(WARNING
                "Link semaphore: wrapper not found at '${_olo_link_wrapper}' — linking "
                "WITHOUT cross-tree throttling.")
        else()
            # A launcher is a semicolon-separated LIST: each element becomes one argv
            # entry, so the wrapper path is never re-parsed as a string and a path with
            # spaces cannot split. The real linker and its arguments are appended by
            # CMake after these.
            set(CMAKE_C_LINKER_LAUNCHER   "${OLO_PWSH};-NoProfile;-File;${_olo_link_wrapper}")
            set(CMAKE_CXX_LINKER_LAUNCHER "${OLO_PWSH};-NoProfile;-File;${_olo_link_wrapper}")
            message(STATUS
                "Link semaphore: ON, ${OLO_LINK_SEMAPHORE_SLOTS} permit(s) shared across every build tree")
        endif()
    endif()
endif()
