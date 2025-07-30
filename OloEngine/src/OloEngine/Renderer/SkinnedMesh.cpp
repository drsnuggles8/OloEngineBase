#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinnedMesh.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{    SkinnedMesh::SkinnedMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        Build();
        CalculateBounds();
    }

    SkinnedMesh::SkinnedMesh(std::vector<SkinnedVertex>&& vertices, std::vector<u32>&& indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        Build();
        CalculateBounds();
    }

    void SkinnedMesh::SetVertices(const std::vector<SkinnedVertex>& vertices)
    {
        m_Vertices = vertices;
        m_Built = false;
    }

    void SkinnedMesh::SetVertices(std::vector<SkinnedVertex>&& vertices)
    {
        m_Vertices = std::move(vertices);
        m_Built = false;
    }

    void SkinnedMesh::SetIndices(const std::vector<u32>& indices)
    {
        m_Indices = indices;
        m_Built = false;
    }

    void SkinnedMesh::SetIndices(std::vector<u32>&& indices)
    {
        m_Indices = std::move(indices);
        m_Built = false;
    }

    void SkinnedMesh::Build()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Vertices.empty() || m_Indices.empty())
        {
            OLO_CORE_WARN("SkinnedMesh::Build: Attempting to build a mesh with no vertices or indices!");
            return;
        }

        m_VertexArray = VertexArray::Create();

        m_VertexBuffer = VertexBuffer::Create(
            reinterpret_cast<f32*>(m_Vertices.data()),
            static_cast<u32>(m_Vertices.size() * sizeof(SkinnedVertex))
        );
        
        m_VertexBuffer->SetLayout(SkinnedVertex::GetLayout());
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);

        m_IndexBuffer = IndexBuffer::Create(
            m_Indices.data(),
            static_cast<u32>(m_Indices.size())
        );

        m_VertexArray->SetIndexBuffer(m_IndexBuffer);
        
        CalculateBounds();
        
        m_Built = true;
    }

    void SkinnedMesh::CalculateBounds()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Vertices.empty())
        {
            m_BoundingBox = BoundingBox();
            m_BoundingSphere = BoundingSphere();
            return;
        }

        glm::vec3 minBounds = m_Vertices[0].Position;
        glm::vec3 maxBounds = m_Vertices[0].Position;

        for (const auto& vertex : m_Vertices)
        {
            minBounds = glm::min(minBounds, vertex.Position);
            maxBounds = glm::max(maxBounds, vertex.Position);
        }

        m_BoundingBox = BoundingBox(minBounds, maxBounds);

        const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
        f32 maxDistanceSquared = 0.0f;        for (const auto& vertex : m_Vertices)
        {
            const f32 distanceSquared = glm::dot(vertex.Position - center, vertex.Position - center);
            maxDistanceSquared = glm::max(maxDistanceSquared, distanceSquared);
        }

        m_BoundingSphere = BoundingSphere(center, glm::sqrt(maxDistanceSquared));
    }    void SkinnedMesh::Draw() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Built)
        {
            OLO_CORE_WARN("SkinnedMesh::Draw: Attempting to draw a mesh that hasn't been built!");
            return;
        }        m_VertexArray->Bind();
        RenderCommand::DrawIndexed(m_VertexArray);
    }

    AssetRef<SkinnedMesh> SkinnedMesh::CreateCube()
    {
        OLO_PROFILE_FUNCTION();

        // Create cube vertices with bone indices and weights (all vertices affected by bone 0)
        std::vector<SkinnedVertex> vertices = {
            // Front face (Z+)
            {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Back face (Z-)
            {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Left face (X-)
            {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Right face (X+)
            {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Bottom face (Y-)
            {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Top face (Y+)
            {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}
        };

        std::vector<u32> indices = {
            // Front face
            0, 1, 2, 2, 3, 0,
            // Back face
            4, 5, 6, 6, 7, 4,
            // Left face
            8, 9, 10, 10, 11, 8,
            // Right face
            12, 13, 14, 14, 15, 12,
            // Bottom face
            16, 17, 18, 18, 19, 16,
            // Top face
            20, 21, 22, 22, 23, 20
        };

        return AssetRef<SkinnedMesh>::Create(std::move(vertices), std::move(indices));
    }

    AssetRef<SkinnedMesh> SkinnedMesh::CreateMultiBoneCube()
    {
        OLO_PROFILE_FUNCTION();

        // Create a cube with more interesting bone weighting
        // Top half influenced by bone 1, bottom half by bone 0, with blending in the middle
        std::vector<SkinnedVertex> vertices = {
            // Front face (Z+) - bottom vertices influenced by bone 0, top by bone 1
            {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom left - bone 0
            {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom right - bone 0
            {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top right - bone 1
            {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top left - bone 1

            // Back face (Z-) - same pattern
            {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom left - bone 0
            {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top left - bone 1
            {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top right - bone 1
            {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom right - bone 0

            // Left face (X-) - blend bones 0 and 1 based on Y position
            {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom - bone 0
            {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom - bone 0
            {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top - bone 1
            {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top - bone 1

            // Right face (X+) - blend bones 0 and 1 based on Y position
            {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom - bone 0
            {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top - bone 1
            {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Top - bone 1
            {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}, // Bottom - bone 0

            // Bottom face (Y-) - all influenced by bone 0
            {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

            // Top face (Y+) - all influenced by bone 1
            {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}
        };

        std::vector<u32> indices = {
            // Front face
            0, 1, 2, 2, 3, 0,
            // Back face
            4, 5, 6, 6, 7, 4,
            // Left face
            8, 9, 10, 10, 11, 8,
            // Right face
            12, 13, 14, 14, 15, 12,
            // Bottom face
            16, 17, 18, 18, 19, 16,
            // Top face
            20, 21, 22, 22, 23, 20
        };

        return AssetRef<SkinnedMesh>::Create(std::move(vertices), std::move(indices));
    }
}
