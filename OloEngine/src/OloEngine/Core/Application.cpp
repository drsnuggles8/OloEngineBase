#include "OloEnginePCH.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/BuildInfo.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Core/CVar.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Debug/DebugOverlayLayer.h"
#include "OloEngine/Debug/PerformanceLayer.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/BackendSelection.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Video/VideoSystem.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/NamedThreads.h"
#include "Platform/Steam/SteamManager.h"

#include <chrono>
#include <stdexcept>
#include <ranges>
#include <thread>
#include <utility>

namespace OloEngine
{
    Application* Application::s_Instance = nullptr;
    std::filesystem::path Application::s_StartupWorkingDirectory;
    Application::Application(ApplicationSpecification specification)
        : m_Specification(std::move(specification))
    {
        OLO_PROFILE_FUNCTION();

        // Say which debug levers are on, before anything acts on one. A run
        // that behaves oddly because someone left OLO_RG_POISON_TRANSIENTS set
        // should say so in its own log, not in a shell history nobody has.
        // Silent when everything is at its default, which is the normal case.
        Levers::LogActive();

        // Initialize Game Thread identity for the task system
        LowLevelTasks::InitGameThreadId();
        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);

        OLO_CORE_ASSERT(!s_Instance, "Application already exists!");
        s_Instance = this;
        // Set working directory here
        if (!m_Specification.WorkingDirectory.empty())
        {
            std::filesystem::current_path(m_Specification.WorkingDirectory);
        }
        s_StartupWorkingDirectory = std::filesystem::current_path();

        // Player accessibility preferences (issue #458). Loaded here rather than
        // in a layer so the editor, the runtime and the server all get them from
        // one place, and AFTER the working-directory switch above so the relative
        // prefs path resolves against the app's real cwd. A missing file is the
        // normal first-run case and leaves the construction defaults installed.
        (void)Accessibility::LoadFromFile(Accessibility::DefaultSettingsPath());

        // Start the task scheduler workers
        LowLevelTasks::FScheduler::Get().StartWorkers();

        // RHI backend selection (ADR 0011 §2, #691). HARD ordering
        // contract: this must run BEFORE the Window::Create below — WindowsWindow /
        // LinuxWindow read Renderer::GetAPI() ahead of glfwCreateWindow to pick the
        // window's client API. Parsed after the working-directory switch above so
        // the config-file fallback resolves against the app's real cwd.
        bool vulkanSelectedFromConfig = false;
        if (!m_Specification.IsHeadless)
        {
            const BackendSelection backend = SelectRendererBackend(
                m_Specification.CommandLineArgs.Count, m_Specification.CommandLineArgs.Args,
                DefaultRendererConfigPath());
            if (!backend.Diagnostic.empty())
            {
                OLO_CORE_ERROR("[RHI] {}", backend.Diagnostic);
            }
            RendererAPI::SetAPI(backend.Api);
            // ADR 0011 amendment (39): RenderCommand::s_RendererAPI was built
            // at static init, BEFORE the selection above — always the OpenGL
            // default. Re-create it now so the facade matches the selection;
            // nothing has routed through it yet (no window, no context), which
            // is the only moment this swap is legal.
            RenderCommand::RecreateForSelectedBackend();
            OLO_CORE_INFO("[RHI] Backend: {} (source: {})",
                          backend.Api == RendererAPI::API::Vulkan ? "Vulkan" : "OpenGL", backend.Source);
            // A persisted preference is the SOFT half of the selection chain
            // (#691): if Vulkan came from the config file and cannot
            // actually initialise on this machine, the boot below retries on
            // OpenGL and rewrites the file, instead of bricking the install
            // with EXIT_FAILURE on every launch. An explicit `--rhi=vulkan`
            // flag remains a hard assertion and keeps ADR 0010's
            // refuse-to-init behaviour.
            vulkanSelectedFromConfig =
                backend.Api == RendererAPI::API::Vulkan && backend.Source == "config file";
        }

        // #691: the renderer now comes up on BOTH backends —
        // the whole pass suite is ported and the swapchain is importable, so
        // Renderer::Init and the layer stack run under --rhi=vulkan and the
        // render graph draws the real frame. What is still GL-only, and so
        // still gated, is (a) ShaderDebugger (a glad-call site) and (b) the
        // ImGui RENDERER backend — the ImGui layer itself is pushed either way
        // and runs platform-only under Vulkan (see ImGuiLayer::OnAttach).
        // GPUResourceInspector came off this list in #810: it has a Vulkan arm
        // now, so it initialises on both backends (and self-gates to a no-op if
        // the factory ever returns no backend).
        // Not const: the config-sourced Vulkan fallback below can switch the
        // API back to OpenGL after a failed window creation, and this flag
        // must follow (it also steers the unwind path in the catch block).
        bool glOnlyTooling = RendererAPI::GetAPI() != RendererAPI::API::Vulkan;

        // Register the application name with the crash reporter
        CrashReporter::SetApplicationInfo(m_Specification.Name, OLO_ENGINE_VERSION);
        CrashReporter::SetHeadless(m_Specification.IsHeadless);

        // Build identity (#894) — the one line every bug report can quote back
        // to name the exact build it came from.
        OLO_CORE_INFO("[{}] Build: {}", m_Specification.Name, BuildInfo::GetBuildId());

        try
        {
            if (!m_Specification.IsHeadless)
            {
                try
                {
                    m_Window = Window::Create(WindowProps(m_Specification.Name));
                }
                catch (const std::exception& e)
                {
                    // #691: a Vulkan selection persisted in
                    // config/renderer.yaml must not brick the install on a
                    // machine whose device/driver cannot satisfy it — retry on
                    // OpenGL, loudly, and rewrite the file so the next launch
                    // starts clean. An explicit --rhi=vulkan flag never takes
                    // this path (ADR 0010: a capability failure refuses to
                    // init); neither does a failure on the OpenGL arm.
                    if (!vulkanSelectedFromConfig)
                    {
                        throw;
                    }
                    OLO_CORE_ERROR("[RHI] Vulkan selected by config file but initialisation failed: {}",
                                   e.what());
                    const auto configPath = DefaultRendererConfigPath();
                    if (WriteRendererConfig(configPath, RendererAPI::API::OpenGL))
                    {
                        OLO_CORE_ERROR("[RHI] Rewrote {} to opengl; retrying on OpenGL", configPath.string());
                    }
                    else
                    {
                        OLO_CORE_ERROR("[RHI] Could not rewrite {}; retrying on OpenGL for this session only",
                                       configPath.string());
                    }
                    RendererAPI::SetAPI(RendererAPI::API::OpenGL);
                    RenderCommand::RecreateForSelectedBackend();
                    glOnlyTooling = true;
                    m_Window = Window::Create(WindowProps(m_Specification.Name));
                }
                m_Window->SetEventCallback(OLO_BIND_EVENT_FN(Application::OnEvent));

// Initialize debug tools before Renderer to catch all resource creation.
#ifdef OLO_DEBUG
                GPUResourceInspector::GetInstance().Initialize();
                if (glOnlyTooling)
                {
                    ShaderDebugger::GetInstance().Initialize();
                    OLO_CORE_INFO("GPU Resource Inspector and Shader Debugger initialized before Renderer");
                }
                else
                {
                    OLO_CORE_INFO("[RHI] Vulkan: GPU Resource Inspector initialized (#810); ShaderDebugger "
                                  "and the ImGui renderer backend are skipped");
                }
#else
                if (!glOnlyTooling)
                {
                    OLO_CORE_INFO("[RHI] Vulkan (#691): the GL debug tools and the ImGui renderer "
                                  "backend are skipped; the renderer and the render graph run");
                }
#endif

                m_Window->SetTitle(m_Specification.Name + " — Loading shaders...");
                Renderer::Init(m_Specification.PreferredRenderer, m_Window.get());
                // Surface the non-default backend where a player can see it
                // (#691) — the log line above is invisible in a
                // shipped game, and "which backend am I actually on?" is the
                // first diagnostic question.
                m_Window->SetTitle(glOnlyTooling ? m_Specification.Name
                                                 : m_Specification.Name + " [Vulkan]");

                // #691: a backend whose swap path owns frame
                // recording (Vulkan: acquire → record → submit → present)
                // cannot have the frame drawn before SwapBuffers — there is
                // no open command buffer then, and no acquired backbuffer to
                // resolve "the default framebuffer" to. Hand it the frame's
                // layer work instead; Run() then skips the inline call.
                if (auto* context = m_Window->GetGraphicsContext(); context != nullptr)
                {
                    m_BackendDrivesFrameRendering = !glOnlyTooling;
                    if (m_BackendDrivesFrameRendering)
                    {
                        context->SetFrameRenderCallback(
                            [this](const GraphicsContext::FrameRenderTarget& target) -> bool
                            {
                                // Declined until the main loop owns the frame
                                // (see m_FrameLoopStarted) — startup presents
                                // from inside OnAttach/Renderer3D::Init.
                                if (!m_FrameLoopStarted || target.Width == 0 || target.Height == 0)
                                    return false;
                                return RenderFrameLayers(m_PendingFrameTimestep);
                            });
                    }
                }

                if (!AudioEngine::Init())
                {
                    OLO_CORE_CRITICAL("Failed to initialize AudioEngine! Application cannot continue.");
                    throw std::runtime_error("AudioEngine initialization failed");
                }

                GamepadManager::Initialize();
                InputActionManager::Init();

                // Non-owning pointer — ownership is transferred to LayerStack
                // via PushOverlay; kept here for convenient access. Pushed on
                // both backends: under Vulkan the layer is platform-only, and
                // the editor's update path calls into ImGui (input drain,
                // viewport mouse) whether or not anything is drawn.
                m_ImGuiLayer = new ImGuiLayer();
                PushOverlay(std::unique_ptr<Layer>(m_ImGuiLayer));

                // Debug/performance overlay layers (toggle with F3/F4)
                PushOverlay(std::make_unique<DebugOverlayLayer>());
                PushOverlay(std::make_unique<PerformanceLayer>());
            }
            else
            {
                OLO_CORE_INFO("Running in headless mode — no window, renderer, or audio");
            }

            if (!NetworkManager::Init())
            {
                OLO_CORE_CRITICAL("Failed to initialize NetworkManager!");
                throw std::runtime_error("NetworkManager initialization failed");
            }
            OLO_CORE_INFO("NetworkManager initialized successfully");

            // Steamworks platform services (#644). Deliberately NOT guarded by a throw, unlike
            // AudioEngine and NetworkManager above: a missing or non-running Steam client is an
            // ordinary state (developer machine, CI runner, exe launched outside Steam) and must
            // never stop the engine starting. SteamManager::Initialize() warns and disables.
            //
            // Skipped headless: OloServer has no Steam client to talk to, and the client API is
            // the wrong one for a dedicated server anyway (that would be ISteamGameServer, which
            // is out of scope here). Shutdown is still called unconditionally below — it is a
            // no-op when Initialize() never ran, so the two guards cannot drift into a leak.
            //
            // Before the script engines so scripts can query Steam during their own init.
            if (!m_Specification.IsHeadless)
            {
                SteamManager::Initialize();
            }

            ScriptEngine::Init();
            LuaScriptEngine::Init();
        }
        catch (...)
        {
            // Detach layers before shutting down subsystems they depend on.
            m_LayerStack.Clear();
            m_ImGuiLayer = nullptr;

            // Same two GPU-owning caches the destructor path releases, and for the same
            // reason: a layer that ran OnAttach before the throw can already have created
            // them. OloRuntime puts up a studio-logo splash through
            // VideoSystem::ShowFullscreenImage() from its layer attach, so this is a real
            // path, not symmetry for its own sake. Both are no-ops when nothing was
            // created (#839).
            VideoSystem::Shutdown();
            Scene::ReleaseSharedRenderDefaults();

            // Unwind subsystems in reverse initialization order.
            // Each Shutdown() is safe to call even if its Init() wasn't reached.
            if (!m_Specification.IsHeadless)
            {
                InputActionManager::Shutdown();
                GamepadManager::Shutdown();
            }
            LuaScriptEngine::Shutdown();
            ScriptEngine::Shutdown();
            // Shutdown site 1 of 2 (exception path). Unconditional and safe when Initialize()
            // never ran; missing either site leaks the Steam session.
            SteamManager::Shutdown();
            NetworkManager::Shutdown();
            if (!m_Specification.IsHeadless)
            {
                AudioEngine::Shutdown();
                Renderer::Shutdown();
                // ShaderDebugger is still GL-only; the inspector runs on both
                // backends since #810 and self-gates when un-initialized.
#ifdef OLO_DEBUG
                if (glOnlyTooling)
                {
                    ShaderDebugger::GetInstance().Shutdown();
                }
                GPUResourceInspector::GetInstance().Shutdown();
#endif

                m_Window.reset();
            }
            s_Instance = nullptr;

            // Shutdown task scheduler started before the try block.
            LowLevelTasks::FScheduler::Get().StopWorkers();
            Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::GameThread);
            throw;
        }
    }
    Application::~Application()
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("Application: teardown begins");
        // Clear calls OnDetach() on all layers and releases their memory.
        // Do this before shutting down subsystems so layers detach while systems are live.
        m_LayerStack.Clear();
        m_ImGuiLayer = nullptr;
        OLO_CORE_INFO("Application: layers detached");

        // Drop the active project + asset manager NOW, while the renderer is
        // still alive: their static Refs otherwise keep every loaded asset's
        // GPU buffers alive past the window's graphics-context teardown —
        // which vmaDestroyAllocator answers with an "allocations not freed"
        // abort on Vulkan (#691, the close-button crash).
        Project::Unload();

        // Two process-wide caches that own GPU memory but do NOT live under Renderer/,
        // so releasing them from Renderer::Shutdown() would invert the layering. This is
        // the narrowest teardown that is unconditional for every session that can create
        // them AND still runs while the graphics context is alive, which is the rule
        // (#814/#839, docs/agent-rules/lazy-static-release-ownership.md) one level up
        // from "release it from Renderer::Shutdown()".
        //
        // VideoSystem's global fullscreen player holds a decoded-frame GPU texture. Its
        // Shutdown() was written with the comment "Call on engine/scene shutdown" and had
        // ZERO callers, so a splash/cutscene overlay leaked for the life of the process —
        // a shipped OloRuntime showing a studio logo hit this on every launch.
        //
        // Scene's cached default material is a plain PBR stand-in that owns no textures
        // today, but Material can own five Texture2Ds and three cubemaps, so the shape is
        // one ConfigureIBL() call away from being a real leak. Released here rather than
        // left to static destruction, where a Ref's destructor runs after the Meyer's
        // singletons it frees through are already gone.
        VideoSystem::Shutdown();
        Scene::ReleaseSharedRenderDefaults();

        if (!m_Specification.IsHeadless)
        {
            InputActionManager::Shutdown();
            GamepadManager::Shutdown();
        }
        LuaScriptEngine::Shutdown();
        ScriptEngine::Shutdown();
        // Shutdown site 2 of 2 (normal destructor path). Reverse of the init order above.
        SteamManager::Shutdown();
        NetworkManager::Shutdown();
        if (!m_Specification.IsHeadless)
        {
            // Before StopWorkers() below: this is what joins the audio thread.
            AudioEngine::Shutdown();
            // ShaderDebugger is still GL-only (#691); the inspector runs on
            // both backends since #810.
#ifdef OLO_DEBUG
            // Shutdown debug tools before Renderer
            if (RendererAPI::GetAPI() != RendererAPI::API::Vulkan)
            {
                ShaderDebugger::GetInstance().Shutdown();
            }
            GPUResourceInspector::GetInstance().Shutdown();
            OLO_CORE_INFO("GPU Resource Inspector and Shader Debugger shutdown");
#endif
            Renderer::Shutdown();
        }

        // Shutdown task scheduler
        LowLevelTasks::FScheduler::Get().StopWorkers();
        Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::GameThread);
    }

    void Application::KeepWindowAlive()
    {
        if (s_Instance && s_Instance->m_Window)
        {
            s_Instance->m_Window->PollEvents();
        }
    }

    void Application::ReportLoadingProgress(u32 current, u32 total, std::string_view label)
    {
        if (s_Instance && s_Instance->m_Window)
        {
            std::string title = s_Instance->m_Specification.Name + " — Loading " +
                                std::string(label) + " (" + std::to_string(current) + "/" + std::to_string(total) + ")";
            s_Instance->m_Window->SetTitle(title);
            s_Instance->m_Window->PollEvents();
        }
    }

    void Application::PushLayer(std::unique_ptr<Layer> layer)
    {
        OLO_PROFILE_FUNCTION();

        m_LayerStack.PushLayer(std::move(layer));
    }

    void Application::PushOverlay(std::unique_ptr<Layer> layer)
    {
        OLO_PROFILE_FUNCTION();

        m_LayerStack.PushOverlay(std::move(layer));
    }

    void Application::PopLayer(Layer* const layer)
    {
        m_LayerStack.PopLayer(layer);
    }

    void Application::PopOverlay(Layer* const layer)
    {
        m_LayerStack.PopOverlay(layer);
    }

    void Application::Close()
    {
        OLO_CORE_INFO("Application: close requested");
        m_Running = false;
    }

    void Application::CancelClose()
    {
        OLO_PROFILE_FUNCTION();

        m_Running = true;
    }

    void Application::SetRandomSeed(u64 seed)
    {
        // Store-only: the seed is applied to the game-thread gameplay RNG by
        // Scene::OnRuntimeStart at every Play / runtime launch. We deliberately do
        // NOT eagerly call RandomUtils::SetGlobalSeed here — the RNG is
        // thread_local, so an eager call would only seed whatever thread happened
        // to call this setter (e.g. a config-load thread), not the game thread,
        // and would be overwritten at OnRuntimeStart anyway (issue #452).
        m_RandomSeed = seed;
    }

    void Application::OnEvent(Event& e)
    {
        OLO_PROFILE_FUNCTION();

        // Notify GamepadManager of keyboard/mouse activity for device switching
        if (e.GetEventType() == EventType::KeyPressed || e.GetEventType() == EventType::MouseButtonPressed || e.GetEventType() == EventType::MouseMoved)
        {
            GamepadManager::NotifyKeyboardMouseActivity();
        }

        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<WindowCloseEvent>(OLO_BIND_EVENT_FN(Application::OnWindowClose));
        dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(Application::OnWindowResize));

        for (auto& it : std::ranges::reverse_view(m_LayerStack))
        {
            if (e.Handled)
            {
                break;
            }
            it->OnEvent(e);
        }
    }

    bool Application::RenderFrameLayers(const Timestep timestep)
    {
        if (m_Minimized)
        {
            return false;
        }

        // RE-ENTRANCY. On a backend that records the frame inside SwapBuffers
        // this function runs from the swap path — and the engine presents from
        // places that are NOT the frame loop: ShaderWarmup::RenderProgressFrame
        // calls Window::SwapBuffers per compiled shader, from inside
        // Renderer3D::Init, which the editor triggers LAZILY from its own
        // OnUpdate. Without this latch that nested swap re-enters the layer
        // loop against a half-built renderer — an access violation on the
        // first 3D progress frame, which is exactly how this was found.
        // Declining the nested frame makes the backend present its clear
        // (the loading screen's own draw is still recorded and shown by the
        // outer frame).
        if (m_FrameLayersInProgress)
        {
            return false;
        }
        struct ReentrancyLatch
        {
            bool& Flag;
            explicit ReentrancyLatch(bool& flag)
                : Flag(flag)
            {
                Flag = true;
            }
            ~ReentrancyLatch()
            {
                Flag = false;
            }
        } latch{ m_FrameLayersInProgress };

        {
            OLO_PROFILE_FRAMEMARK_START("LayerStack OnUpdate");
            for (Layer* const layer : m_LayerStack)
            {
                layer->OnUpdate(timestep);
            }
            OLO_PROFILE_FRAMEMARK_END("LayerStack OnUpdate");
        }

        // Null only in headless / early-failure paths — the layer is pushed on
        // both backends now (platform-only under Vulkan, see ImGuiLayer).
        if (m_ImGuiLayer != nullptr)
        {
            OloEngine::ImGuiLayer::Begin();
            {
                OLO_PROFILE_FRAMEMARK_START("LayerStack OnImGuiRender");
                for (Layer* const layer : m_LayerStack)
                {
                    layer->OnImGuiRender();
                }
                OLO_PROFILE_FRAMEMARK_END("LayerStack OnImGuiRender");
            }
            OloEngine::ImGuiLayer::End();
        }
        return true;
    }

    void Application::Run()
    {
        OLO_PROFILE_FUNCTION();

        // Seed the frame timer so the first iteration doesn't include
        // the entire startup/attach time as one gigantic delta.
        m_LastFrameTime = Time::GetTime();
        // From here on a swap IS a frame (see m_FrameLoopStarted).
        m_FrameLoopStarted = true;

        while (m_Running)
        {
            // Console-variable changes land here and nowhere else: a write from
            // the console, an MCP worker or a test only MARKS its cvar, and this
            // is where the observers run — on the game thread, before anything
            // in the frame reads a value. Cheap when nothing changed, which is
            // every frame but the ones you are debugging.
            CVars::DispatchPendingChanges();

            const auto timeNow = Time::GetTime();
            const f32 rawDelta = timeNow - m_LastFrameTime;
            m_UnscaledDeltaTime = rawDelta;
            // EMA-smooth the delta handed to layers to damp frame-time jitter
            // (issue #456). With smoothing disabled (default) this returns the
            // raw delta unchanged, so behaviour is identical until opted in.
            const f32 smoothedDelta = m_FramePacer.SmoothDelta(rawDelta);
            const Timestep timestep = std::min(smoothedDelta, s_MaxTimestep) * m_TimeScale;
            m_LastFrameTime = timeNow;

            // Poll OS events first so GLFW key state is fresh for this frame
            OLO_PROFILE_FRAMEMARK_START("Window PollEvents");
            m_Window->PollEvents();
            OLO_PROFILE_FRAMEMARK_END("Window PollEvents");

            // A WindowCloseEvent during PollEvents may have set m_Running = false
            if (!m_Running)
            {
                break;
            }

            // Snapshot raw keyboard state for just-pressed / just-released detection
            Input::Update();

            // Update gamepad state (polls GLFW for gamepad button/axis states)
            OLO_PROFILE_FRAMEMARK_START("GamepadManager Update");
            GamepadManager::Update();
            OLO_PROFILE_FRAMEMARK_END("GamepadManager Update");

            // Update action mapping state (reads fresh GLFW state)
            OLO_PROFILE_FRAMEMARK_START("InputActionManager Update");
            InputActionManager::Update();
            OLO_PROFILE_FRAMEMARK_END("InputActionManager Update");

            // Drain Steam's callback queue (#644). Position matters twice over:
            //   * AFTER the input/platform block and BEFORE ProcessTasks below, so a Steam
            //     callback that enqueues a game-thread task is drained in the SAME frame rather
            //     than sitting until the next one;
            //   * in Run(), NOT in RenderFrameLayers — that function has a re-entrancy latch and
            //     the nested-swap path would pump Steam callbacks twice per frame.
            OLO_PROFILE_FRAMEMARK_START("Steam RunCallbacks");
            SteamManager::RunCallbacks();
            OLO_PROFILE_FRAMEMARK_END("Steam RunCallbacks");

            // Process tasks targeted at the Game Thread
            Tasks::FNamedThreadManager::Get().ProcessTasks(true);

            // #691: with a backend that owns frame recording the layer
            // work happens inside SwapBuffers, in the backend's command-buffer
            // bracket, against the acquired backbuffer — so it is HANDED OVER
            // here rather than run. Everything above (input, tasks) still runs
            // at the top of the frame exactly as before.
            m_PendingFrameTimestep = timestep;
            if (!m_BackendDrivesFrameRendering)
            {
                (void)RenderFrameLayers(timestep);
            }

            // Timed separately from the FRAMEMARK scope: SwapBuffers is the
            // real GPU-sync point for a windowed app (vsync, or the driver
            // throttling submission when the GPU is behind) and previously
            // wasn't attributed to gpuWaitMs at all — only the
            // FrameResourceManager fence wait was, which left gpuWaitMs
            // reading ~0 even when clearly GPU-bound (#519). EndFrame() has
            // already finalized this frame's numbers by the time SwapBuffers
            // returns, so the wait is handed to the profiler and patched into
            // this already-recorded frame at the next BeginFrame() (see
            // RendererProfiler::BeginFrame()).
            //
            // Only reported while !m_Minimized: RendererProfiler::BeginFrame()/
            // EndFrame() are only called from the render path above, which is
            // itself skipped while minimized. Reporting unconditionally would
            // accumulate into m_PendingPostFrameGPUWaitTime for as long as the
            // window stays minimized (no BeginFrame() runs to drain it), and
            // the whole pent-up total would then land on the first frame
            // rendered after restore, reading as a bogus multi-{minutes,hours}
            // GPU-wait spike on that one frame.
            const auto swapStart = std::chrono::high_resolution_clock::now();
            OLO_PROFILE_FRAMEMARK_START("Window SwapBuffers");
            m_Window->SwapBuffers();
            OLO_PROFILE_FRAMEMARK_END("Window SwapBuffers");
            const auto swapEnd = std::chrono::high_resolution_clock::now();
            if (!m_Minimized)
            {
                RendererProfiler::GetInstance().AddPostFrameGPUWaitTime(
                    std::chrono::duration<f64, std::milli>(swapEnd - swapStart).count());
            }

            // Frame-rate cap (issue #456). Placed AFTER SwapBuffers so it only
            // sleeps for the budget vsync (if on) didn't already consume — no
            // double-throttle. Measured from this frame's start (timeNow).
            OLO_PROFILE_FRAMEMARK_START("FramePacer LimitFrameRate");
            m_FramePacer.LimitFrameRate(timeNow);
            OLO_PROFILE_FRAMEMARK_END("FramePacer LimitFrameRate");

            // Snapshot per-function performance data for this frame
            m_PerformanceProfiler.EndFrame();

            // Launch-smoke-test mode: after the configured number of ticks the
            // app has proven it starts and the loop advances — close cleanly.
            if (m_Specification.SmokeTestTickLimit > 0 &&
                ++m_SmokeTestTicksCompleted >= m_Specification.SmokeTestTickLimit)
            {
                OLO_CORE_INFO("[SmokeTest] Completed {} tick(s); shutting down cleanly.", m_SmokeTestTicksCompleted);
                m_Running = false;
            }
        }
        OLO_CORE_INFO("Application: frame loop exited");
    }

    void Application::RunHeadless()
    {
        OLO_PROFILE_FUNCTION();

        const u32 tickRateHz = m_Specification.HeadlessTickRate > 0 ? m_Specification.HeadlessTickRate : 60;
        const f32 tickInterval = 1.0f / static_cast<f32>(tickRateHz);
        Timer timer;
        f32 accumulator = 0.0f;

        OLO_CORE_INFO("Headless loop started (tick rate: {} Hz)", tickRateHz);

        while (m_Running)
        {
            const f32 elapsed = timer.Elapsed();
            timer.Reset();
            accumulator += elapsed;

            while (accumulator >= tickInterval)
            {
                // Same contract as the windowed loop: observers run on the game
                // thread at the top of the tick. OloServer has no console, but
                // it does have `--set` and the MCP surface.
                CVars::DispatchPendingChanges();

                const Timestep timestep(tickInterval * m_TimeScale);

                // Process tasks targeted at the Game Thread
                Tasks::FNamedThreadManager::Get().ProcessTasks(true);

                Timer tickTimer;
                OLO_PROFILE_FRAMEMARK_START("LayerStack OnUpdate");
                for (Layer* const layer : m_LayerStack)
                {
                    layer->OnUpdate(timestep);
                }
                OLO_PROFILE_FRAMEMARK_END("LayerStack OnUpdate");

                // Tick budget warning
                if (const f32 tickDuration = tickTimer.Elapsed(); tickDuration > tickInterval)
                {
                    OLO_CORE_WARN("Server tick exceeded budget: {:.2f} ms (budget: {:.2f} ms)",
                                  tickDuration * 1000.0f, tickInterval * 1000.0f);
                }

                accumulator -= tickInterval;

                // Launch-smoke-test mode: stop once the loop has advanced the
                // configured number of ticks (proves headless startup works).
                if (m_Specification.SmokeTestTickLimit > 0 &&
                    ++m_SmokeTestTicksCompleted >= m_Specification.SmokeTestTickLimit)
                {
                    OLO_CORE_INFO("[SmokeTest] Completed {} tick(s); shutting down cleanly.", m_SmokeTestTicksCompleted);
                    m_Running = false;
                    break;
                }
            }

            m_PerformanceProfiler.EndFrame();

            // Sleep remaining time to avoid spinning the CPU
            const f32 sleepTime = tickInterval - accumulator;
            if (sleepTime > 0.001f)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<i32>(sleepTime * 1000)));
            }
        }

        OLO_CORE_INFO("Headless loop stopped");
    }

    bool Application::OnWindowClose([[maybe_unused]] WindowCloseEvent const& e)
    {
        OLO_PROFILE_FUNCTION();

        m_Running = false;
        return false; // Allow layers to intercept and potentially cancel the close
    }

    bool Application::OnWindowResize(WindowResizeEvent const& e)
    {
        OLO_PROFILE_FUNCTION();

        if ((0 == e.GetWidth()) || (0 == e.GetHeight()))
        {
            m_Minimized = true;
            return false;
        }

        m_Minimized = false;

        // Get the framebuffer size which might be different on high DPI displays
        u32 fbWidth = m_Window->GetFramebufferWidth();
        u32 fbHeight = m_Window->GetFramebufferHeight();

        OLO_CORE_INFO("Application::OnWindowResize - Window: {}x{}, Framebuffer: {}x{}",
                      e.GetWidth(), e.GetHeight(), fbWidth, fbHeight);

        // Use framebuffer size for renderer. On Vulkan the SWAPCHAIN half is
        // still the context's own business (it recreates on
        // VK_ERROR_OUT_OF_DATE_KHR inside SwapBuffers); this is the engine
        // half — the render graph's own targets (#691).
        Renderer::OnWindowResize(fbWidth, fbHeight);

        return false;
    }

    PerformanceProfiler* GetGlobalPerformanceProfiler()
    {
        if (auto* app = Application::TryGet())
            return app->GetPerformanceProfiler();
        return nullptr;
    }

} // namespace OloEngine
