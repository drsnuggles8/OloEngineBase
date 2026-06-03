#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Cinematic/CinematicCurve.h"

#include <imgui.h>

#include <filesystem>

namespace OloEngine
{
    class Scene;
    class CinematicSequence;

    /**
     * @brief Authoring timeline for CinematicSequence (.olocine) assets — the
     *        deferred follow-up to the cinematic runtime (issue #177).
     *
     * The runtime (CinematicPlayer / CinematicSystem), serialization, and the
     * CinematicComponent inspector already shipped; sequences were authored as
     * YAML by hand. This panel is the missing authoring surface: track lanes,
     * draggable / insertable / deletable keyframes, per-key interpolation + value
     * editing, a curve preview for scalar channels, and a draggable playhead that
     * poses the live scene (edit-mode scrubbing through CinematicSystem::ApplyAtTime).
     *
     * Editing is in-place on the AssetManager's cached Ref<CinematicSequence>, so
     * the change is immediately visible to any CinematicComponent referencing the
     * same asset (and to the inspector's own scrub slider). Save writes the same
     * object back to its `.olocine` via CinematicSequenceSerializer.
     *
     * All non-trivial mutation goes through OloEngine::CinematicEdit so the
     * sort-by-time invariant is enforced in one tested place (CinematicEditTest).
     */
    class CinematicTimelinePanel
    {
      public:
        CinematicTimelinePanel() = default;

        /// The scene used for edit-mode scrubbing (posing entities) and for
        /// sampling an entity's current transform when inserting a key.
        void SetContext(const Ref<Scene>& context)
        {
            m_Context = context;
        }

        /// Open a sequence asset for editing. The handle form resolves the cached
        /// asset Ref so edits are live; the path form imports first to get a handle.
        void OpenSequence(AssetHandle handle);
        void OpenSequence(const std::filesystem::path& path);

        void OnImGuiRender(bool* open);

        [[nodiscard]] bool IsOpen() const
        {
            return m_Open;
        }
        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_Dirty;
        }

        /// Drop the edited sequence (e.g. on project switch) so a stale Ref/path
        /// from a previous project can't be saved into the new one.
        void Reset();

      private:
        // A lane is one editable key row. Continuous tracks expand into several
        // lanes (one per channel); discrete tracks are a single lane.
        enum class LaneKind : u8
        {
            TransformTranslation,
            TransformRotation,
            TransformScale,
            CameraPosition,
            CameraRotation,
            CameraFov,
            Visibility,
            Event
        };

        struct Selection
        {
            bool Valid = false;
            LaneKind Kind = LaneKind::Event;
            sizet TrackIndex = 0;
            sizet KeyIndex = 0;
        };

        // --- top-level sections ---
        void DrawToolbar();
        void DrawTracksAndTimeline();
        void DrawInspector();
        void DrawCurvePreview();
        void DrawAddTrackPopup();

        // --- timeline drawing ---
        void DrawRuler(ImDrawList* drawList, const ImVec2& origin, f32 laneWidth, f32 height);
        void DrawPlayhead(ImDrawList* drawList, const ImVec2& origin, f32 laneWidth, f32 timelineHeight);
        /// Draw + interact with one lane's keys. Returns true if the sequence was
        /// mutated this frame (so the caller marks dirty / recomputes duration).
        template<typename KeyVec>
        bool DrawKeyLane(KeyVec& keys, LaneKind kind, sizet trackIndex,
                         const ImVec2& laneMin, const ImVec2& laneMax, f32 trackHeaderWidth);

        // --- editing ---
        void InsertKeyAtPlayhead(LaneKind kind, sizet trackIndex);
        void DeleteSelected();
        void UpdatePreview(f32 dt);
        void ApplyPlayhead();
        void MarkDirty();

        // --- helpers ---
        [[nodiscard]] f32 TimeToX(f32 time, f32 contentLeft) const;
        [[nodiscard]] f32 XToTime(f32 x, f32 contentLeft) const;
        [[nodiscard]] f32 SnapTime(f32 time) const;
        [[nodiscard]] f32 SequenceDuration() const;
        bool Save();

      private:
        Ref<Scene> m_Context;
        Ref<CinematicSequence> m_Sequence;
        AssetHandle m_Handle = 0;
        std::filesystem::path m_FilePath;

        bool m_Open = true;
        bool m_Dirty = false;
        bool m_FocusRequested = false; ///< bring the panel's tab to front on open

        // Playhead / preview playback (edit-mode only — independent of any
        // CinematicComponent; drives ApplyAtTime directly).
        f32 m_Playhead = 0.0f;
        bool m_Playing = false;
        bool m_PreviewLoop = true;

        // View / zoom: horizontal scale + the time at the left edge of the
        // scrollable content.
        f32 m_PixelsPerSecond = 120.0f;
        f32 m_ScrollSeconds = 0.0f;

        // Snapping for key drags and playhead.
        bool m_SnapEnabled = true;
        f32 m_SnapSeconds = 0.1f;

        Selection m_Selection;

        // For renaming an event key in the inspector.
        char m_EventNameBuffer[128] = { 0 };
        // Pending "add track" choices in the popup.
        i32 m_AddTrackKind = 0;
        UUID m_AddTrackTarget = 0;

        // Layout constants.
        static constexpr f32 s_TrackHeaderWidth = 190.0f;
        static constexpr f32 s_LaneHeight = 22.0f;
        static constexpr f32 s_RulerHeight = 24.0f;
        static constexpr f32 s_KeyHalfSize = 6.0f;
        static constexpr f32 s_MinPixelsPerSecond = 10.0f;
        static constexpr f32 s_MaxPixelsPerSecond = 600.0f;
    };
} // namespace OloEngine
