#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "OloEngine/Server/ServerConsole.h"
#include "OloEngine/Server/ServerConfig.h"
#include "OloEngine/Server/ServerConfigSerializer.h"
#include "OloEngine/Server/ServerMonitor.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Core/Timer.h"

#include <charconv>

static_assert(OLO_HEADLESS, "OloServer must be compiled with OLO_HEADLESS=1");

namespace OloEngine
{
    class ServerLayer : public Layer
    {
      public:
        explicit ServerLayer(const ServerConfig& config)
            : Layer("ServerLayer"), m_Config(config)
        {
        }

        void OnAttach() override
        {
            OLO_CORE_INFO("[Server] Starting server on port {}", m_Config.Port);
            OLO_CORE_INFO("[Server] Max players: {}", m_Config.MaxPlayers);
            OLO_CORE_INFO("[Server] Tick rate: {} Hz", m_Config.TickRate);

            // Compute tick budget for monitor
            const f32 tickBudget = m_Config.TickRate > 0 ? 1.0f / static_cast<f32>(m_Config.TickRate) : 0.0f;
            m_Monitor.SetTickBudget(tickBudget);

            // Initialize server console
            m_Console.Initialize();
            RegisterConsoleCommands();

            // Load scene if specified
            if (!m_Config.ScenePath.empty())
            {
                if (!LoadScene(m_Config.ScenePath))
                {
                    OLO_CORE_ERROR("[Server] Aborting startup — scene load failed");
                    Application::Get().Close();
                    return;
                }
            }

            // Start listening for network connections
            if (NetworkManager::IsInitialized())
            {
                if (NetworkManager::StartServer(m_Config.Port))
                {
                    if (auto* server = NetworkManager::GetServer())
                    {
                        server->SetMaxConnections(m_Config.MaxPlayers);
                    }
                    OLO_CORE_INFO("[Server] Listening on port {}", m_Config.Port);
                }
                else
                {
                    OLO_CORE_ERROR("[Server] Failed to start network server on port {}!", m_Config.Port);
                }
            }
        }

        void OnDetach() override
        {
            if (NetworkManager::IsServer())
            {
                NetworkManager::StopServer();
            }

            if (m_ActiveScene)
            {
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }

            m_Console.Shutdown();
            OLO_CORE_INFO("[Server] Server shut down.");
        }

        void OnUpdate(Timestep ts) override
        {
            // Process console commands
            m_Console.ProcessInput();

            // Update active scene simulation
            Timer tickTimer;
            if (m_ActiveScene)
            {
                m_ActiveScene->OnUpdateRuntime(ts);
            }
            const f32 tickDuration = tickTimer.Elapsed();

            // Record measured tick execution time for monitoring
            m_Monitor.RecordTick(tickDuration);
        }

      private:
        bool LoadScene(const std::string& scenePath)
        {
            OLO_CORE_INFO("[Server] Loading scene: {}", scenePath);
            auto tempScene = Scene::Create();
            SceneSerializer serializer(tempScene);
            if (serializer.Deserialize(scenePath))
            {
                tempScene->OnRuntimeStart();

                // Success — swap in the new scene
                if (m_ActiveScene)
                {
                    m_ActiveScene->OnRuntimeStop();
                }
                m_ActiveScene = tempScene;
                OLO_CORE_INFO("[Server] Scene loaded and started");
                return true;
            }

            OLO_CORE_ERROR("[Server] Failed to load scene: {}", scenePath);
            return false;
        }

        void RegisterConsoleCommands()
        {
            m_Console.RegisterCommand("players", [this](const std::vector<std::string>&)
                                      { CmdPlayers(); });
            m_Console.RegisterCommand("kick", [this](const std::vector<std::string>& args)
                                      { CmdKick(args); });
            m_Console.RegisterCommand("say", [this](const std::vector<std::string>& args)
                                      { CmdSay(args); });
            m_Console.RegisterCommand("save", [this](const std::vector<std::string>&)
                                      { CmdSave(); });
            m_Console.RegisterCommand("reload", [this](const std::vector<std::string>&)
                                      { CmdReload(); });
            m_Console.RegisterCommand("stats", [this](const std::vector<std::string>&)
                                      { CmdStats(); });
        }

        void CmdPlayers() const
        {
            auto* server = NetworkManager::GetServer();
            if (!server)
            {
                OLO_CORE_INFO("[Server] Network server not running.");
                return;
            }

            const u32 count = server->GetConnectionCount();
            OLO_CORE_INFO("[Server] Connected players: {}/{}", count, m_Config.MaxPlayers);
            server->ForEachConnection([&](HSteamNetConnection handle, const NetworkConnection& conn)
                                      { OLO_CORE_INFO("  Client {} (conn {}): ping {} ms",
                                                      conn.GetClientID(), static_cast<u32>(handle),
                                                      server->GetClientPingMs(handle)); });
        }

        void CmdKick(const std::vector<std::string>& args) const
        {
            if (args.empty())
            {
                OLO_CORE_WARN("[Server] Usage: kick <client_id>");
                return;
            }

            auto* server = NetworkManager::GetServer();
            if (!server)
            {
                OLO_CORE_INFO("[Server] Network server not running.");
                return;
            }

            u32 targetId = 0;
            const char* val = args[0].c_str();
            const char* end = val + args[0].size();
            auto [ptr, ec] = std::from_chars(val, end, targetId);
            if (ec != std::errc{} || ptr != end)
            {
                OLO_CORE_WARN("[Server] Invalid client ID '{}'. Usage: kick <client_id>", args[0]);
                return;
            }

            HSteamNetConnection targetHandle = k_HSteamNetConnection_Invalid;
            server->ForEachConnection([&](HSteamNetConnection handle, const NetworkConnection& conn)
                                      {
                if (conn.GetClientID() == targetId)
                {
                    targetHandle = handle;
                } });

            if (targetHandle != k_HSteamNetConnection_Invalid)
            {
                OLO_CORE_INFO("[Server] Kicking client {} (conn {})", targetId, static_cast<u32>(targetHandle));
                server->CloseConnection(targetHandle, 0, "Kicked by server");
            }
            else
            {
                OLO_CORE_WARN("[Server] Client {} not found.", targetId);
            }
        }

        void CmdSay(const std::vector<std::string>& args) const
        {
            if (args.empty())
            {
                OLO_CORE_WARN("[Server] Usage: say <message>");
                return;
            }

            // Join all args into a single message
            std::string message;
            for (const auto& arg : args)
            {
                if (!message.empty())
                {
                    message += ' ';
                }
                message += arg;
            }

            OLO_CORE_INFO("[Server] Broadcast: {}", message);

            auto* server = NetworkManager::GetServer();
            if (server)
            {
                // Broadcast as a chat message to all connected clients
                server->BroadcastMessage(ENetworkMessageType::ChatReceive,
                                         reinterpret_cast<const u8*>(message.data()),
                                         static_cast<u32>(message.size()),
                                         k_nSteamNetworkingSend_Reliable);
            }
        }

        void CmdSave() const
        {
            if (!m_ActiveScene || m_Config.ScenePath.empty())
            {
                OLO_CORE_WARN("[Server] No active scene to save.");
                return;
            }

            SceneSerializer serializer(m_ActiveScene);
            serializer.Serialize(m_Config.ScenePath);
            OLO_CORE_INFO("[Server] Scene saved to '{}'", m_Config.ScenePath);
        }

        void CmdReload()
        {
            if (m_Config.ScenePath.empty())
            {
                OLO_CORE_WARN("[Server] No scene path configured for reload.");
                return;
            }

            OLO_CORE_INFO("[Server] Reloading scene from '{}'...", m_Config.ScenePath);
            LoadScene(m_Config.ScenePath);
        }

        void CmdStats()
        {
            m_Monitor.ForceReport();
        }

      private:
        ServerConfig m_Config;
        ServerConsole m_Console;
        ServerMonitor m_Monitor{ 30.0f };
        Ref<Scene> m_ActiveScene;
    };

    class OloServerApplication : public Application
    {
      public:
        explicit OloServerApplication(const ApplicationSpecification& spec, const ServerConfig& config)
            : Application(spec)
        {
            PushLayer(new ServerLayer(config));
        }

        ~OloServerApplication() final = default;
    };

    Application* CreateApplication(ApplicationCommandLineArgs const args)
    {
        // Parse server configuration from command line
        ServerConfig config = ServerConfigSerializer::ParseCommandLine(args.Count, args.Args);

        ApplicationSpecification spec;
        spec.Name = "OloEngine Server";
        spec.IsHeadless = true;
        spec.HeadlessTickRate = config.TickRate;
        spec.CommandLineArgs = args;

        return new OloServerApplication(spec, config);
    }
} // namespace OloEngine
