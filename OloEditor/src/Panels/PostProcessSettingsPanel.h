#pragma once

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine
{
    class PostProcessSettingsPanel
    {
      public:
        PostProcessSettingsPanel() = default;
        ~PostProcessSettingsPanel() = default;

        void OnImGuiRender();

      private:
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
    };
} // namespace OloEngine
