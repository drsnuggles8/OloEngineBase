#pragma once

#include "OloEngine/Core/Base.h"

#include <compare>
#include <string>

namespace OloEngine
{
    // One scene view of the 3D renderer (ADR 0026, issue #1352): a camera and
    // everything measured against it. View 0 is the main view, which exists
    // for as long as the renderer does. Every other id is handed out by
    // Renderer3D::CreateView and is never reused within one renderer run, so
    // a handle that outlived its view names nothing instead of naming the view
    // created after it. The same number is the `View` of every
    // TemporalHistoryKey the view's render graph acquires.
    struct RenderViewId
    {
        u64 Value = 0;

        [[nodiscard]] constexpr bool IsMain() const
        {
            return Value == 0;
        }

        auto operator<=>(const RenderViewId&) const = default;
    };

    inline constexpr RenderViewId kMainRenderView{};

    // What a new view starts with. The size is the display size of the view's
    // render graph, as Renderer3D::OnWindowResize takes it for the main view.
    // Its renderer, post-process and shadow settings start as a copy of the
    // settings of the view that is active when it is created.
    struct RenderViewDesc
    {
        u32 Width = 0;
        u32 Height = 0;
        std::string DebugName;
    };

    // What the renderer knows about one view, for diagnostics and tests.
    struct RenderViewInfo
    {
        RenderViewId Id{};
        std::string DebugName;
        // Display size of the view's render graph.
        u32 Width = 0;
        u32 Height = 0;
        // Scene frames this view has rendered since it was created.
        u64 FramesRendered = 0;
        // The physical frame (Renderer3D::GetPhysicalFrameSerial) it last
        // rendered in, and whether it was that frame's primary view: the first
        // one begun, which advances the scene for every view of the frame.
        u64 LastRenderedFrameSerial = 0;
        bool WasPrimary = false;
        bool Active = false;
        // Wall time of the view's last scene frame, BeginScene to the end of
        // EndScene: submission, the graph, and for a primary view the frame's
        // scene extraction and world simulation as well. The per-view half of
        // "what does a second view cost".
        f64 LastFrameCpuMs = 0.0;
    };
} // namespace OloEngine
