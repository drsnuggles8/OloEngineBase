#include "OloEnginePCH.h"
#include "OloEngine.h"
#include "OloEngine/Core/EntryPoint.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/BuildInfo.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneTransition.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/UI/RuntimeInputRebindMenu.h"
#include "OloEngine/UI/UINavigationSystem.h"

#include <filesystem>
#include <optional>
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

            // Mount an in-memory project rooted at the game directory BEFORE
            // anything loads. A shipped game has no .oloproj — the build
            // pipeline flattens it into game.manifest + the asset pack — but the
            // engine still resolves asset-relative paths through the Project
            // statics, which assert without an active project. The one that
            // bites hardest is `Scene::OnRuntimeStart`'s Lua sweep
            // (`Project::GetAssetFileSystemPath(LuaScriptComponent::ScriptFile)`):
            // without this, a shipped game died the instant it loaded a scene
            // carrying a Lua script, so Lua scripting was editor-only. The
            // asset root mirrors the layout the build pipeline writes —
            // `<game>/Assets/<asset-relative path>` — which is where
            // CopyScriptFiles puts the loose .lua files.
            //
            // The manifest is read once here and handed to each consumer, so
            // the project config, the start scene and the rendering mode can't
            // disagree about what the file said.
            const YAML::Node manifest = LoadGameManifest();

            // Build identity (#894) — read from the manifest first so a shipped
            // build (which may be OLDER than the engine that's now reading it,
            // if a player keeps a stale copy) reports the identity it was
            // PACKAGED with, not whatever this binary happens to be. Falls back
            // to this binary's own BuildInfo when the manifest predates the
            // BuildId field.
            {
                std::string buildId = BuildInfo::GetBuildId();
                try
                {
                    if (manifest["Game"] && manifest["Game"]["BuildId"])
                    {
                        // A defined-but-empty scalar ("BuildId: \"\"") passes
                        // the presence check above and parses fine, so only
                        // overwrite the fallback when there's actually
                        // something to report — an empty string would
                        // otherwise silently replace a perfectly good id with
                        // nothing.
                        if (std::string parsed = manifest["Game"]["BuildId"].as<std::string>(); !parsed.empty())
                        {
                            buildId = parsed;
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    // A malformed BuildId node (e.g. a map/sequence instead of
                    // a scalar, from a hand-edited or corrupted manifest) must
                    // not stop the game from starting — fall back to this
                    // binary's own BuildInfo rather than letting .as<std::string>()'s
                    // exception propagate out of OnAttach.
                    OLO_CORE_WARN("[Runtime] Failed to read BuildId from the manifest: {}", e.what());
                }
                OLO_CORE_INFO("[Runtime] Build: {}", buildId);
            }

            MountGameProject(manifest);
            SaveGameManager::Initialize();

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
            std::filesystem::path startScenePath = FindStartScene(manifest);
            if (startScenePath.empty())
            {
                OLO_CORE_ERROR("[Runtime] No scene files found. Cannot start game.");
                Application::Get().Close();
                return;
            }

            OLO_CORE_INFO("[Runtime] Loading start scene: {}", startScenePath.string());

            // Determine rendering mode from manifest
            m_Is3DMode = ReadIs3DModeFromManifest(manifest);

            if (m_Is3DMode)
            {
                // Initialize 3D rendering systems (render graph, UBOs, IBL, etc.)
                // Guard on HasInitialized() ("Init ran"), not IsInitialized()
                // ("render graph ready"): if Init already ran at a 0x0 size the
                // graph isn't built yet, but re-running Init would double-init the
                // one-shot singletons. The OnWindowResize below completes the build.
                if (!Renderer3D::HasInitialized())
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

            // Input maps must exist before scene scripts receive OnCreate: the
            // Drift menu selects the Menu context there, and gameplay scripts
            // select Vehicle. The project-authored maps are shipped loose by
            // GameBuildPipeline so saved rebinds can overwrite the same path.
            LoadInputActions();
            UINavigationSystem::InstallDefaultMenuActions();

            // Load + start the runtime (physics, scripts, audio, animations).
            // Shares ActivateScene with reload and script-driven scene switching
            // (issue #642), so the start scene is validated by exactly the same
            // rules a switched-to scene is.
            if (!ActivateScene(startScenePath))
            {
                Application::Get().Close();
                return;
            }

            OLO_CORE_INFO("[Runtime] Game started successfully");
        }

        // Restore project and player key bindings so rebinds persist across
        // restarts (issue #475). Generic Gameplay defaults are merged only for
        // hosts/content that still depend on them; authored contexts such as
        // Drift's Vehicle map remain intact for the settings panel to edit.
        void LoadInputActions()
        {
            if (std::filesystem::exists(kInputActionsPath))
            {
                if (auto loaded = InputActionSerializer::DeserializeContexts(kInputActionsPath); loaded && !loaded->empty())
                {
                    InputActionManager::ReplaceAllContextMaps(*loaded);

                    // Merge any MISSING default Gameplay actions into the loaded map while keeping
                    // the player's saved rebinds. This covers a fully-empty Gameplay context (never
                    // written because Save skips empty maps) AND a partially-populated one (a
                    // default action added in a newer build that the old save predates), so the
                    // generic gameplay and its F1 fallback always see the full set.
                    InputActionMap& gameplay = InputActionManager::GetActionMapMutable(InputContextType::Gameplay);
                    const InputActionMap defaults = CreateDefaultGameActions();
                    u32 addedDefaults = 0;
                    for (const auto& [name, action] : defaults.Actions)
                    {
                        if (!gameplay.HasAction(name))
                        {
                            gameplay.AddAction(action);
                            ++addedDefaults;
                        }
                    }
                    if (addedDefaults > 0)
                    {
                        OLO_CORE_WARN("[Runtime] Merged {} missing default Gameplay action(s) into loaded input bindings", addedDefaults);
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

        void ToggleRebindMenu(std::optional<InputContextType> targetContext = std::nullopt)
        {
            if (!m_ActiveScene)
            {
                return;
            }

            if (m_RebindMenu.IsOpen())
            {
                if (targetContext)
                {
                    m_RebindMenu.Open(*m_ActiveScene, *targetContext, kInputActionsPath);
                }
                else
                {
                    CloseRebindMenu();
                }
            }
            else
            {
                const InputContextType contextToEdit = targetContext.value_or(InputActionManager::GetInputContext());

                // Suppress gameplay input while remapping by pushing the Menu context; Open()
                // edits the caller-supplied target context rather than always Gameplay.
                InputActionManager::PushContext(InputContextType::Menu);
                m_MenuContextPushed = true;
                // The panel needs a visible cursor to click; restore the game's cursor on close.
                m_PrevCursorMode = Input::GetCursorMode();
                Input::SetCursorMode(CursorMode::Normal);
                m_RebindMenu.Open(*m_ActiveScene, contextToEdit == InputContextType::Menu ? InputContextType::Gameplay : contextToEdit,
                                  kInputActionsPath);
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
                NetworkManager::SetActiveScene(nullptr);
                m_ActiveScene->OnRuntimeStop();
                m_ActiveScene = nullptr;
            }
            SaveGameManager::Shutdown();
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
            SaveGameManager::Tick(ts, *m_ActiveScene);

            // Drive networking AFTER the simulation step: on a client this polls the
            // transport (spawning/despawning replicated entities and running RPC
            // handlers on this thread) and advances snapshot interpolation; on a
            // listen server it also runs the replication tick, which must observe
            // the state the tick above just produced. Game thread only — see the
            // threading contract on NetworkManager.
            NetworkManager::Tick(ts);

            // A scene script can request the settings panel during its tick. Open
            // it only after the tick returns: Open() creates UI entities, which
            // would invalidate a live script/component view if done inline.
            if (const auto requestedContext = InputActionManager::ConsumeRebindMenuRequest())
            {
                ToggleRebindMenu(*requestedContext);
            }

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

            // Handle script-triggered scene transitions at the END of the frame
            // (issue #642) — never mid-tick, since the swap destroys the scene
            // the requesting script is running in. A load and a reload are
            // mutually exclusive by construction (Scene's setters clear each
            // other), so the order of these two branches is not a precedence
            // rule, just a check order.
            if (m_ActiveScene->HasPendingSceneLoad())
            {
                const std::string request = m_ActiveScene->GetPendingSceneLoad();
                const std::string saveSlot = m_ActiveScene->GetPendingSceneLoadSaveSlot();
                m_ActiveScene->ClearPendingSceneLoad();
                SwitchScene(request, saveSlot);
            }
            else if (m_ActiveScene->GetPendingReload())
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

        /// Make the game directory the active (in-memory) project.
        ///
        /// A shipped game has no `.oloproj`, so the config is reconstructed from
        /// `game.manifest` + the fixed layout the build pipeline writes. Only
        /// `AssetDirectory` really matters at runtime — it is what turns a
        /// project-relative `LuaScriptComponent::ScriptFile` into a real file —
        /// but Name/StartScene are filled in so diagnostics read sensibly.
        static void MountGameProject(const YAML::Node& manifest)
        {
            ProjectConfig config;
            config.AssetDirectory = "Assets";

            std::error_code ec;
            const auto gameRoot = std::filesystem::current_path(ec);

            try
            {
                if (manifest["Game"] && manifest["Game"]["Name"])
                {
                    config.Name = manifest["Game"]["Name"].as<std::string>();
                }
                if (manifest["StartScene"])
                {
                    config.StartScene = manifest["StartScene"].as<std::string>();
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("[Runtime] Failed to read project config from manifest: {}", e.what());
            }

            Project::NewInMemory(ec ? std::filesystem::path{} : gameRoot, config);
            OLO_CORE_INFO("[Runtime] Mounted game project '{}' at '{}' (assets: '{}')",
                          config.Name, Project::GetProjectDirectory().string(), Project::GetAssetDirectory().string());
        }

        /// Read and parse `game.manifest` once per launch.
        ///
        /// Startup needs three unrelated things out of it (project config, the
        /// start scene, the rendering mode) and each used to open, read and
        /// parse the file for itself — three chances for the same file to be
        /// read inconsistently, and three yaml-cpp throw sites for one small
        /// document. Returns an invalid Node when the manifest is missing or
        /// unparseable; every consumer already treats "key absent" as "use the
        /// default", and an invalid Node answers `operator[]` that way.
        [[nodiscard]] static YAML::Node LoadGameManifest()
        {
            const std::filesystem::path manifestPath = "game.manifest";
            if (!std::filesystem::exists(manifestPath))
            {
                return {};
            }

            try
            {
                return YAML::LoadFile(manifestPath.string());
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("[Runtime] Failed to parse game manifest: {}", e.what());
                return {};
            }
        }

        /// Find the start scene path — uses the manifest if it named one, then scans Scenes/
        [[nodiscard]] static std::filesystem::path FindStartScene(const YAML::Node& manifest)
        {
            // 1. An explicit start scene from game.manifest
            try
            {
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
                OLO_CORE_WARN("[Runtime] Failed to read the start scene from the manifest: {}", e.what());
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

        /// Read the Is3DMode flag from the manifest. Defaults to true if missing.
        [[nodiscard]] static bool ReadIs3DModeFromManifest(const YAML::Node& manifest)
        {
            try
            {
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
            if (!ActivateScene(m_ScenePath))
            {
                // A reload of the scene we are already running is a path we
                // validated at load time, so a failure here means the file
                // changed or vanished underneath us. There is nothing sane
                // left to run.
                OLO_CORE_ERROR("[Runtime] Failed to reload scene — closing.");
                Application::Get().Close();
            }
        }

        /// Service a script-requested scene switch (issue #642). `request` is
        /// whatever the script passed; it is resolved against the game
        /// directory here. A failure is NON-FATAL — the current scene keeps
        /// running — because a bad scene name from script is a content bug, and
        /// killing the game gives the player nothing to act on while the log
        /// says exactly what was wrong.
        void SwitchScene(const std::string& request, const std::string& saveSlot = {})
        {
            // The game's data directory is the working directory (that is where
            // game.manifest and Scenes/ are looked up from at startup).
            const auto resolved = SceneTransition::ResolveScenePath(request);
            if (resolved.empty())
            {
                OLO_CORE_ERROR("[Runtime] Scene switch to '{}' failed: no matching scene file under the game directory. "
                               "Staying on '{}'.",
                               request, m_ScenePath.string());
                return;
            }

            OLO_CORE_INFO("[Runtime] Switching scene: {} -> {}", m_ScenePath.string(), resolved.string());
            if (!ActivateScene(resolved, saveSlot))
            {
                OLO_CORE_ERROR("[Runtime] Staying on '{}'.", m_ScenePath.string());
            }
        }

        /// Load `path` and make it the running scene — the shared body of both
        /// reload and switch.
        ///
        /// The new scene is deserialized and validated BEFORE the current one
        /// is stopped, so a bad target leaves the game running on the scene it
        /// already had instead of dropping it into a torn-down state.
        [[nodiscard]] bool ActivateScene(const std::filesystem::path& path, const std::string& saveSlot = {})
        {
            auto loaded = SceneTransition::LoadSceneFile(path, /*requirePrimaryCamera=*/true, saveSlot);
            if (!loaded)
            {
                OLO_CORE_ERROR("[Runtime] {}", loaded.Error);
                return false;
            }

            // Close the rebind menu first — it holds a Scene* / entity handles into the scene
            // we are about to destroy, which would dangle on the next OnUpdate.
            CloseRebindMenu();

            // Reset time scale in case we were paused
            Application::Get().SetTimeScale(1.0f);

            // Stop before starting: ScriptEngine / LuaScriptEngine hold a single
            // process-wide scene context, so the outgoing scene must release it
            // before the incoming one claims it.
            if (m_ActiveScene)
            {
                // Unregister BEFORE the scene is released: the replication drivers
                // hold a raw Scene*, and a tick between the swap and the
                // re-registration would dereference the outgoing scene.
                NetworkManager::SetActiveScene(nullptr);
                m_ActiveScene->OnRuntimeStop();
            }

            m_ActiveScene = loaded.LoadedScene;
            m_ScenePath = path;

            m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);

            const auto& window = Application::Get().GetWindow();
            u32 w = window.GetFramebufferWidth();
            u32 h = window.GetFramebufferHeight();
            if (w > 0 && h > 0)
            {
                m_ActiveScene->OnViewportResize(w, h);
                m_ViewportWidth = w;
                m_ViewportHeight = h;
            }

            m_ActiveScene->OnRuntimeStart();

            // Register the live scene with networking so replication has something
            // to capture into / apply onto. Without this the whole loop early-outs
            // on a null scene, which is exactly how it stayed dead.
            NetworkManager::SetActiveScene(m_ActiveScene.get());
            return true;
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
            // asset pack / start scene and renders the full pipeline, which needs
            // a real graphics device. ImGui isn't initialized in that window-less
            // path either, so GetIO() must not be touched. See CreateApplication
            // below.
            //
            // #691: the layer is pushed on BOTH backends, mirroring the
            // editor's un-gate (OloEditorApp.cpp). The two reasons the old
            // gate cited are both gone — the scene renders through the
            // Vulkan graph now, and the ImGui layer is pushed on both
            // backends (platform-only vs. VulkanImGuiBackend is an Application
            // concern, not ours).
            if (pushRuntimeLayer)
            {
                // Disable ImGui ini persistence — the runtime doesn't need it and
                // loading the editor's imgui.ini from CWD would cause stale state.
                ImGui::GetIO().IniFilename = nullptr;

                PushLayer(std::make_unique<RuntimeLayer>());
                if (Renderer::GetAPI() != RendererAPI::API::OpenGL)
                {
                    OLO_CORE_INFO("[RHI] RuntimeLayer under --rhi=vulkan: full game session through "
                                  "the Vulkan graph (#691)");
                }
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
