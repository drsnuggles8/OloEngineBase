#pragma once

#include "MorphTargetComponents.h"
#include "MorphTargetEvaluator.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    class MorphTargetSystem
    {
      public:
        // Sample morph target keyframes from an animation clip at the given time
        // and apply the resulting weights to the MorphTargetComponent
        static void SampleMorphKeyframes(
            const Ref<AnimationClip>& clip,
            f64 timeSeconds,
            MorphTargetComponent& morphComp);

        // The morph half of a clip cross-fade (issue #1533): samples both clips
        // and writes mix(from, to, alpha) per target. Without it a face's blink,
        // brow and mouth weights jump from one clip's values to the other's on
        // the frame the skeletal blend completes. A target only one clip keys
        // blends against the weight it has now: the other clip does not animate
        // it, so it must not be dragged to zero either.
        static void SampleMorphKeyframesBlended(
            const Ref<AnimationClip>& from,
            f64 fromSeconds,
            const Ref<AnimationClip>& to,
            f64 toSeconds,
            f32 alpha,
            MorphTargetComponent& morphComp);

        // Evaluate morph targets for an entity that has active weights.
        // Extracts base positions/normals from the MeshSource vertices,
        // runs the CPU evaluator, and writes results back.
        // Returns true if morph deformation was actually applied.
        static bool EvaluateMorphTargets(
            MorphTargetComponent& morphComp,
            const std::vector<glm::vec3>& basePositions,
            const std::vector<glm::vec3>& baseNormals,
            std::vector<glm::vec3>& outPositions,
            std::vector<glm::vec3>& outNormals);
    };
} // namespace OloEngine
