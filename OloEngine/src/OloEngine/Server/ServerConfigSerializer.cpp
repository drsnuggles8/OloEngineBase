#include "OloEnginePCH.h"
#include "ServerConfigSerializer.h"

#include "OloEngine/Core/Log.h"

#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    ServerConfig ServerConfigSerializer::LoadFromFile(const std::string& filepath)
    {
        ServerConfig config;

        if (!std::filesystem::exists(filepath))
        {
            OLO_CORE_WARN("[ServerConfig] Config file '{}' not found, using defaults.", filepath);
            return config;
        }

        try
        {
            YAML::Node root = YAML::LoadFile(filepath);

            if (root["port"])
            {
                config.Port = root["port"].as<u16>();
            }
            if (root["maxPlayers"])
            {
                config.MaxPlayers = root["maxPlayers"].as<u32>();
            }
            if (root["tickRate"])
            {
                config.TickRate = root["tickRate"].as<u32>();
            }
            if (root["scene"])
            {
                config.ScenePath = root["scene"].as<std::string>();
            }
            if (root["password"])
            {
                config.Password = root["password"].as<std::string>();
            }
            if (root["logLevel"])
            {
                config.LogLevel = root["logLevel"].as<std::string>();
            }
            if (root["autoSaveInterval"])
            {
                config.AutoSaveInterval = root["autoSaveInterval"].as<u32>();
            }

            OLO_CORE_INFO("[ServerConfig] Loaded config from '{}'", filepath);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("[ServerConfig] Failed to parse '{}': {}", filepath, e.what());
        }

        return config;
    }

    void ServerConfigSerializer::SaveToFile(const ServerConfig& config, const std::string& filepath)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "port" << YAML::Value << config.Port;
        out << YAML::Key << "maxPlayers" << YAML::Value << config.MaxPlayers;
        out << YAML::Key << "tickRate" << YAML::Value << config.TickRate;
        out << YAML::Key << "scene" << YAML::Value << config.ScenePath;
        out << YAML::Key << "password" << YAML::Value << config.Password;
        out << YAML::Key << "logLevel" << YAML::Value << config.LogLevel;
        out << YAML::Key << "autoSaveInterval" << YAML::Value << config.AutoSaveInterval;
        out << YAML::EndMap;

        std::ofstream fout(filepath);
        fout << out.c_str();
    }

    ServerConfig ServerConfigSerializer::ParseCommandLine(int argc, char** argv)
    {
        ServerConfig config;
        std::string configFile;

        // First pass: look for --config to load base config from file
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc)
            {
                configFile = argv[++i];
            }
        }

        if (!configFile.empty())
        {
            config = LoadFromFile(configFile);
        }

        // Second pass: command-line flags override file values
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc)
            {
                config.Port = static_cast<u16>(std::stoi(argv[++i]));
            }
            else if (arg == "--max-players" && i + 1 < argc)
            {
                config.MaxPlayers = static_cast<u32>(std::stoi(argv[++i]));
            }
            else if (arg == "--tick-rate" && i + 1 < argc)
            {
                config.TickRate = static_cast<u32>(std::stoi(argv[++i]));
            }
            else if (arg == "--scene" && i + 1 < argc)
            {
                config.ScenePath = argv[++i];
            }
            else if (arg == "--config" && i + 1 < argc)
            {
                ++i; // Already handled in first pass
            }
        }

        return config;
    }

} // namespace OloEngine
