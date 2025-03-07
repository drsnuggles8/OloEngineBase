#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        Build();
    }

    void Mesh::SetVertices(const std::vector<Vertex>& vertices)
    {
        m_Vertices = vertices;
        m_Built = false;
    }

    void Mesh::SetIndices(const std::vector<uint32_t>& indices)
    {
        m_Indices = indices;
        m_Built = false;
    }

    void Mesh::Build()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Vertices.empty() || m_Indices.empty())
        {
            OLO_CORE_WARN("Mesh::Build: Attempting to build a mesh with no vertices or indices!");
            return;
        }

        m_VertexArray = VertexArray::Create();

        m_VertexBuffer = VertexBuffer::Create(
            reinterpret_cast<f32*>(m_Vertices.data()),
            static_cast<u32>(m_Vertices.size() * sizeof(Vertex))
        );
        
        m_VertexBuffer->SetLayout(Vertex::GetLayout());
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);

        m_IndexBuffer = IndexBuffer::Create(
            m_Indices.data(),
            static_cast<u32>(m_Indices.size())
        );

        m_VertexArray->SetIndexBuffer(m_IndexBuffer);
        m_Built = true;
    }

    void Mesh::Draw() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Built)
        {
            OLO_CORE_WARN("Mesh::Draw: Attempting to draw a mesh that hasn't been built!");
            return;
        }

        m_VertexArray->Bind();
        RenderCommand::DrawIndexed(m_VertexArray);
    }

    Ref<Mesh> Mesh::CreateCube()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices = {
            // Front face
            { { 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f} }, // 0
            { { 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f} }, // 1
            { {-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} }, // 2
            { {-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f} }, // 3

            // Back face
            { { 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f} }, // 4
            { { 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} }, // 5
            { {-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f} }, // 6
            { {-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f} }, // 7

            // Right face
            { { 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} }, // 8
            { { 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} }, // 9
            { { 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} }, // 10
            { { 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} }, // 11

            // Left face
            { {-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} }, // 12
            { {-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} }, // 13
            { {-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} }, // 14
            { {-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} }, // 15

            // Top face
            { { 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f} }, // 16
            { { 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f} }, // 17
            { {-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f} }, // 18
            { {-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} }, // 19

            // Bottom face
            { { 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f} }, // 20
            { { 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f} }, // 21
            { {-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} }, // 22
            { {-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f} }  // 23
        };

        std::vector<uint32_t> indices = {
            // Front face
            0, 1, 3, 1, 2, 3,
            // Back face
            4, 5, 7, 5, 6, 7,
            // Right face
            8, 9, 11, 9, 10, 11,
            // Left face
            12, 13, 15, 13, 14, 15,
            // Top face
            16, 17, 19, 17, 18, 19,
            // Bottom face
            20, 21, 23, 21, 22, 23
        };

        return CreateRef<Mesh>(vertices, indices);
    }

    Ref<Mesh> Mesh::CreateSphere(float radius, uint32_t segments)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        const uint32_t rings = segments;
        const uint32_t sectors = segments * 2;

        const float R = 1.0f / static_cast<float>(rings - 1);
        const float S = 1.0f / static_cast<float>(sectors - 1);

        vertices.reserve(rings * sectors);
        
        // Generate vertices
        for (uint32_t r = 0; r < rings; ++r)
        {
            for (uint32_t s = 0; s < sectors; ++s)
            {
                const float y = sin(-glm::half_pi<float>() + glm::pi<float>() * r * R);
                const float x = cos(2 * glm::pi<float>() * s * S) * sin(glm::pi<float>() * r * R);
                const float z = sin(2 * glm::pi<float>() * s * S) * sin(glm::pi<float>() * r * R);

                // Position
                glm::vec3 position = glm::vec3(x, y, z) * radius;
                
                // Normal (normalized position for a sphere)
                glm::vec3 normal = glm::normalize(position);
                
                // Texture coordinates
                glm::vec2 texCoord = glm::vec2(s * S, r * R);
                
                vertices.emplace_back(position, normal, texCoord);
            }
        }

        // Generate indices
        indices.reserve((rings - 1) * (sectors) * 6);
        
        for (uint32_t r = 0; r < rings - 1; ++r)
        {
            for (uint32_t s = 0; s < sectors - 1; ++s)
            {
                indices.push_back(r * sectors + s);
                indices.push_back(r * sectors + (s + 1));
                indices.push_back((r + 1) * sectors + (s + 1));

                indices.push_back(r * sectors + s);
                indices.push_back((r + 1) * sectors + (s + 1));
                indices.push_back((r + 1) * sectors + s);
            }
            
            // Complete the last quad in the ring
            indices.push_back(r * sectors + (sectors - 1));
            indices.push_back(r * sectors);
            indices.push_back((r + 1) * sectors);
            
            indices.push_back(r * sectors + (sectors - 1));
            indices.push_back((r + 1) * sectors);
            indices.push_back((r + 1) * sectors + (sectors - 1));
        }

        return CreateRef<Mesh>(vertices, indices);
    }

    Ref<Mesh> Mesh::CreatePlane(float width, float length)
    {
        OLO_PROFILE_FUNCTION();

        const float halfWidth = width * 0.5f;
        const float halfLength = length * 0.5f;
        
        std::vector<Vertex> vertices = {
            // Top face (facing positive Y)
            { { halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 1.0f} },
            { { halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 0.0f} },
            { {-halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
            { {-halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f} }
        };

        std::vector<uint32_t> indices = {
            0, 1, 3, 1, 2, 3 // Top face
        };

        return CreateRef<Mesh>(vertices, indices);
    }
}