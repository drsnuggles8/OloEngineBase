#include "OloEnginePCH.h"
#include "ServerConsole.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Timer.h"

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/select.h>
#endif

#include <iostream>
#include <sstream>

namespace OloEngine
{
    ServerConsole::ServerConsole() = default;
    ServerConsole::~ServerConsole() = default;

    void ServerConsole::Initialize()
    {
        RegisterBuiltInCommands();
        m_Initialized = true;
        OLO_CORE_INFO("[ServerConsole] Initialized. Type 'help' for available commands.");
    }

    void ServerConsole::Shutdown()
    {
        m_Commands.clear();
        m_Initialized = false;
    }

    void ServerConsole::RegisterCommand(const std::string& name, CommandHandler handler)
    {
        m_Commands[name] = std::move(handler);
    }

    void ServerConsole::ProcessInput()
    {
        if (!m_Initialized)
        {
            return;
        }

        // Non-blocking check for stdin input
#ifdef _WIN32
        if (!_kbhit())
        {
            return;
        }
#else
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(STDIN_FILENO, &readSet);
        struct timeval timeout = { 0, 0 };
        if (select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) <= 0)
        {
            return;
        }
#endif

        std::string line;
        if (std::getline(std::cin, line))
        {
            if (!line.empty())
            {
                ExecuteCommand(line);
            }
        }
    }

    void ServerConsole::ExecuteCommand(const std::string& line)
    {
        std::istringstream iss(line);
        std::string command;
        iss >> command;

        // Lowercase the command for case-insensitive matching
        std::ranges::transform(command, command.begin(), ::tolower);

        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg)
        {
            args.push_back(std::move(arg));
        }

        auto it = m_Commands.find(command);
        if (it != m_Commands.end())
        {
            it->second(args);
        }
        else
        {
            OLO_CORE_WARN("[ServerConsole] Unknown command: '{}'. Type 'help' for available commands.", command);
        }
    }

    void ServerConsole::RegisterBuiltInCommands()
    {
        RegisterCommand("status", [this](const auto& args) { CmdStatus(args); });
        RegisterCommand("stop", [this](const auto& args) { CmdStop(args); });
        RegisterCommand("quit", [this](const auto& args) { CmdStop(args); });
        RegisterCommand("help", [this](const auto& args) { CmdHelp(args); });
    }

    void ServerConsole::CmdStatus([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_CORE_INFO("[Server] Status: Running");
        OLO_CORE_INFO("[Server] Application: {}", Application::Get().GetSpecification().Name);
    }

    void ServerConsole::CmdStop([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_CORE_INFO("[ServerConsole] Shutting down server...");
        Application::Get().Close();
    }

    void ServerConsole::CmdHelp([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_CORE_INFO("[ServerConsole] Available commands:");
        OLO_CORE_INFO("  status    - Show server status");
        OLO_CORE_INFO("  stop      - Graceful shutdown");
        OLO_CORE_INFO("  quit      - Graceful shutdown (alias)");
        OLO_CORE_INFO("  help      - Show this help message");
    }

} // namespace OloEngine
