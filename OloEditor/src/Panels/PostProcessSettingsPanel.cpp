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
        DrawSnowSection();
        DrawWindSection();
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

    void PostProcessSettingsPanel::DrawSnowSection()
    {
        auto& settings = Renderer3D::GetSnowSettings();

        if (ImGui::CollapsingHeader("Snow"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##Snow", &settings.Enabled);

            if (settings.Enabled)
            {
                ImGui::SeparatorText("Coverage");
                ImGui::DragFloat("Height Start##Snow", &settings.HeightStart, 0.5f, -100.0f, 500.0f, "%.1f");
                ImGui::DragFloat("Height Full##Snow", &settings.HeightFull, 0.5f, -100.0f, 500.0f, "%.1f");
                ImGui::DragFloat("Slope Start##Snow", &settings.SlopeStart, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Slope Full##Snow", &settings.SlopeFull, 0.01f, 0.0f, 1.0f, "%.2f");
                // Enforce valid intervals
                if (settings.HeightFull < settings.HeightStart)
                {
                    settings.HeightFull = settings.HeightStart;
                }
                if (settings.SlopeFull > settings.SlopeStart)
                {
                    settings.SlopeFull = settings.SlopeStart;
                }

                ImGui::SeparatorText("Material");
                ImGui::ColorEdit3("Albedo##Snow", &settings.Albedo.x);
                ImGui::DragFloat("Roughness##Snow", &settings.Roughness, 0.01f, 0.0f, 1.0f, "%.2f");

                ImGui::SeparatorText("Subsurface Scattering");
                ImGui::ColorEdit3("SSS Color##Snow", &settings.SSSColor.x);
                ImGui::DragFloat("SSS Intensity##Snow", &settings.SSSIntensity, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::Checkbox("SSS Blur##Snow", &settings.SSSBlurEnabled);
                if (settings.SSSBlurEnabled)
                {
                    ImGui::DragFloat("Blur Radius##Snow", &settings.SSSBlurRadius, 0.1f, 0.5f, 10.0f, "%.1f");
                    ImGui::DragFloat("Blur Falloff##Snow", &settings.SSSBlurFalloff, 0.1f, 0.1f, 10.0f, "%.1f");
                }

                ImGui::SeparatorText("Sparkle");
                ImGui::DragFloat("Intensity##SnowSparkle", &settings.SparkleIntensity, 0.01f, 0.0f, 3.0f, "%.2f");
                ImGui::DragFloat("Density##SnowSparkle", &settings.SparkleDensity, 1.0f, 1.0f, 500.0f, "%.0f");
                ImGui::DragFloat("Scale##SnowSparkle", &settings.SparkleScale, 0.1f, 0.1f, 10.0f, "%.1f");

                ImGui::SeparatorText("Normal Detail");
                ImGui::DragFloat("Perturbation##Snow", &settings.NormalPerturbStrength, 0.01f, 0.0f, 1.0f, "%.2f");

                ImGui::SeparatorText("Wind Drift");
                ImGui::DragFloat("Drift Factor##Snow", &settings.WindDriftFactor, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How much wind direction affects snow coverage.\n"
                                      "Windward surfaces accumulate more snow, leeward less.");
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawWindSection()
    {
        OLO_PROFILE_FUNCTION();

        auto& settings = Renderer3D::GetWindSettings();

        if (ImGui::CollapsingHeader("Wind"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##Wind", &settings.Enabled);

            if (settings.Enabled)
            {
                ImGui::SeparatorText("Direction & Speed");
                ImGui::DragFloat3("Direction##Wind", &settings.Direction.x, 0.01f, -1.0f, 1.0f, "%.2f");
                // Auto-normalize direction if user changed it
                float len = glm::length(settings.Direction);
                if (len > 0.001f)
                {
                    settings.Direction /= len;
                }
                else
                {
                    settings.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
                }
                ImGui::DragFloat("Speed (m/s)##Wind", &settings.Speed, 0.1f, 0.0f, 50.0f, "%.1f");

                ImGui::SeparatorText("Gusts");
                ImGui::DragFloat("Strength##Gust", &settings.GustStrength, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Frequency (Hz)##Gust", &settings.GustFrequency, 0.01f, 0.01f, 5.0f, "%.2f");

                ImGui::SeparatorText("Turbulence");
                ImGui::DragFloat("Intensity##Turb", &settings.TurbulenceIntensity, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::DragFloat("Scale##Turb", &settings.TurbulenceScale, 0.005f, 0.01f, 1.0f, "%.3f");

                ImGui::SeparatorText("Grid");
                ImGui::DragFloat("World Size (m)##Grid", &settings.GridWorldSize, 1.0f, 10.0f, 1000.0f, "%.0f");

                // Grid resolution selector (power of 2)
                const char* resItems[] = { "64", "128", "256" };
                int current = (settings.GridResolution <= 64) ? 0 : (settings.GridResolution <= 128 ? 1 : 2);
                if (ImGui::Combo("Resolution##Grid", &current, resItems, IM_ARRAYSIZE(resItems)))
                {
                    static constexpr u32 resValues[] = { 64, 128, 256 };
                    settings.GridResolution = resValues[current];
                }
            }

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
