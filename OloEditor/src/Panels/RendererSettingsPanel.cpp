#include "OloEnginePCH.h"
#include "RendererSettingsPanel.h"
#include "SettingsChangeLog.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/BackendSelection.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        const char* QualityPresetName(const QualityPreset preset)
        {
            switch (preset)
            {
                case QualityPreset::Low:
                    return "Low";
                case QualityPreset::Medium:
                    return "Medium";
                case QualityPreset::High:
                    return "High";
                case QualityPreset::Ultra:
                    return "Ultra";
                case QualityPreset::Custom:
                    return "Custom";
                default:
                    return "Unknown";
            }
        }

        const char* AOTechniqueName(const AOTechnique technique)
        {
            switch (technique)
            {
                case AOTechnique::None:
                    return "None";
                case AOTechnique::SSAO:
                    return "SSAO";
                case AOTechnique::GTAO:
                    return "GTAO";
                default:
                    return "Unknown";
            }
        }

        const char* RenderingPathName(const RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "Forward+";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Unknown";
            }
        }

        // Names mirror the combo entries in DrawRenderingPathSection so the
        // log diff matches what the user clicked. Keep in sync if channels
        // are added.
        const char* DeferredDebugChannelName(u32 channel)
        {
            switch (channel)
            {
                case 0:
                    return "Off (lit)";
                case 1:
                    return "Albedo";
                case 2:
                    return "Normal";
                case 3:
                    return "Roughness/Metallic/AO";
                case 4:
                    return "Emissive";
                case 5:
                    return "Velocity";
                default:
                    return "Unknown";
            }
        }

        void LogRendererSettingsChanges(const RendererSettings& before, const RendererSettings& after)
        {
            using SettingsChangeLog::AppendChange;

            std::vector<std::string> changes;
            changes.reserve(24);

            if (before.Path != after.Path)
            {
                std::ostringstream oss;
                oss << "Path: " << RenderingPathName(before.Path) << " -> " << RenderingPathName(after.Path);
                changes.emplace_back(oss.str());
            }

            AppendChange(changes, "FrustumCulling", before.FrustumCullingEnabled, after.FrustumCullingEnabled);
            AppendChange(changes, "OcclusionCulling", before.OcclusionCullingEnabled, after.OcclusionCullingEnabled);
            AppendChange(changes, "HZBOcclusionCulling", before.HZBOcclusionCullingEnabled, after.HZBOcclusionCullingEnabled);
            AppendChange(changes, "DepthPrepass", before.DepthPrepassEnabled, after.DepthPrepassEnabled);

            AppendChange(changes, "ForwardPlusAutoSwitch", before.ForwardPlusAutoSwitch, after.ForwardPlusAutoSwitch);
            AppendChange(changes, "ForwardPlusLightThreshold", before.ForwardPlusLightThreshold, after.ForwardPlusLightThreshold);
            AppendChange(changes, "ForwardPlusLightThresholdDown", before.ForwardPlusLightThresholdDown, after.ForwardPlusLightThresholdDown);
            AppendChange(changes, "ForwardPlusDebugHeatmap", before.ForwardPlusDebugHeatmap, after.ForwardPlusDebugHeatmap);

            AppendChange(changes, "Deferred.MSAASampleCount", before.Deferred.MSAASampleCount, after.Deferred.MSAASampleCount);
            AppendChange(changes, "Deferred.PerSampleLighting", before.Deferred.PerSampleLighting, after.Deferred.PerSampleLighting);
            AppendChange(changes, "OITEnabled", before.OITEnabled, after.OITEnabled);
            AppendChange(changes, "Deferred.GBufferDecalsEnabled", before.Deferred.GBufferDecalsEnabled, after.Deferred.GBufferDecalsEnabled);
            AppendChange(changes, "Deferred.EnableLightProbes", before.Deferred.EnableLightProbes, after.Deferred.EnableLightProbes);

            // Realtime DDGI (issues #632 / #707). EnableDDGI was never logged
            // here; the cascade knobs are new. All of them change what the
            // renderer does per frame, so all of them belong in the change log.
            AppendChange(changes, "EnableDDGI", before.EnableDDGI, after.EnableDDGI);
            AppendChange(changes, "DDGICascadesEnabled", before.DDGICascadesEnabled, after.DDGICascadesEnabled);
            AppendChange(changes, "DDGICascadeCount", before.DDGICascadeCount, after.DDGICascadeCount);
            AppendChange(changes, "DDGICascadeResolution", before.DDGICascadeResolution, after.DDGICascadeResolution);
            AppendChange(changes, "DDGICascadeBaseSpacing", before.DDGICascadeBaseSpacing, after.DDGICascadeBaseSpacing);
            AppendChange(changes, "DDGICascadeBlendBand", before.DDGICascadeBlendBand, after.DDGICascadeBlendBand);
            AppendChange(changes, "DDGISparsityEnabled", before.DDGISparsityEnabled, after.DDGISparsityEnabled);
            AppendChange(changes, "DDGIUpdateRateDivisor", before.DDGIUpdateRateDivisor, after.DDGIUpdateRateDivisor);
            AppendChange(changes, "DDGICameraSeedRadius", before.DDGICameraSeedRadius, after.DDGICameraSeedRadius);
            if (before.Deferred.DebugChannel != after.Deferred.DebugChannel)
            {
                std::ostringstream oss;
                oss << "Deferred.DebugChannel: " << DeferredDebugChannelName(before.Deferred.DebugChannel)
                    << " -> " << DeferredDebugChannelName(after.Deferred.DebugChannel);
                changes.emplace_back(oss.str());
            }

            AppendChange(changes, "WireframeOverlay", before.WireframeOverlay, after.WireframeOverlay);
            AppendChange(changes, "ShowGrid", before.ShowGrid, after.ShowGrid);
            AppendChange(changes, "ShowPhysicsColliders", before.ShowPhysicsColliders, after.ShowPhysicsColliders);
            AppendChange(changes, "ShowLightGizmos", before.ShowLightGizmos, after.ShowLightGizmos);
            AppendChange(changes, "ShowWorldAxisHelper", before.ShowWorldAxisHelper, after.ShowWorldAxisHelper);
            AppendChange(changes, "ShowCameraFrustums", before.ShowCameraFrustums, after.ShowCameraFrustums);
            AppendChange(changes, "ShowBoundingBoxes", before.ShowBoundingBoxes, after.ShowBoundingBoxes);
            AppendChange(changes, "DebugVelocityOverlayForward", before.DebugVelocityOverlayForward, after.DebugVelocityOverlayForward);
            AppendChange(changes, "ShaderDebugDrawEnabled", before.ShaderDebugDrawEnabled, after.ShaderDebugDrawEnabled);
            AppendChange(changes, "ObserverCameraEnabled", before.ObserverCameraEnabled, after.ObserverCameraEnabled);
            AppendChange(changes, "ObserverCameraDrawFrustum", before.ObserverCameraDrawFrustum, after.ObserverCameraDrawFrustum);
            AppendChange(changes, "ShaderDebugDrawClusterBounds", before.ShaderDebugDrawClusterBounds, after.ShaderDebugDrawClusterBounds);

            SettingsChangeLog::EmitLog("RendererSettingsPanel", changes);
        }
    } // namespace

    void RendererSettingsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Renderer Settings", p_open);

        const RendererSettings settingsBefore = Renderer3D::GetRendererSettings();

        DrawBackendSection();
        DrawQualityTieringSection();
        DrawRenderingPathSection();
        DrawCullingSection();
        DrawForwardPlusSection();
        DrawTransparencySection();
        DrawDebugSection();

        const RendererSettings settingsAfter = Renderer3D::GetRendererSettings();
        LogRendererSettingsChanges(settingsBefore, settingsAfter);

        ImGui::End();
    }

    void RendererSettingsPanel::ApplyQualityTieringToRuntime(const QualityTieringSettings& qt)
    {
        ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
        ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadowCopy);
        Renderer3D::GetShadowMap().SetSettings(shadowCopy);
        ApplyTieringToRendererSettings(qt, Renderer3D::GetRendererSettings());
    }

    void RendererSettingsPanel::DrawPresetControls(QualityTieringSettings& qt)
    {
        static const char* presetItems[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        int currentPreset = static_cast<int>(std::to_underlying(qt.Preset));
        if (ImGui::Combo("Preset", &currentPreset, presetItems, IM_ARRAYSIZE(presetItems)))
        {
            auto selected = static_cast<QualityPreset>(currentPreset);
            OLO_CORE_INFO("RendererSettingsPanel: Quality preset selected -> {}", QualityPresetName(selected));
            if (selected != QualityPreset::Custom)
            {
                qt = GetPresetSettings(selected);
                ApplyQualityTieringToRuntime(qt);
                OLO_CORE_INFO("RendererSettingsPanel: Applied preset {} (AO={}, SSAOEnabled={}, GTAOEnabled={})",
                              QualityPresetName(qt.Preset), AOTechniqueName(qt.AO),
                              qt.AO == AOTechnique::SSAO, qt.AO == AOTechnique::GTAO);
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
        if (ImGui::Checkbox("Soft Shadows (PCSS)##qt", &qt.SoftShadows))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Percentage-Closer Soft Shadows: contact-hardening variable\n"
                              "penumbra (sharp at contact, softening with distance).\n"
                              "Off uses the legacy fixed PCF. 'Shadow Softness' is the\n"
                              "light apparent size when PCSS is on.\n"
                              "PCSS is by far the most expensive shadow path (measured\n"
                              "~14x the scene-pass cost of PCF on Sponza) — Ultra tier only\n"
                              "by default.");
        if (ImGui::SliderFloat("Shadow Softness##qt", &qt.ShadowSoftness, 0.0f, 2.0f))
            changed = true;

        DrawVirtualShadowMapControls();
    }

    void RendererSettingsPanel::DrawVirtualShadowMapControls()
    {
        // Edits ShadowSettings DIRECTLY rather than through QualityTieringSettings:
        // VSM's knobs are structural (pool size, world extents) rather than a
        // quality dial, and folding them into a preset would let "Low" silently
        // resize a 64 MB allocation.
        ImGui::Spacing();
        ImGui::TextDisabled("Virtual Shadow Maps (directional)");

        auto& shadowMap = Renderer3D::GetShadowMap();
        ShadowSettings settings = shadowMap.GetSettings();
        VirtualShadowMapSettings& vsm = settings.VSM;
        bool vsmChanged = false;

        if (ImGui::Checkbox("Enable VSM##vsm", &vsm.Enabled))
            vsmChanged = true;
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Sparse page-table directional shadows (issue #702). Replaces the\n"
                              "four CSM cascades for the SUN only; the local-light atlas is\n"
                              "unaffected.\n\n"
                              "Covers static and skinned MESH casters. Terrain, foliage, voxel and\n"
                              "virtualized-geometry casters still need CSM, so a scene that relies\n"
                              "on those should leave this off.");
        }

        if (vsm.Enabled)
        {
            static constexpr const char* kPoolItems[] = { "1024 (4 MB)", "2048 (16 MB)", "4096 (64 MB)",
                                                          "8192 (256 MB)" };
            static constexpr u32 kPoolValues[] = { 1024u, 2048u, 4096u, 8192u };
            int poolIdx = 2;
            for (int i = 0; i < 4; ++i)
            {
                if (kPoolValues[i] == vsm.PhysicalResolution)
                {
                    poolIdx = i;
                    break;
                }
            }
            if (ImGui::Combo("Physical Pool##vsm", &poolIdx, kPoolItems, IM_ARRAYSIZE(kPoolItems)))
            {
                vsm.PhysicalResolution = kPoolValues[poolIdx];
                vsmChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Backing store for the page table. 2048 is exactly the VRAM a\n"
                                  "default 4x1024 CSM costs - the equal-VRAM comparison.\n\n"
                                  "Watch 'alloc failed' below: a non-zero count means the pool is\n"
                                  "too small for this view and shadows are degrading to coarser\n"
                                  "clip levels.");
            }

            if (ImGui::SliderFloat("Clip 0 Half Extent (m)##vsm", &vsm.Clip0HalfExtent, 0.25f, 16.0f, "%.2f"))
                vsmChanged = true;
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("World half-width of the finest clip level. The 16 levels double\n"
                                  "from here, so 2 m reaches 2 * 2^15 = 65 km.");
            }

            if (ImGui::SliderFloat("Clip Selection Bias##vsm", &vsm.ClipSelectionBias, 0.25f, 4.0f, "%.2f"))
                vsmChanged = true;
            if (ImGui::SliderFloat("Depth Bias (m)##vsm", &vsm.DepthBiasMeters, 0.0f, 0.5f, "%.4f"))
                vsmChanged = true;
            if (ImGui::SliderFloat("Normal Bias (m)##vsm", &vsm.NormalBias, 0.0f, 0.5f, "%.4f"))
                vsmChanged = true;

            ImGui::Spacing();
            if (ImGui::Checkbox("Local Lights (point / spot)##vsm", &vsm.LocalLights))
                vsmChanged = true;
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Point and spot lights read the same page table instead of the\n"
                                  "budgeted 4096^2 atlas (issue #703). No 16-light / 32-entry cap and\n"
                                  "no priority rank: a light costs the pages its on-screen footprint\n"
                                  "asks for.\n\n"
                                  "Off keeps the atlas, so 'VSM for the sun, atlas for the lamps'\n"
                                  "stays available.");
            }

            if (vsm.LocalLights)
            {
                if (ImGui::SliderFloat("Local Detail Bias##vsm", &vsm.LocalDetailBias, 0.25f, 4.0f, "%.2f"))
                    vsmChanged = true;
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Scales the mip a local light resolves to. >1 is coarser and\n"
                                      "cheaper, <1 sharper. Changing it invalidates every cached page —\n"
                                      "the page MARKER and the SAMPLER both run this heuristic and must\n"
                                      "agree on it.");
                }
                if (ImGui::SliderFloat("Local Depth Bias (m)##vsm", &vsm.LocalDepthBiasMeters, 0.0f, 0.25f,
                                       "%.4f"))
                {
                    vsmChanged = true;
                }
            }

            // 4-7 are the bring-up views: they take the sampler apart so a wrong
            // frame says WHICH half is wrong. "Shadow factor" is the important
            // one — it renders the number the lit pass receives, so a shadow that
            // shows there but not in the beauty frame is a term being dropped by
            // the caller rather than a page-table fault.
            static constexpr const char* kDebugItems[] = { "Off", "Clip level", "Page address",
                                                           "Residency", "Shadow test", "Stored depth",
                                                           "Receiver depth", "Shadow factor" };
            int debugIdx = std::clamp(vsm.DebugMode, 0, 7);
            if (ImGui::Combo("Debug View##vsm", &debugIdx, kDebugItems, IM_ARRAYSIZE(kDebugItems)))
            {
                vsm.DebugMode = debugIdx;
                vsmChanged = true;
            }

            // THE page-draw counter acceptance criterion #2 asks for. One frame
            // stale by construction — it is read back from the buffer the previous
            // frame wrote, so observing it never stalls the GPU.
            const VSM::Statistics& stats = shadowMap.GetVirtualShadowMap().GetStatistics();
            ImGui::Separator();
            ImGui::Text("pages drawn %u / resident %u", stats.PagesDrawn, stats.PagesResident);
            ImGui::Text("requested %u  allocated %u  freed %u", stats.PagesRequested, stats.PagesAllocated,
                        stats.PagesFreed);
            if (stats.PagesFailed > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "alloc failed %u — pool too small", stats.PagesFailed);
            if (stats.CullOverflows > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "cull overflow %u", stats.CullOverflows);
            ImGui::Text("draw instances %u  VRAM %.1f MB", stats.DrawInstances,
                        static_cast<f64>(shadowMap.GetVirtualShadowMap().GetVRAMBytes()) / (1024.0 * 1024.0));

            // Issue #703's two acceptance criteria, as numbers you can read off a
            // running scene: "lights" against the atlas' 16-light / 32-entry cap,
            // and "starved" — the flag AtlasCasterRecord::Allocated used to
            // record — which is the one that has to stay at zero.
            if (vsm.LocalLights)
            {
                const auto& virtualShadowMap = shadowMap.GetVirtualShadowMap();
                ImGui::Text("local lights %u  layers %u/%u", virtualShadowMap.GetLocalLightCount(),
                            virtualShadowMap.GetLocalLayerCount(), VSM::kMaxLocalLayers);
                ImGui::Text("local pages drawn %u / resident %u", stats.LocalPagesDrawn,
                            stats.LocalPagesResident);
                if (const u32 starved = virtualShadowMap.GetLocalLightsStarved(); starved > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                       "local starved %u — layer pool exhausted", starved);
                }
            }
        }

        if (vsmChanged)
        {
            shadowMap.SetSettings(settings);
            OLO_CORE_INFO("RendererSettingsPanel: VSM {} (pool {}, clip0 {:.2f} m)",
                          shadowMap.IsVirtualShadowMapActive() ? "enabled" : "disabled",
                          vsm.PhysicalResolution, vsm.Clip0HalfExtent);
        }
    }

    void RendererSettingsPanel::DrawAOControls(QualityTieringSettings& qt, bool& changed)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Ambient Occlusion");
        static const char* aoItems[] = { "None", "SSAO", "GTAO" };
        if (int aoIdx = std::clamp(std::to_underlying(qt.AO), 0, 2); ImGui::Combo("AO Technique##qt", &aoIdx, aoItems, IM_ARRAYSIZE(aoItems)))
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

    void RendererSettingsPanel::DrawBackendSection()
    {
        if (!ImGui::CollapsingHeader("Backend"))
            return;

        // ADR 0011 §2: backend selection is a RUNTIME switch, startup-scoped —
        // `--rhi=` beats the config file, the config file beats the OpenGL
        // default, and nothing hot-swaps a live device (same model as
        // Unreal/Unity/Godot). This dropdown is the "editor exposes it as a
        // dropdown that takes effect on restart" half of that decision: it
        // writes `Renderer: { RHI: <name> }` to config/renderer.yaml (relative
        // to the editor working directory, which Application pins before
        // selection runs) and the next launch picks it up.
        const bool activeIsVulkan = RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
        ImGui::Text("Active: %s", activeIsVulkan ? "Vulkan" : "OpenGL");

        if (m_ConfiguredBackend < 0)
        {
            // First draw: reflect a pending choice from a previous session if
            // the config file already holds one; otherwise mirror the active
            // backend. Uses the SAME parser the engine boots with (no argv →
            // config-file → default chain) — a substring scan here could
            // false-positive on a comment mentioning the other backend.
            const BackendSelection configured = SelectRendererBackend(0, nullptr, DefaultRendererConfigPath());
            if (configured.Source == "config file")
                m_ConfiguredBackend = configured.Api == RendererAPI::API::Vulkan ? 1 : 0;
            else
                m_ConfiguredBackend = activeIsVulkan ? 1 : 0;
        }

        static constexpr std::array<const char*, 2> kBackendNames{ "OpenGL", "Vulkan" };
        int selection = m_ConfiguredBackend;
        if (ImGui::Combo("Backend##RendererBackend", &selection, kBackendNames.data(),
                         static_cast<int>(kBackendNames.size())) &&
            selection != m_ConfiguredBackend)
        {
            // One writer shape, owned by BackendSelection.cpp next to the parser
            // (#691) — the schema cannot drift between the two, and a
            // future runtime settings screen writes through the same helper.
            const auto path = DefaultRendererConfigPath();
            if (WriteRendererConfig(path, selection == 1 ? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL))
            {
                m_ConfiguredBackend = selection;
                OLO_CORE_INFO("RendererSettingsPanel: wrote {} = {} (applies on restart)", path.string(),
                              selection == 1 ? "vulkan" : "opengl");
            }
            else
            {
                OLO_CORE_ERROR("RendererSettingsPanel: could not write '{}' — backend selection not saved",
                               path.string());
            }
        }

        if ((m_ConfiguredBackend == 1) != activeIsVulkan)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Takes effect on the next editor launch.");
        }
        ImGui::TextDisabled("Written to %s; a --rhi= command-line flag overrides it.",
                            DefaultRendererConfigPath().string().c_str());
        ImGui::Separator();
    }

    void RendererSettingsPanel::DrawQualityTieringSection() const
    {
        auto project = Project::GetActive();
        if (!project)
        {
            return;
        }

        auto& qt = project->GetConfig().QualityTiering;

        if (ImGui::CollapsingHeader("Quality Preset"))
        {
            ImGui::Indent();

            const QualityTieringSettings before = qt;

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
                OLO_CORE_INFO("RendererSettingsPanel: Tier overrides applied (AO={} -> {}, ShadowEnabled={} -> {}, Bloom={} -> {}, FXAA={} -> {})",
                              AOTechniqueName(before.AO), AOTechniqueName(qt.AO),
                              before.ShadowEnabled, qt.ShadowEnabled,
                              before.BloomEnabled, qt.BloomEnabled,
                              before.FXAAEnabled, qt.FXAAEnabled);
            }

            if (qt.Preset == QualityPreset::Custom)
            {
                ImGui::Spacing();
                if (ImGui::Button("Reset to High Preset"))
                {
                    qt = GetPresetSettings(QualityPreset::High);
                    ApplyQualityTieringToRuntime(qt);
                    OLO_CORE_INFO("RendererSettingsPanel: Reset quality to High preset");
                }
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawRenderingPathSection() const
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
            if (int currentPath = static_cast<int>(std::to_underlying(settings.Path)); ImGui::Combo("Active Path", &currentPath, pathItems, IM_ARRAYSIZE(pathItems)))
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
                    if (int threshold = static_cast<int>(settings.ForwardPlusLightThreshold); ImGui::SliderInt("Light Threshold", &threshold, 1, 64))
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
                ImGui::TextDisabled("2D tiled light culling for many lights.");

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

                if (ImGui::Checkbox("G-Buffer Decals", &deferred.GBufferDecalsEnabled))
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

                // --- Virtualized geometry (Nanite, issue #629) ---
                ImGui::Separator();
                ImGui::TextDisabled("Virtualized Geometry (Nanite)");

                if (ImGui::Checkbox("Enable virtual geometry", &settings.VirtualGeometryEnabled))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Master switch for the cluster-LOD-DAG renderer.\n\n"
                                      "When OFF, every VirtualMeshComponent is drawn through the\n"
                                      "CLASSIC mesh path instead — same geometry, same materials,\n"
                                      "no cluster LOD. The scene does not change; only the renderer\n"
                                      "does, which makes this a true A/B when virtual geometry is\n"
                                      "the suspect in a visual bug.");
                }

                if (ImGui::Checkbox("Show debug view in viewport", &settings.VirtualDebugToViewport))
                {
                    Renderer3D::ApplyRendererSettings();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Composite the active virtual-geometry debug view (cluster id /\n"
                                      "LOD / overdraw) over the lit viewport image.\n\n"
                                      "Pick WHICH view in the Statistics panel -> Virtual Geometry\n"
                                      "(Nanite) -> Debug view. With this off, that view is written\n"
                                      "only to the 'VirtualGeometryDebug' MCP capture target and is\n"
                                      "invisible here.");
                }
            }
            else
            {
                // No additional handling required.
            }

            // Show active path status
            ImGui::Separator();
            const auto& fplus = Renderer3D::GetForwardPlus();
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

            ImGui::TextDisabled("Live graph topology, execution order, per-pass\n"
                                "diagnostics, and JSON export live in\n"
                                "View \xE2\x86\x92 Render Graph Debugger.");

            ImGui::Unindent();
        }

        // Realtime DDGI (issues #632 / #707). OUTSIDE the per-path branches
        // above on purpose: DDGI irradiance feeds the ambient ladder of BOTH
        // the deferred and the forward(+) lit passes (RenderingPath.h), so
        // putting its master switch inside the Deferred branch — next to
        // "Enable Light Probes", where it would naturally sit — would hide it
        // from half the users it applies to.
        if (ImGui::CollapsingHeader("Realtime GI (DDGI)"))
        {
            ImGui::Indent();

            if (ImGui::Checkbox("Enable DDGI", &settings.EnableDDGI))
            {
                Renderer3D::ApplyRendererSettings();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Master switch for realtime probe relighting.\n"
                                  "Off makes a Realtime/Hybrid probe volume behave as Baked\n"
                                  "and disables the camera-centred cascades below.");
            }

            if (ImGui::Checkbox("Camera-centred cascades", &settings.DDGICascadesEnabled))
            {
                Renderer3D::ApplyRendererSettings();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Issue #707. Probe grids centred on the camera, each cascade\n"
                                  "twice the previous one's spacing, so GI covers a large scene\n"
                                  "with NO authored probe volume.\n\n"
                                  "An ACTIVE Realtime/Hybrid LightProbeVolumeComponent still wins\n"
                                  "where one exists: the cascades cover what nobody authored.\n\n"
                                  "Off by default, because turning it on gives realtime GI to\n"
                                  "every scene without a probe volume - a deliberate opt-in\n"
                                  "rather than a silent change to every existing scene.");
            }

            if (settings.DDGICascadesEnabled)
            {
                ImGui::Indent();
                bool cascadeDirty = false;
                // Bounds come from the header constants rather than literals, so the
                // slider cannot outrun what the UBO arrays and the atlas sizing allow.
                if (ImGui::SliderInt("Cascades", &settings.DDGICascadeCount, 1, DDGI::kMaxCascades))
                    cascadeDirty = true;
                if (ImGui::SliderInt("Probes per axis", &settings.DDGICascadeResolution, 4,
                                     DDGI::kMaxCascadeResolution))
                    cascadeDirty = true;
                if (ImGui::SliderFloat("Base spacing (m)", &settings.DDGICascadeBaseSpacing,
                                       0.25f, 8.0f, "%.2f"))
                    cascadeDirty = true;
                if (ImGui::SliderFloat("Blend band", &settings.DDGICascadeBlendBand,
                                       0.0f, 0.9f, "%.2f"))
                    cascadeDirty = true;

                if (ImGui::Checkbox("Sparsity (request-driven relight)",
                                    &settings.DDGISparsityEnabled))
                    cascadeDirty = true;
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Relight only probes a shaded screen pixel or another live\n"
                                      "probe's cached hit point asked for. This is what makes a\n"
                                      "cascaded field affordable at all.");
                }

                static const char* updateRateItems[] = { "Every frame", "1 in 2", "1 in 8",
                                                         "1 in 16", "1 in 32", "1 in 64" };
                static const i32 updateRateValues[] = { 1, 2, 8, 16, 32, 64 };
                int rateSelection = 2; // 1-in-8, the default
                for (int i = 0; i < IM_ARRAYSIZE(updateRateValues); ++i)
                {
                    if (updateRateValues[i] == settings.DDGIUpdateRateDivisor)
                    {
                        rateSelection = i;
                        break;
                    }
                }
                if (ImGui::Combo("Update rate", &rateSelection, updateRateItems, IM_ARRAYSIZE(updateRateItems)))
                {
                    settings.DDGIUpdateRateDivisor = updateRateValues[rateSelection];
                    cascadeDirty = true;
                }

                if (ImGui::SliderFloat("Camera seed radius (m)", &settings.DDGICameraSeedRadius,
                                       0.0f, 64.0f, "%.1f"))
                    cascadeDirty = true;
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Probes within this radius are requested every frame no\n"
                                      "matter what the screen asked for - the floor under\n"
                                      "sparsity, so GI around the viewer cannot go dark.");
                }

                // The dense storage cost is what an author can walk off a cliff
                // with here, so it is SHOWN rather than described. 6 x 32^3 (the
                // figure PGI quotes) is ~1.6 GB in this engine, because our
                // capture is a rasterized hit-point cache rather than re-traced
                // rays. ~4.3 KB/probe is that cache plus the radiance,
                // irradiance and visibility atlases at the 8-texel default.
                const i64 probeCount = static_cast<i64>(settings.DDGICascadeCount) *
                                       static_cast<i64>(settings.DDGICascadeResolution) *
                                       static_cast<i64>(settings.DDGICascadeResolution) *
                                       static_cast<i64>(settings.DDGICascadeResolution);
                const f64 megabytes = static_cast<f64>(probeCount) * 4.3e-3;
                ImGui::TextDisabled("%lld probes, ~%.0f MB of probe atlases",
                                    static_cast<long long>(probeCount), megabytes);

                if (cascadeDirty)
                {
                    Renderer3D::ApplyRendererSettings();
                }
                ImGui::Unindent();
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawCullingSection() const
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

            if (ImGui::Checkbox("GPU Hi-Z Occlusion Cull (instanced)", &settings.HZBOcclusionCullingEnabled))
            {
                Renderer3D::ApplyRendererSettings();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Rejects instanced static meshes hidden behind the previous frame's\n"
                                  "depth pyramid before the indirect draw (#431). One-frame-latent;\n"
                                  "only affects dense instanced submissions above the GPU-cull threshold.");
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

    void RendererSettingsPanel::DrawForwardPlusSection() const
    {
        auto& settings = Renderer3D::GetRendererSettings();
        const auto& fplus = Renderer3D::GetForwardPlus();

        if (ImGui::CollapsingHeader("Forward+ Settings"))
        {
            ImGui::Indent();

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
                ImGui::Text("Clusters:     %ux%ux%u froxels", fplus.GetClusterCountX(),
                            fplus.GetClusterCountY(), fplus.GetClusterCountZ());
            }

            ImGui::Unindent();
        }
    }

    void RendererSettingsPanel::DrawTransparencySection() const
    {
        auto& settings = Renderer3D::GetRendererSettings();

        if (ImGui::CollapsingHeader("Transparency"))
        {
            ImGui::Indent();

            if (ImGui::Checkbox("Weighted-Blended OIT", &settings.OITEnabled))
            {
                Renderer3D::ApplyRendererSettings();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Order-Independent Transparency (McGuire/Bavoil 2013).\n"
                                  "Works in Forward, Forward+, and Deferred paths.\n"
                                  "Contributors: Particles, Decals.");
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
            if (ImGui::Checkbox("Show World Axis Helper", &settings.ShowWorldAxisHelper))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::Checkbox("Show Camera Frustums", &settings.ShowCameraFrustums))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::Checkbox("Show Bounding Boxes", &settings.ShowBoundingBoxes))
            {
                m_DebugSettingsChanged = true;
            }

            // --- GPU-pushable shader debug draws (issue #725) ---
            ImGui::Separator();
            if (ImGui::Checkbox("Shader Debug Draws", &settings.ShaderDebugDrawEnabled))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Draw the GPU-pushable debug primitives any shader can append to\n"
                                  "(include/DebugDrawCommon.glsl). Off costs nothing.");
            }
            if (settings.ShaderDebugDrawEnabled)
            {
                ImGui::Indent();
                if (ImGui::SliderFloat("Line Width", &settings.ShaderDebugDrawLineWidth, 1.0f, 16.0f, "%.1f px"))
                {
                    m_DebugSettingsChanged = true;
                }

                // The shipped consumer. Each bit is one cull verdict, so the
                // combination answers "which test removed that cluster" directly
                // instead of by elimination.
                static constexpr std::array<std::pair<const char*, u32>, 4> kClusterBoundsBits{ {
                    { "Clusters drawn (green)", 1u },
                    { "Frustum-culled (red)", 2u },
                    { "Cone-culled (blue)", 4u },
                    { "Hi-Z occluded (yellow)", 8u },
                } };
                ImGui::TextUnformatted("Virtual-geometry cluster bounds:");
                for (const auto& [label, bit] : kClusterBoundsBits)
                {
                    bool on = (settings.ShaderDebugDrawClusterBounds & bit) != 0u;
                    if (ImGui::Checkbox(label, &on))
                    {
                        settings.ShaderDebugDrawClusterBounds =
                            on ? (settings.ShaderDebugDrawClusterBounds | bit)
                               : (settings.ShaderDebugDrawClusterBounds & ~bit);
                        m_DebugSettingsChanged = true;
                    }
                }
                if (settings.ShaderDebugDrawClusterBounds != 0u)
                {
                    auto stride = static_cast<i32>(settings.ShaderDebugDrawClusterStride);
                    if (ImGui::SliderInt("Cluster Stride", &stride, 1, 256))
                    {
                        settings.ShaderDebugDrawClusterStride = static_cast<u32>(std::max(stride, 1));
                        m_DebugSettingsChanged = true;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Emit only every Nth cluster. A Nanite-class scene has far more\n"
                                          "clusters than the channel holds, so 1 just overflows.");
                    }
                }

                // Live overflow readout. "I drew nothing" and "I overflowed and
                // everything after slot N was dropped" are the two failure modes
                // the issue calls out as otherwise indistinguishable.
                const auto& stats = ShaderDebugDraw::GetStats();
                if (stats.StatsValid)
                {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Channels (previous frame):");
                    for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
                    {
                        const auto& channel = stats.Channels[i];
                        if (channel.Requested == 0 && channel.Capacity == 0)
                            continue;
                        const char* name =
                            ShaderDebugDrawContract::Name(static_cast<ShaderDebugDrawPrimitive>(i));
                        if (channel.Overflowed())
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f),
                                               "%s: %u/%u drawn — OVERFLOW, %u dropped", name, channel.Drawn,
                                               channel.Requested, channel.Dropped());
                        }
                        else if (channel.Drawn > 0)
                        {
                            ImGui::Text("%s: %u drawn (%u from CPU) / capacity %u", name, channel.Drawn,
                                        channel.CpuPushes, channel.Capacity);
                        }
                        else
                        {
                            ImGui::TextDisabled("%s: none", name);
                        }
                    }
                }
                ImGui::Unindent();
            }

            // --- Observer camera (issue #726) ---
            ImGui::Separator();
            if (ImGui::Checkbox("Observer Camera (freeze culling)", &settings.ObserverCameraEnabled))
            {
                m_DebugSettingsChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Freeze the camera used for culling, LOD selection and Hi-Z at its\n"
                                  "current pose, then fly the viewport away to look at the frozen cut\n"
                                  "from outside. Anything the frozen camera culled stays culled, so a\n"
                                  "wrongly-culled object is visibly missing instead of invisible.");
            }
            if (settings.ObserverCameraEnabled)
            {
                ImGui::Indent();
                if (ImGui::Checkbox("Draw frozen frustum", &settings.ObserverCameraDrawFrustum))
                {
                    m_DebugSettingsChanged = true;
                }
                if (!settings.ShaderDebugDrawEnabled && settings.ObserverCameraDrawFrustum)
                {
                    // Not an error, and worth saying out loud: the pass that
                    // consumes the channel is not declared at all while shader
                    // debug draws are off, so the frustum silently never appears.
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                       "Enable \"Shader Debug Draws\" above to see the frustum.");
                }
                const glm::vec3& cullPos = Renderer3D::GetCullViewPosition();
                ImGui::Text("Frozen at: %.2f, %.2f, %.2f", cullPos.x, cullPos.y, cullPos.z);
                ImGui::Unindent();
            }

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
