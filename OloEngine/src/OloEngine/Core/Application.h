#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/LayerStack.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Renderer/RendererTypes.h"

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
    };

    struct ApplicationSpecification
    {
        std::string Name = "OloEngine Application";
        std::string WorkingDirectory;
        ApplicationCommandLineArgs CommandLineArgs;
        RendererType PreferredRenderer = RendererType::Renderer2D;
        bool IsHeadless = false;
        bool IsEditor = false;
        u32 HeadlessTickRate = 60; // Hz — only used when IsHeadless == true
    };

    class Application
    {
      public:
        explicit Application(ApplicationSpecification specification);
        virtual ~Application();

        void OnEvent(Event& e);

        void PushLayer(Layer* layer);
        void PushOverlay(Layer* layer);
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

        [[nodiscard("Store this!")]] static const std::filesystem::path& GetStartupWorkingDirectory()
        {
            return s_StartupWorkingDirectory;
        }

        [[nodiscard("Store this!")]] f32 GetTimeScale() const
        {
            return m_TimeScale;
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
        ImGuiLayer* m_ImGuiLayer;
        bool m_Running = true;
        bool m_Minimized = false;
        LayerStack m_LayerStack;
        static constexpr f32 s_MaxTimestep = 0.25f;
        f32 m_LastFrameTime = 0.0f;
        f32 m_TimeScale = 1.0f;

      private:
        static Application* s_Instance;
        static std::filesystem::path s_StartupWorkingDirectory;
        friend int ::main(int argc, char** argv);
    };

    // To be defined in CLIENT
    Application* CreateApplication(ApplicationCommandLineArgs args);

} // namespace OloEngine
