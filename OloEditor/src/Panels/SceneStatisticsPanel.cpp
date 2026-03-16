#include "SceneStatisticsPanel.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"

#include <imgui.h>

namespace OloEngine
{
    void SceneStatisticsPanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        ImGui::SetNextWindowBgAlpha(0.6f);
        if (!ImGui::Begin("Scene Statistics", nullptr, ImGuiWindowFlags_NoFocusOnAppearing))
        {
            ImGui::End();
            return;
        }

        // FPS / Frame Time
        f32 const fps = ImGui::GetIO().Framerate;
        ImGui::Text("FPS: %.0f  (%.2f ms)", fps, 1000.0f / fps);
        ImGui::Separator();

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

        // 2D stats
        auto const stats2D = Renderer2D::GetStats();
        // 3D stats
        auto const stats3D = Renderer3D::GetStats();

        u32 const totalDrawCalls = stats2D.DrawCalls + stats3D.DrawCalls;
        u32 const triangles2D = stats2D.QuadCount * 2;

        ImGui::Text("Draw Calls: %u", totalDrawCalls);
        ImGui::Text("Triangles (2D): %u", triangles2D);
        ImGui::Text("Meshes: %u (culled %u)", stats3D.TotalMeshes, stats3D.CulledMeshes);

        ImGui::End();
    }

} // namespace OloEngine
