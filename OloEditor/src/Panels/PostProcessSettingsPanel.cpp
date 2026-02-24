#include "OloEnginePCH.h"
#include "PostProcessSettingsPanel.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

namespace OloEngine
{
    void PostProcessSettingsPanel::OnImGuiRender()
    {
        ImGui::Begin("Post Processing");

        auto& settings = Renderer3D::GetPostProcessSettings();

        DrawToneMappingSection();
        DrawSSAOSection();
        DrawBloomSection();
        DrawVignetteSection();
        DrawChromaticAberrationSection();
        DrawColorGradingSection();
        DrawFXAASection();
        DrawDOFSection();
        DrawMotionBlurSection();

        ImGui::End();
    }

    void PostProcessSettingsPanel::DrawToneMappingSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            const char* tonemapItems[] = { "None", "Reinhard", "ACES", "Uncharted2" };
            int currentTonemap = static_cast<int>(settings.Tonemap);
            if (ImGui::Combo("Operator", &currentTonemap, tonemapItems, IM_ARRAYSIZE(tonemapItems)))
            {
                settings.Tonemap = static_cast<TonemapOperator>(currentTonemap);
            }

            ImGui::DragFloat("Exposure", &settings.Exposure, 0.01f, 0.0f, 20.0f, "%.2f");
            ImGui::DragFloat("Gamma", &settings.Gamma, 0.01f, 0.1f, 5.0f, "%.2f");

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawBloomSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Bloom"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##Bloom", &settings.BloomEnabled);

            if (settings.BloomEnabled)
            {
                ImGui::DragFloat("Threshold", &settings.BloomThreshold, 0.01f, 0.0f, 10.0f, "%.2f");
                ImGui::DragFloat("Intensity##Bloom", &settings.BloomIntensity, 0.01f, 0.0f, 5.0f, "%.2f");
                ImGui::SliderInt("Iterations", &settings.BloomIterations, 1, 8);
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawVignetteSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Vignette"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##Vignette", &settings.VignetteEnabled);

            if (settings.VignetteEnabled)
            {
                ImGui::DragFloat("Intensity##Vignette", &settings.VignetteIntensity, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::DragFloat("Smoothness", &settings.VignetteSmoothness, 0.01f, 0.0f, 2.0f, "%.2f");
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawChromaticAberrationSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Chromatic Aberration"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##ChromAb", &settings.ChromaticAberrationEnabled);

            if (settings.ChromaticAberrationEnabled)
            {
                ImGui::DragFloat("Intensity##ChromAb", &settings.ChromaticAberrationIntensity, 0.001f, 0.0f, 0.1f, "%.3f");
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawColorGradingSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Color Grading"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##ColorGrading", &settings.ColorGradingEnabled);

            if (settings.ColorGradingEnabled)
            {
                ImGui::TextDisabled("LUT texture selection coming soon");
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawFXAASection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("FXAA"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##FXAA", &settings.FXAAEnabled);

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawDOFSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Depth of Field"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##DOF", &settings.DOFEnabled);

            if (settings.DOFEnabled)
            {
                ImGui::DragFloat("Focus Distance", &settings.DOFFocusDistance, 0.1f, 0.0f, 1000.0f, "%.1f");
                ImGui::DragFloat("Focus Range", &settings.DOFFocusRange, 0.1f, 0.0f, 100.0f, "%.1f");
                ImGui::DragFloat("Bokeh Radius", &settings.DOFBokehRadius, 0.1f, 0.0f, 20.0f, "%.1f");
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawMotionBlurSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("Motion Blur"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##MotionBlur", &settings.MotionBlurEnabled);

            if (settings.MotionBlurEnabled)
            {
                ImGui::DragFloat("Strength", &settings.MotionBlurStrength, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::SliderInt("Samples", &settings.MotionBlurSamples, 1, 32);
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawSSAOSection()
    {
        auto& settings = Renderer3D::GetPostProcessSettings();

        if (ImGui::CollapsingHeader("SSAO"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##SSAO", &settings.SSAOEnabled);

            if (settings.SSAOEnabled)
            {
                ImGui::DragFloat("Radius##SSAO", &settings.SSAORadius, 0.01f, 0.01f, 5.0f, "%.2f");
                ImGui::DragFloat("Bias##SSAO", &settings.SSAOBias, 0.001f, 0.0f, 0.1f, "%.3f");
                ImGui::DragFloat("Intensity##SSAO", &settings.SSAOIntensity, 0.01f, 0.0f, 3.0f, "%.2f");
                ImGui::SliderInt("Samples##SSAO", &settings.SSAOSamples, 4, 64);
                ImGui::Checkbox("Show AO Only##SSAO", &settings.SSAODebugView);
            }

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
