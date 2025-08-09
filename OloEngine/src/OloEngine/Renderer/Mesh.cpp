#include "OloEnginePCH.h"
#include "Mesh.h"
#include "VertexArray.h"

namespace OloEngine
{
    Mesh::Mesh(Ref<MeshSource> meshSource, u32 submeshIndex)
        : m_MeshSource(meshSource), m_SubmeshIndex(submeshIndex)
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        OLO_CORE_ASSERT(m_SubmeshIndex < m_MeshSource->GetSubmeshes().size(), "Submesh index out of range!");
    }

    const std::vector<Vertex>& Mesh::GetVertices() const
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        return m_MeshSource->GetVertices();
    }

    const std::vector<u32>& Mesh::GetIndices() const
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        return m_MeshSource->GetIndices();
    }

    const Ref<VertexArray>& Mesh::GetVertexArray() const
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        return m_MeshSource->GetVertexArray();
    }

    const Submesh& Mesh::GetSubmesh() const
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        OLO_CORE_ASSERT(m_SubmeshIndex < m_MeshSource->GetSubmeshes().size(), "Submesh index out of range!");
        return m_MeshSource->GetSubmeshes()[m_SubmeshIndex];
    }

    bool Mesh::IsRigged() const
    {
        return m_MeshSource && m_MeshSource->IsSubmeshRigged(m_SubmeshIndex);
    }

    BoundingBox Mesh::GetBoundingBox() const
    {
        if (!m_MeshSource)
            return BoundingBox();
        
        #ifdef OLO_DEBUG_FRUSTUM_CULLING
        // Debug mode: use overall MeshSource bounds to debug frustum culling issue
        return m_MeshSource->GetBoundingBox();
        #else
        // Production mode: use submesh-specific bounding box
        const auto& submeshes = m_MeshSource->GetSubmeshes();
        if (m_SubmeshIndex < submeshes.size())
        {
            return submeshes[m_SubmeshIndex].m_BoundingBox;
        }
        
        // Fallback to overall MeshSource bounds
        return m_MeshSource->GetBoundingBox();
        #endif
    }

    BoundingSphere Mesh::GetBoundingSphere() const
    {
        if (!m_MeshSource)
            return BoundingSphere();
        
        // Calculate sphere from submesh bounding box
        const auto& boundingBox = GetBoundingBox();
        glm::vec3 center = (boundingBox.Min + boundingBox.Max) * 0.5f;
        f32 radius = glm::length(boundingBox.Max - center);
        return BoundingSphere(center, radius);
    }

    BoundingBox Mesh::GetTransformedBoundingBox(const glm::mat4& transform) const
    {
        return GetBoundingBox().Transform(transform);
    }

    BoundingSphere Mesh::GetTransformedBoundingSphere(const glm::mat4& transform) const
    {
        return GetBoundingSphere().Transform(transform);
    }

    u32 Mesh::GetRendererID() const
    {
        if (!m_MeshSource)
            return 0;
        return m_MeshSource->GetVertexArray() ? m_MeshSource->GetVertexArray()->GetRendererID() : 0;
    }

    u32 Mesh::GetIndexCount() const
    {
        if (!m_MeshSource)
            return 0;
        
    const auto& submesh = GetSubmesh();
    return submesh.m_IndexCount;
    }
}
