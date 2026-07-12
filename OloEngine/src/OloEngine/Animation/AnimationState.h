#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include <string>
#include <vector>

namespace OloEngine
{
    class AnimationState
    {
      public:
        std::string Name;
        f32 Speed = 1.0f;
        bool Looping = true;

        enum class MotionType : u8
        {
            SingleClip,
            BlendTree
        };

        MotionType Type = MotionType::SingleClip;

        // For SingleClip
        Ref<AnimationClip> Clip;
        std::string ClipName; // Persisted name for serialization/linking

        // For BlendTree
        Ref<BlendTree> Tree;

        void Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                      sizet boneCount, const PoseEvalContext& ctx,
                      std::vector<BoneTransform>& outBoneTransforms) const;

        // Root-motion extraction over one tick of this state (issue #631):
        // stateStartTime is the PRE-advance state time (effective seconds, i.e.
        // Speed already folded into effectiveDuration), dt the tick's advance.
        // The state's normalized playback window maps into each contributing
        // clip's own time domain, exactly like Evaluate does for the pose.
        [[nodiscard("extracted delta must be used")]] Animation::RootMotionDelta ExtractRootMotion(
            f32 stateStartTime, f32 dt, f32 effectiveDuration,
            const AnimationParameterSet& params, const PoseEvalContext& ctx) const;

        [[nodiscard("clip duration needed for time calculations")]] f32 GetClipDuration() const;
        [[nodiscard("effective duration accounts for speed multiplier")]] f32 GetEffectiveDuration(const AnimationParameterSet& params) const;
    };
} // namespace OloEngine
