#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "OloEngine/Server/ServerConsole.h"
#include "OloEngine/Server/ServerConfig.h"
#include "OloEngine/Server/ServerConfigSerializer.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"

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
        }

        void OnDetach() override
        {
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
        }

      private:
        ServerConfig m_Config;
        ServerConsole m_Console;
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
        spec.WorkingDirectory = "OloEditor/"; // Asset paths resolve relative to this
        spec.CommandLineArgs = args;

        return new OloServerApplication(spec, config);
    }
} // namespace OloEngine
