#include "OloEnginePCH.h"
#include "ServerConfigSerializer.h"

#include "OloEngine/Core/Log.h"

#include <charconv>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    ServerConfig ServerConfigSerializer::LoadFromFile(const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        ServerConfig config;

        std::error_code fsEc;
        bool fileExists = std::filesystem::exists(filepath, fsEc);
        if (fsEc)
        {
            OLO_CORE_ERROR("[ServerConfig] Failed to check '{}': {}", filepath, fsEc.message());
            return config;
        }
        if (!fileExists)
        {
            OLO_CORE_WARN("[ServerConfig] Config file '{}' not found, using defaults.", filepath);
            return config;
        }

        try
        {
            YAML::Node root = YAML::LoadFile(filepath);
            ServerConfig tempConfig;

            if (root["port"])
            {
                auto port = root["port"].as<int>();
                if (port < 1 || port > 65535)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid port {} in '{}': must be 1-65535", port, filepath);
                }
                else
                {
                    tempConfig.Port = static_cast<u16>(port);
                }
            }
            if (root["maxPlayers"])
            {
                auto val = root["maxPlayers"].as<u32>();
                if (val == 0)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid maxPlayers 0 in '{}': must be > 0", filepath);
                }
                else
                {
                    tempConfig.MaxPlayers = val;
                }
            }
            if (root["tickRate"])
            {
                auto val = root["tickRate"].as<u32>();
                if (val == 0)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid tickRate 0 in '{}': must be > 0", filepath);
                }
                else
                {
                    tempConfig.TickRate = val;
                }
            }
            if (root["scene"])
            {
                tempConfig.ScenePath = root["scene"].as<std::string>();
            }
            if (root["password"])
            {
                tempConfig.Password = root["password"].as<std::string>();
            }
            if (root["logLevel"])
            {
                tempConfig.LogLevel = root["logLevel"].as<std::string>();
            }
            if (root["autoSaveInterval"])
            {
                tempConfig.AutoSaveInterval = root["autoSaveInterval"].as<u32>();
            }

            config = std::move(tempConfig);
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
        OLO_PROFILE_FUNCTION();

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

        // Atomic save: write to a temp file, flush, then rename over the original
        const std::string tempPath = filepath + ".tmp";
        {
            std::ofstream fout(tempPath);
            if (!fout.is_open())
            {
                OLO_CORE_ERROR("[ServerConfig] Failed to open '{}' for writing", tempPath);
                return;
            }
            fout << out.c_str();
            fout.flush();
            if (!fout.good())
            {
                OLO_CORE_ERROR("[ServerConfig] Failed to write config to '{}'", tempPath);
                fout.close();
                std::error_code ec;
                std::filesystem::remove(tempPath, ec);
                return;
            }
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, filepath, ec);
        if (ec)
        {
            OLO_CORE_ERROR("[ServerConfig] Failed to rename '{}' -> '{}': {}", tempPath, filepath, ec.message());
            std::filesystem::remove(tempPath, ec);
        }
    }

    ServerConfig ServerConfigSerializer::ParseCommandLine(int argc, char** argv)
    {
        OLO_PROFILE_FUNCTION();

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

        // Helper: returns true if a string looks like an option flag (starts with '-')
        auto isOptionToken = [](const char* s)
        { return s != nullptr && s[0] == '-'; };

        // Second pass: command-line flags override file values
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc)
            {
                if (isOptionToken(argv[i + 1]))
                {
                    OLO_CORE_ERROR("[ServerConfig] Missing value for --port");
                    continue;
                }
                const char* val = argv[++i];
                const char* end = val + std::strlen(val);
                int parsed = 0;
                auto [ptr, ec] = std::from_chars(val, end, parsed);
                if (ec != std::errc{} || ptr != end || parsed < 1 || parsed > 65535)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid --port value '{}': must be 1-65535", val);
                }
                else
                {
                    config.Port = static_cast<u16>(parsed);
                }
            }
            else if (arg == "--max-players" && i + 1 < argc)
            {
                if (isOptionToken(argv[i + 1]))
                {
                    OLO_CORE_ERROR("[ServerConfig] Missing value for --max-players");
                    continue;
                }
                const char* val = argv[++i];
                const char* end = val + std::strlen(val);
                u32 parsed = 0;
                auto [ptr, ec] = std::from_chars(val, end, parsed);
                if (ec != std::errc{} || ptr != end || parsed == 0)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid --max-players value '{}': must be a positive integer", val);
                }
                else
                {
                    config.MaxPlayers = parsed;
                }
            }
            else if (arg == "--tick-rate" && i + 1 < argc)
            {
                if (isOptionToken(argv[i + 1]))
                {
                    OLO_CORE_ERROR("[ServerConfig] Missing value for --tick-rate");
                    continue;
                }
                const char* val = argv[++i];
                const char* end = val + std::strlen(val);
                u32 parsed = 0;
                auto [ptr, ec] = std::from_chars(val, end, parsed);
                if (ec != std::errc{} || ptr != end || parsed == 0)
                {
                    OLO_CORE_ERROR("[ServerConfig] Invalid --tick-rate value '{}': must be a positive integer", val);
                }
                else
                {
                    config.TickRate = parsed;
                }
            }
            else if (arg == "--scene" && i + 1 < argc)
            {
                if (isOptionToken(argv[i + 1]))
                {
                    OLO_CORE_ERROR("[ServerConfig] Missing value for --scene");
                    continue;
                }
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
