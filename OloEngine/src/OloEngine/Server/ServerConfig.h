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
        std::string ScenePath;
        std::string Password;
        std::string LogLevel = "Info";
        u32 AutoSaveInterval = 300; // seconds
    };
} // namespace OloEngine
