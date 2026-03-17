#include "OloEnginePCH.h"
#include "ServerConsole.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"

#include <cctype>
#include <iostream>
#include <sstream>

#ifdef OLO_PLATFORM_WINDOWS
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

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

#ifndef OLO_PLATFORM_WINDOWS
        // Create a self-pipe so Shutdown() can wake the input thread
        if (::pipe(m_WakeupPipe) != 0)
        {
            OLO_CORE_ERROR("[ServerConsole] Failed to create wakeup pipe — aborting initialization");
            m_Commands.clear();
            m_Initialized = false;
            return;
        }
#endif

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
#ifdef OLO_PLATFORM_WINDOWS
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdin != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(hStdin, nullptr);
        }
#else
        // Write a byte to the wakeup pipe to unblock poll() in the input thread
        if (m_WakeupPipe[1] != -1)
        {
            char dummy = 0;
            (void)::write(m_WakeupPipe[1], &dummy, 1);
        }
#endif

        if (m_InputThread.joinable())
        {
            m_InputThread.join();
        }

#ifndef OLO_PLATFORM_WINDOWS
        // Close the wakeup pipe
        for (auto& fd : m_WakeupPipe)
        {
            if (fd != -1)
            {
                ::close(fd);
                fd = -1;
            }
        }
#endif

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

#ifndef OLO_PLATFORM_WINDOWS
            // On POSIX, poll stdin and the wakeup pipe so Shutdown() can unblock us
            struct pollfd fds[2]{};
            fds[0].fd = STDIN_FILENO;
            fds[0].events = POLLIN;
            fds[1].fd = m_WakeupPipe[0];
            fds[1].events = POLLIN;

            int ret = ::poll(fds, 2, -1); // block until ready
            if (ret <= 0 || !m_InputThreadRunning)
            {
                break;
            }
            // Wakeup pipe signalled — time to exit
            if (fds[1].revents & POLLIN)
            {
                break;
            }
            // stdin not ready (shouldn't happen, but guard)
            if (!(fds[0].revents & POLLIN))
            {
                continue;
            }
#endif

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
