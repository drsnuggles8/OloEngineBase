#pragma once

#include "OloEngine/Renderer/Buffer.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Terrain vertex — position, UV, normal. Tangents computed from heightmap derivatives in shader.
    struct TerrainVertex
    {
        glm::vec3 Position; // 12 bytes
        glm::vec2 TexCoord; // 8 bytes
        glm::vec3 Normal;   // 12 bytes
        // Total: 32 bytes

        TerrainVertex() = default;

        TerrainVertex(const glm::vec3& position, const glm::vec2& texCoord, const glm::vec3& normal)
            : Position(position), TexCoord(texCoord), Normal(normal)
        {
        }

        static BufferLayout GetLayout()
        {
            return {
                { ShaderDataType::Float3, "a_Position" },
                { ShaderDataType::Float2, "a_TexCoord" },
                { ShaderDataType::Float3, "a_Normal" }
            };
        }
    };

    static_assert(sizeof(TerrainVertex) == 32, "TerrainVertex must be 32 bytes (3+2+3 floats)");
} // namespace OloEngine
