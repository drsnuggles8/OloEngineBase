#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        Build();
    }

    Mesh::Mesh(std::vector<Vertex>&& vertices, std::vector<u32>&& indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        Build();
    }

    void Mesh::SetVertices(const std::vector<Vertex>& vertices)
    {
        m_Vertices = vertices;
        m_Built = false;
    }

    void Mesh::SetVertices(std::vector<Vertex>&& vertices)
    {
        m_Vertices = std::move(vertices);
        m_Built = false;
    }

    void Mesh::SetIndices(const std::vector<u32>& indices)
    {
        m_Indices = indices;
        m_Built = false;
    }

    void Mesh::SetIndices(std::vector<u32>&& indices)
    {
        m_Indices = std::move(indices);
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
        
        // Calculate bounding volumes
        CalculateBounds();
        
        m_Built = true;
    }

    void Mesh::CalculateBounds()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_Vertices.empty())
        {
            // Default to a unit cube around the origin if no vertices
            m_BoundingBox = BoundingBox(glm::vec3(-0.5f), glm::vec3(0.5f));
            m_BoundingSphere = BoundingSphere(glm::vec3(0.0f), 0.5f);
            return;
        }
        
        // Extract positions from vertices for bounding volume calculation
        std::vector<glm::vec3> positions;
        positions.reserve(m_Vertices.size());
        
        for (const auto& vertex : m_Vertices)
        {
            positions.push_back(vertex.Position);
        }
        
        // Create bounding box from positions
        m_BoundingBox = BoundingBox(positions.data(), positions.size());
        
        // Create bounding sphere from bounding box (more efficient than from points)
        m_BoundingSphere = BoundingSphere(m_BoundingBox);
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

    AssetRef<Mesh> Mesh::CreateCube()
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

        std::vector<u32> indices = {
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

        return AssetRef<Mesh>::Create(vertices, indices);
    }

    AssetRef<Mesh> Mesh::CreateSphere(f32 radius, u32 segments)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        const u32 rings = segments;
        const u32 sectors = segments * 2;

        const f32 R = 1.0f / static_cast<f32>(rings - 1);
        const f32 S = 1.0f / static_cast<f32>(sectors - 1);

        vertices.reserve(rings * sectors);
        
        // Generate vertices
        for (u32 r = 0; r < rings; ++r)
        {
            for (u32 s = 0; s < sectors; ++s)
            {
                const f32 y = sin(-glm::half_pi<f32>() + glm::pi<f32>() * r * R);
                const f32 x = cos(2 * glm::pi<f32>() * s * S) * sin(glm::pi<f32>() * r * R);
                const f32 z = sin(2 * glm::pi<f32>() * s * S) * sin(glm::pi<f32>() * r * R);

                // Position
                glm::vec3 position = glm::vec3(x, y, z) * radius;
                
                // Normal (normalized position for a sphere)
                glm::vec3 normal = glm::normalize(position);
                
                // Texture coordinates
                auto texCoord = glm::vec2(s * S, r * R);
                
                vertices.emplace_back(position, normal, texCoord);
            }
        }

        // Generate indices
        indices.reserve((rings - 1) * (sectors) * 6);
        
        for (u32 r = 0; r < rings - 1; ++r)
        {
            for (u32 s = 0; s < sectors - 1; ++s)
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

        return AssetRef<Mesh>::Create(vertices, indices);
    }

    Ref<Mesh> Mesh::CreatePlane(f32 width, f32 length)
    {
        OLO_PROFILE_FUNCTION();

        const f32 halfWidth = width * 0.5f;
        const f32 halfLength = length * 0.5f;
        
        std::vector<Vertex> vertices = {
            // Top face (facing positive Y)
            { { halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 1.0f} },
            { { halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 0.0f} },
            { {-halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
            { {-halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f} }
        };

        std::vector<u32> indices = {
            0, 1, 3, 1, 2, 3 // Top face
        };

        return CreateRef<Mesh>(vertices, indices);
    }

    Ref<Mesh> Mesh::CreateSkyboxCube()
    {
        OLO_PROFILE_FUNCTION();

        // For a skybox, we only need positions as they'll be used as the texture coordinates
        std::vector<Vertex> vertices = {
            // Right face (+X)
            { { 1.0f,  1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f,  1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            
            // Left face (-X)
            { {-1.0f,  1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { {-1.0f, -1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { {-1.0f, -1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            { {-1.0f,  1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            
            // Top face (+Y)
            { {-1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
            { {-1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
            
            // Bottom face (-Y)
            { {-1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
            { {-1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
            
            // Front face (+Z)
            { {-1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
            { { 1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
            { {-1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
            
            // Back face (-Z)
            { { 1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
            { {-1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
            { {-1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
            { { 1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} }
        };

        std::vector<u32> indices = {
            // Right face
             0,  1,  2,  2,  3,  0,
            // Left face
             4,  5,  6,  6,  7,  4,
            // Top face
             8,  9, 10, 10, 11,  8,
            // Bottom face
            12, 13, 14, 14, 15, 12,
            // Front face
            16, 17, 18, 18, 19, 16,
            // Back face
            20, 21, 22, 22, 23, 20
        };

        return CreateRef<Mesh>(vertices, indices);
    }
}
