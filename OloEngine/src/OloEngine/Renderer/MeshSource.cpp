#include "OloEnginePCH.h"
#include "MeshSource.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"

namespace OloEngine
{
    MeshSource::MeshSource(const std::vector<Vertex>& vertices, const std::vector<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.resize(vertices.size());
        CalculateBounds();
    }

    MeshSource::MeshSource(std::vector<Vertex>&& vertices, std::vector<u32>&& indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.resize(m_Vertices.size());
        CalculateBounds();
    }

    void MeshSource::Build()
    {
        if (m_Built)
            return;

        // Ensure bounds are calculated before building GPU resources
        CalculateBounds();
        CalculateSubmeshBounds();

        BuildVertexBuffer();
        BuildIndexBuffer();

        // Build bone influence buffer if we have rigged data
        if (HasSkeleton() && !m_BoneInfluences.empty())
        {
            BuildBoneInfluenceBuffer();
        }

        m_VertexArray = VertexArray::Create();
        m_VertexArray->Bind();

        m_VertexBuffer->Bind();
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);

        // Add bone influence buffer as second vertex buffer if available
        if (m_BoneInfluenceBuffer)
        {
            m_BoneInfluenceBuffer->Bind();
            m_VertexArray->AddVertexBuffer(m_BoneInfluenceBuffer);
        }

        m_IndexBuffer->Bind();
        m_VertexArray->SetIndexBuffer(m_IndexBuffer);

        m_VertexArray->Unbind();

        m_Built = true;
    }

    void MeshSource::BuildVertexBuffer()
    {
        if (m_Vertices.empty())
            return;

        m_VertexBuffer = VertexBuffer::Create(static_cast<const void*>(m_Vertices.data()),
                                              static_cast<u32>(m_Vertices.size() * sizeof(Vertex)));
        m_VertexBuffer->SetLayout(Vertex::GetLayout());
    }

    void MeshSource::BuildIndexBuffer()
    {
        if (m_Indices.empty())
            return;

        m_IndexBuffer = IndexBuffer::Create(m_Indices.data(),
                                            static_cast<u32>(m_Indices.size()));
    }

    void MeshSource::BuildBoneInfluenceBuffer()
    {
        if (m_BoneInfluences.empty())
            return;

        // Create buffer layout for bone influences
        BufferLayout boneInfluenceLayout = {
            { ShaderDataType::Int4, "a_BoneIDs" },      // 4 bone IDs
            { ShaderDataType::Float4, "a_BoneWeights" } // 4 bone weights
        };

        m_BoneInfluenceBuffer = VertexBuffer::Create(static_cast<const void*>(m_BoneInfluences.data()),
                                                     static_cast<u32>(m_BoneInfluences.size() * sizeof(BoneInfluence)));
        m_BoneInfluenceBuffer->SetLayout(boneInfluenceLayout);
    }

    void MeshSource::CalculateBounds()
    {
        if (m_Vertices.empty())
        {
            m_BoundingBox = BoundingBox();
            m_BoundingSphere = BoundingSphere();
            return;
        }

        glm::vec3 min = m_Vertices[0].Position;
        glm::vec3 max = m_Vertices[0].Position;

        for (const auto& vertex : m_Vertices)
        {
            min = glm::min(min, vertex.Position);
            max = glm::max(max, vertex.Position);
        }

        // For animated meshes, expand bounds to account for bone transformations
        if (HasBoneInfluences())
        {
            // Calculate the size of the bounding box
            glm::vec3 size = max - min;
            f32 maxDimension = glm::max(glm::max(size.x, size.y), size.z);

            // Expand by a large percentage for skeletal animation (200% to handle extended limbs)
            f32 expansionFactor = maxDimension * 2.0f;

            // Also ensure a substantial minimum expansion for small models
            expansionFactor = glm::max(expansionFactor, 0.5f);

            min -= glm::vec3(expansionFactor);
            max += glm::vec3(expansionFactor);
        }

        m_BoundingBox = BoundingBox(min, max);

        glm::vec3 center = (min + max) * 0.5f;
        f32 radius = 0.0f;

        for (const auto& vertex : m_Vertices)
        {
            f32 distance = glm::length(vertex.Position - center);
            radius = glm::max(radius, distance);
        }

        // For animated meshes, also expand the bounding sphere radius
        if (HasBoneInfluences())
        {
            radius *= 1.5f; // 50% expansion for animated models
        }

        m_BoundingSphere = BoundingSphere(center, radius);
    }

    void MeshSource::CalculateSubmeshBounds()
    {
        for (auto& submesh : m_Submeshes)
        {
            if (submesh.m_VertexCount == 0 || submesh.m_BaseVertex >= m_Vertices.size())
            {
                submesh.m_BoundingBox = BoundingBox();
                continue;
            }

            // Calculate bounds for this specific submesh
            u32 startVertex = submesh.m_BaseVertex;
            u32 endVertex = std::min(startVertex + submesh.m_VertexCount, static_cast<u32>(m_Vertices.size()));

            if (startVertex >= endVertex)
            {
                submesh.m_BoundingBox = BoundingBox();
                continue;
            }

            glm::vec3 min = m_Vertices[startVertex].Position;
            glm::vec3 max = m_Vertices[startVertex].Position;

            for (u32 i = startVertex; i < endVertex; i++)
            {
                min = glm::min(min, m_Vertices[i].Position);
                max = glm::max(max, m_Vertices[i].Position);
            }

            // For animated meshes, expand submesh bounds to account for bone transformations
            if (HasBoneInfluences())
            {
                // Calculate the size of the submesh bounding box
                glm::vec3 size = max - min;
                f32 maxDimension = glm::max(glm::max(size.x, size.y), size.z);

                // Expand by a large percentage for skeletal animation (200% to handle extended limbs)
                f32 expansionFactor = maxDimension * 2.0f;

                // Also ensure a substantial minimum expansion for small submeshes
                expansionFactor = glm::max(expansionFactor, 0.5f);

                min -= glm::vec3(expansionFactor);
                max += glm::vec3(expansionFactor);
            }

            submesh.m_BoundingBox = BoundingBox(min, max);
        }
    }
} // namespace OloEngine
