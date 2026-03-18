#include "OloEnginePCH.h"
#include "MorphTargetEvaluator.h"
#include "OloEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace OloEngine
{
    void MorphTargetEvaluator::EvaluateCPU(
        const std::vector<glm::vec3>& basePositions,
        const std::vector<glm::vec3>& baseNormals,
        const MorphTargetSet& morphTargets,
        const std::vector<f32>& weights,
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::vec3>& outNormals)
    {
        OLO_PROFILE_FUNCTION();

        const u32 vertexCount = static_cast<u32>(basePositions.size());

        OLO_CORE_ASSERT(basePositions.size() == baseNormals.size(),
                        "MorphTargetEvaluator::EvaluateCPU: basePositions and baseNormals size mismatch");

        outPositions.resize(vertexCount);
        outNormals.resize(vertexCount);

        // Start with base mesh data
        std::copy(basePositions.begin(), basePositions.end(), outPositions.begin());
        std::copy(baseNormals.begin(), baseNormals.end(), outNormals.begin());

        const u32 targetCount = morphTargets.GetTargetCount();
        const u32 weightCount = static_cast<u32>(weights.size());

        for (u32 t = 0; t < targetCount && t < weightCount; ++t)
        {
            const f32 w = weights[t];
            if (w < 1e-4f)
                continue;

            const auto& target = morphTargets.Targets[t];

            if (target.IsSparse)
            {
                // Sparse path: only iterate affected vertices
                for (const auto& entry : target.SparseVertices)
                {
                    if (entry.VertexIndex < vertexCount)
                    {
                        outPositions[entry.VertexIndex] += entry.Delta.DeltaPosition * w;
                        outNormals[entry.VertexIndex] += entry.Delta.DeltaNormal * w;
                    }
                }
            }
            else
            {
                // Dense path: iterate all vertices
                const u32 count = std::min(vertexCount, static_cast<u32>(target.Vertices.size()));
                for (u32 v = 0; v < count; ++v)
                {
                    outPositions[v] += target.Vertices[v].DeltaPosition * w;
                    outNormals[v] += target.Vertices[v].DeltaNormal * w;
                }
            }
        }

        // Normalize output normals
        for (u32 v = 0; v < vertexCount; ++v)
        {
            f32 len = glm::length(outNormals[v]);
            if (len > 1e-6f)
                outNormals[v] /= len;
        }
    }

    void MorphTargetEvaluator::EvaluateGPU(
        u32 baseVertexSSBO,
        u32 morphDeltaSSBO,
        u32 weightsSSBO,
        u32 outputVertexSSBO,
        u32 vertexCount,
        u32 targetCount)
    {
        OLO_PROFILE_FUNCTION();

        // GPU compute shader dispatch
        // This requires the MorphTargetEval compute shader to be loaded
        // The shader bindings are:
        //   binding 0: BaseVertices (readonly SSBO)
        //   binding 1: MorphDeltas  (readonly SSBO)
        //   binding 2: Weights      (readonly SSBO)
        //   binding 3: OutputVerts  (writeonly SSBO)

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, baseVertexSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, morphDeltaSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, weightsSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputVertexSSBO);

        // Dispatch compute shader with enough work groups to cover all vertices
        const u32 workGroupSize = 256;
        const u32 numGroups = (vertexCount + workGroupSize - 1) / workGroupSize;
        glDispatchCompute(numGroups, 1, 1);

        // Memory barrier to ensure compute shader writes are visible
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // Unbind SSBOs
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    }
} // namespace OloEngine
