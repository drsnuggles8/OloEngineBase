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

        [[nodiscard("clip duration needed for time calculations")]] f32 GetClipDuration() const;
        [[nodiscard("effective duration accounts for speed multiplier")]] f32 GetEffectiveDuration(const AnimationParameterSet& params) const;
    };
} // namespace OloEngine
