#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    /**
     * @brief Runtime game layer — loads an asset pack and runs the start scene
     *
     * This is the main layer for standalone game builds. It:
     * 1. Loads the asset pack from the game directory
     * 2. Deserializes and starts the configured start scene
     * 3. Runs the scene loop (physics, scripts, rendering) each frame
     * 4. Handles window events (resize, close)
     */
    class RuntimeLayer : public Layer
    {
      public:
        RuntimeLayer() : Layer("RuntimeLayer") {}

        void OnAttach() override
        {
            // Show a loading screen immediately so the user doesn't see a blank white window
            // while assets/shaders/scenes are being loaded.
            RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
            RenderCommand::Clear();
            Application::Get().GetWindow().SwapBuffers();

            // Set up runtime asset manager with pack-based loading
            auto runtimeAssetManager = Ref<RuntimeAssetManager>::Create();
            Project::SetAssetManager(runtimeAssetManager);

            // Load the asset pack (textures, meshes, etc.)
            const std::filesystem::path packPath = "Assets/AssetPack.olopack";
            if (std::filesystem::exists(packPath))
            {
                if (!runtimeAssetManager->LoadAssetPack(packPath))
                {
                    OLO_CORE_WARN("[Runtime] Failed to load asset pack: {} (continuing without packed assets)", packPath.string());
                }
                else
                {
                    OLO_CORE_INFO("[Runtime] Asset pack loaded successfully");
                }
            }
            else
            {
                OLO_CORE_WARN("[Runtime] No asset pack found at: {}", packPath.string());
            }

            // Find the start scene — check manifest first, then scan Scenes/ directory
            std::filesystem::path startScenePath = FindStartScene();
            if (startScenePath.empty())
            {
                OLO_CORE_ERROR("[Runtime] No scene files found. Cannot start game.");
                Application::Get().Close();
                return;
            }

            OLO_CORE_INFO("[Runtime] Loading start scene: {}", startScenePath.string());

            m_ActiveScene = Ref<Scene>::Create();
            SceneSerializer serializer(m_ActiveScene);
            if (!serializer.Deserialize(startScenePath.string()))
            {
                OLO_CORE_ERROR("[Runtime] Failed to deserialize scene: {}", startScenePath.string());
                Application::Get().Close();
                return;
            }

            // Validate the scene has a primary camera
            Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
            if (!cameraEntity)
            {
                OLO_CORE_ERROR("[Runtime] Start scene has no primary camera. "
                               "Add an entity with CameraComponent (Primary = true) in the editor before building.");
                Application::Get().Close();
                return;
            }

            // Determine rendering mode from manifest
            m_Is3DMode = ReadIs3DModeFromManifest();

            if (m_Is3DMode)
            {
                // Initialize 3D rendering systems (render graph, UBOs, IBL, etc.)
                if (!Renderer3D::IsInitialized())
                {
                    Renderer3D::Init();
                }

                // Always ensure proper initial resize — this must run even if
                // Renderer3D was already initialized (e.g. via Renderer::Init
                // with PreferredRenderer == Renderer3D).
                auto& window = Application::Get().GetWindow();
                u32 fbWidth = window.GetFramebufferWidth();
                u32 fbHeight = window.GetFramebufferHeight();
                if (fbWidth > 0 && fbHeight > 0)
                {
                    Renderer3D::OnWindowResize(fbWidth, fbHeight);
                }
            }

            // Start the runtime (physics, scripts, audio, animations)
            m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
            m_ActiveScene->OnRuntimeStart();

            OLO_CORE_INFO("[Runtime] Game started successfully");
        }

        void OnDetach() override
        {
            if (m_ActiveScene)
            {
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }
        }

        void OnUpdate(Timestep const ts) override
        {
            if (!m_ActiveScene)
            {
                return;
            }

            // Handle window resize
            auto& window = Application::Get().GetWindow();
            u32 width = window.GetFramebufferWidth();
            u32 height = window.GetFramebufferHeight();

            if (width > 0 && height > 0 && (width != m_ViewportWidth || height != m_ViewportHeight))
            {
                m_ViewportWidth = width;
                m_ViewportHeight = height;
                m_ActiveScene->OnViewportResize(width, height);

                if (m_Is3DMode && Renderer3D::IsInitialized())
                {
                    Renderer3D::OnWindowResize(width, height);
                }
            }

            // Update the scene (physics, scripts, rendering)
            m_ActiveScene->OnUpdateRuntime(ts);
        }

        void OnEvent(Event& e) override
        {
            EventDispatcher dispatcher(e);
            dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(RuntimeLayer::OnWindowResize));
        }

      private:
        bool OnWindowResize(WindowResizeEvent const& e)
        {
            // Event carries logical pixels; query real framebuffer size
            auto& window = Application::Get().GetWindow();
            u32 width = window.GetFramebufferWidth();
            u32 height = window.GetFramebufferHeight();

            if (width == 0 || height == 0)
            {
                return false;
            }

            if (m_ActiveScene)
            {
                m_ActiveScene->OnViewportResize(width, height);
            }

            // Resize Renderer3D framebuffers — Application::OnWindowResize only
            // dispatches to the *preferred* renderer (Renderer2D), so we must
            // handle Renderer3D resize here explicitly.
            if (m_Is3DMode && Renderer3D::IsInitialized())
            {
                Renderer3D::OnWindowResize(width, height);
            }

            m_ViewportWidth = width;
            m_ViewportHeight = height;

            return false;
        }

        /// Find the start scene path — reads manifest if available, then scans Scenes/
        [[nodiscard]] static std::filesystem::path FindStartScene()
        {
            // 1. Check game.manifest for an explicit start scene
            const std::filesystem::path manifestPath = "game.manifest";
            if (std::filesystem::exists(manifestPath))
            {
                try
                {
                    YAML::Node manifest = YAML::LoadFile(manifestPath.string());
                    if (manifest["StartScene"])
                    {
                        auto startScene = manifest["StartScene"].as<std::string>();
                        // Try the path as-is (relative to game root)
                        if (std::filesystem::exists(startScene))
                        {
                            return startScene;
                        }
                        // Try under Scenes/ in case it's just a filename
                        std::filesystem::path scenePath = "Scenes" / std::filesystem::path(startScene);
                        if (std::filesystem::exists(scenePath))
                        {
                            return scenePath;
                        }
                        OLO_CORE_WARN("[Runtime] Start scene from manifest not found: {}", startScene);
                    }

                    // Check for scene directory override
                    if (manifest["Assets"] && manifest["Assets"]["SceneDirectory"])
                    {
                        auto sceneDir = manifest["Assets"]["SceneDirectory"].as<std::string>();
                        return FindFirstSceneInDirectory(sceneDir);
                    }
                }
                catch (const std::exception& e)
                {
                    OLO_CORE_WARN("[Runtime] Failed to parse game manifest: {}", e.what());
                }
            }

            // 2. Fallback: scan Scenes/ directory for the first .olo file
            return FindFirstSceneInDirectory("Scenes");
        }

        /// Scan a directory recursively for the first .olo scene file
        [[nodiscard]] static std::filesystem::path FindFirstSceneInDirectory(const std::filesystem::path& directory)
        {
            std::error_code ec;
            if (!std::filesystem::exists(directory, ec))
            {
                return {};
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".olo")
                {
                    return entry.path();
                }
            }

            return {};
        }

        /// Read the Is3DMode flag from game.manifest. Defaults to true if missing.
        [[nodiscard]] static bool ReadIs3DModeFromManifest()
        {
            const std::filesystem::path manifestPath = "game.manifest";
            if (!std::filesystem::exists(manifestPath))
            {
                return true;
            }

            try
            {
                YAML::Node manifest = YAML::LoadFile(manifestPath.string());
                if (manifest["Rendering"] && manifest["Rendering"]["Is3DMode"])
                {
                    return manifest["Rendering"]["Is3DMode"].as<bool>();
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("[Runtime] Failed to read rendering mode from manifest: {}", e.what());
            }

            return true; // default to 3D
        }

      private:
        Ref<Scene> m_ActiveScene;
        bool m_Is3DMode = true;
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
    };

    /**
     * @brief Standalone game runtime application
     *
     * This is the entry point for shipped games built with OloEngine.
     * It creates a minimal application with only the RuntimeLayer (no editor UI).
     */
    class OloGameRuntime : public Application
    {
      public:
        explicit OloGameRuntime(const ApplicationSpecification& spec)
            : Application(spec)
        {
            PushLayer(new RuntimeLayer());
        }

        ~OloGameRuntime() final = default;
    };

    Application* CreateApplication(ApplicationCommandLineArgs const args)
    {
        ApplicationSpecification spec;
        spec.Name = "OloEngine Game";
        spec.CommandLineArgs = args;

        // Read game name from manifest if available.
        // PreferredRenderer stays as Renderer2D (the default) — Renderer3D is
        // lazily initialized in RuntimeLayer::OnAttach, matching the editor flow.
        // Setting PreferredRenderer to Renderer3D here would cause Renderer::Init
        // to skip Renderer2D initialization while Scene always needs it for 2D
        // sprite/text overlays.
        const std::filesystem::path manifestPath = "game.manifest";
        if (std::filesystem::exists(manifestPath))
        {
            try
            {
                YAML::Node manifest = YAML::LoadFile(manifestPath.string());
                if (manifest["Game"] && manifest["Game"]["Name"])
                {
                    spec.Name = manifest["Game"]["Name"].as<std::string>();
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("[Runtime] Failed to parse game manifest: {}", e.what());
            }
        }

        return new OloGameRuntime(spec);
    }
} // namespace OloEngine
