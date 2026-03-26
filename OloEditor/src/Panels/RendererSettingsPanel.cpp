#include "OloEnginePCH.h"
#include "RendererSettingsPanel.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

namespace OloEngine
{
    void RendererSettingsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Renderer Settings", p_open);

        DrawQualityTieringSection();
        DrawRenderingPathSection();
        DrawCullingSection();
        DrawForwardPlusSection();
        DrawDebugSection();

        ImGui::End();
    }

    void RendererSettingsPanel::DrawQualityTieringSection()
    {
        auto project = Project::GetActive();
        if (!project)
        {
            return;
        }

        auto& qt = project->GetConfig().QualityTiering;

        if (ImGui::CollapsingHeader("Quality Preset", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            // Preset dropdown
            static const char* presetItems[] = { "Low", "Medium", "High", "Ultra", "Custom" };
            int currentPreset = static_cast<int>(qt.Preset);
            if (ImGui::Combo("Preset", &currentPreset, presetItems, IM_ARRAYSIZE(presetItems)))
            {
                auto selected = static_cast<QualityPreset>(currentPreset);
                if (selected != QualityPreset::Custom)
                {
                    qt = GetPresetSettings(selected);
                    ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
                    ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadowCopy);
                    Renderer3D::GetShadowMap().SetSettings(shadowCopy);
                }
                else
                {
                    qt.Preset = QualityPreset::Custom;
                }
            }

            ImGui::Separator();

            // Individual overrides — any change marks Custom
            bool changed = false;

            // Shadow
            ImGui::TextDisabled("Shadows");
            changed |= ImGui::Checkbox("Shadow Enabled##qt", &qt.ShadowEnabled);

            int shadowRes = static_cast<int>(qt.ShadowResolution);
            static const char* shadowResItems[] = { "512", "1024", "2048", "4096" };
            static const int shadowResValues[] = { 512, 1024, 2048, 4096 };
            int shadowResIdx = 3;
            for (int i = 0; i < 4; ++i)
            {
                if (shadowResValues[i] == shadowRes)
                {
                    shadowResIdx = i;
                    break;
                }
            }
            if (ImGui::Combo("Shadow Resolution##qt", &shadowResIdx, shadowResItems, IM_ARRAYSIZE(shadowResItems)))
            {
                qt.ShadowResolution = static_cast<u32>(shadowResValues[shadowResIdx]);
                changed = true;
            }
            changed |= ImGui::SliderFloat("Shadow Softness##qt", &qt.ShadowSoftness, 0.0f, 2.0f);

            // AO
            ImGui::Spacing();
            ImGui::TextDisabled("Ambient Occlusion");
            static const char* aoItems[] = { "None", "SSAO", "GTAO" };
            int aoIdx = std::clamp(static_cast<int>(qt.AO), 0, 2);
            if (ImGui::Combo("AO Technique##qt", &aoIdx, aoItems, IM_ARRAYSIZE(aoItems)))
            {
                qt.AO = static_cast<AOTechnique>(aoIdx);
                changed = true;
            }
            if (qt.AO == AOTechnique::SSAO)
            {
                changed |= ImGui::SliderInt("SSAO Samples##qt", &qt.SSAOSamples, 8, 64);
            }
            if (qt.AO == AOTechnique::GTAO)
            {
                changed |= ImGui::SliderFloat("GTAO Radius##qt", &qt.GTAORadius, 0.1f, 2.0f);
                changed |= ImGui::SliderInt("GTAO Denoise Passes##qt", &qt.GTAODenoisePasses, 1, 8);
            }

            // Post-process toggles
            ImGui::Spacing();
            ImGui::TextDisabled("Post-Processing");
            changed |= ImGui::Checkbox("Bloom##qt", &qt.BloomEnabled);
            if (qt.BloomEnabled)
            {
                changed |= ImGui::SliderInt("Bloom Iterations##qt", &qt.BloomIterations, 1, 10);
            }
            changed |= ImGui::Checkbox("FXAA##qt", &qt.FXAAEnabled);
            changed |= ImGui::Checkbox("Depth of Field##qt", &qt.DOFEnabled);
            changed |= ImGui::Checkbox("Motion Blur##qt", &qt.MotionBlurEnabled);
            changed |= ImGui::Checkbox("Vignette##qt", &qt.VignetteEnabled);
            changed |= ImGui::Checkbox("Chromatic Aberration##qt", &qt.ChromaticAberrationEnabled);

            if (changed)
            {
                qt.Preset = QualityPreset::Custom;
                ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
                ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadowCopy);
                Renderer3D::GetShadowMap().SetSettings(shadowCopy);
            }

            // Reset button when Custom
            if (qt.Preset == QualityPreset::Custom)
            {
                ImGui::Spacing();
                if (ImGui::Button("Reset to High Preset"))
                {
                    qt = GetPresetSettings(QualityPreset::High);
                    ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
                    ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadowCopy);
                    Renderer3D::GetShadowMap().SetSettings(shadowCopy);
                }
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawRenderingPathSection()
    {
        auto& settings = Renderer3D::GetRendererSettings();

        if (ImGui::CollapsingHeader("Rendering Path", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            static const char* pathItems[] = {
                "Forward",
                "Forward+"
            };
            int currentPath = static_cast<int>(settings.Path);
            if (ImGui::Combo("Active Path", &currentPath, pathItems, IM_ARRAYSIZE(pathItems)))
            {
                settings.Path = static_cast<RenderingPath>(currentPath);
                Renderer3D::ApplyRendererSettings();
            }

            if (settings.Path == RenderingPath::Forward)
            {
                if (ImGui::Checkbox("Auto-upgrade to Forward+ at light threshold",
                                    &settings.ForwardPlusAutoSwitch))
                {
                    Renderer3D::ApplyRendererSettings();
                }

                if (settings.ForwardPlusAutoSwitch)
                {
                    int threshold = static_cast<int>(settings.ForwardPlusLightThreshold);
                    if (ImGui::SliderInt("Light Threshold", &threshold, 1, 64))
                    {
                        settings.ForwardPlusLightThreshold = static_cast<u32>(threshold);
                        Renderer3D::ApplyRendererSettings();
                    }
                    ImGui::TextDisabled("Switch to Forward+ when point+spot lights exceed this.");
                }
            }

            // Show active path status
            ImGui::Separator();
            auto& fplus = Renderer3D::GetForwardPlus();
            if (fplus.IsActive())
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Active: Forward+");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Active: Forward");
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawCullingSection()
    {
        auto& settings = Renderer3D::GetRendererSettings();
        const bool forwardPlusForced = (settings.Path == RenderingPath::ForwardPlus);

        if (ImGui::CollapsingHeader("Culling & Optimization"))
        {
            ImGui::Indent();

            if (ImGui::Checkbox("Frustum Culling", &settings.FrustumCullingEnabled))
            {
                Renderer3D::ApplyRendererSettings();
            }

            if (ImGui::Checkbox("Occlusion Culling", &settings.OcclusionCullingEnabled))
            {
                Renderer3D::ApplyRendererSettings();
            }

            // Depth pre-pass is forced on when Forward+ is selected
            if (forwardPlusForced)
            {
                ImGui::BeginDisabled();
                bool forced = true;
                ImGui::Checkbox("Depth Pre-pass", &forced);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Automatically enabled by Forward+ (required for compute culling).");
            }
            else
            {
                if (ImGui::Checkbox("Depth Pre-pass", &settings.DepthPrepassEnabled))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                ImGui::TextDisabled("Required for Forward+ light culling and occlusion queries.");
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawForwardPlusSection()
    {
        auto& settings = Renderer3D::GetRendererSettings();
        auto& fplus = Renderer3D::GetForwardPlus();

        if (ImGui::CollapsingHeader("Forward+ Settings"))
        {
            ImGui::Indent();

            // Tile size
            static const char* tileSizeItems[] = { "8", "16", "32" };
            static const u32 tileSizeValues[] = { 8, 16, 32 };
            int tileSizeIdx = 1;
            for (int i = 0; i < 3; ++i)
            {
                if (tileSizeValues[i] == settings.ForwardPlusTileSize)
                {
                    tileSizeIdx = i;
                    break;
                }
            }
            if (ImGui::Combo("Tile Size (px)", &tileSizeIdx, tileSizeItems, IM_ARRAYSIZE(tileSizeItems)))
            {
                settings.ForwardPlusTileSize = tileSizeValues[tileSizeIdx];
                Renderer3D::ApplyRendererSettings();
            }
            if (settings.ForwardPlusTileSize != 16)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Compute shader workgroup is 16x16; other tile sizes partially sample depth.");
            }

            if (ImGui::Checkbox("Debug Heatmap Overlay", &settings.ForwardPlusDebugHeatmap))
            {
                Renderer3D::ApplyRendererSettings();
            }
            if (settings.ForwardPlusDebugHeatmap && !fplus.IsActive())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Heatmap requires active Forward+ (add point/spot lights).");
            }

            // Runtime stats
            if (fplus.IsInitialized())
            {
                ImGui::Separator();
                ImGui::Text("Point Lights: %u", fplus.GetPointLightCount());
                ImGui::Text("Spot Lights:  %u", fplus.GetSpotLightCount());
                ImGui::Text("Grid:         %ux%u tiles", fplus.GetTileCountX(), fplus.GetTileCountY());
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawDebugSection()
    {
        auto& settings = Renderer3D::GetRendererSettings();

        if (ImGui::CollapsingHeader("Debug Overlays"))
        {
            ImGui::Indent();

            if (ImGui::Checkbox("Wireframe Overlay", &settings.WireframeOverlay))
            {
                Renderer3D::ApplyRendererSettings();
            }

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
