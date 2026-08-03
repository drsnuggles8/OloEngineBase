#include "OloEnginePCH.h"
#include "OloEngine.h"
#include "OloEngine/Core/EntryPoint.h"

#include "OloEngine/Server/ServerConsole.h"
#include "OloEngine/Server/ServerConfig.h"
#include "OloEngine/Server/ServerConfigSerializer.h"
#include "OloEngine/Server/ServerMonitor.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Project/Project.h"

#include <charconv>
#include <filesystem>
#include <system_error>

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

            // Mount the project BEFORE loading the scene: deserialization resolves
            // script files (and anything else) through Project::GetAssetFileSystemPath,
            // so a scene loaded without a project silently comes up with none of its
            // scripts attached — the server hosts the world but runs no server-side
            // gameplay. Mirrors the Project::NewInMemory mount OloRuntime does.
            MountProject();

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
            if (!NetworkManager::IsInitialized())
            {
                OLO_CORE_ERROR("[Server] NetworkManager not initialized — aborting startup");
                Application::Get().Close();
                return;
            }

            if (!NetworkManager::StartServer(m_Config.Port))
            {
                OLO_CORE_ERROR("[Server] Failed to start network server on port {} — aborting startup", m_Config.Port);
                Application::Get().Close();
                return;
            }

            if (auto* server = NetworkManager::GetServer())
            {
                server->SetMaxConnections(m_Config.MaxPlayers);
            }

            // The replication tick runs at the configured snapshot rate, which is
            // deliberately independent of the simulation tick rate: 60 Hz of physics
            // does not mean 60 Hz of wire traffic.
            NetworkManager::SetSnapshotRate(m_Config.SnapshotRate);

            OLO_CORE_INFO("[Server] Listening on port {} (snapshot rate {} Hz)", m_Config.Port, m_Config.SnapshotRate);
        }

        void OnDetach() override
        {
            if (NetworkManager::IsServer())
            {
                NetworkManager::StopServer();
            }

            if (m_ActiveScene)
            {
                // Unregister before releasing — the replication drivers hold a raw
                // Scene* and would dereference it on the next tick.
                NetworkManager::SetActiveScene(nullptr);
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }

            m_Console.Shutdown();
            OLO_CORE_INFO("[Server] Server shut down.");
        }

        void OnUpdate(Timestep ts) override
        {
            // Measure full tick duration (console + scene + monitoring)
            Timer tickTimer;

            // Process console commands
            m_Console.ProcessInput();

            // Update active scene simulation at the canonical fixed timestep so
            // the authoritative server advances gameplay at the same rate clients
            // do (Application::GetFixedTimeStep), independent of the server's
            // configured tick rate — the lockstep determinism the feature exists
            // for (issue #452). RunHeadless already feeds a fixed `ts` per tick;
            // the accumulator then steps the canonical fixed dt.
            if (m_ActiveScene)
            {
                m_ActiveScene->OnUpdateRuntimeFixed(ts, Application::Get().GetFixedTimeStep());
            }

            // Drive the authoritative networking loop AFTER the simulation step so
            // the snapshot this tick sends describes the state the step produced,
            // and so client inputs that arrived this frame are applied to the sim
            // before it runs. Game thread only — see NetworkManager's threading
            // contract. This is the call site the whole replication stack was
            // missing: without it, capture / delta / broadcast / player lifecycle
            // never ran at all.
            NetworkManager::Tick(ts);

            const f32 tickDuration = tickTimer.Elapsed();

            // Record measured tick execution time for monitoring
            m_Monitor.RecordTick(tickDuration);
        }

      private:
        // Infer the project root from the scene path when one was not given: walk up
        // from the scene until we find the `Assets` directory, whose parent is the
        // project. `--project` overrides it for layouts this heuristic cannot see.
        [[nodiscard]] static std::filesystem::path InferProjectDirectory(const std::filesystem::path& scenePath)
        {
            for (std::filesystem::path dir = scenePath.parent_path(); !dir.empty(); dir = dir.parent_path())
            {
                if (dir.filename() == "Assets")
                {
                    return dir.parent_path();
                }
                if (dir == dir.parent_path())
                {
                    break; // Reached the root without finding it.
                }
            }
            return {};
        }

        void MountProject()
        {
            std::filesystem::path projectDir = m_Config.ProjectPath.empty()
                                                   ? InferProjectDirectory(m_Config.ScenePath)
                                                   : std::filesystem::path(m_Config.ProjectPath);

            if (projectDir.empty())
            {
                OLO_CORE_WARN("[Server] No project directory (and none inferable from '{}') — scripts and other "
                              "project-relative assets will not resolve. Pass --project <dir>.",
                              m_Config.ScenePath);
                return;
            }

            std::error_code ec;
            if (!std::filesystem::is_directory(projectDir, ec) || ec)
            {
                // exists() is true for a regular FILE too; mounting one would log
                // success and then fail every later asset resolution silently, which
                // is the exact failure this function exists to prevent.
                OLO_CORE_WARN("[Server] Project path '{}' is not a directory — scripts will not resolve",
                              projectDir.string());
                return;
            }
            if (!std::filesystem::is_directory(projectDir / "Assets", ec) || ec)
            {
                OLO_CORE_WARN("[Server] Project directory '{}' has no Assets/ subdirectory — scripts will not resolve",
                              projectDir.string());
                return;
            }

            ProjectConfig config;
            // A trailing separator makes filename() empty, so `--project C:/MyGame/`
            // would otherwise name the project "".
            config.Name = projectDir.lexically_normal().filename().string();
            if (config.Name.empty())
            {
                config.Name = "Untitled";
            }
            config.AssetDirectory = "Assets";

            Project::NewInMemory(projectDir, config);
            OLO_CORE_INFO("[Server] Mounted project '{}' at '{}' (assets: '{}')", config.Name,
                          Project::GetProjectDirectory().string(), Project::GetAssetDirectory().string());
        }

        bool LoadScene(const std::string& scenePath)
        {
            OLO_CORE_INFO("[Server] Loading scene: {}", scenePath);
            auto tempScene = Scene::Create();
            if (SceneSerializer serializer(tempScene); serializer.Deserialize(scenePath))
            {
                tempScene->OnRuntimeStart();

                // Success — swap in the new scene
                if (m_ActiveScene)
                {
                    // Detach the outgoing scene from replication before it dies;
                    // the drivers hold a raw Scene*.
                    NetworkManager::SetActiveScene(nullptr);

                    // SetActiveScene(nullptr) only forgets the pointer. Every
                    // per-client baseline, known-entity set and history snapshot
                    // still describes entities from the scene about to be destroyed,
                    // so a `reload` with connections held open would delta the new
                    // world against the old one. Drop that state; connected clients
                    // re-receive spawns on the next relevance pass.
                    NetworkManager::GetServerDriver().Reset();

                    m_ActiveScene->OnRuntimeStop();
                }
                m_ActiveScene = tempScene;

                // Register the running scene with networking. Everything downstream
                // — capture, interest scoping, spawn/despawn, RPC handlers — early-
                // outs without it.
                NetworkManager::SetActiveScene(m_ActiveScene.get());

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
            const auto* server = NetworkManager::GetServer();
            if (!server)
            {
                OLO_CORE_INFO("[Server] Network server not running.");
                return;
            }

            const u32 count = server->GetConnectionCount();
            OLO_CORE_INFO("[Server] Connected players: {}/{}", count, m_Config.MaxPlayers);
            // Snapshot first, then query pings — GetClientPingMs re-takes the mutex
            // ForEachConnection holds, so querying inside the iteration deadlocks.
            const auto& driver = NetworkManager::GetServerDriver();
            for (const auto& info : server->GetConnectionSnapshot())
            {
                OLO_CORE_INFO("  Client {} (conn {}): ping {} ms, pawn {}", info.ClientID,
                              static_cast<u32>(info.Handle), server->GetClientPingMs(info.Handle),
                              driver.GetPlayerEntity(info.ClientID));
            }
            OLO_CORE_INFO("[Server] Replication tick: {} @ {} Hz", NetworkManager::GetCurrentTick(),
                          NetworkManager::GetSnapshotRate());
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
            server->ForEachConnection([&targetId, &targetHandle](HSteamNetConnection handle, const NetworkConnection& conn)
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
            PushLayer(std::make_unique<ServerLayer>(config));
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

        // `--smoke-test`: run the full headless startup (DLL load, subsystem
        // init, scene load if configured, network listen) and then auto-close
        // after a few ticks with EXIT_SUCCESS. Used by CI and the in-suite
        // AppLaunchSmokeTest to prove the shipped binary launches with all its
        // runtime DLLs present (issue #303).
        if (args.Contains("--smoke-test"))
        {
            spec.SmokeTestTickLimit = SmokeTestTickCount;
        }

        return new OloServerApplication(spec, config);
    }
} // namespace OloEngine
