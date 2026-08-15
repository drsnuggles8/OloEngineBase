add_executable(OloServer
    OloServerApp.cpp
)

# Deploy the FFmpeg runtime libs next to the exe (no-op when OLO_VIDEO_FFMPEG=OFF).
# Must live in the same directory scope as add_executable — see the note on
# olo_copy_ffmpeg_runtime in cmake/CommonProperties.cmake.
olo_copy_ffmpeg_runtime(OloServer)

# Stage steam_api64.dll next to the exe (#644) even though OloServer NEVER calls Steam — the
# headless path skips SteamManager::Initialize() entirely.
#
# It is still required. OloEngine links steam_api64.lib, an IMPORT library, so every executable
# linking the engine carries a static import on steam_api64.dll and Windows resolves it at
# PROCESS LOAD, before a single line of our code runs. Without the DLL beside the binary the
# server fails to start with a bare "the code execution cannot proceed" dialog and no log line —
# a failure that looks nothing like its cause. No-op unless OLO_WITH_STEAM is on with the real SDK.
olo_copy_steam_runtime(OloServer)
