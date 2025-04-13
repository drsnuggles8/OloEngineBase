#pragma once

#include <glm/glm.hpp>
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
}