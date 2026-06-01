#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Cinematic/CinematicCurve.h"

#include <string>
#include <vector>

namespace OloEngine
{
    // ---------------------------------------------------------------------
    // Cinematic tracks. A track binds one or more keyframe channels (or a
    // discrete key list) to a single scene entity (referenced by UUID, since
    // a sequence is authored independently of any one Scene instance).
    //
    // Tracks are plain value types so an entire CinematicSequence copies and
    // serializes trivially. The runtime application (UUID -> Entity lookup,
    // writing TransformComponent / CameraComponent / ModelComponent) lives in
    // CinematicSystem; the tracks themselves carry no Scene dependency.
    // ---------------------------------------------------------------------

    /// Animates a target entity's TransformComponent. Only channels that have
    /// keys are applied, so a track may drive translation only, rotation only,
    /// or any combination.
    struct CinematicTransformTrack
    {
        UUID Target = 0;
        std::string Name;
        CinematicVec3Channel Translation;
        CinematicQuatChannel Rotation;
        CinematicVec3Channel Scale;
    };

    /// Drives a camera entity: its TransformComponent (position + rotation)
    /// plus the perspective vertical FOV (radians) on its CameraComponent.
    struct CinematicCameraTrack
    {
        UUID Target = 0;
        std::string Name;
        CinematicVec3Channel Position;
        CinematicQuatChannel Rotation;
        CinematicFloatChannel VerticalFovRadians;
    };

    struct CinematicVisibilityKey
    {
        f32 Time = 0.0f;
        bool Visible = true;
    };

    /// Show/hide a target entity (currently its ModelComponent visibility).
    /// Discrete: the latest key with Time <= playhead wins (step semantics).
    struct CinematicVisibilityTrack
    {
        UUID Target = 0;
        std::string Name;
        std::vector<CinematicVisibilityKey> Keys; ///< sorted by Time ascending

        /// Visibility at `time`. Returns `fallback` when there are no keys or
        /// `time` precedes the first key. Keys are assumed sorted ascending.
        [[nodiscard]] bool EvaluateAt(f32 time, bool fallback = true) const
        {
            bool result = fallback;
            for (const auto& key : Keys)
            {
                if (key.Time <= time)
                {
                    result = key.Visible;
                }
                else
                {
                    break;
                }
            }
            return result;
        }
    };

    struct CinematicEventKey
    {
        f32 Time = 0.0f;
        std::string Name; ///< event identifier fired when the playhead crosses Time
    };

    /// Fires named events as the playhead crosses each key's Time. Firing is
    /// edge-triggered by CinematicPlayer (see CinematicPlayer::Tick), so an
    /// event fires exactly once per crossing.
    struct CinematicEventTrack
    {
        std::string Name;
        std::vector<CinematicEventKey> Keys; ///< sorted by Time ascending
    };
} // namespace OloEngine
