#pragma once

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "../UndoRedo/SpecializedCommands.h"

namespace OloEngine
{
    class CommandHistory;

    class PostProcessSettingsPanel
    {
      public:
        PostProcessSettingsPanel() = default;
        ~PostProcessSettingsPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

      private:
        CommandHistory* m_CommandHistory = nullptr;
        bool m_IsEditing = false;
        std::optional<PostProcessFullSnapshot> m_Snapshot;
        void DrawToneMappingSection() const;
        void DrawBloomSection() const;
        void DrawVignetteSection() const;
        void DrawChromaticAberrationSection() const;
        void DrawColorGradingSection() const;
        void DrawFXAASection() const;
        void DrawTAASection() const;
        void DrawDOFSection() const;
        void DrawMotionBlurSection() const;
        void DrawAOSection() const;
        void DrawSnowSection() const;
        void DrawWindSection() const;
        void DrawSnowAccumulationSection() const;
        void DrawSnowEjectaSection() const;
        void DrawPrecipitationSection() const;
        void DrawFogSection() const;
        static void DrawAtmosphericScatteringSection(FogSettings& fog);
        static void DrawVolumetricFogSection(FogSettings& fog);
    };
} // namespace OloEngine
