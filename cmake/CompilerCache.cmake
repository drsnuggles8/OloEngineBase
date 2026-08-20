# CompilerCache.cmake
# Optional compiler caching (sccache / ccache) to speed up rebuilds by reusing
# object files. The big win is CI: object files are cached across runs, so a
# push only recompiles the translation units that actually changed.
#
# Enable with -DOLO_ENABLE_COMPILER_CACHE=ON (CI does this). Off by default for
# local dev so a plain configure behaves exactly as before.
#
# IMPORTANT — generator support: compiler launchers
# (CMAKE_<LANG>_COMPILER_LAUNCHER) are honored ONLY by the Ninja and Makefile
# generators. The Visual Studio (MSBuild) generator IGNORES them, so caching
# has NO effect there. Use a Ninja-based configuration to benefit — CI's Windows
# job switched to "Ninja Multi-Config" for exactly this reason, and the local
# `clangcl` preset is already Ninja-based.
#
# This module is included AFTER cmake/SetupConfigurations.cmake on purpose: that
# file sets CMAKE_MSVC_DEBUG_INFORMATION_FORMAT to ProgramDatabase (/Zi), and the
# Embedded (/Z7) override below must run later to win.

option(OLO_ENABLE_COMPILER_CACHE "Use sccache/ccache as a compiler launcher when available (Ninja/Makefiles only)" OFF)

if(OLO_ENABLE_COMPILER_CACHE)
    # Honor an explicit -DOLO_COMPILER_CACHE_TOOL=... ; otherwise prefer sccache and
    # fall back to ccache.
    #
    # The original reason for preferring sccache — "the only one with a usable
    # GitHub Actions cache backend" — is OBSOLETE: every CI job now uses a
    # local-disk SCCACHE_DIR persisted as a single actions/cache entry, precisely
    # because the GHA backend stores one entry PER OBJECT FILE and blew the cache
    # cap (see asan.yml's header). The order is kept anyway because the Linux CI
    # jobs and Windows.yml are green on sccache today, and selection order is not
    # worth destabilising for a preference that only matters locally.
    #
    # WINDOWS CAVEAT, worth knowing before you rely on sccache locally: the Windows
    # sccache server has a real crash mode under clang-cl — asan.yml's Windows job
    # disables sccache outright for it ("reliably crashes ... removes the crash
    # entirely"). For local Windows dev prefer ccache, pinned explicitly:
    #
    #     -DOLO_COMPILER_CACHE_TOOL=<absolute path to ccache.exe>
    #
    # Pin by ABSOLUTE PATH rather than trusting PATH: a bare "ccache" can resolve to
    # an unrelated bundled copy (on the maintainer's box C:\Strawberry\c\bin — which
    # also ships ninja/ctest/nasm — precedes LLVM and CMake on PATH). A machine-local
    # CMakeUserPresets.json is the right place for that absolute path; it is
    # gitignored.
    #
    # Cross-machine/worktree sharing needs the TOOL configured too, not just selected:
    # ccache wants base_dir=<worktree root> + hash_dir=false; sccache's equivalent is
    # SCCACHE_BASEDIRS, which unlike ccache's single base_dir accepts several paths
    # (";"-separated on Windows) — useful if worktrees span more than one drive.
    #
    # THAT SINGLE base_dir IS A HARD CONSTRAINT ON WHERE WORKTREES MAY LIVE, and getting it
    # wrong is silent. base_dir only rewrites absolute paths UNDER it, and the /Z7 override
    # below bakes the source path into every object — so the path is part of the hash and a
    # worktree outside base_dir shares with nobody. It still caches its OWN rebuilds, which
    # is exactly why the failure hides: the cache looks alive, just never warm.
    #
    # Measured 2026-08-20 with base_dir=D:\repos while /start-work was placing worktrees on
    # E:, real flags: D:->D: HIT, D:->E: MISS, and E:->E: SIBLINGS also MISS. Two pieces of
    # our own tooling disagreed about where worktrees live, so cross-worktree caching was
    # off for every worktree that command had ever created. base_dir is now C:\repos and
    # /start-work puts worktrees there; the two MUST be changed together.
    # An ENVIRONMENT variable is honoured between the explicit -D and PATH discovery.
    # This is what lets a machine pin the right binary ONCE and have every worktree
    # inherit it: a CMakeUserPresets.json is gitignored, so it does not propagate to a
    # new worktree, and PATH is not trustworthy for this (see the absolute-path note
    # below). Set OLO_COMPILER_CACHE_TOOL in the user environment and every configure,
    # in every worktree, picks the same tool without any per-tree setup.
    # Resolution order: explicit -D  >  environment  >  PATH discovery.
    #
    # Discovery deliberately lands in a SEPARATE cache entry. find_program() writes
    # its result into the cache under the variable it is given, so if it wrote to
    # OLO_COMPILER_CACHE_TOOL directly, the first configure's PATH pick would make
    # that variable permanently truthy — and every later configure would skip the
    # environment branch and silently keep using the stale tool. (Observed exactly
    # that: a first configure cached C:/Strawberry/c/bin/ccache.exe and the env var
    # was ignored from then on. Same family as
    # docs/agent-rules/configure-time-variable-visibility.md, inverted: not "unset on
    # the first configure" but "set forever after it".)
    if(NOT OLO_COMPILER_CACHE_TOOL)
        # NOT `if(DEFINED ENV{...})`: that is TRUE for a variable that exists with an
        # EMPTY value, which then sets the tool to "" and silently disables caching
        # instead of falling through to discovery. An empty value is how a shell
        # "clears" the variable in practice (PowerShell's SetEnvironmentVariable($null)
        # leaves it present-but-empty), so the empty case is the common one, not exotic.
        if(NOT "$ENV{OLO_COMPILER_CACHE_TOOL}" STREQUAL "")
            set(OLO_COMPILER_CACHE_TOOL "$ENV{OLO_COMPILER_CACHE_TOOL}")
        else()
            # NO_CACHE so the sccache-before-ccache preference is re-evaluated on EVERY
            # configure. A cached find_program result is sticky: install ccache after a
            # configure that already found sccache and the switch would never happen —
            # the same "set forever after the first configure" trap described above, one
            # level down. The unset() clears an entry written by an earlier revision of
            # this file, which did let find_program cache it.
            unset(OLO_COMPILER_CACHE_TOOL_DISCOVERED CACHE)
            find_program(OLO_COMPILER_CACHE_TOOL_DISCOVERED NAMES sccache ccache NO_CACHE)
            set(OLO_COMPILER_CACHE_TOOL "${OLO_COMPILER_CACHE_TOOL_DISCOVERED}")
        endif()
    endif()

    # Whatever we ended up with must actually exist — an env var pointing at an
    # upgraded-away path (the winget package directory embeds the VERSION) would
    # otherwise configure "successfully" and silently build uncached.
    if(OLO_COMPILER_CACHE_TOOL AND NOT EXISTS "${OLO_COMPILER_CACHE_TOOL}")
        find_program(_olo_cache_resolved NAMES "${OLO_COMPILER_CACHE_TOOL}")
        if(_olo_cache_resolved)
            set(OLO_COMPILER_CACHE_TOOL "${_olo_cache_resolved}")
        else()
            message(WARNING
                "OLO_COMPILER_CACHE_TOOL is set to '${OLO_COMPILER_CACHE_TOOL}' but that does "
                "not exist and is not on PATH — building WITHOUT a compiler cache. If this is a "
                "stale absolute path, a tool upgrade probably moved it.")
            set(OLO_COMPILER_CACHE_TOOL "")
        endif()
    endif()

    if(OLO_COMPILER_CACHE_TOOL)
        message(STATUS "Compiler cache enabled: ${OLO_COMPILER_CACHE_TOOL}")

        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            message(WARNING
                "OLO_ENABLE_COMPILER_CACHE is ON but the Visual Studio generator "
                "ignores CMAKE_<LANG>_COMPILER_LAUNCHER, so no caching will occur. "
                "Configure with a Ninja-based generator to benefit from "
                "'${OLO_COMPILER_CACHE_TOOL}'.")
        endif()

        # Set as normal (non-cache) variables in the top-level scope so they
        # propagate into every add_subdirectory() child that creates targets,
        # without sticking in the cache across an OLO_ENABLE_COMPILER_CACHE=OFF
        # reconfigure.
        set(CMAKE_C_COMPILER_LAUNCHER   "${OLO_COMPILER_CACHE_TOOL}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${OLO_COMPILER_CACHE_TOOL}")

        # MSVC with /Zi or /ZI (ProgramDatabase) writes debug info to a *shared*
        # .pdb. That output is not a pure function of a single TU's inputs, so
        # sccache/ccache refuse to cache it (near-zero hit rate). /Z7 (Embedded)
        # puts the debug info inside each .obj, making every object
        # self-contained and cacheable. SetupConfigurations.cmake set the default
        # to ProgramDatabase above; override it here (this include runs later).
        if(MSVC)
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,Release>:Embedded>")
        endif()

        # Disable precompiled headers while caching. sccache/ccache CANNOT cache a
        # compile that consumes a PCH (it reports the TU non-cacheable for /Fp /Yu),
        # so with PCH on the whole engine + editor + runtime + tests — the bulk of
        # the build and the part that changes per push — recompiles every run and
        # the cache only covers vendor libs. PCH and the cache optimize opposite
        # cases: PCH speeds a *cold* compile; the cache makes a *warm* compile a
        # near-instant hit. For CI's common case (incremental pushes = warm cache)
        # caching the engine is the far bigger win; the occasional cold run (first
        # build / toolchain bump / cache eviction) pays a re-parse penalty.
        # Every engine TU #includes OloEnginePCH.h explicitly, so dropping the
        # precompile (and its force-include) is safe. OLO_ENABLE_PCH is an option()
        # in the top-level CMakeLists; override it here.
        set(OLO_ENABLE_PCH OFF CACHE BOOL "Disabled: MSVC PCH (/Fp) is non-cacheable under sccache/ccache" FORCE)

        # Disable unity (jumbo) builds while caching — same reasoning as PCH above, opposite
        # trade-off. Unity batches 16 TUs into one object; editing a single line in any file
        # of a batch changes that whole jumbo object's inputs, so its cache key misses and all
        # 16 TUs recompile. On CI's common case (incremental push = warm cache) that turns a
        # 1-TU recompile into a 16-TU recompile — a net loss. Unity speeds *cold* builds (it
        # amortizes header re-parsing across the batch); the cache speeds *warm* builds. They
        # optimize opposite cases and don't compose, so when caching is on the cache wins and
        # unity is forced off. OLO_ENABLE_UNITY_BUILD is an option() in the top-level
        # CMakeLists; override it here (mirrors the PCH override above).
        set(OLO_ENABLE_UNITY_BUILD OFF CACHE BOOL "Disabled: jumbo TUs bust the per-object cache key under sccache/ccache" FORCE)

        # NOTE: a few vendored libraries force MSVC /Zi + a separate PDB regardless
        # of the format above, which sccache/ccache cannot cache. Those are turned
        # off (only when caching) next to their other options in
        # OloEngine/vendor/CMakeLists.txt — search OLO_ENABLE_COMPILER_CACHE there.
    else()
        message(WARNING
            "OLO_ENABLE_COMPILER_CACHE is ON but neither 'sccache' nor 'ccache' "
            "was found on PATH — building without a compiler cache.")
    endif()
endif()
