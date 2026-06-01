#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
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
        AssetHandle Sequence = 0; ///< CinematicSequence asset to play

        OLO_PROPERTY()
        bool PlayOnStart = false; ///< begin playing on Scene::OnRuntimeStart

        OLO_PROPERTY()
        bool Loop = false; ///< restart from 0 when the playhead reaches the end

        OLO_PROPERTY()
        f32 PlaybackSpeed = 1.0f; ///< time scale (>= 0; reverse playback is future work)

        // ----- Runtime state (never serialized) -----
        /// Resolved sequence. Set directly for code/tests, or lazily loaded
        /// from `Sequence` by CinematicSystem when null.
        Ref<CinematicSequence> RuntimeSequence;
        bool Playing = false;
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
