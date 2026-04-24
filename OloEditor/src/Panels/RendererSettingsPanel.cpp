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

    void RendererSettingsPanel::ApplyQualityTieringToRuntime(const QualityTieringSettings& qt)
    {
        ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
        ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadowCopy);
        Renderer3D::GetShadowMap().SetSettings(shadowCopy);
    }

    void RendererSettingsPanel::DrawPresetControls(QualityTieringSettings& qt)
    {
        static const char* presetItems[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        int currentPreset = static_cast<int>(qt.Preset);
        if (ImGui::Combo("Preset", &currentPreset, presetItems, IM_ARRAYSIZE(presetItems)))
        {
            auto selected = static_cast<QualityPreset>(currentPreset);
            if (selected != QualityPreset::Custom)
            {
                qt = GetPresetSettings(selected);
                ApplyQualityTieringToRuntime(qt);
            }
            else
            {
                qt.Preset = QualityPreset::Custom;
            }
        }
    }

    void RendererSettingsPanel::DrawShadowControls(QualityTieringSettings& qt, bool& changed)
    {
        ImGui::TextDisabled("Shadows");
        if (ImGui::Checkbox("Shadow Enabled##qt", &qt.ShadowEnabled))
            changed = true;

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
        if (ImGui::SliderFloat("Shadow Softness##qt", &qt.ShadowSoftness, 0.0f, 2.0f))
            changed = true;
    }

    void RendererSettingsPanel::DrawAOControls(QualityTieringSettings& qt, bool& changed)
    {
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
            if (ImGui::SliderInt("SSAO Samples##qt", &qt.SSAOSamples, 8, 64))
                changed = true;
            if (ImGui::SliderFloat("SSAO Radius##qt", &qt.SSAORadius, 0.1f, 2.0f))
                changed = true;
            if (ImGui::SliderFloat("SSAO Bias##qt", &qt.SSAOBias, 0.001f, 0.1f))
                changed = true;
        }
        if (qt.AO == AOTechnique::GTAO)
        {
            if (ImGui::SliderFloat("GTAO Radius##qt", &qt.GTAORadius, 0.1f, 2.0f))
                changed = true;
            if (ImGui::SliderInt("GTAO Denoise Passes##qt", &qt.GTAODenoisePasses, 1, 8))
                changed = true;
            if (ImGui::SliderFloat("GTAO Power##qt", &qt.GTAOPower, 0.5f, 5.0f))
                changed = true;
        }
    }

    void RendererSettingsPanel::DrawPostProcessControls(QualityTieringSettings& qt, bool& changed)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Post-Processing");
        if (ImGui::Checkbox("Bloom##qt", &qt.BloomEnabled))
            changed = true;
        if (qt.BloomEnabled)
        {
            if (ImGui::SliderInt("Bloom Iterations##qt", &qt.BloomIterations, 1, 10))
                changed = true;
        }
        if (ImGui::Checkbox("FXAA##qt", &qt.FXAAEnabled))
            changed = true;
        if (ImGui::Checkbox("Depth of Field##qt", &qt.DOFEnabled))
            changed = true;
        if (ImGui::Checkbox("Motion Blur##qt", &qt.MotionBlurEnabled))
            changed = true;
        if (ImGui::Checkbox("Vignette##qt", &qt.VignetteEnabled))
            changed = true;
        if (ImGui::Checkbox("Chromatic Aberration##qt", &qt.ChromaticAberrationEnabled))
            changed = true;
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

            DrawPresetControls(qt);
            ImGui::Separator();

            bool changed = false;
            DrawShadowControls(qt, changed);
            DrawAOControls(qt, changed);
            DrawPostProcessControls(qt, changed);

            if (changed)
            {
                qt.Preset = QualityPreset::Custom;
                ApplyQualityTieringToRuntime(qt);
            }

            if (qt.Preset == QualityPreset::Custom)
            {
                ImGui::Spacing();
                if (ImGui::Button("Reset to High Preset"))
                {
                    qt = GetPresetSettings(QualityPreset::High);
                    ApplyQualityTieringToRuntime(qt);
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
                "Forward+",
                "Deferred"
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
                        // Keep the downgrade floor < upgrade threshold (allow 0 when threshold == 1).
                        if (settings.ForwardPlusLightThresholdDown >= settings.ForwardPlusLightThreshold)
                            settings.ForwardPlusLightThresholdDown = settings.ForwardPlusLightThreshold > 0 ? settings.ForwardPlusLightThreshold - 1 : 0;
                        Renderer3D::ApplyRendererSettings();
                    }
                    ImGui::TextDisabled("Switch to Forward+ when point+spot lights exceed this.");

                    int downThreshold = static_cast<int>(settings.ForwardPlusLightThresholdDown);
                    const int downMax = std::max(0, static_cast<int>(settings.ForwardPlusLightThreshold) - 1);
                    if (ImGui::SliderInt("Downgrade Threshold", &downThreshold, 0, downMax))
                    {
                        settings.ForwardPlusLightThresholdDown = static_cast<u32>(downThreshold);
                        Renderer3D::ApplyRendererSettings();
                    }
                    ImGui::TextDisabled("Hysteresis floor — once Forward+ is active, drop back to\n"
                                        "Forward only when lights fall to/below this value.");
                }

                // Velocity debug overlay (parity with Deferred DebugChannel=5).
                if (ImGui::Checkbox("Debug: Velocity Overlay", &settings.DebugVelocityOverlayForward))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Visualise the per-object screen-space velocity buffer.\n"
                                      "Red = +X motion, green = +Y motion.\n"
                                      "Scene FB attachment 3 (RG16F) → colour[0].");
                }
            }
            else if (settings.Path == RenderingPath::ForwardPlus)
            {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                                   "Forward+ pipeline active.");
                ImGui::TextDisabled("Tile-based clustered culling for many lights.");

                // Velocity debug overlay (parity with Deferred DebugChannel=5).
                if (ImGui::Checkbox("Debug: Velocity Overlay", &settings.DebugVelocityOverlayForward))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Visualise the per-object screen-space velocity buffer.\n"
                                      "Red = +X motion, green = +Y motion.\n"
                                      "Scene FB attachment 3 (RG16F) → colour[0].");
                }
            }
            else if (settings.Path == RenderingPath::Deferred)
            {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                                   "Deferred pipeline active.");
                ImGui::TextDisabled("G-Buffer MRT + PBR lighting + shadows + IBL + Forward+ tiles.");
                ImGui::TextDisabled("MSAA G-Buffer with hardware resolve before lighting.");

                ImGui::Spacing();
                auto& deferred = settings.Deferred;

                static const char* sampleItems[] = { "1x (off)", "2x", "4x", "8x" };
                static const u32 sampleValues[] = { 1, 2, 4, 8 };
                int sampleIdx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(sampleValues); ++i)
                {
                    if (sampleValues[i] == deferred.MSAASampleCount)
                    {
                        sampleIdx = i;
                        break;
                    }
                }

                // Reflect the driver cap in the UI: disable combo entries
                // the GPU can't satisfy. Zero means "not queried yet" — in
                // that case we show everything and trust ApplyRendererSettings
                // to clamp on first use.
                const u32 driverMax = Renderer3D::GetMaxMSAASamples();
                if (ImGui::BeginCombo("G-Buffer MSAA", sampleItems[sampleIdx]))
                {
                    for (int i = 0; i < IM_ARRAYSIZE(sampleValues); ++i)
                    {
                        const bool supported = (driverMax == 0) || (sampleValues[i] <= driverMax);
                        if (!supported)
                            ImGui::BeginDisabled();
                        const bool isSelected = (sampleIdx == i);
                        if (ImGui::Selectable(sampleItems[i], isSelected))
                        {
                            sampleIdx = i;
                            deferred.MSAASampleCount = sampleValues[sampleIdx];
                            Renderer3D::ApplyRendererSettings();
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                        if (!supported)
                        {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip("Driver supports up to %ux MSAA.", driverMax);
                        }
                    }
                    ImGui::EndCombo();
                }

                // Per-sample shading is only meaningful when MSAA is active.
                // Greyed when sample count == 1 but still togglable so the
                // setting survives round-trips through 1x.
                {
                    const bool msaaActive = deferred.MSAASampleCount > 1;
                    if (!msaaActive)
                        ImGui::BeginDisabled();
                    if (ImGui::Checkbox("Per-sample Deferred Lighting", &deferred.PerSampleLighting))
                    {
                        Renderer3D::ApplyRendererSettings();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("When enabled, PBR lighting is evaluated for every MSAA\n"
                                          "sub-sample and averaged (correct edge AA on materials).\n"
                                          "When disabled, the G-Buffer is resolved before lighting\n"
                                          "(cheaper, but edges only anti-alias at geometry boundaries).");
                    }
                    if (!msaaActive)
                        ImGui::EndDisabled();
                }

                if (ImGui::Checkbox("Weighted-Blended OIT", &deferred.OITEnabled))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::Checkbox("G-Buffer Decals (Phase 4)", &deferred.GBufferDecalsEnabled))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::Checkbox("Enable Light Probes", &deferred.EnableLightProbes))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Contribute the active light-probe volume's SH\n"
                                      "coefficients to the deferred ambient term. When\n"
                                      "disabled (or no active volume), the shader falls\n"
                                      "back to the global IBL cubemap only.");
                }

                static const char* channelItems[] = {
                    "Off (lit)",
                    "Albedo",
                    "Normal",
                    "Roughness / Metallic / AO",
                    "Emissive",
                    "Velocity"
                };
                int channelIdx = static_cast<int>(std::min<u32>(deferred.DebugChannel, 5));
                if (ImGui::Combo("Debug G-Buffer Channel", &channelIdx, channelItems, IM_ARRAYSIZE(channelItems)))
                {
                    deferred.DebugChannel = static_cast<u32>(channelIdx);
                    Renderer3D::ApplyRendererSettings();
                }
            }

            // Show active path status
            ImGui::Separator();
            auto& fplus = Renderer3D::GetForwardPlus();
            if (settings.Path == RenderingPath::Deferred)
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Active: Deferred");
            }
            else if (fplus.IsActive())
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Active: Forward+");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Active: Forward");
            }

            // Live RenderGraph topology — rebuilt per RenderingPath, so the
            // pass list here reflects exactly what runs this frame.
            if (const auto& graph = Renderer3D::GetRenderGraph(); graph)
            {
                if (ImGui::TreeNode("Render Graph (live topology)"))
                {
                    const auto& order = graph->GetPassOrder();
                    if (order.empty())
                    {
                        ImGui::TextDisabled("(Execution order not yet computed — run a frame first.)");
                    }
                    else
                    {
                        ImGui::Text("%zu passes in execution order:", order.size());
                        for (std::size_t i = 0; i < order.size(); ++i)
                            ImGui::BulletText("%zu. %s", i + 1, order[i].c_str());
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Export DOT..."))
                    {
                        const std::string path = "rendergraph.dot";
                        if (graph->DumpToDot(path))
                            OLO_CORE_INFO("RenderGraph exported to '{}'. Render with 'dot -Tsvg {} -o graph.svg'.",
                                          path, path);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("→ rendergraph.dot (cwd)");

                    ImGui::TreePop();
                }
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawCullingSection()
    {
        auto& settings = Renderer3D::GetRendererSettings();
        const bool forwardPlusForced = (settings.Path == RenderingPath::ForwardPlus) || (settings.Path == RenderingPath::Deferred);

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

            // WireframeOverlay changes GL polygon mode, so it must be applied
            // immediately via Renderer3D::ApplyRendererSettings(). ShowGrid,
            // ShowPhysicsColliders, and ShowLightGizmos are editor-only visual
            // toggles consumed each frame during rendering — no immediate apply needed.
            if (ImGui::Checkbox("Wireframe Overlay", &settings.WireframeOverlay))
            {
                Renderer3D::ApplyRendererSettings();
                m_DebugSettingsChanged = true;
            }
            if (ImGui::Checkbox("Show Grid", &settings.ShowGrid))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::Checkbox("Show Physics Colliders", &settings.ShowPhysicsColliders))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::Checkbox("Show Light Gizmos", &settings.ShowLightGizmos))
            {
                m_DebugSettingsChanged = true;
            }

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
