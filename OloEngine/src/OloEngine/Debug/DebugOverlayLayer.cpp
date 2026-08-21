#include "OloEnginePCH.h"
#include "DebugOverlayLayer.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

namespace OloEngine
{
    DebugOverlayLayer::DebugOverlayLayer()
        : Layer("DebugOverlayLayer")
    {
    }

    void DebugOverlayLayer::OnAttach()
    {
        OLO_CORE_INFO("DebugOverlayLayer attached (toggle with F3)");
    }

    void DebugOverlayLayer::OnDetach()
    {
    }

    void DebugOverlayLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Visible)
        {
            return;
        }

        // Use unscaled delta so the FPS/frame-time HUD reports real wall-clock
        // frame rate regardless of Application::m_TimeScale.
        auto const* app = Application::TryGet();
        f32 const unscaledMs = app ? app->GetUnscaledDeltaTime() * 1000.0f : ts.GetMilliseconds();
        m_FrameTime = unscaledMs;
        if (m_FrameTime > 0.0f)
        {
            m_FPS = 1000.0f / m_FrameTime;
        }
    }

    void DebugOverlayLayer::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Visible)
        {
            return;
        }

        DrawOverlayHUD();
    }

    void DebugOverlayLayer::OnEvent(Event& e)
    {
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(DebugOverlayLayer::OnKeyPressed));
    }

    bool DebugOverlayLayer::OnKeyPressed(KeyPressedEvent const& e)
    {
        if (e.IsRepeat())
        {
            return false;
        }

        if (e.GetKeyCode() == Key::F3)
        {
            m_Visible = !m_Visible;
            return true;
        }

        return false;
    }

    void DebugOverlayLayer::DrawOverlayHUD()
    {
        OLO_PROFILE_SCOPE("DebugOverlay/DrawOverlayHUD");

        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

        constexpr f32 padding = 10.0f;
        auto const* viewport = ImGui::GetMainViewport();
        auto workPos = viewport->WorkPos;

        ImGui::SetNextWindowPos(ImVec2(workPos.x + padding, workPos.y + padding), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.65f);

        if (ImGui::Begin("##DebugOverlay", nullptr, windowFlags))
        {
            ImGui::TextColored(DebugUtils::Colors::Info, "Debug Overlay (F3)");
            ImGui::Separator();

            DrawQuickStats();
            ImGui::Separator();
            DrawVisualizationToggles();
        }
        ImGui::End();
    }

    void DebugOverlayLayer::DrawQuickStats() const
    {
        OLO_PROFILE_SCOPE("DebugOverlay/DrawQuickStats");

        // Frame time / FPS
        auto const fpsColor = DebugUtils::GetPerformanceColor(m_FrameTime, 16.67f, 33.33f);
        ImGui::TextColored(fpsColor, "%.1f FPS (%.2f ms)", m_FPS, m_FrameTime);

        // Renderer stats from RendererProfiler
#ifdef OLO_DEBUG
        auto const& frameData = RendererProfiler::GetInstance().GetCurrentFrameData();
        ImGui::Text("Draw Calls: %u", frameData.m_DrawCalls);
        ImGui::Text("Triangles:  %u", frameData.m_TrianglesRendered);
        ImGui::Text("State Changes: %u", frameData.m_StateChanges);
#endif

        // Renderer2D stats
        if (auto stats = Renderer2D::GetStats(); stats.DrawCalls > 0)
        {
            ImGui::Text("2D Quads: %u (%u draws)", stats.QuadCount, stats.DrawCalls);
        }

        // Memory usage
#ifdef OLO_DEBUG
        auto const totalMem = RendererMemoryTracker::GetInstance().GetTotalMemoryUsage();
        if (totalMem > 0)
        {
            ImGui::Text("GPU Mem: %s", DebugUtils::FormatMemorySize(totalMem).c_str());
        }
#endif
    }

    void DebugOverlayLayer::DrawVisualizationToggles() const
    {
        OLO_PROFILE_SCOPE("DebugOverlay/DrawVisualizationToggles");

        ImGui::Text("Visualization:");

        auto& settings = Renderer3D::GetRendererSettings();

        ImGui::Checkbox("Wireframe", &settings.WireframeOverlay);
        ImGui::Checkbox("Physics Colliders", &settings.ShowPhysicsColliders);
        ImGui::Checkbox("Light Gizmos", &settings.ShowLightGizmos);
        ImGui::Checkbox("Grid", &settings.ShowGrid);
        ImGui::Checkbox("Bounding Boxes", &settings.ShowBoundingBoxes);

        // GPU-pushable shader debug draws (issue #725). Surfaced here as well as
        // in the Renderer Settings panel because the F3 overlay is the one that
        // is reachable in a shipped/runtime build, and this is exactly the
        // instrument you want when a GPU-driven pass is misbehaving there.
        ImGui::Checkbox("Shader Debug Draws", &settings.ShaderDebugDrawEnabled);
        if (settings.ShaderDebugDrawEnabled)
        {
            // Overflow is the failure the feature exists to make visible, so it
            // is reported here even though the full per-channel table lives in
            // the settings panel.
            if (const auto& stats = ShaderDebugDraw::GetStats(); stats.StatsValid)
            {
                for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
                {
                    if (const auto& channel = stats.Channels[i]; channel.Overflowed())
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "  %s overflow: %u dropped",
                                           ShaderDebugDrawContract::Name(static_cast<ShaderDebugDrawPrimitive>(i)),
                                           channel.Dropped());
                    }
                }
            }
        }

        // GPU readback-stats channel (issue #721). Same argument as the debug
        // draws above for living on the F3 overlay: this is the surface that
        // exists in a runtime build, and an overflow flag nobody can see in the
        // build where it fires is not a diagnostic.
        //
        // OVERFLOWS FIRST, unconditionally, and the counters behind a collapsing
        // header. The counters are context; the flags are the thing you needed to
        // know and did not, so they are not allowed to be one click away.
        ImGui::Checkbox("GPU Readback Stats", &settings.GPUReadbackStatsEnabled);
        if (settings.GPUReadbackStatsEnabled)
        {
            if (const auto& frame = GPUReadbackStats::GetLatest(); frame.Valid)
            {
                for (u32 i = 0; i < kGPUStatFlagCount; ++i)
                {
                    const auto flag = static_cast<GPUStatFlag>(i);
                    if (!frame.Overflowed(flag))
                        continue;
                    // %.*s, not %s: the registry hands back std::string_view, and
                    // although every one of them is currently a string literal
                    // (so it happens to be NUL-terminated), .data() on a
                    // string_view is not a contract you get to rely on.
                    const auto desc = GPUStatFlagDescription(flag);
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "  OVERFLOW: %.*s",
                                       static_cast<int>(desc.size()), desc.data());
                }

                if (ImGui::TreeNode("GPU counters"))
                {
                    // The frame index and latency are shown next to the numbers
                    // rather than in a tooltip. A counter quoted without them
                    // cannot be told apart from a counter that has stopped
                    // updating, and "the number looked plausible" is how a
                    // diagnostic channel wastes an afternoon.
                    ImGui::Text("frame %llu (%u frames late)", static_cast<unsigned long long>(frame.FrameIndex),
                                frame.Latency);
                    // A saturated ring means every slot is still executing, so
                    // captures are being skipped and the numbers above are older
                    // than `Latency` last managed to report. Worth a line: it is
                    // the one state where the channel is quietly less fresh than
                    // it claims.
                    if (const u32 inFlight = GPUReadbackStats::GetSlotsInFlight();
                        inFlight >= GPUReadbackStats::kRingSlots)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "ring saturated (%u in flight)", inFlight);
                    }
                    for (u32 i = 0; i < kGPUStatCounterCount; ++i)
                    {
                        const auto counter = static_cast<GPUStatCounter>(i);
                        const auto name = GPUStatCounterName(counter);
                        ImGui::Text("%-32.*s %u", static_cast<int>(name.size()), name.data(), frame.Get(counter));
                    }
                    ImGui::TreePop();
                }
            }
            else
            {
                ImGui::TextDisabled("  waiting for the first readback...");
            }
        }

        ImGui::BeginDisabled();
        bool showEntityNames = false;
        ImGui::Checkbox("Entity Names", &showEntityNames);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Not yet implemented");
        }
        ImGui::EndDisabled();
    }
} // namespace OloEngine
