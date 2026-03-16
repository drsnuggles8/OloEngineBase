#include "SceneStatisticsPanel.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Scene/Components.h"

#include <imgui.h>

namespace OloEngine
{
    void SceneStatisticsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::SetNextWindowBgAlpha(0.6f);
        if (!ImGui::Begin("Scene Statistics", p_open, ImGuiWindowFlags_NoFocusOnAppearing))
        {
            ImGui::End();
            return;
        }

        // FPS / Frame Time
        f32 const fps = ImGui::GetIO().Framerate;
        if (fps > 0.0f)
        {
            ImGui::Text("FPS: %.0f  (%.2f ms)", fps, 1000.0f / fps);
        }
        else
        {
            ImGui::Text("FPS: -  (- ms)");
        }
        ImGui::Separator();

        // Hovered Entity
        std::string hoveredName = "None";
        if (m_HoveredEntity && m_Context && m_HoveredEntity.GetScene() == m_Context.get() && m_HoveredEntity.HasComponent<TagComponent>())
        {
            hoveredName = m_HoveredEntity.GetComponent<TagComponent>().Tag;
        }
        ImGui::Text("Hovered Entity: %s", hoveredName.c_str());

        // Entity count
        if (m_Context)
        {
            auto view = m_Context->GetAllEntitiesWith<TransformComponent>();
            ImGui::Text("Entities: %u", static_cast<u32>(view.size()));
        }
        else
        {
            ImGui::Text("Entities: -");
        }

        ImGui::Separator();

        // Renderer2D Stats
        auto const stats2D = Renderer2D::GetStats();
        if (ImGui::CollapsingHeader("Renderer2D", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Draw Calls: %d", stats2D.DrawCalls);
            ImGui::Text("Quads: %d", stats2D.QuadCount);
            ImGui::Text("Vertices: %d", stats2D.GetTotalVertexCount());
            ImGui::Text("Indices: %d", stats2D.GetTotalIndexCount());
        }

        // Renderer3D Stats
        {
            OLO_PROFILE_SCOPE("Renderer3D Stats");
            auto const stats3D = Renderer3D::GetStats();
            if (ImGui::CollapsingHeader("Renderer3D", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Total Meshes: %u", stats3D.TotalMeshes);
                ImGui::Text("Culled Meshes: %u", stats3D.CulledMeshes);
                ImGui::Text("Draw Calls: %u", stats3D.DrawCalls);
                ImGui::Text("LOD Switches: %u", stats3D.LODSwitches);
                for (u32 i = 0; i < static_cast<u32>(stats3D.ObjectsPerLODLevel.size()); ++i)
                {
                    if (stats3D.ObjectsPerLODLevel[i] > 0)
                    {
                        ImGui::Text("  LOD %u Objects: %u", i, stats3D.ObjectsPerLODLevel[i]);
                    }
                }
            }

            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, stats3D.DrawCalls);
        }

        ImGui::End();
    }

} // namespace OloEngine
