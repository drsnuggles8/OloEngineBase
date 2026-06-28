#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Math/Math.h"                 // Math::BitwiseEqual
#include "OloEngine/Scene/ComponentReflection.h" // OLO_PROPERTY

#include <string>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Plays a CinematicSequence on an entity.
     *
     * Serialized state is just the sequence reference + a few playback flags;
     * the runtime playhead and resolved sequence Ref are rebuilt at play time
     * (mirrors AnimationGraphComponent's RuntimeGraph handling). CinematicSystem
     * advances every CinematicComponent each runtime tick.
     */
    struct CinematicComponent
    {
        // ----- Serialized authoring data -----
        OLO_PROPERTY()
        AssetHandle Sequence = 0; ///< CinematicSequence asset to play

        OLO_PROPERTY()
        bool PlayOnStart = false; ///< begin playing on Scene::OnRuntimeStart

        OLO_PROPERTY()
        bool Loop = false; ///< restart from 0 when the playhead reaches the end

        OLO_PROPERTY()
        f32 PlaybackSpeed = 1.0f; ///< time scale; negative plays the sequence backward, 0 holds the playhead

        // ----- Runtime state (never serialized) -----
        /// Resolved sequence. Set directly for code/tests, or lazily loaded
        /// from `Sequence` by CinematicSystem when null.
        Ref<CinematicSequence> RuntimeSequence;
        OLO_PROPERTY()
        bool Playing = false;     ///< runtime playback flag (exposed for scripts; not serialized)
        f32 Time = 0.0f;          ///< current playhead in seconds
        f32 PreviousTime = -1.0f; ///< playhead last tick (sentinel < 0 fires t==0 events)
        bool Finished = false;    ///< reached the end this run (non-looping)
        /// Event identifiers fired during the most recent CinematicSystem tick.
        std::vector<std::string> EventsFiredThisFrame;

        CinematicComponent() = default;

        // Copying shares only the authoring data; the runtime Ref + playhead are
        // reset so a scene copy / prefab instance gets its own playback state
        // (the RuntimeSequence Ref is otherwise aliased — see AnimationGraphComponent).
        CinematicComponent(const CinematicComponent& other)
            : Sequence(other.Sequence), PlayOnStart(other.PlayOnStart), Loop(other.Loop), PlaybackSpeed(other.PlaybackSpeed)
        {
        }
        CinematicComponent& operator=(const CinematicComponent& other)
        {
            if (this != &other)
            {
                Sequence = other.Sequence;
                PlayOnStart = other.PlayOnStart;
                Loop = other.Loop;
                PlaybackSpeed = other.PlaybackSpeed;
                RuntimeSequence = nullptr;
                Playing = false;
                Time = 0.0f;
                PreviousTime = -1.0f;
                Finished = false;
                EventsFiredThisFrame.clear();
            }
            return *this;
        }
        CinematicComponent(CinematicComponent&&) = default;
        CinematicComponent& operator=(CinematicComponent&&) = default;

        // Authoring-only equality for the editor's undo/redo diff (SceneHierarchy
        // DrawComponent tier-2). Deliberately excludes the runtime playhead /
        // preview state (Time, Playing, Finished, RuntimeSequence,
        // EventsFiredThisFrame) so scrubbing or previewing in the inspector never
        // registers as an edit — same rationale as TagComponent's transient
        // `renaming` flag. PlaybackSpeed goes through BitwiseEqual per
        // cpp-coding-quality §2 (no float ==).
        auto operator==(const CinematicComponent& other) const -> bool
        {
            return Sequence == other.Sequence && PlayOnStart == other.PlayOnStart && Loop == other.Loop && Math::BitwiseEqual(PlaybackSpeed, other.PlaybackSpeed);
        }

        /// Start (or resume) playback from the current playhead.
        void Play()
        {
            Playing = true;
            Finished = false;
        }
        /// Start playback from the beginning.
        void PlayFromStart()
        {
            Time = 0.0f;
            PreviousTime = -1.0f;
            Playing = true;
            Finished = false;
            EventsFiredThisFrame.clear();
        }
        void Pause()
        {
            Playing = false;
        }
        /// Stop and rewind to the start.
        void Stop()
        {
            Playing = false;
            Time = 0.0f;
            PreviousTime = -1.0f;
            Finished = false;
            EventsFiredThisFrame.clear();
        }
    };
} // namespace OloEngine
