#include "OloEnginePCH.h"
#include "DebugOverlayLayer.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/DebugUtils.h"
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
