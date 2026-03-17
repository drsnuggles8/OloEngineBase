#pragma once

#include "OloEngine/Core/Base.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Abstract console interface — allows server code to work with both
    // headless (stdin) and GUI (ImGui panel) consoles interchangeably.
    class IConsole
    {
      public:
        using MessageSendCallback = std::function<void(const std::string& message)>;

        virtual ~IConsole() = default;

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;

        // Called once per tick to process queued input
        virtual void ProcessInput() = 0;

        // Display a message in the console output
        virtual void AddMessage(const std::string& message) = 0;
        virtual void AddTaggedMessage(const std::string& tag, const std::string& message) = 0;

        // Register a callback invoked when the user submits a message/command
        virtual void SetMessageSendCallback(MessageSendCallback callback) = 0;
    };

    class ServerConsole final : public IConsole
    {
      public:
        using CommandHandler = std::function<void(const std::vector<std::string>& args)>;

        ServerConsole();
        ~ServerConsole() override;

        void Initialize() override;
        void Shutdown() override;

        // Drain the thread-safe command queue — call once per frame/tick
        void ProcessInput() override;

        void AddMessage(const std::string& message) override;
        void AddTaggedMessage(const std::string& tag, const std::string& message) override;
        void SetMessageSendCallback(MessageSendCallback callback) override;

        // Register a custom command handler
        void RegisterCommand(const std::string& name, CommandHandler handler);

        // Inject a command string for testing — queues it as if typed on stdin
        void InjectInput(const std::string& line);

      private:
        void RegisterBuiltInCommands();
        void ExecuteCommand(const std::string& line);
        void InputThreadFunc();

        // Built-in command implementations
        void CmdStatus(const std::vector<std::string>& args);
        void CmdStop(const std::vector<std::string>& args);
        void CmdHelp(const std::vector<std::string>& args);

      private:
        std::unordered_map<std::string, CommandHandler> m_Commands;
        bool m_Initialized = false;

        // Background input thread + thread-safe queue
        std::thread m_InputThread;
        std::atomic<bool> m_InputThreadRunning{ false };
        std::mutex m_InputQueueMutex;
        std::queue<std::string> m_InputQueue;

        MessageSendCallback m_MessageSendCallback;
    };
} // namespace OloEngine
