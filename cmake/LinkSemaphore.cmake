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
# This wires every link step through a wrapper that takes one of N permits shared by
# every tree on the machine. There are two, picked by platform, because the ownership
# property that makes a permit reclaimable when its holder is killed has to be obtained
# differently on each:
#
#   Windows   .claude/skills/run-oloengine/link-semaphore.ps1 — N named MUTEXES, because
#             a Windows semaphore's count is owned by nobody and a killed holder leaks
#             its permit forever (that script's header has the measurement).
#   POSIX     scripts/link-semaphore.py — flock(), where the kernel gives ownership for
#             free: the lock belongs to the open file description and is released when
#             the process dies, however it dies.
#
# THE POSIX HALF IS NEW (issue #1313) AND IT CLOSED A REAL HOLE. Before it, a non-Windows
# host found no pwsh, warned, and linked unthrottled — so on the olo-ci box NOTHING
# bounded a measured 11.17 GiB link (Debug+ASan; 10.03 GiB with no sanitizer) against a
# 14 GiB unit cap and a 19 GiB slice shared by both runner slots. The Ninja pools do not
# cover it there: every Linux job configures with no -G and gets Unix Makefiles, where
# `olo_link` and `olo_heavy` silently do not exist.
#
# Both wrappers FAIL OPEN by design: if the permit mechanism is unavailable, or every
# permit is busy past the timeout, the link runs unthrottled rather than failing. A
# throttle that can fail a build is worse than no throttle.
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
        # PICK THE WRAPPER BY PLATFORM. Both implement the same contract — take one of
        # OLO_LINK_SEMAPHORE_SLOTS permits, run the link, fail OPEN on any problem — but
        # they cannot share an implementation: the Windows one needs N named mutexes to
        # get ownership semantics a Windows semaphore does not have, while POSIX gets
        # ownership from the kernel for free because an flock belongs to the open file
        # description and dies with the process holding it.
        #
        # THE POSIX HALF IS WHY THIS BLOCK EXISTS AT ALL (issue #1313). Until it landed,
        # a non-Windows host found no pwsh, warned, and linked unthrottled — which on the
        # olo-ci box meant NOTHING bounded a measured 11.17 GiB link against a 14 GiB unit
        # cap, because OLO_LINK_JOBS and olo_heavy are Ninja pools and every Linux job
        # gets Unix Makefiles.
        if(WIN32)
            find_program(OLO_LINK_SEMAPHORE_RUNNER NAMES pwsh powershell)
            set(_olo_link_wrapper "${CMAKE_SOURCE_DIR}/.claude/skills/run-oloengine/link-semaphore.ps1")
            set(_olo_link_runner_args "-NoProfile;-File")
            set(_olo_link_runner_kind "pwsh")
        else()
            find_program(OLO_LINK_SEMAPHORE_RUNNER NAMES python3 python)
            set(_olo_link_wrapper "${CMAKE_SOURCE_DIR}/scripts/link-semaphore.py")
            set(_olo_link_runner_args "")
            set(_olo_link_runner_kind "python3")
        endif()

        if(NOT OLO_LINK_SEMAPHORE_RUNNER)
            message(WARNING
                "Link semaphore: no '${_olo_link_runner_kind}' on PATH — linking WITHOUT "
                "cross-tree throttling. Concurrent builds, or concurrent links inside one "
                "Makefiles build, could then overlap. See issue #1313 for what that costs.")
        elseif(NOT EXISTS "${_olo_link_wrapper}")
            message(WARNING
                "Link semaphore: wrapper not found at '${_olo_link_wrapper}' — linking "
                "WITHOUT cross-tree throttling.")
        else()
            # THE PERMIT COUNT TRAVELS IN THE ENVIRONMENT, and `cmake -E env` is what puts
            # it there. Both wrappers read OLO_LINK_SEMAPHORE_SLOTS from the environment
            # rather than argv, because a linker command line is full of tokens an argument
            # parser would try to interpret (`-o`, `--start-group`, a bare `--`) — so every
            # token after the wrapper path has to be forwarded verbatim as the command.
            #
            # Without this the cache variable was decorative: it changed the STATUS message
            # below and nothing else, so `-DOLO_LINK_SEMAPHORE_SLOTS=4` silently kept
            # throttling at the wrapper's built-in default of 2. Setting it here makes the
            # documented knob the authority.
            #
            # A launcher is a semicolon-separated LIST: each element becomes one argv entry,
            # so a path with spaces cannot split. The real linker and its arguments are
            # appended by CMake after these.
            set(_olo_link_launcher
                "${CMAKE_COMMAND};-E;env;OLO_LINK_SEMAPHORE_SLOTS=${OLO_LINK_SEMAPHORE_SLOTS};${OLO_LINK_SEMAPHORE_RUNNER}")
            if(_olo_link_runner_args)
                set(_olo_link_launcher "${CMAKE_COMMAND};-E;env;OLO_LINK_SEMAPHORE_SLOTS=${OLO_LINK_SEMAPHORE_SLOTS};${OLO_LINK_SEMAPHORE_RUNNER};${_olo_link_runner_args}")
            endif()
            set(CMAKE_C_LINKER_LAUNCHER   "${_olo_link_launcher};${_olo_link_wrapper}")
            set(CMAKE_CXX_LINKER_LAUNCHER "${_olo_link_launcher};${_olo_link_wrapper}")
            message(STATUS
                "Link semaphore: ON, ${OLO_LINK_SEMAPHORE_SLOTS} permit(s) shared across every build tree "
                "(${_olo_link_runner_kind})")
        endif()
    endif()
endif()
