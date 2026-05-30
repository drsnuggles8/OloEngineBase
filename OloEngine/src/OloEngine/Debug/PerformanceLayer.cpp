#include "OloEnginePCH.h"
#include "PerformanceLayer.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
#include "OloEngine/Renderer/Renderer2D.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine
{
    PerformanceLayer::PerformanceLayer()
        : Layer("PerformanceLayer")
    {
    }

    void PerformanceLayer::OnAttach()
    {
        OLO_CORE_INFO("PerformanceLayer attached (toggle with F4)");
    }

    void PerformanceLayer::OnDetach()
    {
    }

    void PerformanceLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Visible || m_Paused)
        {
            return;
        }

        // Use unscaled delta so the FPS/frame-time display reports real wall-clock
        // frame rate regardless of Application::m_TimeScale.
        auto const* app = Application::TryGet();
        f32 const unscaledMs = app ? app->GetUnscaledDeltaTime() * 1000.0f : ts.GetMilliseconds();
        m_CurrentFrameTime = unscaledMs;
        if (m_CurrentFrameTime > 0.0f)
        {
            m_CurrentFPS = 1000.0f / m_CurrentFrameTime;
        }

        // Update frame time ring buffer
        m_FrameTimeHistory[m_FrameHistoryIndex] = m_CurrentFrameTime;
        m_FrameHistoryIndex = (m_FrameHistoryIndex + 1) % s_FrameHistorySize;
        if (m_FrameHistoryCount < s_FrameHistorySize)
        {
            ++m_FrameHistoryCount;
        }

        // Track min/max over the visible history
        m_FrameTimeMin = FLT_MAX;
        m_FrameTimeMax = 0.0f;
        auto const count = m_FrameHistoryCount;
        for (u32 i = 0; i < count; ++i)
        {
            m_FrameTimeMin = std::min(m_FrameTimeMin, m_FrameTimeHistory[i]);
            m_FrameTimeMax = std::max(m_FrameTimeMax, m_FrameTimeHistory[i]);
        }
    }

    void PerformanceLayer::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Visible)
        {
            return;
        }

        DrawPerformanceWindow();
    }

    void PerformanceLayer::OnEvent(Event& e)
    {
        OLO_PROFILE_FUNCTION();

        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(PerformanceLayer::OnKeyPressed));
    }

    bool PerformanceLayer::OnKeyPressed(KeyPressedEvent const& e)
    {
        OLO_PROFILE_FUNCTION();

        if (e.IsRepeat())
        {
            return false;
        }

        if (e.GetKeyCode() == Key::F4)
        {
            m_Visible = !m_Visible;
            return true;
        }

        return false;
    }

    void PerformanceLayer::DrawPerformanceWindow()
    {
        OLO_PROFILE_SCOPE("Performance/DrawPerformanceWindow");

        ImGui::SetNextWindowSize(ImVec2(520.0f, 720.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Performance", &m_Visible))
        {
            ImGui::TextColored(DebugUtils::Colors::Info, "Performance (F4 to toggle)");
            ImGui::SameLine();
            if (ImGui::SmallButton(m_Paused ? "Resume" : "Pause"))
            {
                m_Paused = !m_Paused;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Export Snapshot"))
            {
                m_LastExportPath = ExportSnapshotToFile();
            }
            if (!m_LastExportPath.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("-> %s", m_LastExportPath.c_str());
            }

            ImGui::Separator();
            DrawFrameTimeGraph();
            ImGui::Separator();
            DrawTimingBreakdown();
            ImGui::Separator();
            DrawMemoryStats();
            ImGui::Separator();
            DrawCPUProfileScopes();
        }
        ImGui::End();
    }

    void PerformanceLayer::DrawFrameTimeGraph() const
    {
        OLO_PROFILE_SCOPE("Performance/DrawFrameTimeGraph");

        if (m_FrameHistoryCount == 0)
        {
            ImGui::TextDisabled("-- FPS (-- ms)");
            ImGui::TextDisabled("Min: N/A  Max: N/A");
            return;
        }

        // FPS header
        auto const fpsColor = DebugUtils::GetPerformanceColor(m_CurrentFrameTime, 16.67f, 33.33f);
        ImGui::TextColored(fpsColor, "%.1f FPS (%.2f ms)", m_CurrentFPS, m_CurrentFrameTime);
        ImGui::Text("Min: %.2f ms  Max: %.2f ms", m_FrameTimeMin, m_FrameTimeMax);

        // Frame time graph using ring buffer
        // ImGui::PlotLines needs a contiguous float array and a getter, or we reorder
        {
            // Use a lambda getter that reads from the ring buffer in chronological order
            struct PlotData
            {
                const std::array<f32, s_FrameHistorySize>* History;
                u32 Offset;
                u32 Count;
            };

            PlotData plotData{ &m_FrameTimeHistory, m_FrameHistoryIndex, m_FrameHistoryCount };

            auto getter = [](void* data, int idx) -> f32
            {
                auto const* pd = static_cast<PlotData const*>(data);
                // Read oldest to newest
                auto const ringIdx = (pd->Offset + s_FrameHistorySize - pd->Count + static_cast<u32>(idx)) % s_FrameHistorySize;
                return (*pd->History)[ringIdx];
            };

            // Scale: show at least up to 33ms, but auto-expand if needed
            auto const scaleMax = std::max(33.33f, m_FrameTimeMax * 1.1f);

            ImGui::PlotLines("##FrameTime", getter, &plotData,
                             static_cast<int>(m_FrameHistoryCount), 0,
                             nullptr, 0.0f, scaleMax, ImVec2(280.0f, 60.0f));

            // Reference lines
            ImGui::TextDisabled("--- 16.67ms (60fps) --- 33.33ms (30fps)");
        }
    }

    void PerformanceLayer::DrawTimingBreakdown() const
    {
        OLO_PROFILE_SCOPE("Performance/DrawTimingBreakdown");

#ifdef OLO_DEBUG
        auto const& frameData = RendererProfiler::GetInstance().GetCurrentFrameData();

        ImGui::Text("Timing Breakdown:");

        auto const cpuColor = DebugUtils::GetPerformanceColor(
            static_cast<f32>(frameData.m_CPUTime), 8.0f, 16.0f);
        auto const gpuColor = DebugUtils::GetPerformanceColor(
            static_cast<f32>(frameData.m_GPUTime), 8.0f, 16.0f);

        ImGui::TextColored(cpuColor, "  CPU: %s", DebugUtils::FormatDuration(frameData.m_CPUTime).c_str());
        ImGui::TextColored(gpuColor, "  GPU: %s", DebugUtils::FormatDuration(frameData.m_GPUTime).c_str());

        if (frameData.m_SortingTime > 0.0)
        {
            ImGui::Text("  Sort: %s", DebugUtils::FormatDuration(frameData.m_SortingTime).c_str());
        }
        if (frameData.m_CullingTime > 0.0)
        {
            ImGui::Text("  Cull: %s", DebugUtils::FormatDuration(frameData.m_CullingTime).c_str());
        }

        ImGui::Spacing();
        ImGui::Text("Render Stats:");
        ImGui::Text("  Draw Calls: %u", frameData.m_DrawCalls);
        ImGui::Text("  Triangles:  %u", frameData.m_TrianglesRendered);
        ImGui::Text("  Vertices:   %u", frameData.m_VerticesRendered);
        ImGui::Text("  State Changes: %u", frameData.m_StateChanges);
        ImGui::Text("  Shader Binds:  %u", frameData.m_ShaderBinds);
        ImGui::Text("  Texture Binds: %u", frameData.m_TextureBinds);
        ImGui::Text("  Cmd Packets:   %u", frameData.m_CommandPackets);
#else
        ImGui::TextDisabled("Detailed timing: Debug builds only");
#endif
    }

    void PerformanceLayer::DrawMemoryStats() const
    {
        OLO_PROFILE_SCOPE("Performance/DrawMemoryStats");

#ifdef OLO_DEBUG
        auto& tracker = RendererMemoryTracker::GetInstance();
        auto const totalMem = tracker.GetTotalMemoryUsage();
        if (totalMem == 0)
        {
            return;
        }

        ImGui::Text("GPU Memory:");
        ImGui::Text("  Total: %s", DebugUtils::FormatMemorySize(totalMem).c_str());

        using RT = RendererMemoryTracker::ResourceType;
        struct MemCategory
        {
            const char* Label;
            RT Type;
        };
        static constexpr MemCategory categories[] = {
            { "Textures", RT::Texture2D },
            { "Cubemaps", RT::TextureCubemap },
            { "VBOs", RT::VertexBuffer },
            { "IBOs", RT::IndexBuffer },
            { "UBOs", RT::UniformBuffer },
            { "FBOs", RT::Framebuffer },
            { "Shaders", RT::Shader },
        };

        for (auto const& [label, type] : categories)
        {
            if (auto const mem = tracker.GetMemoryUsage(type); mem > 0)
            {
                ImGui::Text("  %s: %s", label, DebugUtils::FormatMemorySize(mem).c_str());
            }
        }
#else
        ImGui::TextDisabled("Memory stats: Debug builds only");
#endif
    }

    void PerformanceLayer::DrawCPUProfileScopes()
    {
        OLO_PROFILE_SCOPE("Performance/DrawCPUProfileScopes");

        auto const* app = Application::TryGet();
        if (!app)
        {
            return;
        }

        auto const& profileData = app->GetProfilerPreviousFrameData();
        if (profileData.empty())
        {
            ImGui::Text("CPU Scopes: (no data — instrument code with OLO_PERF_SCOPE_AUTO)");
            return;
        }

        struct ScopeEntry
        {
            const std::string* Name;
            f32 Time;
            u32 Samples;
        };

        std::vector<ScopeEntry> entries;
        entries.reserve(profileData.size());
        for (auto const& [name, data] : profileData)
        {
            entries.push_back({ &name, data.Time, data.Samples });
        }

        std::ranges::sort(entries, [](auto const& a, auto const& b)
                          { return a.Time > b.Time; });

        ImGui::Text("CPU Scopes (%zu):", entries.size());
        ImGui::SameLine();
        i32 limit = static_cast<i32>(m_CPUScopeLimit);
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::DragInt("limit", &limit, 1.0f, 4, 256))
            m_CPUScopeLimit = static_cast<u32>(std::max(4, limit));

        if (ImGui::BeginTable("CPUScopes", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 320.0f)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            auto const count = std::min(static_cast<u32>(entries.size()), m_CPUScopeLimit);
            for (u32 i = 0; i < count; ++i)
            {
                auto const& entry = entries[i];
                auto const color = DebugUtils::GetPerformanceColor(entry.Time, 2.0f, 8.0f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(color, "%s", entry.Name->c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(color, "%.3f", entry.Time);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", entry.Samples);
            }
            ImGui::EndTable();
        }

        if (entries.size() > m_CPUScopeLimit)
        {
            ImGui::TextDisabled("  ... +%zu more (raise limit to see)", entries.size() - m_CPUScopeLimit);
        }
    }

    std::string PerformanceLayer::ExportSnapshotToFile() const
    {
        auto const now = std::chrono::system_clock::now();
        auto const time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif

        std::ostringstream filename;
        filename << "perf-snapshot-"
                 << std::put_time(&tm, "%Y%m%d-%H%M%S")
                 << ".txt";

        std::ofstream out(filename.str());
        if (!out)
            return {};

        out << "OloEngine Performance Snapshot\n";
        out << "Captured: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n";
#if defined(OLO_DEBUG)
        out << "Build: Debug\n";
#elif defined(OLO_RELEASE)
        out << "Build: Release\n";
#elif defined(OLO_DIST)
        out << "Build: Distribution\n";
#endif
        out << "----------------------------------------\n\n";

        out << "Frame timing:\n";
        out << "  Current FPS:   " << m_CurrentFPS << "\n";
        out << "  Current frame: " << m_CurrentFrameTime << " ms\n";
        out << "  Min:           " << m_FrameTimeMin << " ms\n";
        out << "  Max:           " << m_FrameTimeMax << " ms\n";
        if (m_FrameHistoryCount > 0)
        {
            f64 sum = 0.0;
            for (u32 i = 0; i < m_FrameHistoryCount; ++i)
                sum += m_FrameTimeHistory[i];
            out << "  Avg (" << m_FrameHistoryCount << " frames): "
                << (sum / m_FrameHistoryCount) << " ms\n";
        }
        out << "\n";

#if defined(OLO_DEBUG)
        {
            auto const& fd = RendererProfiler::GetInstance().GetCurrentFrameData();
            out << "Renderer (current frame):\n";
            out << "  CPU:           " << fd.m_CPUTime << " ms\n";
            out << "  GPU:           " << fd.m_GPUTime << " ms\n";
            out << "  Sort:          " << fd.m_SortingTime << " ms\n";
            out << "  Cull:          " << fd.m_CullingTime << " ms\n";
            out << "  Draw calls:    " << fd.m_DrawCalls << "\n";
            out << "  Triangles:     " << fd.m_TrianglesRendered << "\n";
            out << "  Vertices:      " << fd.m_VerticesRendered << "\n";
            out << "  State changes: " << fd.m_StateChanges << "\n";
            out << "  Shader binds:  " << fd.m_ShaderBinds << "\n";
            out << "  Texture binds: " << fd.m_TextureBinds << "\n";
            out << "  Buffer binds:  " << fd.m_BufferBinds << "\n";
            out << "  Cmd packets:   " << fd.m_CommandPackets << "\n";
            out << "\n";
        }
#endif

        if (auto const* app = Application::TryGet())
        {
            auto const& profileData = app->GetProfilerPreviousFrameData();
            struct ExportEntry
            {
                std::string Name;
                f32 Time;
                u32 Samples;
            };
            std::vector<ExportEntry> entries;
            entries.reserve(profileData.size());
            for (auto const& [name, data] : profileData)
                entries.push_back({ name, data.Time, data.Samples });

            std::ranges::sort(entries, [](auto const& a, auto const& b)
                              { return a.Time > b.Time; });

            out << "CPU scopes (sorted by total time descending, full list):\n";
            out << "  " << std::left;
            // Header
            char header[256];
            std::snprintf(header, sizeof(header),
                          "  %-60s %10s %8s\n", "Scope", "Time (ms)", "Calls");
            out << header;
            out << "  ----------------------------------------------------------------------------------\n";

            for (auto const& e : entries)
            {
                char line[512];
                std::snprintf(line, sizeof(line),
                              "  %-60s %10.4f %8u\n",
                              e.Name.c_str(), e.Time, e.Samples);
                out << line;
            }
        }

        out << "\n----------------------------------------\n";
        out << "Frame time history (oldest -> newest, ms):\n";
        for (u32 i = 0; i < m_FrameHistoryCount; ++i)
        {
            auto const idx = (m_FrameHistoryIndex + s_FrameHistorySize - m_FrameHistoryCount + i) % s_FrameHistorySize;
            if (i > 0 && (i % 10) == 0)
                out << "\n";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%6.2f ", m_FrameTimeHistory[idx]);
            out << buf;
        }
        out << "\n";

        return filename.str();
    }
} // namespace OloEngine
