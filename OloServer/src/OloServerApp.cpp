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
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Project/ProjectSelection.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"

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

            if (!MountProject())
            {
                Application::Get().Close();
                return;
            }

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
        bool MountProject()
        {
            const auto commandLine = Application::Get().GetSpecification().CommandLineArgs;
            auto launch = ParseProjectLaunchArguments(commandLine.Count, commandLine.Args);
            if (!launch.MissingValueFor.empty())
            {
                OLO_CORE_ERROR("[Server] Missing path after {}", launch.MissingValueFor);
                return false;
            }
            if (launch.Project.empty())
                launch.Project = m_Config.ProjectPath;
            if (launch.Scene.empty())
                launch.Scene = m_Config.ScenePath;

            const auto& workingDirectory = Application::GetStartupWorkingDirectory();
            const auto selection = ResolveProjectSelection(launch, workingDirectory, {}, /*allowLegacyDirectory=*/true);
            if (!launch.Scene.empty())
                m_Config.ScenePath = (workingDirectory / launch.Scene).string();
            std::error_code ec;
            if (std::filesystem::is_regular_file(selection.Path, ec))
            {
                const auto project = Project::Load(selection.Path);
                if (!project)
                    return false;
                const auto& config = project->GetConfig();
                ScriptEngine::SetProjectAssembly(config.ScriptModulePath);
                // A server without a requested scene can still start in lobby mode.
                if (m_Config.ScenePath.empty() && selection.Source == ProjectSelectionSource::Explicit &&
                    std::filesystem::is_regular_file(workingDirectory / launch.Project, ec))
                    m_Config.ScenePath = config.StartScene.string();
            }
            else if (std::filesystem::is_directory(selection.Path / "Assets", ec))
            {
                ProjectConfig config;
                if (const auto name = selection.Path.filename().string(); !name.empty())
                    config.Name = name;
                config.AssetDirectory = "Assets";
                Project::NewInMemory(selection.Path, config);
            }
            else if (selection.Source == ProjectSelectionSource::SandboxFallback)
            {
                OLO_CORE_WARN("[Server] No project found at '{}'; project-relative assets are unavailable", selection.Path.string());
                return true;
            }
            else
            {
                OLO_CORE_ERROR("[Server] Project '{}' is neither a descriptor nor a directory with Assets/", selection.Path.string());
                return false;
            }

            OLO_CORE_INFO("[Server] Mounted project '{}' at '{}' (assets: '{}')", Project::GetActive()->GetConfig().Name,
                          Project::GetProjectDirectory().string(), Project::GetAssetDirectory().string());
            return true;
        }

        bool LoadScene(const std::string& scenePath)
        {
            OLO_CORE_INFO("[Server] Loading scene: {}", scenePath);
            auto tempScene = Scene::Create();
            if (SceneSerializer serializer(tempScene); serializer.Deserialize(scenePath))
            {
                // Success — swap in the new scene
                if (m_ActiveScene)
                {
                    // Detach the outgoing scene from replication before it dies;
                    // the drivers hold a raw Scene*.
                    NetworkManager::SetActiveScene(nullptr);
                    m_ActiveScene->OnRuntimeStop();
                }
                m_ActiveScene = tempScene;
                m_ActiveScene->OnRuntimeStart();

                // Register the running scene with networking. Everything downstream
                // — capture, interest scoping, spawn/despawn, RPC handlers — early-
                // outs without it.
                // NOTE: no explicit replication reset here. Every per-client baseline,
                // known-entity set and history snapshot still describes entities from
                // the scene we just destroyed, but SetActiveScene now records the swap
                // and the next NetworkManager::Tick rebuilds the driver against the
                // new scene — keeping the connections, which is what a `reload` with
                // sockets held open requires. Doing it in the facade rather than here
                // means the editor's Stop-then-Play and OloRuntime get it too.
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
            if (!serializer.Serialize(m_Config.ScenePath))
            {
                OLO_CORE_ERROR("[Server] Scene NOT saved to '{}'", m_Config.ScenePath);
                return;
            }
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
