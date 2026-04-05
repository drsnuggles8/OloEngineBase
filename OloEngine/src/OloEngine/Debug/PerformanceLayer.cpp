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
#include <cmath>
#include <cfloat>

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
        if (!m_Visible || m_Paused)
        {
            return;
        }

        m_CurrentFrameTime = ts.GetMilliseconds();
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
        for (u32 i = 0; i < m_FrameHistoryCount; ++i)
        {
            m_FrameTimeMin = std::min(m_FrameTimeMin, m_FrameTimeHistory[i]);
            m_FrameTimeMax = std::max(m_FrameTimeMax, m_FrameTimeHistory[i]);
        }
    }

    void PerformanceLayer::OnImGuiRender()
    {
        if (!m_Visible)
        {
            return;
        }

        DrawPerformanceWindow();
    }

    void PerformanceLayer::OnEvent(Event& e)
    {
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(PerformanceLayer::OnKeyPressed));
    }

    bool PerformanceLayer::OnKeyPressed(KeyPressedEvent const& e)
    {
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
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoMove;

        constexpr f32 padding = 10.0f;
        auto const* viewport = ImGui::GetMainViewport();
        auto workPos = viewport->WorkPos;
        auto workSize = viewport->WorkSize;

        // Position in top-right corner
        ImGui::SetNextWindowPos(
            ImVec2(workPos.x + workSize.x - padding, workPos.y + padding),
            ImGuiCond_Always,
            ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.65f);

        if (ImGui::Begin("##PerformanceOverlay", nullptr, windowFlags))
        {
            ImGui::TextColored(DebugUtils::Colors::Info, "Performance (F4)");

            if (ImGui::SmallButton(m_Paused ? "Resume" : "Pause"))
            {
                m_Paused = !m_Paused;
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

    void PerformanceLayer::DrawFrameTimeGraph()
    {
        // FPS header
        auto const fpsColor = DebugUtils::GetPerformanceColor(m_CurrentFrameTime, 16.67f, 33.33f);
        ImGui::TextColored(fpsColor, "%.1f FPS (%.2f ms)", m_CurrentFPS, m_CurrentFrameTime);
        ImGui::Text("Min: %.2f ms  Max: %.2f ms", m_FrameTimeMin, m_FrameTimeMax);

        // Frame time graph using ring buffer
        // ImGui::PlotLines needs a contiguous float array and a getter, or we reorder
        if (m_FrameHistoryCount > 0)
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

    void PerformanceLayer::DrawTimingBreakdown()
    {
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

    void PerformanceLayer::DrawMemoryStats()
    {
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
            { "VBOs",     RT::VertexBuffer },
            { "IBOs",     RT::IndexBuffer },
            { "UBOs",     RT::UniformBuffer },
            { "FBOs",     RT::Framebuffer },
            { "Shaders",  RT::Shader },
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
        auto const* app = Application::TryGet();
        if (!app)
        {
            return;
        }

        auto const& profileData = app->GetProfilerPreviousFrameData();
        if (profileData.empty())
        {
            return;
        }

        ImGui::Text("CPU Scopes:");

        // Sort by time descending, show top entries
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

        std::ranges::sort(entries, [](auto const& a, auto const& b) { return a.Time > b.Time; });

        constexpr u32 maxShown = 8;
        auto const count = std::min(static_cast<u32>(entries.size()), maxShown);
        for (u32 i = 0; i < count; ++i)
        {
            auto const& entry = entries[i];
            auto const color = DebugUtils::GetPerformanceColor(entry.Time, 2.0f, 8.0f);
            if (entry.Samples > 1)
            {
                ImGui::TextColored(color, "  %s: %.2fms (%ux)", entry.Name->c_str(), entry.Time, entry.Samples);
            }
            else
            {
                ImGui::TextColored(color, "  %s: %.2fms", entry.Name->c_str(), entry.Time);
            }
        }

        if (entries.size() > maxShown)
        {
            ImGui::TextDisabled("  ... +%zu more", entries.size() - maxShown);
        }
    }
} // namespace OloEngine
