#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Cinematic/CinematicTrack.h"

#include <string>
#include <vector>

namespace OloEngine
{
    /**
     * @brief A timeline-based cinematic sequence (cutscene) asset.
     *
     * A sequence is a collection of tracks that drive scene entities over
     * time: cameras, transforms, visibility toggles, and timed events. It is
     * authored independently of any single Scene — tracks reference their
     * targets by entity UUID — and is played back per-entity via a
     * CinematicComponent + CinematicSystem.
     *
     * The data is pure value types (see CinematicTrack.h); the asset is
     * serialized to a `.olocine` YAML file by CinematicSequenceSerializer.
     */
    class CinematicSequence : public Asset
    {
      public:
        CinematicSequence() = default;

        static AssetType GetStaticType()
        {
            return AssetType::CinematicSequence;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Authoring data — public for ergonomic construction in tools, tests,
        // and the serializer.
        std::string Name;
        f32 Duration = 0.0f; ///< explicit length in seconds; 0 => derive from keys

        std::vector<CinematicTransformTrack> TransformTracks;
        std::vector<CinematicCameraTrack> CameraTracks;
        std::vector<CinematicVisibilityTrack> VisibilityTracks;
        std::vector<CinematicEventTrack> EventTracks;

        /// Largest keyframe time across every track (0 if the sequence is empty).
        [[nodiscard]] f32 ComputeDuration() const;

        /// Playback length: the explicit Duration when > 0, else ComputeDuration().
        [[nodiscard]] f32 GetEffectiveDuration() const
        {
            return (Duration > 0.0f) ? Duration : ComputeDuration();
        }

        /// True when the sequence carries no tracks at all.
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return TransformTracks.empty() && CameraTracks.empty() && VisibilityTracks.empty() && EventTracks.empty();
        }
    };
} // namespace OloEngine
