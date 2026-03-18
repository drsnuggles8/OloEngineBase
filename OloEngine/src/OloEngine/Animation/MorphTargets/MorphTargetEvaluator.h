#pragma once

#include "MorphTargetSet.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class MorphTargetEvaluator
    {
    public:
        // Apply weighted morph targets to base mesh vertices (CPU path)
        static void EvaluateCPU(
            const std::vector<glm::vec3>& basePositions,
            const std::vector<glm::vec3>& baseNormals,
            const MorphTargetSet& morphTargets,
            const std::vector<f32>& weights,
            std::vector<glm::vec3>& outPositions,
            std::vector<glm::vec3>& outNormals);

        // GPU path: compute shader evaluation
        static void EvaluateGPU(
            u32 baseVertexSSBO,
            u32 morphDeltaSSBO,
            u32 weightsUBO,
            u32 outputVertexSSBO,
            u32 vertexCount,
            u32 targetCount);
    };
} // namespace OloEngine
