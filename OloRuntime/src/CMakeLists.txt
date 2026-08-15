if(WIN32)
    add_executable(OloRuntime
        OloRuntimeApp.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/OloRuntime.rc"
    )
else()
    add_executable(OloRuntime
        OloRuntimeApp.cpp
    )
endif()

# Deploy the FFmpeg runtime libs next to the exe (no-op when OLO_VIDEO_FFMPEG=OFF).
# Must live in the same directory scope as add_executable — see the note on
# olo_copy_ffmpeg_runtime in cmake/CommonProperties.cmake.
olo_copy_ffmpeg_runtime(OloRuntime)

# Stage steam_api64.dll next to the exe (#644). No-op unless OLO_WITH_STEAM is on with the real
# SDK. A packaged game needs this beside the binary or the process fails to start.
olo_copy_steam_runtime(OloRuntime)

