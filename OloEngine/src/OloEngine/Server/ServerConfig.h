#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    struct ServerConfig
    {
        u16 Port = 7777;
        u32 MaxPlayers = 64;
        u32 TickRate = 60;
        // Replication ticks per second. Deliberately separate from TickRate: the
        // simulation advances at the fixed timestep for determinism, while the wire
        // rate is a bandwidth decision. 20 Hz is the usual shooter default.
        u32 SnapshotRate = 20;
        std::string ScenePath;
        // Project root to mount (the directory containing `Assets/`). Empty means
        // "infer it from ScenePath". Without a mounted Project, anything resolved
        // through Project::GetAssetFileSystemPath — notably every Lua script a
        // scene references — fails to load, so a dedicated server could host a
        // scene but never run its server-side gameplay scripts.
        std::string ProjectPath;
        std::string Password;
        std::string LogLevel = "Info";
        u32 AutoSaveInterval = 300; // seconds
    };
} // namespace OloEngine
