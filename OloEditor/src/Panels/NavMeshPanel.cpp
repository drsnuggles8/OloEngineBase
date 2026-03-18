#include "OloEnginePCH.h"
#include "NavMeshPanel.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshDebugDraw.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>

#include <chrono>

namespace OloEngine
{
    void NavMeshPanel::RefreshFromScene()
    {
        if (!m_Context)
        {
            m_Settings = {};
            m_LastPolyCount = 0;
            m_LastBakeTimeMs = 0.0f;
            return;
        }

        auto navMesh = m_Context->GetNavMesh();
        if (navMesh && navMesh->IsValid())
        {
            m_Settings = navMesh->GetSettings();
            m_LastPolyCount = navMesh->GetPolyCount();
            m_LastBakeTimeMs = 0.0f;
        }
        else
        {
            m_Settings = {};
            m_LastPolyCount = 0;
            m_LastBakeTimeMs = 0.0f;
        }
    }

    NavMeshPanel::NavMeshPanel(const Ref<Scene>& context)
        : m_Context(context)
    {
        RefreshFromScene();
    }

    void NavMeshPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        RefreshFromScene();
    }

    void NavMeshPanel::OnImGuiRender()
    {
        ImGui::Begin("NavMesh");

        if (!m_Context)
        {
            ImGui::Text("No scene loaded");
            ImGui::End();
            return;
        }

        ImGui::Text("Generation Settings");
        ImGui::Separator();

        ImGui::DragFloat("Cell Size", &m_Settings.CellSize, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Cell Height", &m_Settings.CellHeight, 0.01f, 0.01f, 10.0f);

        ImGui::Separator();
        ImGui::Text("Agent");
        ImGui::DragFloat("Agent Radius", &m_Settings.AgentRadius, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Agent Height", &m_Settings.AgentHeight, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Max Climb", &m_Settings.AgentMaxClimb, 0.01f, 0.0f, 10.0f);
        ImGui::DragFloat("Max Slope", &m_Settings.AgentMaxSlope, 0.5f, 0.0f, 90.0f);

        ImGui::Separator();
        ImGui::Text("Region");
        ImGui::DragInt("Min Region Size", &m_Settings.RegionMinSize, 1, 1, 150);
        ImGui::DragInt("Merge Region Size", &m_Settings.RegionMergeSize, 1, 1, 150);

        ImGui::Separator();
        ImGui::Text("Polygonization");
        ImGui::DragFloat("Edge Max Length", &m_Settings.EdgeMaxLen, 0.1f, 0.0f, 50.0f);
        ImGui::DragFloat("Edge Max Error", &m_Settings.EdgeMaxError, 0.1f, 0.1f, 5.0f);
        ImGui::DragInt("Verts Per Poly", &m_Settings.VertsPerPoly, 1, 3, 6);
        ImGui::DragFloat("Detail Sample Dist", &m_Settings.DetailSampleDist, 0.1f, 0.0f, 16.0f);
        ImGui::DragFloat("Detail Sample Max Error", &m_Settings.DetailSampleMaxError, 0.1f, 0.0f, 16.0f);

        ImGui::Separator();

        // Find NavMeshBoundsComponent for bounds
        glm::vec3 boundsMin(-100.0f, -10.0f, -100.0f);
        glm::vec3 boundsMax(100.0f, 50.0f, 100.0f);

        auto boundsView = m_Context->GetAllEntitiesWith<NavMeshBoundsComponent>();
        bool firstBounds = true;
        for (auto e : boundsView)
        {
            Entity entity = { e, m_Context.Raw() };
            auto& bounds = entity.GetComponent<NavMeshBoundsComponent>();
            if (firstBounds)
            {
                boundsMin = bounds.m_Min;
                boundsMax = bounds.m_Max;
                firstBounds = false;
            }
            else
            {
                boundsMin = glm::min(boundsMin, bounds.m_Min);
                boundsMax = glm::max(boundsMax, bounds.m_Max);
            }
        }

        ImGui::Text("Bounds: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)",
                    boundsMin.x, boundsMin.y, boundsMin.z,
                    boundsMax.x, boundsMax.y, boundsMax.z);

        if (ImGui::Button("Bake NavMesh"))
        {
            auto startTime = std::chrono::high_resolution_clock::now();

            auto navMesh = NavMeshGenerator::Generate(m_Context.Raw(), m_Settings, boundsMin, boundsMax);

            auto endTime = std::chrono::high_resolution_clock::now();
            m_LastBakeTimeMs = std::chrono::duration<f32, std::milli>(endTime - startTime).count();

            if (navMesh)
            {
                m_Context->SetNavMesh(navMesh);
                m_LastPolyCount = navMesh->GetPolyCount();
                OLO_CORE_INFO("NavMesh baked: {} polys in {:.1f}ms", m_LastPolyCount, m_LastBakeTimeMs);
            }
            else
            {
                OLO_CORE_WARN("NavMesh bake failed");
            }
        }

        if (m_Context->GetNavMesh())
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear NavMesh"))
            {
                m_Context->SetNavMesh(nullptr);
                m_LastPolyCount = 0;
                m_LastBakeTimeMs = 0.0f;
            }
        }

        ImGui::Separator();
        ImGui::Text("Stats");
        ImGui::Text("Polygons: %d", m_LastPolyCount);
        ImGui::Text("Last Bake: %.1f ms", m_LastBakeTimeMs);

        ImGui::Checkbox("Debug Draw", &m_ShowDebugDraw);
        if (m_ShowDebugDraw && m_Context->GetNavMesh())
        {
            NavMeshDebugDraw::DrawNavMesh(m_Context->GetNavMesh());
        }

        ImGui::End();
    }
} // namespace OloEngine
