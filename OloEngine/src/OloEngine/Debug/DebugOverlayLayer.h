#pragma once

#include "OloEngine/Core/Layer.h"
#include "OloEngine/Events/KeyEvent.h"

namespace OloEngine
{
    /// @brief Debug overlay layer providing in-viewport debug visualization toggles.
    ///
    /// Pushed as an overlay (renders on top of everything). Toggle visibility with F3.
    /// Provides toggles for wireframe, physics colliders, bounding boxes, and quick debug info.
    class DebugOverlayLayer : public Layer
    {
      public:
        DebugOverlayLayer();
        ~DebugOverlayLayer() override = default;

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

        void DrawOverlayHUD();
        void DrawVisualizationToggles();
        void DrawQuickStats();

      private:
        bool m_Visible = false;

        // Visualization toggles
        bool m_ShowWireframe = false;
        bool m_ShowPhysicsColliders = false;
        bool m_ShowBoundingBoxes = false;
        bool m_ShowLightFrustums = false;
        bool m_ShowGrid = true;
        bool m_ShowEntityNames = false;

        // Stats
        f32 m_FrameTime = 0.0f;
        f32 m_FPS = 0.0f;
    };
} // namespace OloEngine
