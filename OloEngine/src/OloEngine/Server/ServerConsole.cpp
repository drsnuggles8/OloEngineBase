#include "OloEnginePCH.h"
#include "ServerConsole.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#include <cctype>
#include <iostream>
#include <sstream>

namespace OloEngine
{
    ServerConsole::ServerConsole()
    {
        OLO_PROFILE_FUNCTION();
    }

    ServerConsole::~ServerConsole()
    {
        OLO_PROFILE_FUNCTION();
        Shutdown();
    }

    void ServerConsole::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Initialized)
        {
            return;
        }

        RegisterBuiltInCommands();
        m_Initialized = true;

        // Clear any prior EOF state so std::getline succeeds
        std::cin.clear();

        // Platform-specific state used to abort a blocking stdin read during shutdown.
        m_AbortState = ServerConsolePlatform::Create();
        if (!m_AbortState)
        {
            OLO_CORE_ERROR("[ServerConsole] Failed to create platform abort state — aborting initialization");
            m_Commands.clear();
            m_Initialized = false;
            return;
        }

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
        // Cancel the pending stdin read so the thread can observe the flag and exit.
        if (m_AbortState)
        {
            ServerConsolePlatform::Signal(*m_AbortState);
        }

        if (m_InputThread.joinable())
        {
            m_InputThread.join();
        }

        // Release platform abort state (closes pipes on POSIX).
        m_AbortState.reset();

        // Restore stdin so future console instances can read
        std::cin.clear();

        m_Commands.clear();
        {
            std::lock_guard lock(m_InputQueueMutex);
            std::queue<std::string> empty;
            std::swap(m_InputQueue, empty);
        }
        m_MessageSendCallback = nullptr;
        m_Initialized = false;
    }

    void ServerConsole::InputThreadFunc()
    {
        OLO_PROFILE_FUNCTION();

        std::string line;
        while (m_InputThreadRunning)
        {
            OLO_PROFILE_SCOPE("ServerConsole::InputThreadLoop");

            if (!m_AbortState)
            {
                break;
            }

            // Platform backend performs the (possibly non-blocking) read and
            // returns a complete line, or reports abort/EOF. This avoids the
            // old poll+std::getline race where a redirected stdin could hand
            // out bytes without a newline and leave std::getline blocked past
            // Signal().
            auto result = ServerConsolePlatform::ReadLine(*m_AbortState, line);
            if (result != ServerConsolePlatform::ReadResult::Line)
            {
                break; // Aborted or EndOfStream
            }
            if (!m_InputThreadRunning)
            {
                break;
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
        OLO_PROFILE_FUNCTION();

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
