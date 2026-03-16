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

        void OnImGuiRender();

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

      private:
        CommandHistory* m_CommandHistory = nullptr;
        bool m_IsEditing = false;
        std::optional<PostProcessFullSnapshot> m_Snapshot;
        void DrawToneMappingSection();
        void DrawBloomSection();
        void DrawVignetteSection();
        void DrawChromaticAberrationSection();
        void DrawColorGradingSection();
        void DrawFXAASection();
        void DrawDOFSection();
        void DrawMotionBlurSection();
        void DrawSSAOSection();
        void DrawSnowSection();
        void DrawWindSection();
        void DrawSnowAccumulationSection();
        void DrawSnowEjectaSection();
        void DrawPrecipitationSection();
        void DrawFogSection();
        static void DrawAtmosphericScatteringSection(FogSettings& fog);
        static void DrawVolumetricFogSection(FogSettings& fog);
    };
} // namespace OloEngine
