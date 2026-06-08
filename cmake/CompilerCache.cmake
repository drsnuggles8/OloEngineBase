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
    # Honor an explicit -DOLO_COMPILER_CACHE_TOOL=... ; otherwise prefer sccache
    # (the only one with a usable GitHub Actions cache backend) and fall back to
    # ccache for local dev.
    if(NOT OLO_COMPILER_CACHE_TOOL)
        find_program(OLO_COMPILER_CACHE_TOOL NAMES sccache ccache)
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
