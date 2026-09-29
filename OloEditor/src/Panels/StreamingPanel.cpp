#include "OloEnginePCH.h"
#include "StreamingPanel.h"
#include "../UndoRedo/SpecializedCommands.h"
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
    void StreamingPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Scene Streaming", p_open);

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

    static bool StreamingSettingsEqual(const StreamingSettings& a, const StreamingSettings& b)
    {
        // Exact comparison on purpose: this detects "the widget wrote a new value",
        // not numeric closeness.
        return a.Enabled == b.Enabled && a.DefaultLoadRadius == b.DefaultLoadRadius && a.DefaultUnloadRadius == b.DefaultUnloadRadius && a.MaxLoadedRegions == b.MaxLoadedRegions && a.RegionDirectory == b.RegionDirectory && a.MaxResidentMegabytes == b.MaxResidentMegabytes && a.MaxAdmittedMegabytesPerFrame == b.MaxAdmittedMegabytesPerFrame;
    }

    void StreamingPanel::DrawSettingsSection()
    {
        OLO_PROFILE_FUNCTION();

        auto& ss = m_Context->GetStreamingSettings();

        // Snapshot settings before UI interaction
        if (m_CommandHistory && !m_IsEditingSettings)
        {
            m_SettingsSnapshot = ss;
        }

        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            ImGui::Checkbox("Enabled", &ss.Enabled);
            ImGui::DragFloat("Default Load Radius", &ss.DefaultLoadRadius, 1.0f, 10.0f, 10000.0f);
            ImGui::DragFloat("Default Unload Radius", &ss.DefaultUnloadRadius, 1.0f, 10.0f, 10000.0f);

            if (int maxRegions = static_cast<int>(ss.MaxLoadedRegions); ImGui::DragInt("Max Loaded Regions", &maxRegions, 1, 1, 64))
            {
                ss.MaxLoadedRegions = static_cast<u32>(maxRegions);
            }

            // 0 = no byte budget. Clamped to the same range the scene loader enforces.
            if (ImGui::DragFloat("Max Resident MB", &ss.MaxResidentMegabytes, 1.0f, 0.0f, kMaxStreamingBudgetMegabytes, "%.1f"))
            {
                ss.MaxResidentMegabytes = SanitizeStreamingBudgetMegabytes(ss.MaxResidentMegabytes);
            }
            if (ImGui::DragFloat("Max Admitted MB / Frame", &ss.MaxAdmittedMegabytesPerFrame, 0.1f, 0.0f, kMaxStreamingBudgetMegabytes, "%.1f"))
            {
                ss.MaxAdmittedMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(ss.MaxAdmittedMegabytesPerFrame);
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

        // Track edit sessions and push undo commands
        if (m_CommandHistory)
        {
            bool changed = !StreamingSettingsEqual(ss, m_SettingsSnapshot);
            bool activeWidget = ImGui::IsAnyItemActive();

            if (changed)
            {
                m_IsEditingSettings = true;
            }

            if (m_IsEditingSettings && !activeWidget)
            {
                // Editing ended — push undo command if settings actually changed
                if (changed)
                {
                    auto snapshot = m_SettingsSnapshot;
                    auto scene = m_Context;
                    m_CommandHistory->PushAlreadyExecuted(
                        std::make_unique<StreamingSettingsChangeCommand>(
                            snapshot, ss,
                            [scene](const StreamingSettings& s) mutable
                            { scene->GetStreamingSettings() = s; }));
                }
                m_IsEditingSettings = false;
                m_SettingsSnapshot = ss;
            }
        }
    }

    void StreamingPanel::DrawExportSection()
    {
        OLO_PROFILE_FUNCTION();

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
                // Skip entities that are cameras or have streaming volumes (infrastructure)
                if (Entity entity{ e, m_Context.get() }; entity.HasComponent<CameraComponent>() || entity.HasComponent<StreamingVolumeComponent>())
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
        OLO_PROFILE_FUNCTION();
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
            // Skip infrastructure entities
            if (Entity entity{ e, m_Context.get() }; entity.HasComponent<CameraComponent>() || entity.HasComponent<StreamingVolumeComponent>())
            {
                continue;
            }

            bool inBounds = tc.Translation.x >= m_ExportBoundsMin.x && tc.Translation.x <= m_ExportBoundsMax.x &&
                            tc.Translation.y >= m_ExportBoundsMin.y && tc.Translation.y <= m_ExportBoundsMax.y &&
                            tc.Translation.z >= m_ExportBoundsMin.z && tc.Translation.z <= m_ExportBoundsMax.z;

            if (m_ExportUseSceneBounds || inBounds)
            {
                region->m_EntityUUIDs.Add(idc.ID);
            }
        }

        // Serialize using the full serializer (Tag + Transform + all components)
        StreamingRegionSerializer serializer(m_Context);
        serializer.Serialize(region, savePath);

        OLO_CORE_INFO("Exported streaming region '{}' with {} entities to '{}'",
                      m_ExportRegionName, region->m_EntityUUIDs.Num(), savePath);
    }

    void StreamingPanel::DrawRegionsSection()
    {
        OLO_PROFILE_FUNCTION();

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
                const char* stateStr = ToString(region->m_State);

                auto idStr = std::to_string(static_cast<u64>(id));
                ImGui::PushID(idStr.c_str());

                if (bool nodeOpen = ImGui::TreeNode("", "%s [%s]", region->m_Name.GetData(), stateStr))
                {
                    ImGui::Text("ID: %llu", static_cast<unsigned long long>(static_cast<u64>(id)));
                    ImGui::Text("Entities: %d", region->m_EntityUUIDs.Num());
                    if (const auto bytes = region->m_EstimatedSize.GetBytes())
                    {
                        ImGui::Text("Estimated Size: %llu bytes", static_cast<unsigned long long>(*bytes));
                    }
                    else
                    {
                        ImGui::TextDisabled("Estimated Size: unknown");
                    }
                    if (region->m_AdmissionStatus != EStreamingAdmissionStatus::None)
                    {
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Last request: %s (%s)",
                                           ToString(region->m_AdmissionStatus), ToString(region->m_AdmissionReason));
                    }
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
                    else
                    {
                        // No additional handling required.
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
        OLO_PROFILE_FUNCTION();

        auto* streamer = m_Context->GetSceneStreamer();
        if (!streamer)
        {
            return;
        }

        if (ImGui::CollapsingHeader("Debug Info"))
        {
            ImGui::Indent();

            const FSceneStreamingStats stats = streamer->GetStats();
            auto megabytes = [](u64 bytes)
            { return static_cast<f64>(bytes) / (1024.0 * 1024.0); };
            // A byte total with unknown entries says so: "unknown" is not zero.
            auto drawBytes = [&megabytes](const char* label, const FAssetByteTotal& total)
            {
                ImGui::Text("%s: %.2f MB (%u region(s), %u unknown size)", label, megabytes(total.KnownBytes),
                            total.Count, total.UnknownCount);
            };

            ImGui::Text("Loaded Regions: %u / %u", stats.LoadedRegions, stats.MaxLoadedRegions);
            ImGui::Text("Pending Loads: %u (%u abandoned still running)", stats.PendingLoads, stats.AbandonedLoadsRunning);
            drawBytes("Resident", stats.ResidentBytes);
            drawBytes("Pending", stats.PendingBytes);
            if (stats.MaxResidentBytes != 0)
            {
                ImGui::Text("Resident Budget: %.2f MB", megabytes(stats.MaxResidentBytes));
            }
            else
            {
                ImGui::TextDisabled("Resident Budget: none");
            }
            if (stats.MaxAdmittedBytesPerFrame != 0)
            {
                ImGui::Text("Admitted This Frame: %.2f / %.2f MB", megabytes(stats.AdmittedBytesThisFrame),
                            megabytes(stats.MaxAdmittedBytesPerFrame));
            }
            ImGui::Text("Deferred: %u region(s) now, %llu request(s) total", stats.DeferredRegions,
                        static_cast<unsigned long long>(stats.DeferredRequests));
            ImGui::Text("Rejected: %u region(s) now, %llu request(s) total", stats.RejectedRegions,
                        static_cast<unsigned long long>(stats.RejectedRequests));
            ImGui::Text("Cancelled: %llu before start, %llu in flight, %llu completed",
                        static_cast<unsigned long long>(stats.CancelledBeforeStart),
                        static_cast<unsigned long long>(stats.AbandonedInFlight),
                        static_cast<unsigned long long>(stats.DiscardedCompleted));
            ImGui::Text("Evicted: %llu for count, %llu for bytes",
                        static_cast<unsigned long long>(stats.EvictedForCount),
                        static_cast<unsigned long long>(stats.EvictedForBytes));
            ImGui::Text("Load Radius: %.1f", streamer->GetConfig().LoadRadius);
            ImGui::Text("Unload Radius: %.1f", streamer->GetConfig().UnloadRadius);

            ImGui::Unindent();
        }
    }
} // namespace OloEngine
