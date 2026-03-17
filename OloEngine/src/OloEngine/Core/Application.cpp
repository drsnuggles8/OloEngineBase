#include "OloEnginePCH.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/NamedThreads.h"

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

        // Start the task scheduler workers
        LowLevelTasks::FScheduler::Get().StartWorkers();

        try
        {
            if (!m_Specification.IsHeadless)
            {
                m_Window = Window::Create(WindowProps(m_Specification.Name));
                m_Window->SetEventCallback(OLO_BIND_EVENT_FN(Application::OnEvent));
// Initialize debug tools before Renderer to catch all resource creation
#ifdef OLO_DEBUG
                GPUResourceInspector::GetInstance().Initialize();
                ShaderDebugger::GetInstance().Initialize();
                OLO_CORE_INFO("GPU Resource Inspector and Shader Debugger initialized before Renderer");
#endif

                Renderer::Init(m_Specification.PreferredRenderer);

                if (!AudioEngine::Init())
                {
                    OLO_CORE_CRITICAL("Failed to initialize AudioEngine! Application cannot continue.");
                    throw std::runtime_error("AudioEngine initialization failed");
                }

                GamepadManager::Initialize();
                InputActionManager::Init();

                m_ImGuiLayer = new ImGuiLayer();
                PushOverlay(m_ImGuiLayer);
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

            ScriptEngine::Init();
            LuaScriptEngine::Init();
        }
        catch (...)
        {
            // Unwind subsystems in reverse initialization order.
            // Each Shutdown() is safe to call even if its Init() wasn't reached.
            if (!m_Specification.IsHeadless)
            {
                InputActionManager::Shutdown();
                GamepadManager::Shutdown();
            }
            LuaScriptEngine::Shutdown();
            ScriptEngine::Shutdown();
            NetworkManager::Shutdown();
            if (!m_Specification.IsHeadless)
            {
                AudioEngine::Shutdown();
                Renderer::Shutdown();

#ifdef OLO_DEBUG
                ShaderDebugger::GetInstance().Shutdown();
                GPUResourceInspector::GetInstance().Shutdown();
#endif

                m_Window.reset();
            }
            s_Instance = nullptr;
            throw;
        }
    }
    Application::~Application()
    {
        OLO_PROFILE_FUNCTION();

        for (Layer* const layer : m_LayerStack)
        {
            layer->OnDetach();
            delete layer;
        }

        if (!m_Specification.IsHeadless)
        {
            InputActionManager::Shutdown();
            GamepadManager::Shutdown();
        }
        LuaScriptEngine::Shutdown();
        ScriptEngine::Shutdown();
        NetworkManager::Shutdown();
        if (!m_Specification.IsHeadless)
        {
            AudioEngine::Shutdown();
            // Shutdown debug tools before Renderer
#ifdef OLO_DEBUG
            ShaderDebugger::GetInstance().Shutdown();
            GPUResourceInspector::GetInstance().Shutdown();
            OLO_CORE_INFO("GPU Resource Inspector and Shader Debugger shutdown");
#endif

            Renderer::Shutdown();
        }

        // Shutdown task scheduler
        LowLevelTasks::FScheduler::Get().StopWorkers();
        Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::GameThread);
    }

    void Application::PushLayer(Layer* const layer)
    {
        OLO_PROFILE_FUNCTION();

        m_LayerStack.PushLayer(layer);
        layer->OnAttach();
    }

    void Application::PushOverlay(Layer* const layer)
    {
        OLO_PROFILE_FUNCTION();

        m_LayerStack.PushOverlay(layer);
        layer->OnAttach();
    }

    void Application::PopLayer(Layer* const layer)
    {
        m_LayerStack.PopLayer(layer);
        layer->OnDetach();
    }

    void Application::PopOverlay(Layer* const layer)
    {
        m_LayerStack.PopOverlay(layer);
        layer->OnDetach();
    }

    void Application::Close()
    {
        m_Running = false;
    }

    void Application::CancelClose()
    {
        OLO_PROFILE_FUNCTION();

        m_Running = true;
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

    void Application::Run()
    {
        OLO_PROFILE_FUNCTION();

        while (m_Running)
        {

            const auto timeNow = Time::GetTime();
            const Timestep timestep = timeNow - m_LastFrameTime;
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

            // Update gamepad state (polls GLFW for gamepad button/axis states)
            OLO_PROFILE_FRAMEMARK_START("GamepadManager Update");
            GamepadManager::Update();
            OLO_PROFILE_FRAMEMARK_END("GamepadManager Update");

            // Update action mapping state (reads fresh GLFW state)
            OLO_PROFILE_FRAMEMARK_START("InputActionManager Update");
            InputActionManager::Update();
            OLO_PROFILE_FRAMEMARK_END("InputActionManager Update");

            // Process tasks targeted at the Game Thread
            Tasks::FNamedThreadManager::Get().ProcessTasks(true);

            if (!m_Minimized)
            {
                {
                    OLO_PROFILE_FRAMEMARK_START("LayerStack OnUpdate");
                    for (Layer* const layer : m_LayerStack)
                    {
                        layer->OnUpdate(timestep);
                    }
                    OLO_PROFILE_FRAMEMARK_END("LayerStack OnUpdate");
                }

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

            OLO_PROFILE_FRAMEMARK_START("Window SwapBuffers");
            m_Window->SwapBuffers();
            OLO_PROFILE_FRAMEMARK_END("Window SwapBuffers");
        }
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
                const Timestep timestep(tickInterval);

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
                const f32 tickDuration = tickTimer.Elapsed();
                if (tickDuration > tickInterval)
                {
                    OLO_CORE_WARN("Server tick exceeded budget: {:.2f} ms (budget: {:.2f} ms)",
                                  tickDuration * 1000.0f, tickInterval * 1000.0f);
                }

                accumulator -= tickInterval;
            }

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

        // Use framebuffer size for renderer
        Renderer::OnWindowResize(fbWidth, fbHeight);

        return false;
    }

} // namespace OloEngine
