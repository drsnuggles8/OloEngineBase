#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <imgui.h>

namespace OloEngine::EditorUI
{
    //==============================================================================
    /// The view half of a node-graph canvas: pan, zoom, the background grid, the
    /// two coordinate transforms, and wire drawing.
    ///
    /// Four panels share this widget: `VisualScriptEditorPanel` (its first
    /// consumer), `ShaderGraphEditorPanel` (migrated in #1103), and
    /// `SkillTreeEditorPanel` and `DialogueEditorPanel` (#1070). One still
    /// carries a private copy of exactly what is in this file and is what
    /// remains of the migration (#1070): `SoundGraphEditorPanel`.
    ///
    /// **Three panels that earlier counts listed here are not canvases and have
    /// nothing to migrate**, so do not go looking for viewport maths in them:
    /// `AnimationGraphEditorPanel` is a tabbed form over parameters, states and
    /// layers whose states are `ImGui::Selectable` rows; `FSMEditorPanel` and
    /// `BehaviorTreeEditorPanel` are read-only `TreeNode` inspectors. None of the
    /// three declares a pan or zoom, draws a grid, or draws a bezier wire. The
    /// original count of seven came from ranking panels by line count rather than
    /// by what they draw — see
    /// docs/agent-rules/notes-editor-and-assets.md.
    ///
    /// **This widget is new code, not an extraction.** Refactoring the remaining
    /// panels onto it is a separate, riskier change, and it is thinly covered:
    /// NO test references `GraphCanvas` at all, and the only test near the
    /// migration is `ShaderGraphCommandTest.cpp`, which covers that panel's
    /// command/undo layer rather than any viewport maths. So a migration is
    /// verified by driving the live editor, not by a green suite — tracked as
    /// its own item, so it can happen one panel at a time against a widget that
    /// already has real consumers.
    ///
    /// Deliberately owns NO graph data. Node layout, hit-testing, selection,
    /// dragging and link semantics are the panel's, because they are where graph
    /// editors genuinely differ; what they share is the viewport maths, and that
    /// is all this holds.
    class GraphCanvas
    {
      public:
        struct Style
        {
            f32 m_GridSize = 32.0f;
            ImU32 m_GridLine = IM_COL32(60, 60, 70, 90);
            ImU32 m_GridLineMajor = IM_COL32(80, 80, 95, 130);
            ImU32 m_Background = IM_COL32(28, 28, 33, 255);
            u32 m_MajorEvery = 5;
        };

        /// Opens a child region of `size` (0 = fill available), paints the
        /// background and grid, and consumes pan/zoom input. Must be paired with
        /// End(). Returns false when the region is clipped away — skip drawing.
        bool Begin(const char* id, ImVec2 size = ImVec2(0.0f, 0.0f));
        void End();

        //-- Coordinates ----------------------------------------------------------
        /// Graph space is what the asset stores; screen space is what ImGui draws
        /// in. Every panel gets these two wrong at least once, which is most of
        /// why they are here rather than re-derived per panel.
        [[nodiscard]] ImVec2 ToScreen(glm::vec2 graphPos) const;
        [[nodiscard]] glm::vec2 ToGraph(ImVec2 screenPos) const;
        /// A length in graph space scaled to screen space.
        [[nodiscard]] f32 Scaled(f32 graphLength) const
        {
            return graphLength * m_Zoom;
        }

        [[nodiscard]] f32 GetZoom() const
        {
            return m_Zoom;
        }
        [[nodiscard]] glm::vec2 GetPan() const
        {
            return m_Pan;
        }
        void SetPan(glm::vec2 pan)
        {
            m_Pan = pan;
        }
        [[nodiscard]] ImVec2 GetOrigin() const
        {
            return m_Origin;
        }
        [[nodiscard]] ImVec2 GetSize() const
        {
            return m_Size;
        }
        [[nodiscard]] ImDrawList* GetDrawList() const
        {
            return m_DrawList;
        }

        /// True while the user is middle/right-dragging the background. The panel
        /// must not start its own drag in that frame.
        [[nodiscard]] bool IsPanning() const
        {
            return m_IsPanning;
        }
        /// True when the pointer is over the canvas and not over any panel widget
        /// the panel itself declared with ImGui.
        [[nodiscard]] bool IsHovered() const
        {
            return m_IsHovered;
        }
        /// True for the single frame in which a right press-and-release over the
        /// canvas finished WITHOUT having become a pan. This is how a consumer
        /// opens its context menu: because the canvas pans on right-drag, the two
        /// gestures share a button and only the widget knows which one happened —
        /// it owns the drag threshold that separates them.
        ///
        /// Every consumer needs this, so it lives here rather than being
        /// re-derived per panel against ImGui's internal drag bookkeeping, which
        /// has moved between versions.
        [[nodiscard]] bool WasRightClicked() const
        {
            return m_WasRightClicked;
        }

        //-- View controls --------------------------------------------------------
        void CenterOn(glm::vec2 graphPos);
        /// Fits `min`..`max` (graph space) into the current viewport with a margin.
        /// A degenerate box (a single node, or none) is centred at zoom 1 rather
        /// than producing an infinite zoom.
        void FitToBounds(glm::vec2 min, glm::vec2 max, f32 margin = 60.0f);
        void ResetView();

        //-- Wires ----------------------------------------------------------------
        /// A horizontal-tangent cubic bezier, the shape every node editor uses.
        /// `thickness` is in SCREEN pixels and is not scaled by zoom — a hairline
        /// at 0.2x zoom is unclickable and invisible.
        void DrawWire(ImVec2 from, ImVec2 to, ImU32 color, f32 thickness = 2.0f) const;
        /// A wire plus a filled arrowhead at its midpoint. Exec wires are
        /// directional; data wires are not, and telling them apart at a glance is
        /// the whole point of drawing them differently.
        void DrawDirectionalWire(ImVec2 from, ImVec2 to, ImU32 color, f32 thickness = 3.5f) const;

        /// Distance in screen pixels from `point` to the bezier between `from` and
        /// `to`, sampled coarsely. Used for click-to-select / alt-click-to-cut on
        /// a wire; exact enough for a mouse, cheap enough for every wire per frame.
        ///
        /// NOT static: it must use the SAME zoom-scaled control offsets DrawWire
        /// does. A static version computed unzoomed tangents, so at any zoom != 1
        /// the hit curve bowed away from the drawn one and alt+click missed wires
        /// that were plainly under the cursor.
        [[nodiscard]] f32 DistanceToWire(ImVec2 from, ImVec2 to, ImVec2 point) const;

        Style m_Style{};

      private:
        [[nodiscard]] ImVec2 ControlOffset(ImVec2 from, ImVec2 to) const;
        void DrawGrid() const;
        void HandleViewInput();

        glm::vec2 m_Pan{ 0.0f, 0.0f };
        f32 m_Zoom = 1.0f;
        ImVec2 m_Origin{ 0.0f, 0.0f };
        ImVec2 m_Size{ 0.0f, 0.0f };
        ImDrawList* m_DrawList = nullptr;
        bool m_IsPanning = false;
        bool m_IsHovered = false;
        bool m_Active = false;
        /// A pan button is held, but the pointer has not yet moved far enough to
        /// count as a drag. Until it does, IsPanning() stays false so a plain
        /// right-CLICK still reaches the consumer's context menu.
        bool m_PanButtonDown = false;
        ImVec2 m_PanPressPos{ 0.0f, 0.0f };
        /// Set for one frame by HandleViewInput; see WasRightClicked().
        bool m_WasRightClicked = false;

        static constexpr f32 s_MinZoom = 0.15f;
        static constexpr f32 s_MaxZoom = 3.0f;
        static constexpr f32 s_ZoomStep = 1.12f;
        /// Screen pixels of movement before a held pan button becomes a pan.
        /// Small enough that a deliberate drag feels immediate, large enough to
        /// absorb the jitter of a click made with a real mouse.
        static constexpr f32 s_PanDragThreshold = 4.0f;
    };

} // namespace OloEngine::EditorUI
