#include "OloEnginePCH.h"
#include "DebugOverlayLayer.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
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

        // Observer camera (issue #726). Here as well as in the Renderer Settings
        // panel for the same reason as the debug draws above: the F3 overlay is
        // what a shipped/runtime build has, and "is this thing culled or just
        // off-screen?" is a runtime question too.
        ImGui::Checkbox("Observer Camera (freeze culling)", &settings.ObserverCameraEnabled);
        if (settings.ObserverCameraEnabled)
        {
            const glm::vec3& cullPos = Renderer3D::GetCullViewPosition();
            ImGui::Text("  frozen at %.1f, %.1f, %.1f", cullPos.x, cullPos.y, cullPos.z);
            if (!settings.ShaderDebugDrawEnabled)
                ImGui::TextDisabled("  (enable Shader Debug Draws to see the frustum)");
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
