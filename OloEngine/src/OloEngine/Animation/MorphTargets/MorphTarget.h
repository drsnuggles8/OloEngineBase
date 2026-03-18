#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    struct MorphTargetVertex
    {
        glm::vec3 DeltaPosition{ 0.0f };
        glm::vec3 DeltaNormal{ 0.0f };
        glm::vec3 DeltaTangent{ 0.0f };
    };

    class MorphTarget
    {
    public:
        std::string Name;
        std::vector<MorphTargetVertex> Vertices;

        // Sparse representation: only store non-zero deltas for efficiency
        struct SparseEntry
        {
            u32 VertexIndex;
            MorphTargetVertex Delta;
        };
        std::vector<SparseEntry> SparseVertices;
        bool IsSparse = false;

        MorphTarget() = default;
        MorphTarget(std::string name, u32 vertexCount)
            : Name(std::move(name))
        {
            Vertices.resize(vertexCount);
        }

        // Convert dense representation to sparse (only non-zero deltas)
        void ConvertToSparse(f32 epsilon = 1e-6f)
        {
            if (IsSparse)
                return;

            SparseVertices.clear();
            for (u32 i = 0; i < static_cast<u32>(Vertices.size()); ++i)
            {
                const auto& v = Vertices[i];
                if (glm::length(v.DeltaPosition) > epsilon ||
                    glm::length(v.DeltaNormal) > epsilon)
                {
                    SparseVertices.push_back({ i, v });
                }
            }
            IsSparse = true;
        }

        // Convert sparse representation back to dense
        void ConvertToDense(u32 vertexCount)
        {
            if (!IsSparse)
                return;

            Vertices.clear();
            Vertices.resize(vertexCount);
            for (const auto& entry : SparseVertices)
            {
                if (entry.VertexIndex < vertexCount)
                {
                    Vertices[entry.VertexIndex] = entry.Delta;
                }
            }
            IsSparse = false;
        }
    };
} // namespace OloEngine
