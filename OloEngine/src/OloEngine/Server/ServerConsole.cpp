#include "OloEnginePCH.h"
#include "ServerConsole.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"

#include <cctype>
#include <iostream>
#include <sstream>

namespace OloEngine
{
    ServerConsole::ServerConsole() = default;

    ServerConsole::~ServerConsole()
    {
        Shutdown();
    }

    void ServerConsole::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        RegisterBuiltInCommands();
        m_Initialized = true;

        // Clear any prior EOF state so std::getline succeeds
        std::cin.clear();

        // Launch background thread that blocks on std::getline
        m_InputThreadRunning = true;
        m_InputThread = std::thread(&ServerConsole::InputThreadFunc, this);

        OLO_CORE_INFO("[ServerConsole] Initialized. Type 'help' for available commands.");
    }

    void ServerConsole::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        m_InputThreadRunning = false;

        // The input thread may be blocked on std::getline.
        // Closing stdin unblocks it on most platforms.
        std::cin.setstate(std::ios::eofbit);

        if (m_InputThread.joinable())
        {
            m_InputThread.join();
        }

        // Restore stdin so future console instances can read
        std::cin.clear();

        m_Commands.clear();
        m_Initialized = false;
    }

    void ServerConsole::InputThreadFunc()
    {
        OLO_PROFILE_FUNCTION();

        std::string line;
        while (m_InputThreadRunning)
        {
            OLO_PROFILE_SCOPE("ServerConsole::InputThreadLoop");
            if (!std::getline(std::cin, line))
            {
                break; // EOF or stream error
            }

            if (!line.empty())
            {
                std::lock_guard lock(m_InputQueueMutex);
                m_InputQueue.push(std::move(line));
                line = {}; // reset after move
            }
        }
    }

    void ServerConsole::ProcessInput()
    {
        OLO_PROFILE_FUNCTION();

        // Drain the queue — execute commands on the main thread
        std::queue<std::string> pending;
        {
            OLO_PROFILE_SCOPE("ServerConsole::ProcessInputDrain");
            std::lock_guard lock(m_InputQueueMutex);
            std::swap(pending, m_InputQueue);
        }

        while (!pending.empty())
        {
            ExecuteCommand(pending.front());
            pending.pop();
        }
    }

    void ServerConsole::AddMessage(const std::string& message)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("[Console] {}", message);
    }

    void ServerConsole::AddTaggedMessage(const std::string& tag, const std::string& message)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("[{}] {}", tag, message);
    }

    void ServerConsole::SetMessageSendCallback(MessageSendCallback callback)
    {
        OLO_PROFILE_FUNCTION();
        m_MessageSendCallback = std::move(callback);
    }

    void ServerConsole::RegisterCommand(const std::string& name, CommandHandler handler)
    {
        OLO_PROFILE_FUNCTION();
        m_Commands[name] = std::move(handler);
    }

    void ServerConsole::InjectInput(const std::string& line)
    {
        std::lock_guard lock(m_InputQueueMutex);
        m_InputQueue.push(line);
    }

    void ServerConsole::ExecuteCommand(const std::string& line)
    {
        OLO_PROFILE_FUNCTION();

        // If a message callback is set, forward raw input
        if (m_MessageSendCallback)
        {
            m_MessageSendCallback(line);
        }

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        // Lowercase the command for case-insensitive matching (cast to unsigned char to avoid UB)
        std::ranges::transform(command, command.begin(),
                               [](char c)
                               { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg)
        {
            args.push_back(std::move(arg));
        }

        auto it = m_Commands.find(command);
        if (it != m_Commands.end())
        {
            try
            {
                it->second(args);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("[ServerConsole] Command '{}' threw exception: {}", command, e.what());
            }
            catch (...)
            {
                OLO_CORE_ERROR("[ServerConsole] Command '{}' threw an unknown exception", command);
            }
        }
        else
        {
            OLO_CORE_WARN("[ServerConsole] Unknown command: '{}'. Type 'help' for available commands.", command);
        }
    }

    void ServerConsole::RegisterBuiltInCommands()
    {
        OLO_PROFILE_FUNCTION();

        RegisterCommand("status", [this](const auto& args)
                        { CmdStatus(args); });
        RegisterCommand("stop", [this](const auto& args)
                        { CmdStop(args); });
        RegisterCommand("quit", [this](const auto& args)
                        { CmdStop(args); });
        RegisterCommand("help", [this](const auto& args)
                        { CmdHelp(args); });
    }

    void ServerConsole::CmdStatus([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("[Server] Status: Running");
        OLO_CORE_INFO("[Server] Application: {}", Application::Get().GetSpecification().Name);
    }

    void ServerConsole::CmdStop([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("[ServerConsole] Shutting down server...");
        Application::Get().Close();
    }

    void ServerConsole::CmdHelp([[maybe_unused]] const std::vector<std::string>& args)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("[ServerConsole] Available commands:");
        OLO_CORE_INFO("  status    - Show server status");
        OLO_CORE_INFO("  stop      - Graceful shutdown");
        OLO_CORE_INFO("  quit      - Graceful shutdown (alias)");
        OLO_CORE_INFO("  help      - Show this help message");
    }

} // namespace OloEngine
