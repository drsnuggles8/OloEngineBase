#pragma once

#include <glm/glm.hpp>
#include <glm/gtx/integer.hpp>
#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine 
{
    // Represents a vertex in 3D space with position, normal, and texture coordinates
    struct alignas(16) Vertex  // Align to 16-byte boundary for SIMD operations
    {
        glm::vec3 Position;  // 12 bytes
        glm::vec3 Normal;    // 12 bytes
        glm::vec2 TexCoord;  // 8 bytes
        // Total: 32 bytes, aligned to 16-byte boundary

        Vertex() = default;
        Vertex(const Vertex&) = default;
        Vertex(Vertex&&) noexcept = default;
        Vertex& operator=(const Vertex&) = default;
        Vertex& operator=(Vertex&&) noexcept = default;
        
        Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord)
            : Position(position), Normal(normal), TexCoord(texCoord)
        {}
        
        // Static method to create the vertex buffer layout for this vertex structure
        static BufferLayout GetLayout()
        {
            return {
                { ShaderDataType::Float3, "a_Position" },
                { ShaderDataType::Float3, "a_Normal" },
                { ShaderDataType::Float2, "a_TexCoord" }
            };
        }
    };

    // Represents a skinned vertex for animated meshes with bone influences
    struct alignas(16) SkinnedVertex  // Align to 16-byte boundary for SIMD operations
    {
        glm::vec3 Position;      // 12 bytes
        glm::vec3 Normal;        // 12 bytes  
        glm::vec2 TexCoord;      // 8 bytes
        glm::ivec4 BoneIndices;  // 16 bytes - indices of up to 4 bones affecting this vertex
        glm::vec4 BoneWeights;   // 16 bytes - weights for corresponding bones (should sum to 1.0)
        // Total: 64 bytes, aligned to 16-byte boundary

        SkinnedVertex() = default;
        SkinnedVertex(const SkinnedVertex&) = default;
        SkinnedVertex(SkinnedVertex&&) noexcept = default;
        SkinnedVertex& operator=(const SkinnedVertex&) = default;
        SkinnedVertex& operator=(SkinnedVertex&&) noexcept = default;
        
        SkinnedVertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord,
                     const glm::ivec4& boneIndices = glm::ivec4(0), const glm::vec4& boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f))
            : Position(position), Normal(normal), TexCoord(texCoord), BoneIndices(boneIndices), BoneWeights(boneWeights)
        {}

        // Convert from regular Vertex to SkinnedVertex (no bone influences)
        explicit SkinnedVertex(const Vertex& vertex)
            : Position(vertex.Position), Normal(vertex.Normal), TexCoord(vertex.TexCoord)
            , BoneIndices(0), BoneWeights(1.0f, 0.0f, 0.0f, 0.0f)
        {}
        
        // Static method to create the vertex buffer layout for this vertex structure
        static BufferLayout GetLayout()
        {
            return {
                { ShaderDataType::Float3, "a_Position" },
                { ShaderDataType::Float3, "a_Normal" },
                { ShaderDataType::Float2, "a_TexCoord" },
                { ShaderDataType::Int4,   "a_BoneIndices" },
                { ShaderDataType::Float4, "a_BoneWeights" }
            };
        }
    };
}