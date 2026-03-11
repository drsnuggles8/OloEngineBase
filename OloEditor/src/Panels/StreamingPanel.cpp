#include "OloEnginePCH.h"
#include "StreamingPanel.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"
#include "OloEngine/Scene/Streaming/StreamingRegion.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/Streaming/StreamingRegionSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    void StreamingPanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Scene Streaming");

        if (!m_Context)
        {
            ImGui::TextDisabled("No scene loaded");
            ImGui::End();
            return;
        }

        DrawSettingsSection();
        DrawExportSection();
        DrawRegionsSection();
        DrawDebugSection();

        ImGui::End();
    }

    void StreamingPanel::DrawSettingsSection()
    {
        auto& ss = m_Context->GetStreamingSettings();

        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enabled", &ss.Enabled);
            ImGui::DragFloat("Default Load Radius", &ss.DefaultLoadRadius, 1.0f, 10.0f, 10000.0f);
            ImGui::DragFloat("Default Unload Radius", &ss.DefaultUnloadRadius, 1.0f, 10.0f, 10000.0f);

            int maxRegions = static_cast<int>(ss.MaxLoadedRegions);
            if (ImGui::DragInt("Max Loaded Regions", &maxRegions, 1, 1, 64))
            {
                ss.MaxLoadedRegions = static_cast<u32>(maxRegions);
            }

            char buf[256] = {};
            std::strncpy(buf, ss.RegionDirectory.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Region Directory", buf, sizeof(buf)))
            {
                ss.RegionDirectory = buf;
            }

            // Editor-mode streamer controls
            ImGui::Separator();
            if (m_Context->GetSceneStreamer())
            {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Editor Streamer: Active");
                if (ImGui::Button("Stop Editor Streamer"))
                {
                    m_Context->ShutdownEditorStreamer();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Editor Streamer: Inactive");
                if (ss.Enabled && ImGui::Button("Start Editor Streamer"))
                {
                    m_Context->InitializeEditorStreamer();
                }
            }

            ImGui::Unindent();
        }
    }

    void StreamingPanel::DrawExportSection()
    {
        if (ImGui::CollapsingHeader("Export Region"))
        {
            ImGui::Indent();

            ImGui::InputText("Region Name", m_ExportRegionName, sizeof(m_ExportRegionName));

            ImGui::Checkbox("Auto-compute Bounds", &m_ExportUseSceneBounds);
            if (!m_ExportUseSceneBounds)
            {
                ImGui::DragFloat3("Bounds Min", glm::value_ptr(m_ExportBoundsMin), 1.0f);
                ImGui::DragFloat3("Bounds Max", glm::value_ptr(m_ExportBoundsMax), 1.0f);
            }

            // Preview: count entities in bounds
            u32 entityCount = 0;
            glm::vec3 computedMin(std::numeric_limits<f32>::max());
            glm::vec3 computedMax(std::numeric_limits<f32>::lowest());

            auto view = m_Context->GetAllEntitiesWith<TransformComponent>();
            for (auto&& [e, tc] : view.each())
            {
                Entity entity{ e, m_Context.get() };

                // Skip entities that are cameras or have streaming volumes (infrastructure)
                if (entity.HasComponent<CameraComponent>() || entity.HasComponent<StreamingVolumeComponent>())
                {
                    continue;
                }

                if (m_ExportUseSceneBounds)
                {
                    computedMin = glm::min(computedMin, tc.Translation);
                    computedMax = glm::max(computedMax, tc.Translation);
                    ++entityCount;
                }
                else
                {
                    // Check if within user-specified bounds
                    if (tc.Translation.x >= m_ExportBoundsMin.x && tc.Translation.x <= m_ExportBoundsMax.x &&
                        tc.Translation.y >= m_ExportBoundsMin.y && tc.Translation.y <= m_ExportBoundsMax.y &&
                        tc.Translation.z >= m_ExportBoundsMin.z && tc.Translation.z <= m_ExportBoundsMax.z)
                    {
                        ++entityCount;
                    }
                }
            }

            if (m_ExportUseSceneBounds && entityCount > 0)
            {
                m_ExportBoundsMin = computedMin - glm::vec3(5.0f);
                m_ExportBoundsMax = computedMax + glm::vec3(5.0f);
            }

            ImGui::Text("Entities to export: %u", entityCount);
            ImGui::Text("Bounds: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)",
                        m_ExportBoundsMin.x, m_ExportBoundsMin.y, m_ExportBoundsMin.z,
                        m_ExportBoundsMax.x, m_ExportBoundsMax.y, m_ExportBoundsMax.z);

            if (entityCount > 0 && ImGui::Button("Export as .oloregion..."))
            {
                ExportRegion();
            }

            ImGui::Unindent();
        }
    }

    void StreamingPanel::ExportRegion()
    {
        std::string savePath = FileDialogs::SaveFile(
            "Streaming Region (*.oloregion)\0*.oloregion\0");
        if (savePath.empty())
        {
            return;
        }

        // Ensure extension
        if (savePath.find(".oloregion") == std::string::npos)
        {
            savePath += ".oloregion";
        }

        // Build the region
        auto region = Ref<StreamingRegion>::Create();
        region->m_RegionID = UUID();
        region->m_Name = m_ExportRegionName;
        region->m_BoundsMin = m_ExportBoundsMin;
        region->m_BoundsMax = m_ExportBoundsMax;

        // Collect entity UUIDs within bounds
        auto view = m_Context->GetAllEntitiesWith<TransformComponent, IDComponent>();
        for (auto&& [e, tc, idc] : view.each())
        {
            Entity entity{ e, m_Context.get() };

            // Skip infrastructure entities
            if (entity.HasComponent<CameraComponent>() || entity.HasComponent<StreamingVolumeComponent>())
            {
                continue;
            }

            bool inBounds = tc.Translation.x >= m_ExportBoundsMin.x && tc.Translation.x <= m_ExportBoundsMax.x &&
                            tc.Translation.y >= m_ExportBoundsMin.y && tc.Translation.y <= m_ExportBoundsMax.y &&
                            tc.Translation.z >= m_ExportBoundsMin.z && tc.Translation.z <= m_ExportBoundsMax.z;

            if (m_ExportUseSceneBounds || inBounds)
            {
                region->m_EntityUUIDs.push_back(idc.ID);
            }
        }

        // Serialize using the full serializer (Tag + Transform + all components)
        StreamingRegionSerializer serializer(m_Context);
        serializer.Serialize(region, savePath);

        OLO_CORE_INFO("Exported streaming region '{}' with {} entities to '{}'",
                       m_ExportRegionName, region->m_EntityUUIDs.size(), savePath);
    }

    void StreamingPanel::DrawRegionsSection()
    {
        auto* streamer = m_Context->GetSceneStreamer();
        if (!streamer)
        {
            return;
        }

        if (ImGui::CollapsingHeader("Regions", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            if (ImGui::Button("Refresh Regions"))
            {
                // Re-discover by restarting the streamer with same config
                auto config = streamer->GetConfig();
                streamer->Shutdown();
                streamer->Initialize(m_Context.get(), config);
            }

            ImGui::Separator();

            for (auto const& [id, region] : streamer->GetRegions())
            {
                const char* stateStr = "Unknown";
                switch (region->m_State)
                {
                    case StreamingRegion::State::Unloaded:  stateStr = "Unloaded"; break;
                    case StreamingRegion::State::Loading:   stateStr = "Loading";  break;
                    case StreamingRegion::State::Loaded:    stateStr = "Loaded";   break;
                    case StreamingRegion::State::Ready:     stateStr = "Ready";    break;
                    case StreamingRegion::State::Unloading: stateStr = "Unloading"; break;
                }

                ImGui::PushID(static_cast<int>(static_cast<u64>(id)));

                bool nodeOpen = ImGui::TreeNode("", "%s [%s]", region->m_Name.c_str(), stateStr);
                if (nodeOpen)
                {
                    ImGui::Text("ID: %llu", static_cast<unsigned long long>(static_cast<u64>(id)));
                    ImGui::Text("Entities: %zu", region->m_EntityUUIDs.size());
                    ImGui::Text("Source: %s", region->m_SourcePath.c_str());
                    ImGui::Text("Bounds Min: (%.1f, %.1f, %.1f)",
                                region->m_BoundsMin.x, region->m_BoundsMin.y, region->m_BoundsMin.z);
                    ImGui::Text("Bounds Max: (%.1f, %.1f, %.1f)",
                                region->m_BoundsMax.x, region->m_BoundsMax.y, region->m_BoundsMax.z);

                    if (region->m_State == StreamingRegion::State::Unloaded)
                    {
                        if (ImGui::Button("Load"))
                        {
                            streamer->LoadRegion(id);
                        }
                    }
                    else if (region->m_State == StreamingRegion::State::Ready)
                    {
                        if (ImGui::Button("Unload"))
                        {
                            streamer->UnloadRegion(id);
                        }
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }

            ImGui::Unindent();
        }
    }

    void StreamingPanel::DrawDebugSection()
    {
        auto* streamer = m_Context->GetSceneStreamer();
        if (!streamer)
        {
            return;
        }

        if (ImGui::CollapsingHeader("Debug Info"))
        {
            ImGui::Indent();

            ImGui::Text("Loaded Regions: %u / %u",
                        streamer->GetLoadedRegionCount(),
                        streamer->GetConfig().MaxLoadedRegions);
            ImGui::Text("Pending Loads: %u", streamer->GetPendingLoadCount());
            ImGui::Text("Load Radius: %.1f", streamer->GetConfig().LoadRadius);
            ImGui::Text("Unload Radius: %.1f", streamer->GetConfig().UnloadRadius);

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
