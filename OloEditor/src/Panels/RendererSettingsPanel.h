#pragma once

#include "OloEngine/Renderer/QualityTiering.h"

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

        // Returns true (and resets the flag) when a debug overlay toggle
        // changed since the last call.  EditorLayer polls this to keep
        // m_Prefs in sync with the live RendererSettings.
        bool ConsumeDebugSettingsChanged()
        {
            if (m_DebugSettingsChanged)
            {
                m_DebugSettingsChanged = false;
                return true;
            }
            return false;
        }

      private:
        void DrawQualityTieringSection();
        void DrawRenderingPathSection();
        void DrawCullingSection();
        void DrawForwardPlusSection();
        void DrawTransparencySection();
        void DrawDebugSection();

        // Quality-tiering helpers
        static void ApplyQualityTieringToRuntime(const QualityTieringSettings& qt);
        static void DrawPresetControls(QualityTieringSettings& qt);
        static void DrawShadowControls(QualityTieringSettings& qt, bool& changed);
        static void DrawAOControls(QualityTieringSettings& qt, bool& changed);
        static void DrawPostProcessControls(QualityTieringSettings& qt, bool& changed);

        bool m_DebugSettingsChanged = false;
    };
} // namespace OloEngine
