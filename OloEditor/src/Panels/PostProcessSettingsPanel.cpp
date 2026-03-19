#include "OloEnginePCH.h"
#include "PostProcessSettingsPanel.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "../UndoRedo/SpecializedCommands.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    void PostProcessSettingsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Post Processing", p_open);

        // Snapshot all renderer settings before any UI for undo tracking
        if (m_CommandHistory && !m_IsEditing)
        {
            m_Snapshot = PostProcessFullSnapshot::Capture();
        }

        DrawToneMappingSection();
        DrawSSAOSection();
        DrawSnowSection();
        DrawWindSection();
        DrawSnowAccumulationSection();
        DrawSnowEjectaSection();
        DrawPrecipitationSection();
        DrawFogSection();
        DrawBloomSection();
        DrawVignetteSection();
        DrawChromaticAberrationSection();
        DrawColorGradingSection();
        DrawFXAASection();
        DrawDOFSection();
        DrawMotionBlurSection();

        // Forward+ controls moved to RendererSettingsPanel

        // Undo tracking: detect changes and push command when editing ends
        if (m_CommandHistory && m_Snapshot)
        {
            auto current = PostProcessFullSnapshot::Capture();
            const bool changed = (*m_Snapshot != current);

            if (changed && !m_IsEditing)
            {
                m_IsEditing = true;
            }

            if (m_IsEditing && ::GImGui->ActiveId == 0)
            {
                if (changed)
                {
                    m_CommandHistory->PushAlreadyExecuted(
                        std::make_unique<PostProcessFullChangeCommand>(*m_Snapshot, current));
                }
                m_IsEditing = false;
            }
        }

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
        OLO_PROFILE_FUNCTION();

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
                if (ImGui::DragFloat3("Direction##Wind", &settings.Direction.x, 0.01f, -1.0f, 1.0f, "%.2f"))
                {
                    // Normalize direction only when the user actually edits it
                    float len = glm::length(settings.Direction);
                    if (len > 0.001f)
                    {
                        settings.Direction /= len;
                    }
                    else
                    {
                        settings.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
                    }
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
                static constexpr u32 resValues[] = { 64, 128 };
                if (settings.GridResolution != resValues[0] && settings.GridResolution != resValues[1])
                {
                    settings.GridResolution = (settings.GridResolution <= 64) ? resValues[0] : resValues[1];
                }
                const char* resItems[] = { "64", "128" };
                int current = (settings.GridResolution <= 64) ? 0 : 1;
                if (ImGui::Combo("Resolution##Grid", &current, resItems, IM_ARRAYSIZE(resItems)))
                {
                    settings.GridResolution = resValues[current];
                }
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawSnowAccumulationSection()
    {
        OLO_PROFILE_FUNCTION();

        auto& settings = Renderer3D::GetSnowAccumulationSettings();

        if (ImGui::CollapsingHeader("Snow Accumulation"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##SnowAccum", &settings.Enabled);

            if (settings.Enabled)
            {
                ImGui::SeparatorText("Accumulation");
                ImGui::DragFloat("Rate (m/s)##SnowAccum", &settings.AccumulationRate, 0.001f, 0.0f, 1.0f, "%.3f");
                ImGui::DragFloat("Max Depth (m)##SnowAccum", &settings.MaxDepth, 0.01f, 0.01f, 5.0f, "%.2f");
                ImGui::DragFloat("Melt Rate (m/s)##SnowAccum", &settings.MeltRate, 0.001f, 0.0f, 1.0f, "%.3f");
                ImGui::DragFloat("Restoration Rate (m/s)##SnowAccum", &settings.RestorationRate, 0.001f, 0.0f, 1.0f, "%.3f");

                ImGui::SeparatorText("Displacement");
                ImGui::DragFloat("Scale##SnowDisp", &settings.DisplacementScale, 0.01f, 0.0f, 5.0f, "%.2f");
                ImGui::DragFloat("Density##SnowDens", &settings.SnowDensity, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = light powder, 1 = packed ice");

                ImGui::SeparatorText("Clipmap");
                ImGui::DragFloat("Extent (m)##SnowClip", &settings.ClipmapExtent, 1.0f, 1.0f, 500.0f, "%.0f");
                int rings = static_cast<int>(settings.NumClipmapRings);
                if (ImGui::SliderInt("Rings##SnowClip", &rings, 1, 3))
                {
                    settings.NumClipmapRings = static_cast<u32>(rings);
                }

                if (ImGui::Button("Reset Snow Depth##SnowAccum"))
                {
                    SnowAccumulationSystem::Reset();
                }
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawSnowEjectaSection()
    {
        OLO_PROFILE_FUNCTION();

        auto& settings = Renderer3D::GetSnowEjectaSettings();

        if (ImGui::CollapsingHeader("Snow Ejecta"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##SnowEjecta", &settings.Enabled);

            if (settings.Enabled)
            {
                ImGui::SeparatorText("Emission");
                int ppd = static_cast<int>(settings.ParticlesPerDeform);
                if (ImGui::DragInt("Particles/Stamp##Ejecta", &ppd, 1, 1, 128))
                {
                    settings.ParticlesPerDeform = static_cast<u32>(std::clamp(ppd, 1, 128));
                }
                ImGui::DragFloat("Speed (m/s)##Ejecta", &settings.EjectaSpeed, 0.1f, 0.0f, 50.0f, "%.1f");
                ImGui::DragFloat("Speed Variance##Ejecta", &settings.SpeedVariance, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Upward Bias##Ejecta", &settings.UpwardBias, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fraction of velocity directed upward vs outward");
                ImGui::DragFloat("Velocity Threshold##Ejecta", &settings.VelocityThreshold, 0.01f, 0.0f, 10.0f, "%.2f");

                ImGui::SeparatorText("Particle Properties");
                ImGui::DragFloat("Lifetime Min##Ejecta", &settings.LifetimeMin, 0.01f, 0.01f, 10.0f, "%.2f");
                ImGui::DragFloat("Lifetime Max##Ejecta", &settings.LifetimeMax, 0.01f, 0.01f, 10.0f, "%.2f");
                if (settings.LifetimeMax < settings.LifetimeMin)
                {
                    settings.LifetimeMax = settings.LifetimeMin;
                }
                ImGui::DragFloat("Initial Size##Ejecta", &settings.InitialSize, 0.001f, 0.001f, 1.0f, "%.3f");
                ImGui::DragFloat("Size Variance##Ejecta", &settings.SizeVariance, 0.001f, 0.0f, 0.5f, "%.3f");
                ImGui::ColorEdit4("Color##Ejecta", &settings.Color.x);

                ImGui::SeparatorText("Physics");
                ImGui::DragFloat("Gravity Scale##Ejecta", &settings.GravityScale, 0.01f, 0.0f, 5.0f, "%.2f");
                ImGui::DragFloat("Drag##Ejecta", &settings.DragCoefficient, 0.1f, 0.0f, 20.0f, "%.1f");

                ImGui::SeparatorText("Advanced Simulation");
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Modify with care — may cause visual artifacts");
                ImGui::DragFloat("Wind Influence##Ejecta", &settings.WindInfluence, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How strongly wind affects snow puffs (0=none, 1=full)");
                ImGui::DragFloat("Noise Strength##Ejecta", &settings.NoiseStrength, 0.01f, 0.0f, 5.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Turbulence intensity for organic motion");
                ImGui::DragFloat("Noise Frequency##Ejecta", &settings.NoiseFrequency, 0.1f, 0.0f, 20.0f, "%.1f");
                ImGui::DragFloat("Ground Y##Ejecta", &settings.GroundY, 0.1f, -1000.0f, 1000.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Y height of the ground plane for particle collision");
                ImGui::DragFloat("Collision Bounce##Ejecta", &settings.CollisionBounce, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Collision Friction##Ejecta", &settings.CollisionFriction, 0.01f, 0.0f, 1.0f, "%.2f");

                if (ImGui::Button("Reset Ejecta##SnowEjecta"))
                {
                    SnowEjectaSystem::Reset();
                }
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawPrecipitationSection()
    {
        OLO_PROFILE_FUNCTION();

        auto& settings = Renderer3D::GetPrecipitationSettings();

        if (ImGui::CollapsingHeader("Precipitation"))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enable##Precipitation", &settings.Enabled);

            // Type selector
            constexpr std::array typeNames = { "Snow", "Rain", "Hail", "Sleet" };
            const int typeCount = static_cast<int>(typeNames.size());

            // Normalize Type into valid range before any use
            settings.Type = static_cast<PrecipitationType>(std::clamp(static_cast<int>(settings.Type), 0, typeCount - 1));

            int typeIdx = static_cast<int>(settings.Type);
            if (ImGui::Combo("Type##Precip", &typeIdx, typeNames.data(), typeCount))
            {
                settings.Type = static_cast<PrecipitationType>(std::clamp(typeIdx, 0, typeCount - 1));
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply Defaults##Precip"))
            {
                bool wasEnabled = settings.Enabled;
                f32 prevIntensity = settings.Intensity;
                settings = PrecipitationSettings::GetDefaultsForType(settings.Type);
                settings.Enabled = wasEnabled;
                settings.Intensity = prevIntensity;
                PrecipitationSystem::Reset();
                ScreenSpacePrecipitation::Reset();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset parameters to defaults for this type (preserving Enabled and Intensity)");

            if (settings.Enabled)
            {
                // Intensity slider with preset buttons
                ImGui::SeparatorText("Intensity");
                ImGui::SliderFloat("Intensity##Precip", &settings.Intensity, 0.0f, 1.0f, "%.2f");
                if (ImGui::Button("Light##Precip"))
                    settings.Intensity = 0.15f;
                ImGui::SameLine();
                if (ImGui::Button("Moderate##Precip"))
                    settings.Intensity = 0.4f;
                ImGui::SameLine();
                if (ImGui::Button("Heavy##Precip"))
                    settings.Intensity = 0.7f;
                ImGui::SameLine();
                if (ImGui::Button("Blizzard##Precip"))
                    settings.Intensity = 1.0f;
                ImGui::DragFloat("Transition Speed##Precip", &settings.TransitionSpeed, 0.01f, 0.01f, 10.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How quickly intensity ramps to target (higher = faster)");

                ImGui::SeparatorText("Emission");
                settings.BaseEmissionRate = static_cast<u32>(std::clamp(static_cast<int>(settings.BaseEmissionRate), 100, 50000));
                int baseRate = static_cast<int>(settings.BaseEmissionRate);
                if (ImGui::DragInt("Base Rate##Precip", &baseRate, 50, 100, 50000))
                {
                    settings.BaseEmissionRate = static_cast<u32>(std::clamp(baseRate, 100, 50000));
                }

                ImGui::SeparatorText("Near Field");
                ImGui::DragFloat3("Extent##NearPrecip", &settings.NearFieldExtent.x, 0.5f, 1.0f, 100.0f, "%.1f");
                ImGui::DragFloat("Particle Size##NearPrecip", &settings.NearFieldParticleSize, 0.001f, 0.001f, 0.5f, "%.3f");
                ImGui::DragFloat("Size Variance##NearPrecip", &settings.NearFieldSizeVariance, 0.001f, 0.0f, 0.1f, "%.3f");
                ImGui::DragFloat("Speed Min##NearPrecip", &settings.NearFieldSpeedMin, 0.1f, 0.0f, 20.0f, "%.1f");
                ImGui::DragFloat("Speed Max##NearPrecip", &settings.NearFieldSpeedMax, 0.1f, 0.0f, 20.0f, "%.1f");
                settings.NearFieldSpeedMax = std::max(settings.NearFieldSpeedMax, settings.NearFieldSpeedMin);
                ImGui::DragFloat("Lifetime##NearPrecip", &settings.NearFieldLifetime, 0.1f, 0.1f, 30.0f, "%.1f");

                ImGui::SeparatorText("Far Field");
                ImGui::DragFloat3("Extent##FarPrecip", &settings.FarFieldExtent.x, 1.0f, 10.0f, 500.0f, "%.0f");
                ImGui::DragFloat("Particle Size##FarPrecip", &settings.FarFieldParticleSize, 0.001f, 0.001f, 0.3f, "%.3f");
                ImGui::DragFloat("Speed Min##FarPrecip", &settings.FarFieldSpeedMin, 0.1f, 0.0f, 15.0f, "%.1f");
                ImGui::DragFloat("Speed Max##FarPrecip", &settings.FarFieldSpeedMax, 0.1f, 0.0f, 15.0f, "%.1f");
                settings.FarFieldSpeedMax = std::max(settings.FarFieldSpeedMax, settings.FarFieldSpeedMin);
                ImGui::DragFloat("Lifetime##FarPrecip", &settings.FarFieldLifetime, 0.1f, 0.1f, 60.0f, "%.1f");
                ImGui::DragFloat("Alpha Multiplier##FarPrecip", &settings.FarFieldAlphaMultiplier, 0.01f, 0.0f, 1.0f, "%.2f");

                ImGui::SeparatorText("Physics");
                ImGui::DragFloat("Gravity Scale##Precip", &settings.GravityScale, 0.01f, 0.0f, 5.0f, "%.2f");
                ImGui::DragFloat("Wind Influence##Precip", &settings.WindInfluence, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::DragFloat("Drag##Precip", &settings.DragCoefficient, 0.1f, 0.0f, 10.0f, "%.1f");
                ImGui::DragFloat("Turbulence##Precip", &settings.TurbulenceStrength, 0.01f, 0.0f, 5.0f, "%.2f");
                ImGui::DragFloat("Turbulence Freq##Precip", &settings.TurbulenceFrequency, 0.1f, 0.0f, 20.0f, "%.1f");

                ImGui::SeparatorText("Ground Interaction");
                ImGui::Checkbox("Ground Collision##Precip", &settings.GroundCollisionEnabled);
                ImGui::DragFloat("Ground Y##Precip", &settings.GroundY, 0.1f, -1000.0f, 1000.0f, "%.1f");
                ImGui::DragFloat("Bounce##Precip", &settings.CollisionBounce, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = stick/splash, >0 = bounce (hail)");
                ImGui::DragFloat("Friction##Precip", &settings.CollisionFriction, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Feed Accumulation##Precip", &settings.FeedAccumulation);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Landed particles contribute to snow depth clipmap (Snow/Sleet only)");
                ImGui::DragFloat("Feed Rate##Precip", &settings.AccumulationFeedRate, 0.0001f, 0.0f, 1.0f, "%.4f");

                ImGui::SeparatorText("Screen Effects");
                ImGui::Checkbox("Screen Streaks##Precip", &settings.ScreenStreaksEnabled);
                ImGui::DragFloat("Streak Intensity##Precip", &settings.ScreenStreakIntensity, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Streak Length##Precip", &settings.ScreenStreakLength, 0.01f, 0.0f, 2.0f, "%.2f");
                ImGui::Checkbox("Lens Impacts##Precip", &settings.LensImpactsEnabled);
                ImGui::DragFloat("Impact Rate##Precip", &settings.LensImpactRate, 0.1f, 0.0f, 20.0f, "%.1f");
                ImGui::DragFloat("Impact Lifetime##Precip", &settings.LensImpactLifetime, 0.1f, 0.1f, 10.0f, "%.1f");
                ImGui::DragFloat("Impact Size##Precip", &settings.LensImpactSize, 0.001f, 0.001f, 0.3f, "%.3f");

                ImGui::SeparatorText("LOD & Budget");
                ImGui::DragFloat("LOD Near##Precip", &settings.LODNearDistance, 1.0f, 1.0f, 100.0f, "%.0f");
                ImGui::DragFloat("LOD Far##Precip", &settings.LODFarDistance, 1.0f, 10.0f, 500.0f, "%.0f");
                settings.LODFarDistance = std::max(settings.LODFarDistance, settings.LODNearDistance + 1.0f);
                ImGui::DragFloat("Frame Budget (ms)##Precip", &settings.FrameBudgetMs, 0.1f, 0.1f, 5.0f, "%.1f");

                ImGui::SeparatorText("Visual");
                ImGui::ColorEdit4("Particle Color##Precip", &settings.ParticleColor.x);
                ImGui::DragFloat("Color Variance##Precip", &settings.ColorVariance, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloat("Rotation Speed##Precip", &settings.RotationSpeed, 0.1f, 0.0f, 10.0f, "%.1f");

                ImGui::SeparatorText("Statistics");
                auto stats = PrecipitationSystem::GetStatistics();
                ImGui::Text("Effective Emission Rate: %.0f", stats.EffectiveEmissionRate);
                ImGui::Text("GPU Time: %.2f ms", stats.GPUTimeMs);
                ImGui::Text("Current Intensity: %.2f", PrecipitationSystem::GetCurrentIntensity());
                ImGui::Text("Active Lens Impacts: %u", ScreenSpacePrecipitation::GetActiveLensImpactCount());

                if (ImGui::Button("Reset Precipitation##Precip"))
                {
                    PrecipitationSystem::Reset();
                    ScreenSpacePrecipitation::Reset();
                }
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawFogSection()
    {
        if (ImGui::CollapsingHeader("Fog & Atmosphere"))
        {
            ImGui::Indent();

            auto& fog = Renderer3D::GetFogSettings();

            ImGui::Checkbox("Enable Fog", &fog.Enabled);

            if (fog.Enabled)
            {
                // Fog mode dropdown
                const char* fogModes[] = { "Linear", "Exponential", "Exponential\xc2\xb2" };
                int currentMode = static_cast<int>(fog.Mode);
                if (ImGui::Combo("Fog Mode", &currentMode, fogModes, IM_ARRAYSIZE(fogModes)))
                {
                    fog.Mode = static_cast<FogMode>(currentMode);
                }

                ImGui::ColorEdit3("Fog Color", glm::value_ptr(fog.Color));

                if (fog.Mode == FogMode::Linear)
                {
                    ImGui::DragFloat("Start Distance", &fog.Start, 1.0f, 0.0f, 5000.0f, "%.1f");
                    ImGui::DragFloat("End Distance", &fog.End, 1.0f, 0.0f, 10000.0f, "%.1f");
                    if (fog.End < fog.Start)
                    {
                        fog.End = fog.Start + 1.0f;
                    }
                }
                else
                {
                    ImGui::SliderFloat("Density", &fog.Density, 0.0f, 0.5f, "%.4f");
                }

                ImGui::Separator();
                ImGui::Text("Height Fog");
                ImGui::SliderFloat("Height Falloff", &fog.HeightFalloff, 0.001f, 1.0f, "%.3f");
                ImGui::DragFloat("Height Offset", &fog.HeightOffset, 0.5f, -500.0f, 500.0f, "%.1f");

                ImGui::Separator();
                ImGui::SliderFloat("Max Opacity", &fog.MaxOpacity, 0.0f, 1.0f, "%.2f");

                // Atmospheric scattering sub-section
                ImGui::Separator();
                DrawAtmosphericScatteringSection(fog);

                // Volumetric fog sub-section
                ImGui::Separator();
                DrawVolumetricFogSection(fog);
            }

            ImGui::Unindent();
        }
    }

    void PostProcessSettingsPanel::DrawAtmosphericScatteringSection(FogSettings& fog)
    {
        if (ImGui::TreeNode("Atmospheric Scattering"))
        {
            ImGui::Checkbox("Enable Scattering", &fog.EnableScattering);

            if (fog.EnableScattering)
            {
                ImGui::ColorEdit3("Rayleigh Color", glm::value_ptr(fog.RayleighColor));
                ImGui::SliderFloat("Rayleigh Strength", &fog.RayleighStrength, 0.0f, 5.0f, "%.2f");
                ImGui::SliderFloat("Mie Strength", &fog.MieStrength, 0.0f, 0.1f, "%.4f");
                ImGui::SliderFloat("Mie Directionality", &fog.MieDirectionality, 0.0f, 0.99f, "%.2f");
                ImGui::SliderFloat("Sun Intensity", &fog.SunIntensity, 0.0f, 100.0f, "%.1f");
            }

            ImGui::TreePop();
        }
    }

    void PostProcessSettingsPanel::DrawVolumetricFogSection(FogSettings& fog)
    {
        if (ImGui::TreeNode("Volumetric Fog"))
        {
            ImGui::Checkbox("Enable Volumetric", &fog.EnableVolumetric);

            if (fog.EnableVolumetric)
            {
                ImGui::SliderInt("Ray-March Samples", &fog.VolumetricSamples, 4, 128);
                ImGui::SliderFloat("Absorption", &fog.AbsorptionCoefficient, 0.0f, 0.2f, "%.4f");

                ImGui::Separator();
                ImGui::Checkbox("Enable Noise", &fog.EnableNoise);
                if (fog.EnableNoise)
                {
                    ImGui::SliderFloat("Noise Scale", &fog.NoiseScale, 0.001f, 0.1f, "%.4f");
                    ImGui::SliderFloat("Noise Speed", &fog.NoiseSpeed, 0.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat("Noise Intensity", &fog.NoiseIntensity, 0.0f, 1.0f, "%.2f");
                }

                ImGui::Separator();
                ImGui::Checkbox("Enable Light Shafts", &fog.EnableLightShafts);
                if (fog.EnableLightShafts)
                {
                    ImGui::SliderFloat("Light Shaft Intensity", &fog.LightShaftIntensity, 0.0f, 5.0f, "%.2f");
                }
            }

            ImGui::TreePop();
        }
    }
} // namespace OloEngine
