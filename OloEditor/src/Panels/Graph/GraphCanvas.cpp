#include "OloEnginePCH.h"
#include "GraphCanvas.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::EditorUI
{
    namespace
    {
        // Sample count for the wire hit-test. 16 is enough that a 2px-wide wire
        // never has a gap a mouse can slip through at any zoom we allow, and
        // cheap enough to run for every wire, every frame.
        constexpr i32 kWireHitSamples = 16;

        ImVec2 Bezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, f32 t)
        {
            const f32 u = 1.0f - t;
            const f32 w0 = u * u * u;
            const f32 w1 = 3.0f * u * u * t;
            const f32 w2 = 3.0f * u * t * t;
            const f32 w3 = t * t * t;
            return ImVec2(w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
                          w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y);
        }
    } // namespace

    bool GraphCanvas::Begin(const char* id, ImVec2 size)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, m_Style.m_Background);
        m_Active = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                                     // The canvas owns its own pan; ImGui's
                                     // scrolling would fight it, and a moving
                                     // child scroll silently desyncs every
                                     // screen-space hit-test the panel does.
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
        if (!m_Active)
        {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            return false;
        }

        m_Origin = ImGui::GetCursorScreenPos();
        m_Size = ImGui::GetContentRegionAvail();
        // A zero-height region (the panel collapsed, or a first frame before the
        // dock resolves) makes every transform produce NaNs downstream.
        m_Size.x = std::max(m_Size.x, 1.0f);
        m_Size.y = std::max(m_Size.y, 1.0f);
        m_DrawList = ImGui::GetWindowDrawList();

        // An invisible button over the whole region is what gives the canvas a
        // hover/active state ImGui respects, so a click on empty background is
        // distinguishable from a click that landed on a widget the panel drew.
        ImGui::InvisibleButton("##canvas", m_Size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        m_IsHovered = ImGui::IsItemHovered();

        DrawGrid();
        HandleViewInput();
        return true;
    }

    void GraphCanvas::End()
    {
        if (m_Active)
        {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            m_Active = false;
        }
    }

    ImVec2 GraphCanvas::ToScreen(glm::vec2 graphPos) const
    {
        return ImVec2(m_Origin.x + (graphPos.x + m_Pan.x) * m_Zoom,
                      m_Origin.y + (graphPos.y + m_Pan.y) * m_Zoom);
    }

    glm::vec2 GraphCanvas::ToGraph(ImVec2 screenPos) const
    {
        return glm::vec2((screenPos.x - m_Origin.x) / m_Zoom - m_Pan.x,
                         (screenPos.y - m_Origin.y) / m_Zoom - m_Pan.y);
    }

    void GraphCanvas::HandleViewInput()
    {
        if (!m_IsHovered && !m_IsPanning)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();

        // Pan on middle OR right drag. Right-drag is what most artists reach for;
        // middle exists because some mice/tablets have no usable right-drag.
        //
        // Panning starts only once the pointer has actually MOVED past a small
        // threshold, never on the press itself. That distinction is the whole
        // reason a right-CLICK can still open the panel's context menu: consumers
        // suppress their own input handling while IsPanning() is true, so latching
        // it on press would swallow every right-click before its release was ever
        // seen. (It did — the node-search menu could not be opened at all.)
        const bool panHeld = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (!panHeld)
        {
            m_PanButtonDown = false;
            m_IsPanning = false;
        }
        else if (!m_PanButtonDown && m_IsHovered)
        {
            m_PanButtonDown = true;
            m_PanPressPos = io.MousePos;
        }

        if (m_PanButtonDown && !m_IsPanning)
        {
            const f32 dx = io.MousePos.x - m_PanPressPos.x;
            const f32 dy = io.MousePos.y - m_PanPressPos.y;
            if ((dx * dx + dy * dy) > (s_PanDragThreshold * s_PanDragThreshold))
            {
                m_IsPanning = true;
            }
        }

        if (m_IsPanning)
        {
            // Divide by zoom: the drag is a SCREEN delta, the pan is in graph
            // space, so a zoomed-out canvas must move further per pixel or the
            // background visibly lags the cursor.
            m_Pan.x += io.MouseDelta.x / m_Zoom;
            m_Pan.y += io.MouseDelta.y / m_Zoom;
            if (!panHeld)
            {
                m_IsPanning = false;
            }
        }

        if (m_IsHovered && std::fabs(io.MouseWheel) > 0.0f)
        {
            // Zoom about the cursor, not the origin: anchoring to the corner
            // makes a zoom-in walk the thing you were looking at off-screen.
            const glm::vec2 anchor = ToGraph(io.MousePos);
            const f32 factor = io.MouseWheel > 0.0f ? s_ZoomStep : 1.0f / s_ZoomStep;
            m_Zoom = std::clamp(m_Zoom * factor, s_MinZoom, s_MaxZoom);
            const glm::vec2 after = ToGraph(io.MousePos);
            m_Pan += after - anchor;
        }
    }

    void GraphCanvas::DrawGrid() const
    {
        const f32 step = m_Style.m_GridSize * m_Zoom;
        if (step < 4.0f)
        {
            // Below a few pixels the grid is solid noise; drawing it would also
            // push thousands of lines into the draw list at low zoom.
            return;
        }

        const ImVec2 end(m_Origin.x + m_Size.x, m_Origin.y + m_Size.y);
        const f32 offsetX = std::fmod(m_Pan.x * m_Zoom, step);
        const f32 offsetY = std::fmod(m_Pan.y * m_Zoom, step);

        // Index the line against graph-space coordinates, not screen position, so
        // the major lines stay on the same graph gridlines while panning.
        // Must apply the SAME transform ToGraph does, pan included. Dropping the
        // pan term left the major lines indexed off screen position, so they
        // slid across the minor grid as the canvas panned instead of staying
        // locked to fixed graph-space gridlines.
        const auto majorIndex = [this](f32 screenCoord, f32 originCoord, f32 pan)
        {
            const f32 graphCoord = (screenCoord - originCoord) / m_Zoom - pan;
            return static_cast<i64>(std::llround(graphCoord / m_Style.m_GridSize));
        };

        // Integer step counters rather than `for (f32 x = ...; x += step)`: an
        // accumulated float counter drifts, so the last gridlines on a wide canvas
        // creep off the graph-space lattice the major-line index is computed from.
        // `step` is >= 4.0f by the early return above, so neither count can run away.
        const f32 startX = m_Origin.x + offsetX;
        const f32 startY = m_Origin.y + offsetY;
        const i32 columns = static_cast<i32>(std::ceil((end.x - startX) / step));
        const i32 rows = static_cast<i32>(std::ceil((end.y - startY) / step));

        for (i32 i = 0; i < columns; ++i)
        {
            const f32 x = startX + static_cast<f32>(i) * step;
            const bool major = m_Style.m_MajorEvery > 0 && (majorIndex(x, m_Origin.x, m_Pan.x) % static_cast<i64>(m_Style.m_MajorEvery)) == 0;
            m_DrawList->AddLine(ImVec2(x, m_Origin.y), ImVec2(x, end.y), major ? m_Style.m_GridLineMajor : m_Style.m_GridLine);
        }
        for (i32 i = 0; i < rows; ++i)
        {
            const f32 y = startY + static_cast<f32>(i) * step;
            const bool major = m_Style.m_MajorEvery > 0 && (majorIndex(y, m_Origin.y, m_Pan.y) % static_cast<i64>(m_Style.m_MajorEvery)) == 0;
            m_DrawList->AddLine(ImVec2(m_Origin.x, y), ImVec2(end.x, y), major ? m_Style.m_GridLineMajor : m_Style.m_GridLine);
        }
    }

    void GraphCanvas::CenterOn(glm::vec2 graphPos)
    {
        m_Pan.x = (m_Size.x * 0.5f) / m_Zoom - graphPos.x;
        m_Pan.y = (m_Size.y * 0.5f) / m_Zoom - graphPos.y;
    }

    void GraphCanvas::FitToBounds(glm::vec2 min, glm::vec2 max, f32 margin)
    {
        const glm::vec2 span = max - min;
        if (span.x <= 1.0f || span.y <= 1.0f || m_Size.x <= 1.0f || m_Size.y <= 1.0f)
        {
            // A single node (or none) has no meaningful extent — dividing by it
            // would produce an infinite zoom and a blank canvas.
            m_Zoom = 1.0f;
            CenterOn((min + max) * 0.5f);
            return;
        }

        const f32 zoomX = (m_Size.x - margin * 2.0f) / span.x;
        const f32 zoomY = (m_Size.y - margin * 2.0f) / span.y;
        m_Zoom = std::clamp(std::min(zoomX, zoomY), s_MinZoom, s_MaxZoom);
        CenterOn((min + max) * 0.5f);
    }

    void GraphCanvas::ResetView()
    {
        m_Zoom = 1.0f;
        m_Pan = glm::vec2(40.0f, 40.0f);
    }

    ImVec2 GraphCanvas::ControlOffset(ImVec2 from, ImVec2 to) const
    {
        // Horizontal tangents proportional to the horizontal gap, so a
        // near-vertical wire still leaves its pin sideways instead of kinking.
        const f32 dx = std::fabs(to.x - from.x);
        const f32 strength = std::clamp(dx * 0.5f, 30.0f * m_Zoom, 160.0f * m_Zoom);
        return ImVec2(strength, 0.0f);
    }

    void GraphCanvas::DrawWire(ImVec2 from, ImVec2 to, ImU32 color, f32 thickness) const
    {
        const ImVec2 offset = ControlOffset(from, to);
        m_DrawList->AddBezierCubic(from, ImVec2(from.x + offset.x, from.y),
                                   ImVec2(to.x - offset.x, to.y), to, color, thickness);
    }

    void GraphCanvas::DrawDirectionalWire(ImVec2 from, ImVec2 to, ImU32 color, f32 thickness) const
    {
        DrawWire(from, to, color, thickness);

        const ImVec2 offset = ControlOffset(from, to);
        const ImVec2 c0(from.x + offset.x, from.y);
        const ImVec2 c1(to.x - offset.x, to.y);
        const ImVec2 mid = Bezier(from, c0, c1, to, 0.5f);
        const ImVec2 ahead = Bezier(from, c0, c1, to, 0.54f);

        ImVec2 dir(ahead.x - mid.x, ahead.y - mid.y);
        const f32 length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (length < 1e-4f)
        {
            // Degenerate tangent (from == to, e.g. a wire being dragged back onto
            // its own pin): drawing an arrow from a zero vector produces NaNs.
            return;
        }
        dir.x /= length;
        dir.y /= length;
        const ImVec2 normal(-dir.y, dir.x);

        const f32 size = std::max(thickness * 2.2f, 5.0f);
        const ImVec2 tip(mid.x + dir.x * size, mid.y + dir.y * size);
        const ImVec2 left(mid.x - dir.x * size * 0.4f + normal.x * size * 0.7f,
                          mid.y - dir.y * size * 0.4f + normal.y * size * 0.7f);
        const ImVec2 right(mid.x - dir.x * size * 0.4f - normal.x * size * 0.7f,
                           mid.y - dir.y * size * 0.4f - normal.y * size * 0.7f);
        m_DrawList->AddTriangleFilled(tip, left, right, color);
    }

    f32 GraphCanvas::DistanceToWire(ImVec2 from, ImVec2 to, ImVec2 point) const
    {
        // Identical control points to DrawWire — see the header.
        const ImVec2 offset = ControlOffset(from, to);
        const ImVec2 c0(from.x + offset.x, from.y);
        const ImVec2 c1(to.x - offset.x, to.y);

        f32 best = std::numeric_limits<f32>::max();
        for (i32 i = 0; i <= kWireHitSamples; ++i)
        {
            const ImVec2 p = Bezier(from, c0, c1, to, static_cast<f32>(i) / static_cast<f32>(kWireHitSamples));
            const f32 ddx = p.x - point.x;
            const f32 ddy = p.y - point.y;
            best = std::min(best, std::sqrt(ddx * ddx + ddy * ddy));
        }
        return best;
    }

} // namespace OloEngine::EditorUI
