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
            f32 time,
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
