#include "OloEnginePCH.h"
#include "OloEngine.h"
#include "OloEngine/Core/EntryPoint.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/UI/RuntimeInputRebindMenu.h"

#include <filesystem>
#include <imgui.h>
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
            if (const std::filesystem::path packPath = "Assets/AssetPack.olopack"; std::filesystem::exists(packPath))
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
            m_ScenePath = startScenePath;

            m_ActiveScene = Ref<Scene>::Create();
            if (SceneSerializer serializer(m_ActiveScene); !serializer.Deserialize(startScenePath.string()))
            {
                OLO_CORE_ERROR("[Runtime] Failed to deserialize scene: {}", startScenePath.string());
                Application::Get().Close();
                return;
            }

            // Validate the scene has a primary camera
            if (Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity(); !cameraEntity)
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
                    Renderer3D::Init(&Application::Get().GetWindow());
                }

                // Always ensure proper initial resize — this must run even if
                // Renderer3D was already initialized (e.g. via Renderer::Init
                // with PreferredRenderer == Renderer3D).
                const auto& window = Application::Get().GetWindow();
                u32 fbWidth = window.GetFramebufferWidth();
                u32 fbHeight = window.GetFramebufferHeight();
                if (fbWidth > 0 && fbHeight > 0)
                {
                    Renderer3D::OnWindowResize(fbWidth, fbHeight);
                }
            }

            // Start the runtime (physics, scripts, audio, animations)
            m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
            {
                const auto& win = Application::Get().GetWindow();
                u32 vpW = win.GetFramebufferWidth();
                u32 vpH = win.GetFramebufferHeight();
                if (vpW > 0 && vpH > 0)
                {
                    m_ActiveScene->OnViewportResize(vpW, vpH);
                }
            }
            m_ActiveScene->OnRuntimeStart();

            LoadInputActions();

            OLO_CORE_INFO("[Runtime] Game started successfully");
        }

        // Restore saved key bindings so a rebind persists across restarts (issue #475). Falls
        // back to CreateDefaultGameActions so the in-game rebind menu (toggle with F1) always
        // has a populated Gameplay map to remap.
        void LoadInputActions()
        {
            if (std::filesystem::exists(kInputActionsPath))
            {
                if (auto loaded = InputActionSerializer::DeserializeContexts(kInputActionsPath); loaded && !loaded->empty())
                {
                    InputActionManager::ReplaceAllContextMaps(*loaded);

                    // Guarantee playable bindings: if the saved config carried no Gameplay
                    // actions (that context emptied, or never written because Save skips empty
                    // maps), seed the defaults so the game — and the rebind menu, which edits
                    // Gameplay — still has something to bind.
                    if (InputActionManager::GetActionMap(InputContextType::Gameplay).Actions.empty())
                    {
                        InputActionManager::SetActionMap(InputContextType::Gameplay, CreateDefaultGameActions());
                        OLO_CORE_WARN("[Runtime] Saved input bindings had no Gameplay map — seeded default game actions");
                    }
                    else
                    {
                        OLO_CORE_INFO("[Runtime] Loaded input bindings from {}", kInputActionsPath.string());
                    }
                    return;
                }
            }

            InputActionManager::SetActionMap(InputContextType::Gameplay, CreateDefaultGameActions());
            OLO_CORE_INFO("[Runtime] No saved input bindings — seeded default game actions");
        }

        void ToggleRebindMenu()
        {
            if (!m_ActiveScene)
            {
                return;
            }

            if (m_RebindMenu.IsOpen())
            {
                CloseRebindMenu();
            }
            else
            {
                // Suppress gameplay input while remapping by pushing the Menu context; the menu
                // itself always edits the Gameplay map regardless of the active context.
                InputActionManager::PushContext(InputContextType::Menu);
                m_MenuContextPushed = true;
                // The panel needs a visible cursor to click; restore the game's cursor on close.
                m_PrevCursorMode = Input::GetCursorMode();
                Input::SetCursorMode(CursorMode::Normal);
                m_RebindMenu.Open(*m_ActiveScene, InputContextType::Gameplay, kInputActionsPath);
            }
        }

        // Close the menu and undo the context/cursor changes made when it opened. Safe to call
        // whether the menu was closed by F1 or by its own Close button.
        void CloseRebindMenu()
        {
            m_RebindMenu.Close();
            if (m_MenuContextPushed)
            {
                InputActionManager::PopContext();
                m_MenuContextPushed = false;
                Input::SetCursorMode(m_PrevCursorMode);
            }
        }

        void OnDetach() override
        {
            // Tear the menu down before the scene it built into goes away.
            CloseRebindMenu();
            if (m_ActiveScene)
            {
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }
        }

        void OnUpdate(Timestep const ts) override
        {
            // Integrate any assets the runtime asset thread finished loading in the
            // background. Done before the early-out so async loads still complete while
            // no scene is active (e.g. during a load transition).
            AssetManager::SyncWithAssetThread();

            if (!m_ActiveScene)
            {
                return;
            }

            // Handle window resize
            const auto& window = Application::Get().GetWindow();
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

            // Update the scene (physics, scripts, rendering). Deterministic
            // fixed-timestep tick (issue #452): the raw frame delta `ts` is
            // accumulated and gameplay advances in fixed dt steps, rendering once.
            m_ActiveScene->OnUpdateRuntimeFixed(ts, Application::Get().GetFixedTimeStep());

            // Drive the in-game rebind menu AFTER the scene's UI input pass so its button
            // states and captured gamepad input are current this frame.
            if (m_RebindMenu.IsOpen())
            {
                m_RebindMenu.OnUpdate();
                // The menu may have closed itself via its Close button — route through the same
                // teardown as an F1 close so context/cursor restore isn't duplicated. Close() is a
                // no-op on the already-closed menu; CloseRebindMenu only pops if it pushed.
                if (!m_RebindMenu.IsOpen())
                {
                    CloseRebindMenu();
                }
            }

            // Handle script-triggered scene reload (e.g., death/respawn)
            if (m_ActiveScene->GetPendingReload())
            {
                m_ActiveScene->SetPendingReload(false);
                ReloadScene();
            }
        }

        void OnEvent(Event& e) override
        {
            // Feed keyboard/mouse into an active rebind capture first so it consumes the input.
            if (m_RebindMenu.OnEvent(e))
            {
                e.Handled = true;
                return;
            }

            EventDispatcher dispatcher(e);
            dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(RuntimeLayer::OnWindowResize));
            dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(RuntimeLayer::OnKeyPressed));
        }

      private:
        bool OnWindowResize([[maybe_unused]] WindowResizeEvent const& e)
        {
            // Event carries logical pixels; query real framebuffer size
            const auto& window = Application::Get().GetWindow();
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

        bool OnKeyPressed(KeyPressedEvent const& e)
        {
            // Ignore auto-repeat so holding F1 doesn't flip the menu open/closed every frame.
            if (e.IsRepeat())
            {
                return false;
            }
            // F1 toggles the in-game input rebind menu.
            if (e.GetKeyCode() == Key::F1)
            {
                ToggleRebindMenu();
                return true;
            }
            return false;
        }

        /// Find the start scene path — reads manifest if available, then scans Scenes/
        [[nodiscard]] static std::filesystem::path FindStartScene()
        {
            // 1. Check game.manifest for an explicit start scene
            if (const std::filesystem::path manifestPath = "game.manifest"; std::filesystem::exists(manifestPath))
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
                        if (std::filesystem::path scenePath = "Scenes" / std::filesystem::path(startScene); std::filesystem::exists(scenePath))
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

        /// Scan a directory recursively for the first .olo scene file (sorted for determinism)
        [[nodiscard]] static std::filesystem::path FindFirstSceneInDirectory(const std::filesystem::path& directory)
        {
            std::error_code ec;
            if (!std::filesystem::exists(directory, ec))
            {
                return {};
            }

            std::vector<std::filesystem::path> scenes;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".olo")
                {
                    scenes.push_back(entry.path());
                }
            }

            if (scenes.empty())
            {
                return {};
            }

            std::ranges::sort(scenes);
            return scenes.front();
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
        std::filesystem::path m_ScenePath;
        bool m_Is3DMode = true;
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;

        // In-game input rebind menu (issue #475). Toggled with F1; persists to kInputActionsPath.
        RuntimeInputRebindMenu m_RebindMenu;
        bool m_MenuContextPushed = false;
        CursorMode m_PrevCursorMode = CursorMode::Normal;
        static inline const std::filesystem::path kInputActionsPath{ "Config/InputActions.yaml" };

        void ReloadScene()
        {
            OLO_CORE_INFO("[Runtime] Reloading scene: {}", m_ScenePath.string());

            // Close the rebind menu first — it holds a Scene* / entity handles into the scene
            // we are about to destroy, which would dangle on the next OnUpdate.
            CloseRebindMenu();

            // Reset time scale in case we were paused
            Application::Get().SetTimeScale(1.0f);

            m_ActiveScene->OnRuntimeStop();

            m_ActiveScene = Ref<Scene>::Create();
            if (SceneSerializer serializer(m_ActiveScene); !serializer.Deserialize(m_ScenePath.string()))
            {
                OLO_CORE_ERROR("[Runtime] Failed to reload scene");
                Application::Get().Close();
                return;
            }

            m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);

            const auto& window = Application::Get().GetWindow();
            u32 w = window.GetFramebufferWidth();
            u32 h = window.GetFramebufferHeight();
            if (w > 0 && h > 0)
            {
                m_ActiveScene->OnViewportResize(w, h);
            }

            m_ActiveScene->OnRuntimeStart();
        }
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
        explicit OloGameRuntime(const ApplicationSpecification& spec, bool pushRuntimeLayer = true)
            : Application(spec)
        {
            // In `--smoke-test` mode the RuntimeLayer is skipped: it loads the
            // asset pack / start scene and renders through the GL pipeline, which
            // needs a real OpenGL 4.6 context. ImGui isn't initialized in that
            // window-less path either, so GetIO() must not be touched. See
            // CreateApplication below.
            if (pushRuntimeLayer)
            {
                // Disable ImGui ini persistence — the runtime doesn't need it and
                // loading the editor's imgui.ini from CWD would cause stale state.
                ImGui::GetIO().IniFilename = nullptr;

                PushLayer(std::make_unique<RuntimeLayer>());
            }
        }

        ~OloGameRuntime() final = default;
    };

    Application* CreateApplication(ApplicationCommandLineArgs const args)
    {
        ApplicationSpecification spec;
        spec.Name = "OloEngine Game";
        spec.CommandLineArgs = args;

        // `--smoke-test`: window-less launch validation — see OloEditorApp.cpp for
        // the rationale. Proves the runtime binary starts and resolves its runtime
        // DLLs (issue #303) without needing a GPU; the GL-dependent RuntimeLayer is
        // skipped and the app auto-closes after a few ticks with EXIT_SUCCESS.
        if (args.Contains("--smoke-test"))
        {
            spec.IsHeadless = true;
            spec.SmokeTestTickLimit = SmokeTestTickCount;
            return new OloGameRuntime(spec, /*pushRuntimeLayer=*/false);
        }

        // Read game name from manifest if available.
        // PreferredRenderer stays as Renderer2D (the default) — Renderer3D is
        // lazily initialized in RuntimeLayer::OnAttach, matching the editor flow.
        // Setting PreferredRenderer to Renderer3D here would cause Renderer::Init
        // to skip Renderer2D initialization while Scene always needs it for 2D
        // sprite/text overlays.
        if (const std::filesystem::path manifestPath = "game.manifest"; std::filesystem::exists(manifestPath))
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
