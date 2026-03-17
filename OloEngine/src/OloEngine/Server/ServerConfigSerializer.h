#pragma once

#include "OloEngine/Server/ServerConfig.h"

#include <string>

namespace OloEngine
{
    class ServerConfigSerializer
    {
      public:
        // Load config from a YAML file. Returns default config if file doesn't exist.
        static ServerConfig LoadFromFile(const std::string& filepath);

        // Save config to a YAML file.
        static void SaveToFile(const ServerConfig& config, const std::string& filepath);

        // Parse command-line arguments and override config values.
        // Supported flags: --port, --max-players, --tick-rate, --scene, --config
        static ServerConfig ParseCommandLine(int argc, char** argv);
    };
} // namespace OloEngine
