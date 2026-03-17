#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class ServerConsole
    {
      public:
        using CommandHandler = std::function<void(const std::vector<std::string>& args)>;

        ServerConsole();
        ~ServerConsole();

        void Initialize();
        void Shutdown();

        // Non-blocking stdin read — call once per frame/tick
        void ProcessInput();

        // Register a custom command handler
        void RegisterCommand(const std::string& name, CommandHandler handler);

      private:
        void RegisterBuiltInCommands();
        void ExecuteCommand(const std::string& line);

        // Built-in command implementations
        void CmdStatus(const std::vector<std::string>& args);
        void CmdStop(const std::vector<std::string>& args);
        void CmdHelp(const std::vector<std::string>& args);

      private:
        std::unordered_map<std::string, CommandHandler> m_Commands;
        bool m_Initialized = false;
    };
} // namespace OloEngine
