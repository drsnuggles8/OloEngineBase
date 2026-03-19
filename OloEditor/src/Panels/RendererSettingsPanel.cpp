#include "OloEnginePCH.h"
#include "RendererSettingsPanel.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

namespace OloEngine
{
    void RendererSettingsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Renderer Settings", p_open);

        DrawRenderingPathSection();
        DrawCullingSection();
        DrawForwardPlusSection();
        DrawDebugSection();

        ImGui::End();
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
