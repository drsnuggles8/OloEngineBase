#include "OloEnginePCH.h"
#include "StatisticsPanel.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Scene/Components.h"

#include <string>

#include <glad/gl.h>
#include <imgui.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace OloEngine
{
    void StatisticsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::SetNextWindowBgAlpha(0.6f);
        if (!ImGui::Begin("Statistics", p_open, ImGuiWindowFlags_NoFocusOnAppearing))
        {
            ImGui::End();
            return;
        }

        // FPS / Frame Time header (always visible)
        f32 const fps = ImGui::GetIO().Framerate;
        if (fps > 0.0f)
        {
            ImGui::Text("FPS: %.0f  (%.2f ms)", fps, 1000.0f / fps);
        }
        else
        {
            ImGui::Text("FPS: -  (- ms)");
        }

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

        if (ImGui::BeginTabBar("StatisticsTabs"))
        {
            if (ImGui::BeginTabItem("Renderer"))
            {
                DrawRendererTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio"))
            {
                DrawAudioTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Performance"))
            {
                DrawPerformanceTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Memory"))
            {
                DrawMemoryTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void StatisticsPanel::DrawRendererTab() const
    {
        OLO_PROFILE_FUNCTION();

        // GPU Info (query OpenGL directly)
        auto const* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        auto const* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        auto const* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        ImGui::Text("GPU: %s", renderer ? renderer : "Unknown");
        ImGui::Text("Vendor: %s", vendor ? vendor : "Unknown");
        ImGui::Text("GL Version: %s", version ? version : "Unknown");

        // VSync
        auto& window = Application::Get().GetWindow();
        bool vsync = window.IsVSync();
        if (ImGui::Checkbox("VSync", &vsync))
        {
            window.SetVSync(vsync);
        }

        ImGui::Separator();

        // Renderer2D Stats
        auto const stats2D = Renderer2D::GetStats();
        if (ImGui::CollapsingHeader("Renderer2D", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Draw Calls: %u", stats2D.DrawCalls);
            ImGui::Text("Quads: %u", stats2D.QuadCount);
            ImGui::Text("Vertices: %u", stats2D.GetTotalVertexCount());
            ImGui::Text("Indices: %u", stats2D.GetTotalIndexCount());
        }

        // Renderer3D Stats
        auto const& stats3D = Renderer3D::GetStats();
        if (ImGui::CollapsingHeader("Renderer3D", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total Meshes: %u", stats3D.TotalMeshes);
            ImGui::Text("Culled Meshes: %u", stats3D.CulledMeshes);
            ImGui::Text("Draw Calls: %u", stats3D.DrawCalls);
            ImGui::Text("Shader Binds: %u", stats3D.ShaderBinds);
            ImGui::Text("Texture Binds: %u", stats3D.TextureBinds);
            ImGui::Text("LOD Switches: %u", stats3D.LODSwitches);
            for (u32 i = 0; i < static_cast<u32>(stats3D.ObjectsPerLODLevel.size()); ++i)
            {
                if (stats3D.ObjectsPerLODLevel[i] > 0)
                {
                    ImGui::Text("  LOD %u Objects: %u", i, stats3D.ObjectsPerLODLevel[i]);
                }
            }
            if (stats3D.TotalAnimatedMeshes > 0)
            {
                ImGui::Text("Animated Meshes: %u / %u", stats3D.RenderedAnimatedMeshes, stats3D.TotalAnimatedMeshes);
            }
            if (stats3D.TotalEmitters > 0)
            {
                ImGui::Text("Emitters: %u (Culled: %u)", stats3D.TotalEmitters, stats3D.CulledEmitters);
            }
        }

        // Virtual Geometry (Nanite cluster LOD DAG, issue #629). Shown only when
        // virtual meshes are actually registered/submitted this frame.
        {
            auto& vgRegistry = VirtualMeshRegistry::Get();
            const VirtualResidencyStats& residency = vgRegistry.GetResidencyStats();
            u32 const frameInstances = static_cast<u32>(vgRegistry.GetFrameInstances().size());
            if (residency.TotalPages > 0 || frameInstances > 0)
            {
                if (ImGui::CollapsingHeader("Virtual Geometry (Nanite)", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::Text("Instances this frame: %u", frameInstances);

                    ImGui::Separator();
                    ImGui::TextDisabled("Streaming residency");
                    ImGui::Text("Resident pages: %u / %u", residency.ResidentPages, residency.TotalPages);
                    ImGui::Text("Pinned pages: %u", residency.PinnedPages);
                    if (residency.BudgetSlots == 0)
                    {
                        ImGui::Text("Budget: unbounded (eager)");
                    }
                    else
                    {
                        ImGui::Text("Budget slots: %u", residency.BudgetSlots);
                    }
                    ImGui::Text("Page uploads: %llu", static_cast<unsigned long long>(residency.PageUploads));
                    ImGui::Text("Page evictions: %llu", static_cast<unsigned long long>(residency.PageEvictions));

                    // Cluster-cull stats need a small blocking GPU readback of the
                    // args buffer; only fetched here, inside the open header, so a
                    // collapsed section (or a scene without virtual meshes) pays
                    // nothing.
                    ImGui::Separator();
                    ImGui::TextDisabled("Cluster cull (this frame)");
                    VirtualCullStats const cull = vgRegistry.ReadFrameCullStats();
                    ImGui::Text("Tested clusters: %u", cull.TestedClusters);
                    ImGui::Text("DAG-cut selected: %u", cull.CutSelected);
                    ImGui::Text("Drawn: %u  (HW %u / SW %u)", cull.DrawnClusters(), cull.HardwareDraws,
                                cull.SoftwareRasterized);

                    // Debug visualization: drives the "VirtualGeometryDebug"
                    // capture target (olo_render_capture_target / MCP inspection).
                    ImGui::Separator();
                    ImGui::TextDisabled("Debug capture (\"VirtualGeometryDebug\")");
                    static const char* const s_DebugModes[] = { "Off", "Cluster ID", "LOD level", "Overdraw" };
                    int debugMode = static_cast<int>(vgRegistry.GetDebugMode());
                    if (ImGui::Combo("Debug view##VirtualGeom", &debugMode, s_DebugModes, IM_ARRAYSIZE(s_DebugModes)))
                    {
                        vgRegistry.SetDebugMode(static_cast<VirtualDebugMode>(debugMode));
                    }
                }
            }
        }

        // RendererProfiler key counters
        auto const& profilerFrame = RendererProfiler::GetInstance().GetCurrentFrameData();
        if (ImGui::CollapsingHeader("Renderer Profiler"))
        {
            ImGui::Text("Frame Time: %.2f ms", profilerFrame.m_FrameTime);
            ImGui::Text("CPU Time: %.2f ms", profilerFrame.m_CPUTime);
            ImGui::Text("State Changes: %u", profilerFrame.m_StateChanges);
            ImGui::Text("Buffer Binds: %u", profilerFrame.m_BufferBinds);
            ImGui::Text("Vertices Rendered: %u", profilerFrame.m_VerticesRendered);
            ImGui::Text("Triangles Rendered: %u", profilerFrame.m_TrianglesRendered);
            ImGui::Text("Command Packets: %u", profilerFrame.m_CommandPackets);
        }
    }

    void StatisticsPanel::DrawAudioTab()
    {
        OLO_PROFILE_FUNCTION();

        auto const stats = AudioEngine::GetStats();

        ImGui::Text("Sample Rate: %u Hz", stats.SampleRate);
        ImGui::Text("Audio Thread: %s", stats.AudioThreadRunning ? "Running" : "Stopped");
        ImGui::TextColored(stats.ReverbAvailable ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Reverb: %s", stats.ReverbAvailable ? "Available" : "Unavailable");
        ImGui::TextColored(stats.SpatializerAvailable ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Spatializer: %s", stats.SpatializerAvailable ? "Available" : "Unavailable");

        // Count audio source entities in the scene
        if (m_Context)
        {
            auto audioSources = m_Context->GetAllEntitiesWith<AudioSourceComponent>();
            auto audioListeners = m_Context->GetAllEntitiesWith<AudioListenerComponent>();
            ImGui::Separator();
            ImGui::Text("Audio Sources: %u", static_cast<u32>(audioSources.size()));
            ImGui::Text("Audio Listeners: %u", static_cast<u32>(audioListeners.size()));
        }
    }

    void StatisticsPanel::DrawPerformanceTab() const
    {
        OLO_PROFILE_FUNCTION();

        auto const& frameData = Application::Get().GetProfilerPreviousFrameData();

        if (frameData.empty())
        {
            ImGui::TextDisabled("No performance data yet.");
            return;
        }

        // Sort by time descending for readability
        std::vector<std::pair<std::string, PerFrameData>> sorted(frameData.begin(), frameData.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b)
                          { return a.second.Time > b.second.Time; });

        // Table header
        if (ImGui::BeginTable("PerfTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();

            for (auto const& [name, data] : sorted)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", data.Time);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", data.Samples);
            }

            ImGui::EndTable();
        }
    }

    void StatisticsPanel::DrawMemoryTab() const
    {
        OLO_PROFILE_FUNCTION();

        const auto& tracker = RendererMemoryTracker::GetInstance();

        sizet const totalMemory = tracker.GetTotalMemoryUsage();
        ImGui::Text("Total GPU/CPU Tracked: %s", DebugUtils::FormatMemorySize(totalMemory).c_str());

        // Mesh asset cache on disk
        {
            auto cacheSize = MeshCache::GetTotalCacheSize();
            ImGui::Text("Mesh Cache (disk): %s", DebugUtils::FormatMemorySize(cacheSize).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##MeshCache"))
            {
                MeshCache::ClearCache();
            }
        }

        ImGui::Separator();

        // Per-resource-type breakdown
        if (ImGui::BeginTable("MemTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Resource Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (u8 i = 0; i < std::to_underlying(RendererMemoryTracker::ResourceType::COUNT); ++i)
            {
                auto const type = static_cast<RendererMemoryTracker::ResourceType>(i);
                sizet const usage = tracker.GetMemoryUsage(type);
                u32 const count = tracker.GetAllocationCount(type);
                if (count == 0 && usage == 0)
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                static constexpr const char* s_TypeNames[] = {
                    "Vertex Buffer", "Index Buffer", "Uniform Buffer", "Storage Buffer",
                    "Texture 2D", "Texture Cubemap", "Framebuffer", "Shader",
                    "Render Target", "Command Buffer", "Other"
                };
                static_assert(std::size(s_TypeNames) == static_cast<size_t>(std::to_underlying(RendererMemoryTracker::ResourceType::COUNT)),
                              "s_TypeNames must match RendererMemoryTracker::ResourceType::COUNT");
                ImGui::TextUnformatted(s_TypeNames[i]);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", DebugUtils::FormatMemorySize(usage).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", count);
            }

            ImGui::EndTable();
        }

        // Leak detection summary
        auto const leaks = tracker.DetectLeaks();
        if (!leaks.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Potential Leaks: %u", static_cast<u32>(leaks.size()));
        }
    }

} // namespace OloEngine
