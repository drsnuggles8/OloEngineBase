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
        // The ADR 0011 §2 backend dropdown: writes config/renderer.yaml,
        // applies on the next editor launch (the runtime switch is
        // startup-scoped — no live device swap).
        void DrawBackendSection();
        void DrawQualityTieringSection() const;
        void DrawRenderingPathSection() const;
        void DrawCullingSection() const;
        void DrawForwardPlusSection() const;
        void DrawTransparencySection() const;
        void DrawDebugSection();

        // Quality-tiering helpers
        static void ApplyQualityTieringToRuntime(const QualityTieringSettings& qt);
        static void DrawPresetControls(QualityTieringSettings& qt);
        static void DrawShadowControls(QualityTieringSettings& qt, bool& changed);
        // Virtual Shadow Maps (issue #702). Separate from DrawShadowControls'
        // tiering knobs because it edits ShadowSettings directly: VSM's controls
        // are structural (pool size, world extents) rather than a quality dial,
        // and a preset that silently resized a 64 MB allocation would be a trap.
        // Also renders the page-draw counter, which is what makes the caching
        // claim observable rather than asserted.
        static void DrawVirtualShadowMapControls();
        // Hybrid ray-traced shadows (issue #1056): the technique switch, its
        // tuning, and — the part that matters when it does not appear to work —
        // the per-reason fallback counters.
        static void DrawRayTracedShadowControls();
        static void DrawAOControls(QualityTieringSettings& qt, bool& changed);
        static void DrawPostProcessControls(QualityTieringSettings& qt, bool& changed);

        bool m_DebugSettingsChanged = false;

        // Backend-dropdown state: the choice written to config/renderer.yaml
        // this session (or read from it at first draw). -1 = not yet
        // initialised; 0 = OpenGL, 1 = Vulkan (combo order).
        int m_ConfiguredBackend = -1;
    };
} // namespace OloEngine
