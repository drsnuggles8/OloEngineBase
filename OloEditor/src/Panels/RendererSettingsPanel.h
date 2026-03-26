#pragma once

namespace OloEngine
{
    // @brief Editor panel for global renderer settings:
    //        rendering path, culling, depth prepass, Forward+ tuning, debug overlays, quality tiering.
    class RendererSettingsPanel
    {
      public:
        RendererSettingsPanel() = default;
        ~RendererSettingsPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

      private:
        void DrawQualityTieringSection();
        void DrawRenderingPathSection();
        void DrawCullingSection();
        void DrawForwardPlusSection();
        void DrawDebugSection();
    };
} // namespace OloEngine
