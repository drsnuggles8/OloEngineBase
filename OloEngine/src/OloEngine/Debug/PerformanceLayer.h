#pragma once

#include "OloEngine/Core/Layer.h"
#include "OloEngine/Events/KeyEvent.h"

#include <array>
#include <cfloat>

namespace OloEngine
{
    /// @brief Performance analysis overlay layer.
    ///
    /// Pushed as an overlay (renders on top of everything). Toggle visibility with F4.
    /// Shows live frame time graph, GPU/CPU timing breakdown, memory stats, and draw metrics.
    class PerformanceLayer : public Layer
    {
      public:
        PerformanceLayer();
        ~PerformanceLayer() override = default;

        void OnAttach() override;
        void OnDetach() override;
        void OnUpdate(Timestep ts) override;
        void OnImGuiRender() override;
        void OnEvent(Event& e) override;

        [[nodiscard("Store this!")]] bool IsVisible() const
        {
            return m_Visible;
        }
        void SetVisible(bool visible)
        {
            m_Visible = visible;
        }

      private:
        bool OnKeyPressed(KeyPressedEvent const& e);

        void DrawPerformanceWindow();
        void DrawFrameTimeGraph();
        void DrawTimingBreakdown();
        void DrawMemoryStats();
        void DrawCPUProfileScopes();

      private:
        bool m_Visible = false;
        bool m_Paused = false;

        // Frame time ring buffer for graph
        static constexpr u32 s_FrameHistorySize = 240;
        std::array<f32, s_FrameHistorySize> m_FrameTimeHistory{};
        u32 m_FrameHistoryIndex = 0;
        u32 m_FrameHistoryCount = 0;

        f32 m_CurrentFrameTime = 0.0f;
        f32 m_CurrentFPS = 0.0f;
        f32 m_FrameTimeMin = FLT_MAX;
        f32 m_FrameTimeMax = 0.0f;
    };
} // namespace OloEngine
