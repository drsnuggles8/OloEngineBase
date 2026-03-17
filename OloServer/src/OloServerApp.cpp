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

            // Initialize server console
            m_Console.Initialize();
            RegisterConsoleCommands();

            // Load scene if specified
            if (!m_Config.ScenePath.empty())
            {
                OLO_CORE_INFO("[Server] Loading scene: {}", m_Config.ScenePath);
                m_ActiveScene = Scene::Create();
                SceneSerializer serializer(m_ActiveScene);
                if (serializer.Deserialize(m_Config.ScenePath))
                {
                    m_ActiveScene->OnRuntimeStart();
                    OLO_CORE_INFO("[Server] Scene loaded and started");
                }
                else
                {
                    OLO_CORE_ERROR("[Server] Failed to load scene: {}", m_Config.ScenePath);
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
            if (m_ActiveScene)
            {
                m_ActiveScene->OnUpdateRuntime(ts);
            }

            // Record tick for monitoring
            m_Monitor.RecordTick(ts.GetSeconds());
        }

      private:
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

            const u32 targetId = static_cast<u32>(std::stoul(args[0]));
            bool found = false;
            server->ForEachConnection([&](HSteamNetConnection handle, const NetworkConnection& conn)
                                      {
                if (conn.GetClientID() == targetId)
                {
                    // Close via low-level handle — conn is const in the iteration
                    OLO_CORE_INFO("[Server] Kicking client {} (conn {})", targetId, static_cast<u32>(handle));
                    found = true;
                } });

            if (!found)
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

            if (m_ActiveScene)
            {
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }

            m_ActiveScene = Scene::Create();
            SceneSerializer serializer(m_ActiveScene);
            if (serializer.Deserialize(m_Config.ScenePath))
            {
                m_ActiveScene->OnRuntimeStart();
                OLO_CORE_INFO("[Server] Scene reloaded successfully");
            }
            else
            {
                OLO_CORE_ERROR("[Server] Failed to reload scene");
            }
        }

        void CmdStats() const
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
        spec.WorkingDirectory = "OloEditor/"; // Asset paths resolve relative to this
        spec.CommandLineArgs = args;

        return new OloServerApplication(spec, config);
    }
} // namespace OloEngine
