#include "StatisticsPanel.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Scene/Components.h"

#include <glad/gl.h>
#include <imgui.h>

#include <algorithm>
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

    void StatisticsPanel::DrawRendererTab()
    {
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

    void StatisticsPanel::DrawPerformanceTab()
    {
        auto const& frameData = Application::Get().GetProfilerPreviousFrameData();

        if (frameData.empty())
        {
            ImGui::TextDisabled("No performance data yet.");
            return;
        }

        // Sort by time descending for readability
        std::vector<std::pair<std::string, PerFrameData>> sorted(frameData.begin(), frameData.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
        {
            return a.second.Time > b.second.Time;
        });

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

    void StatisticsPanel::DrawMemoryTab()
    {
        auto& tracker = RendererMemoryTracker::GetInstance();

        sizet const totalMemory = tracker.GetTotalMemoryUsage();
        ImGui::Text("Total GPU/CPU Tracked: %s", DebugUtils::FormatMemorySize(totalMemory).c_str());

        ImGui::Separator();

        // Per-resource-type breakdown
        if (ImGui::BeginTable("MemTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Resource Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (u8 i = 0; i < static_cast<u8>(RendererMemoryTracker::ResourceType::COUNT); ++i)
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
                // Use the enum value name as a simple label
                static constexpr const char* s_TypeNames[] = {
                    "Vertex Buffer", "Index Buffer", "Uniform Buffer", "Storage Buffer",
                    "Texture 2D", "Texture Cubemap", "Framebuffer", "Shader",
                    "Render Target", "Command Buffer", "Other"
                };
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
