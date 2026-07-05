#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>
#include <string>
#include <vector>

namespace OloEngine
{
    struct BoneTransform
    {
        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);
    };

    // Per-skeleton context threaded through the animation-graph pose pipeline
    // (graph -> state machine -> state -> blend tree -> clip sampling).
    //
    // A clip's channels must map to skeleton bones BY NAME: channel/array order
    // is exporter-dependent and does NOT match the skeleton's depth-first bone
    // index order, so sampling clip.BoneAnimations[i] onto bone i scrambles the
    // pose (issue #543). BoneNames is index-aligned to the output pose so each
    // bone can look up its channel via AnimationClip::FindBoneAnimation.
    //
    // BindPose holds each bone's rest local transform; bones a clip does not
    // animate fall back to it rather than to identity (mirrors AnimationSystem).
    // Either span may be empty (e.g. a skeleton without a captured bind pose),
    // in which case the corresponding fallback degrades to identity.
    struct PoseEvalContext
    {
        std::span<const std::string> BoneNames;  // index-aligned to the output pose
        std::span<const BoneTransform> BindPose; // bind-pose local TRS per bone (may be empty)
    };

    class BlendNode : public RefCounted
    {
      public:
        virtual ~BlendNode() = default;
        virtual void Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                              sizet boneCount,
                              std::vector<BoneTransform>& outBoneTransforms) const = 0;
        virtual f32 GetDuration(const AnimationParameterSet& params) const = 0;
    };
} // namespace OloEngine
