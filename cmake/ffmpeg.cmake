# =============================================================================
# ffmpeg.cmake — build FFmpeg from source for the OloEngine FFmpeg video backend
# (H.264/HEVC/VP9 in MP4/MOV/MKV; the pl_mpeg backend still handles MPEG-1).
#
# On by default; turn off with -DOLO_VIDEO_FFMPEG=OFF (then only the pl_mpeg MPEG-1
# backend is available). When on, an ExternalProject fetches FFmpeg and runs
# scripts/build-ffmpeg.sh, installing into a per-user cache SHARED BY EVERY WORKTREE
# (see the cache block below). Defines an INTERFACE `ffmpeg` target (includes + libs).
# NOTE: building FFmpeg from source needs nasm (+ a bash; + Visual Studio on Windows)
# on the build machine / CI runner.
#
# Cross-platform:
#   * Windows: MSVC-toolchain build -> import libs (lib/<name>.lib) + DLLs (bin/).
#     Needs a MSYS/MINGW bash, nasm, gmake, and Visual Studio (via vswhere).
#   * Linux:   native gcc/clang build -> shared objects (lib/lib<name>.so). Needs
#     a bash, nasm, and make.
#
# Fast iteration: point at an already-built tree to skip the ExternalProject build:
#   -DOLO_VIDEO_FFMPEG=ON -DOLO_FFMPEG_PREFIX=/path/to/ffmpeg-install
# =============================================================================

option(OLO_VIDEO_FFMPEG "Build FFmpeg from source for H.264/MP4 video decoding" ON)

if(OLO_VIDEO_FFMPEG)
    set(FFMPEG_LIBS avcodec avformat avutil swscale swresample)

    # Per-platform link-library file name + runtime (shared-lib) subdir.
    if(WIN32)
        set(_ff_prefix "")     # import lib:   <name>.lib
        set(_ff_suffix ".lib")
        set(_ff_runtime_subdir "bin")
    else()
        set(_ff_prefix "lib")  # shared object: lib<name>.so
        set(_ff_suffix ".so")
        set(_ff_runtime_subdir "lib")
    endif()

    # Full path to the link library for a given component name.
    macro(_olo_ffmpeg_lib_path _out _name)
        set(${_out} "${_ffmpeg_prefix}/lib/${_ff_prefix}${_name}${_ff_suffix}")
    endmacro()

    # --- Where the build lands: a per-user, TAG-KEYED cache shared by every worktree ---
    #
    # FFmpeg is ~15 minutes of configure + gmake that the compiler cache cannot touch —
    # it builds through its own configure/make, not CMake's compiler launcher — and it
    # depends only on the pinned tag and the host toolchain, never on our sources. It used
    # to install into ${PROJECT_SOURCE_DIR}/vendor, i.e. INSIDE the worktree, so every
    # task worktree rebuilt it from scratch and carried its own ~420 MB source clone
    # (2.5 GB across six worktrees on this box, measured 2026-09-06).
    #
    # Same shape as OpenUSD.cmake's cache, and the same caveat: two worktrees running
    # their FIRST ffmpeg build concurrently share one tree. Sequence the first build; every
    # tree after it is a lock-free cache hit that never writes to the shared tree at all.
    #
    # Bumping OLO_FFMPEG_TAG changes the key, so a version bump invalidates the cache on
    # its own. To force a genuine rebuild, delete the directory the STATUS message names.
    set(OLO_FFMPEG_TAG "n7.1" CACHE STRING "FFmpeg git tag to build")
    if(WIN32)
        set(_olo_ffmpeg_cache_default "$ENV{LOCALAPPDATA}/OloEngine/ffmpeg-${OLO_FFMPEG_TAG}")
    else()
        set(_olo_ffmpeg_cache_default "$ENV{HOME}/.cache/oloengine/ffmpeg-${OLO_FFMPEG_TAG}")
    endif()
    set(OLO_FFMPEG_CACHE_DIR "${_olo_ffmpeg_cache_default}" CACHE PATH
        "Shared FFmpeg source/install cache (per user, keyed by OLO_FFMPEG_TAG)")

    set(_ffmpeg_prefix "${OLO_FFMPEG_CACHE_DIR}/install")
    set(_ffmpeg_src "${OLO_FFMPEG_CACHE_DIR}/src")

    # The old per-worktree tree is dead weight once the cache is in use; say so rather
    # than leaving ~440 MB per worktree that nothing references any more.
    if(EXISTS "${PROJECT_SOURCE_DIR}/vendor/ffmpeg-src")
        message(STATUS "OLO_VIDEO_FFMPEG: ${PROJECT_SOURCE_DIR}/vendor/ffmpeg-{src,install} is the "
                       "pre-cache per-worktree tree and is no longer used - safe to delete (~440 MB)")
    endif()

    if(DEFINED OLO_FFMPEG_PREFIX AND EXISTS "${OLO_FFMPEG_PREFIX}/lib/${_ff_prefix}avcodec${_ff_suffix}")
        # --- Use a pre-built FFmpeg tree (skip the ExternalProject build). ---
        message(STATUS "OLO_VIDEO_FFMPEG: using pre-built FFmpeg at ${OLO_FFMPEG_PREFIX}")
        set(_ffmpeg_prefix "${OLO_FFMPEG_PREFIX}")
        set(_olo_ffmpeg_resolved TRUE)
        add_library(ffmpeg INTERFACE)
    else()
        set(_olo_ffmpeg_resolved FALSE)
        # --- Cache hit: EVERY consumed library is already in the shared cache. ---
        #
        # Skip ExternalProject entirely rather than re-entering a finished tree.
        # ExternalProject's STAMP_DIR defaults to the CONSUMING build tree, so a second
        # worktree has no stamps, concludes every step is out of date, and re-runs the
        # build inside the shared source dir — the artifacts were shared but the "already
        # done" record was not. Treating a complete cache as a prebuilt install means the
        # second and every later worktree never writes to it at all.
        #
        # Deliberately requires ALL of FFMPEG_LIBS: a partial cache (an interrupted or
        # killed first build) falls through and resumes the real build instead of linking
        # against a half-installed tree.
        set(_olo_ffmpeg_cache_complete TRUE)
        foreach(_l ${FFMPEG_LIBS})
            _olo_ffmpeg_lib_path(_p ${_l})
            if(NOT EXISTS "${_p}")
                set(_olo_ffmpeg_cache_complete FALSE)
                break()
            endif()
        endforeach()
    endif()

    if(NOT _olo_ffmpeg_resolved AND _olo_ffmpeg_cache_complete)
        message(STATUS "OLO_VIDEO_FFMPEG: reusing the shared FFmpeg build at ${_ffmpeg_prefix} "
                       "(delete that directory to force a rebuild)")
        add_library(ffmpeg INTERFACE)
    elseif(NOT _olo_ffmpeg_resolved)
        # --- Build FFmpeg from source via ExternalProject, into the shared cache. ---
        message(STATUS "OLO_VIDEO_FFMPEG: building FFmpeg ${OLO_FFMPEG_TAG} into ${OLO_FFMPEG_CACHE_DIR} "
                       "- this is a one-off per machine; later worktrees reuse it")
        include(ExternalProject)

        # Locate a bash to run build-ffmpeg.sh. On Windows the WSL launcher
        # (C:/Windows/System32/bash.exe) identifies itself as bash but runs in
        # the Linux filesystem, so it cannot read Windows-style paths (D:/...)
        # and fails the build with a misleading "No such file or directory".
        # Prefer a MSYS2/Git bash, which handles drive-letter paths. Search Git's
        # install locations first; the unconditional find_program below then falls
        # back to PATH for Linux / other setups (and is a no-op if already found).
        if(WIN32)
            find_program(OLO_BASH_EXE NAMES bash
                PATHS
                    "$ENV{PROGRAMFILES}/Git/usr/bin"
                    "$ENV{PROGRAMFILES}/Git/bin"
                    "$ENV{PROGRAMW6432}/Git/usr/bin"
                    "$ENV{LOCALAPPDATA}/Programs/Git/usr/bin"
                    "C:/msys64/usr/bin"
                    "C:/msys64/mingw64/bin"
                NO_DEFAULT_PATH)
        endif()
        find_program(OLO_BASH_EXE NAMES bash REQUIRED)

        set(_ffmpeg_script "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh")

        set(_ffmpeg_byproducts)
        foreach(_l ${FFMPEG_LIBS})
            _olo_ffmpeg_lib_path(_p ${_l})
            list(APPEND _ffmpeg_byproducts "${_p}")
        endforeach()

        ExternalProject_Add(ffmpeg_external
            GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
            GIT_TAG        ${OLO_FFMPEG_TAG}
            GIT_SHALLOW    TRUE
            SOURCE_DIR     "${_ffmpeg_src}"
            CONFIGURE_COMMAND ""
            BUILD_IN_SOURCE 1
            BUILD_COMMAND  "${OLO_BASH_EXE}" "${_ffmpeg_script}" "${_ffmpeg_src}" "${_ffmpeg_prefix}" build
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${_ffmpeg_byproducts}
            USES_TERMINAL_BUILD 1
        )

        add_library(ffmpeg INTERFACE)
        add_dependencies(ffmpeg ffmpeg_external)
    endif()

    # The include dir may not exist until the build runs; create it so CMake's
    # INTERFACE_INCLUDE_DIRECTORIES existence check passes at configure time.
    file(MAKE_DIRECTORY "${_ffmpeg_prefix}/include")
    target_include_directories(ffmpeg INTERFACE "${_ffmpeg_prefix}/include")
    foreach(_l ${FFMPEG_LIBS})
        _olo_ffmpeg_lib_path(_p ${_l})
        target_link_libraries(ffmpeg INTERFACE "${_p}")
    endforeach()

    # Exposed so app/test targets can copy the runtime shared libs next to their exe
    # (see olo_copy_ffmpeg_runtime, which also sets an $ORIGIN rpath on non-Windows).
    set(OLO_FFMPEG_RUNTIME_DIR "${_ffmpeg_prefix}/${_ff_runtime_subdir}" CACHE INTERNAL "FFmpeg runtime lib dir")
endif()
