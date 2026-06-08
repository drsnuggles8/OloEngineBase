#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/LayerStack.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Renderer/RendererTypes.h"

#include <memory>
#include <string_view>

#ifndef OLO_HEADLESS
#include "OloEngine/Core/Window.h"
#include "OloEngine/ImGui/ImGuiLayer.h"
#include "OloEngine/Renderer/Renderer.h"
#else
// Minimal forward declarations for headless translation units.
// The class layout is unchanged — only heavy #includes are skipped.
namespace OloEngine
{
    class Window;
    class ImGuiLayer;
} // namespace OloEngine
#endif

int main(int argc, char** argv);

namespace OloEngine
{
    struct ApplicationCommandLineArgs
    {
        int Count = 0;
        char** Args = nullptr;

        const char* operator[](const int index) const
        {
            OLO_CORE_ASSERT(index < Count);
            return Args[index];
        }

        // True if `flag` appears among the arguments (argv[0] / program name is
        // skipped). Used to detect mode-switch flags like `--smoke-test`.
        [[nodiscard]] bool Contains(std::string_view flag) const
        {
            for (int i = 1; i < Count; ++i)
            {
                if (Args[i] != nullptr && flag == Args[i])
                {
                    return true;
                }
            }
            return false;
        }
    };

    // Number of update ticks a `--smoke-test` launch runs before it auto-closes
    // with EXIT_SUCCESS. A handful of ticks proves the main loop actually
    // advances past construction while keeping the CI launch check fast.
    inline constexpr u32 SmokeTestTickCount = 3;

    struct ApplicationSpecification
    {
        std::string Name = "OloEngine Application";
        std::string WorkingDirectory;
        ApplicationCommandLineArgs CommandLineArgs;
        RendererType PreferredRenderer = RendererType::Renderer2D;
        bool IsHeadless = false;
        bool IsEditor = false;
        u32 HeadlessTickRate = 60; // Hz — only used when IsHeadless == true

        // When > 0, the app runs in launch-smoke-test mode: it performs full
        // startup (DLL load, subsystem init), runs this many ticks, then closes
        // cleanly (EXIT_SUCCESS). 0 = normal run with no auto-close. See the
        // `--smoke-test` handling in each app's CreateApplication.
        u32 SmokeTestTickLimit = 0;
    };

    class Application
    {
      public:
        explicit Application(ApplicationSpecification specification);
        virtual ~Application();

        void OnEvent(Event& e);

        void PushLayer(std::unique_ptr<Layer> layer);
        void PushOverlay(std::unique_ptr<Layer> layer);
        void PopLayer(Layer* layer);
        void PopOverlay(Layer* layer);

        [[nodiscard("Store this!")]] Window& GetWindow()
        {
            return *m_Window;
        }

        [[nodiscard("Store this!")]] static Application& Get()
        {
            return *s_Instance;
        }

        [[nodiscard("Store this!")]] static Application* TryGet()
        {
            return s_Instance;
        }

        void Close();
        void CancelClose();

        [[nodiscard("Store this!")]] ImGuiLayer* GetImGuiLayer()
        {
            return m_ImGuiLayer;
        }

        [[nodiscard("Store this!")]] const ApplicationSpecification& GetSpecification() const
        {
            return m_Specification;
        }

        [[nodiscard("Store this!")]] bool IsHeadless() const
        {
            return m_Specification.IsHeadless;
        }

        // True if this app was launched in `--smoke-test` mode (auto-closes
        // after SmokeTestTickLimit ticks). See main() in EntryPoint.h.
        [[nodiscard("Store this!")]] bool IsSmokeTest() const
        {
            return m_Specification.SmokeTestTickLimit > 0;
        }

        // True only when a smoke test is active AND the run loop has completed
        // the configured number of ticks. If the app closed earlier (e.g. a
        // layer aborted startup), this is false and main() reports the launch as
        // failed. The explicit SmokeTestTickLimit > 0 check means this is safe to
        // call in any mode — it never reports "passed" for a normal (non-smoke)
        // run, where the limit is 0.
        [[nodiscard("Store this!")]] bool SmokeTestPassed() const
        {
            return m_Specification.SmokeTestTickLimit > 0 &&
                   m_SmokeTestTicksCompleted >= m_Specification.SmokeTestTickLimit;
        }

        [[nodiscard("Store this!")]] static const std::filesystem::path& GetStartupWorkingDirectory()
        {
            return s_StartupWorkingDirectory;
        }

        [[nodiscard("Store this!")]] f32 GetTimeScale() const
        {
            return m_TimeScale;
        }

        [[nodiscard("Store this!")]] f32 GetUnscaledDeltaTime() const
        {
            return m_UnscaledDeltaTime;
        }

        [[nodiscard("Store this!")]] PerformanceProfiler* GetPerformanceProfiler()
        {
            return &m_PerformanceProfiler;
        }

        [[nodiscard("Store this!")]] const std::unordered_map<std::string, PerFrameData>& GetProfilerPreviousFrameData() const
        {
            return m_PerformanceProfiler.GetPreviousFrameData();
        }
        void SetTimeScale(f32 scale)
        {
            if (!std::isfinite(scale))
            {
                scale = 1.0f;
            }
            m_TimeScale = std::max(0.0f, scale);
        }

        // Keep the window responsive during long-running init tasks (e.g. shader compilation).
        // Safe to call even if no window exists yet.
        static void KeepWindowAlive();

        // Call during loading to poll events and show progress in the title bar.
        static void ReportLoadingProgress(u32 current, u32 total, std::string_view label);

      private:
        void Run();
        void RunHeadless();
        bool OnWindowClose(WindowCloseEvent const& e);
        bool OnWindowResize(WindowResizeEvent const& e);

      private:
        ApplicationSpecification m_Specification;
        Scope<Window> m_Window;
        ImGuiLayer* m_ImGuiLayer = nullptr;
        bool m_Running = true;
        bool m_Minimized = false;
        LayerStack m_LayerStack;
        static constexpr f32 s_MaxTimestep = 0.25f;
        f32 m_LastFrameTime = 0.0f;
        f32 m_TimeScale = 1.0f;
        f32 m_UnscaledDeltaTime = 0.0f;
        u32 m_SmokeTestTicksCompleted = 0; // see SmokeTestTickLimit / IsSmokeTest
        PerformanceProfiler m_PerformanceProfiler;

      private:
        static Application* s_Instance;
        static std::filesystem::path s_StartupWorkingDirectory;
        friend int ::main(int argc, char** argv);
    };

    // To be defined in CLIENT
    Application* CreateApplication(ApplicationCommandLineArgs args);

} // namespace OloEngine
