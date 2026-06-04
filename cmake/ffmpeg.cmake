# =============================================================================
# ffmpeg.cmake — build FFmpeg from source for the OloEngine FFmpeg video backend
# (H.264/HEVC/VP9 in MP4/MOV/MKV; the pl_mpeg backend still handles MPEG-1).
#
# On by default; turn off with -DOLO_VIDEO_FFMPEG=OFF (then only the pl_mpeg MPEG-1
# backend is available). When on, an ExternalProject fetches FFmpeg (n7.1) and runs
# scripts/build-ffmpeg.sh, installing under vendor/ffmpeg-install. Defines an INTERFACE
# `ffmpeg` target (includes + libs). NOTE: building FFmpeg from source needs nasm (+ a
# bash; + Visual Studio on Windows) on the build machine / CI runner.
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

    if(DEFINED OLO_FFMPEG_PREFIX AND EXISTS "${OLO_FFMPEG_PREFIX}/lib/${_ff_prefix}avcodec${_ff_suffix}")
        # --- Use a pre-built FFmpeg tree (skip the ExternalProject build). ---
        message(STATUS "OLO_VIDEO_FFMPEG: using pre-built FFmpeg at ${OLO_FFMPEG_PREFIX}")
        set(_ffmpeg_prefix "${OLO_FFMPEG_PREFIX}")
        add_library(ffmpeg INTERFACE)
    else()
        # --- Build FFmpeg from source via ExternalProject. ---
        include(ExternalProject)
        find_program(OLO_BASH_EXE NAMES bash REQUIRED)

        set(_ffmpeg_prefix "${PROJECT_SOURCE_DIR}/vendor/ffmpeg-install")
        set(_ffmpeg_src "${PROJECT_SOURCE_DIR}/vendor/ffmpeg-src")
        set(_ffmpeg_script "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh")

        set(_ffmpeg_byproducts)
        foreach(_l ${FFMPEG_LIBS})
            _olo_ffmpeg_lib_path(_p ${_l})
            list(APPEND _ffmpeg_byproducts "${_p}")
        endforeach()

        ExternalProject_Add(ffmpeg_external
            GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
            GIT_TAG        n7.1
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
