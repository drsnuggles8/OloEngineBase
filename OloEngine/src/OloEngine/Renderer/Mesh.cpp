#include "OloEnginePCH.h"
#include "Mesh.h"
#include "VertexArray.h"
#include "MaterialAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include <numeric>

namespace OloEngine
{
    Mesh::Mesh(Ref<MeshSource> meshSource, u32 submeshIndex)
        : m_MeshSource(meshSource), m_SubmeshIndex(submeshIndex)
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        OLO_CORE_ASSERT(m_SubmeshIndex < m_MeshSource->GetSubmeshes().size(), "Submesh index out of range!");
    }

    void Mesh::SetMeshSource(Ref<MeshSource> meshSource)
    {
        OLO_CORE_ASSERT(meshSource, "MeshSource cannot be null!");
        
        // If changing to a different MeshSource, validate submesh index is still valid
        if (meshSource != m_MeshSource)
        {
            // Adjust submesh index if it exceeds new meshSource's submesh count
            if (m_SubmeshIndex >= meshSource->GetSubmeshes().size())
            {
                OLO_CORE_WARN("Mesh::SetMeshSource: Submesh index {} exceeds new MeshSource submesh count ({}), resetting to 0",
                              m_SubmeshIndex, meshSource->GetSubmeshes().size());
                m_SubmeshIndex = 0;
            }
        }
        
        m_MeshSource = meshSource;
    }

    void Mesh::SetSubmeshIndex(u32 submeshIndex)
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null! Cannot set submesh index on invalid Mesh.");
        if (submeshIndex >= m_MeshSource->GetSubmeshes().size())
        {
            OLO_CORE_ERROR("Submesh index {} out of range! MeshSource has {} submeshes.", 
                           submeshIndex, m_MeshSource->GetSubmeshes().size());
            OLO_CORE_ASSERT(false);
        }
        m_SubmeshIndex = submeshIndex;
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

    Ref<VertexArray> Mesh::GetVertexArray() const
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
        if (!m_MeshSource)
            return false;
        
        const auto& submeshes = m_MeshSource->GetSubmeshes();
        if (m_SubmeshIndex >= submeshes.size())
            return false;
        
        return m_MeshSource->IsSubmeshRigged(m_SubmeshIndex);
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
        
        const auto& submeshes = m_MeshSource->GetSubmeshes();
        if (m_SubmeshIndex < submeshes.size())
        {
            return submeshes[m_SubmeshIndex].m_IndexCount;
        }
        
        // Return 0 if submesh index is invalid
        return 0;
    }

    ////////////////////////////////////////////////////////
    // StaticMesh //////////////////////////////////////////
    ////////////////////////////////////////////////////////

    StaticMesh::StaticMesh(AssetHandle meshSource, bool generateColliders)
        : m_MeshSource(meshSource), m_GenerateColliders(generateColliders)
    {
        SetupStaticMesh();
    }

    StaticMesh::StaticMesh(AssetHandle meshSource, const std::vector<u32>& submeshes, bool generateColliders)
        : m_MeshSource(meshSource), m_Submeshes(submeshes), m_GenerateColliders(generateColliders)
    {
        SetupStaticMesh();
    }

    void StaticMesh::OnDependencyUpdated(AssetHandle handle)
    {
        if (handle == m_MeshSource)
        {
            // Reload mesh when the source asset is updated
            SetupStaticMesh();
        }
    }

    void StaticMesh::SetSubmeshes(const std::vector<u32>& submeshes)
    {
        m_Submeshes = submeshes;
        
        // Re-setup with new submeshes (validation will be handled in SetupStaticMesh)
        SetupStaticMesh();
    }

    void StaticMesh::SetupStaticMesh()
    {
        if (m_MeshSource == 0)
        {
            OLO_CORE_WARN("StaticMesh::SetupStaticMesh - Invalid mesh source handle");
            return;
        }

        // Get the mesh source asset
        auto meshSourceAsset = AssetManager::GetAsset<MeshSource>(m_MeshSource);
        if (!meshSourceAsset)
        {
            OLO_CORE_WARN("StaticMesh::SetupStaticMesh - Failed to load mesh source asset {}", m_MeshSource);
            return;
        }

        // Create material table if it doesn't exist
        if (!m_Materials)
        {
            m_Materials = Ref<MaterialTable>::Create(1);
        }

        // Copy materials from mesh source
        const auto& sourceMaterials = meshSourceAsset->GetMaterials();
        
        // Copy materials from the mesh source map
        for (const auto& [materialIndex, materialHandle] : sourceMaterials)
        {
            if (materialHandle != 0) // Only set valid material handles
            {
                m_Materials->SetMaterial(materialIndex, materialHandle);
            }
        }

        // If no specific submeshes were requested, use all submeshes
        if (m_Submeshes.empty())
        {
            const auto& submeshes = meshSourceAsset->GetSubmeshes();
            m_Submeshes.resize(submeshes.size());
            std::iota(m_Submeshes.begin(), m_Submeshes.end(), 0u);
        }

        // Validate submesh indices
        const auto& submeshes = meshSourceAsset->GetSubmeshes();
        for (auto it = m_Submeshes.begin(); it != m_Submeshes.end();)
        {
            if (*it >= submeshes.size())
            {
                OLO_CORE_WARN("StaticMesh::SetupStaticMesh - Invalid submesh index {} (max: {}), removing", *it, submeshes.size() - 1);
                it = m_Submeshes.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // TODO: Generate colliders if requested
        if (m_GenerateColliders)
        {
            // Collider generation would go here
            OLO_CORE_TRACE("StaticMesh::SetupStaticMesh - Collider generation not yet implemented");
        }
    }
}
