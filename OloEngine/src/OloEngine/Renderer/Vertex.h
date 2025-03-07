#pragma once

#include <glm/glm.hpp>
#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine 
{
    // Represents a vertex in 3D space with position, normal, and texture coordinates
    struct Vertex 
    {
        glm::vec3 Position;  // Position of the vertex in 3D space
        glm::vec3 Normal;    // Surface normal at the vertex
        glm::vec2 TexCoord;  // Texture coordinates

        Vertex() = default;
        
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