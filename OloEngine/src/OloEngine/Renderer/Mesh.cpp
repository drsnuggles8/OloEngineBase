#include "OloEnginePCH.h"
#include "Mesh.h"
#include "VertexArray.h"
#include "MaterialAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Physics3D/MeshColliderCache.h"
#include <numeric>

namespace OloEngine
{
    Mesh::Mesh(Ref<MeshSource> meshSource, u32 submeshIndex)
        : m_MeshSource(meshSource), m_SubmeshIndex(submeshIndex)
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        OLO_CORE_ASSERT(m_SubmeshIndex < static_cast<u32>(m_MeshSource->GetSubmeshes().Num()), "Submesh index out of range!");
    }

    void Mesh::SetMeshSource(Ref<MeshSource> meshSource)
    {
        OLO_CORE_ASSERT(meshSource, "MeshSource cannot be null!");

        // If changing to a different MeshSource, validate submesh index is still valid
        if (meshSource != m_MeshSource)
        {
            // Adjust submesh index if it exceeds new meshSource's submesh count
            if (m_SubmeshIndex >= static_cast<u32>(meshSource->GetSubmeshes().Num()))
            {
                OLO_CORE_WARN("Mesh::SetMeshSource: Submesh index {} exceeds new MeshSource submesh count ({}), resetting to 0",
                              m_SubmeshIndex, meshSource->GetSubmeshes().Num());
                m_SubmeshIndex = 0;
            }
        }

        m_MeshSource = meshSource;
    }

    void Mesh::SetSubmeshIndex(u32 submeshIndex)
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null! Cannot set submesh index on invalid Mesh.");
        if (submeshIndex >= static_cast<u32>(m_MeshSource->GetSubmeshes().Num()))
        {
            OLO_CORE_ERROR("Submesh index {} out of range! MeshSource has {} submeshes.",
                           submeshIndex, m_MeshSource->GetSubmeshes().Num());
            OLO_CORE_ASSERT(false, "Submesh index {} out of range", submeshIndex);
        }
        m_SubmeshIndex = submeshIndex;
    }

    const TArray<Vertex>& Mesh::GetVertices() const
    {
        OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
        return m_MeshSource->GetVertices();
    }

    const TArray<u32>& Mesh::GetIndices() const
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
        OLO_CORE_ASSERT(m_SubmeshIndex < static_cast<u32>(m_MeshSource->GetSubmeshes().Num()), "Submesh index out of range!");
        return m_MeshSource->GetSubmeshes()[m_SubmeshIndex];
    }

    bool Mesh::IsRigged() const
    {
        if (!m_MeshSource)
            return false;

        if (const auto& submeshes = m_MeshSource->GetSubmeshes(); m_SubmeshIndex >= static_cast<u32>(submeshes.Num()))
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
        if (const auto& submeshes = m_MeshSource->GetSubmeshes(); m_SubmeshIndex < static_cast<u32>(submeshes.Num()))
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

        if (const auto& submeshes = m_MeshSource->GetSubmeshes(); m_SubmeshIndex < static_cast<u32>(submeshes.Num()))
        {
            return submeshes[m_SubmeshIndex].m_IndexCount;
        }

        // Return 0 if submesh index is invalid
        return 0;
    }

    u32 Mesh::GetBaseIndex() const
    {
        if (!m_MeshSource)
            return 0;

        if (const auto& submeshes = m_MeshSource->GetSubmeshes(); m_SubmeshIndex < static_cast<u32>(submeshes.Num()))
        {
            return submeshes[m_SubmeshIndex].m_BaseIndex;
        }

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

    StaticMesh::StaticMesh(AssetHandle meshSource, const TArray<u32>& submeshes, bool generateColliders)
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

    void StaticMesh::SetSubmeshes(const TArray<u32>& submeshes)
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

        // Copy materials from mesh source (use const reference to access const GetMaterials())
        const MeshSource& meshSourceRef = *meshSourceAsset;
        const auto& sourceMaterials = meshSourceRef.GetMaterials();

        // Copy materials from the mesh source map
        for (const auto& [materialIndex, materialHandle] : sourceMaterials)
        {
            if (materialHandle != 0) // Only set valid material handles
            {
                m_Materials->SetMaterial(materialIndex, materialHandle);
            }
        }

        // If no specific submeshes were requested, use all submeshes
        if (m_Submeshes.IsEmpty())
        {
            const auto& submeshes = meshSourceAsset->GetSubmeshes();
            m_Submeshes.SetNum(submeshes.Num());
            for (i32 i = 0; i < m_Submeshes.Num(); ++i)
            {
                m_Submeshes[i] = static_cast<u32>(i);
            }
        }

        // Validate submesh indices
        const auto& submeshes = meshSourceAsset->GetSubmeshes();
        for (i32 i = m_Submeshes.Num() - 1; i >= 0; --i)
        {
            if (m_Submeshes[i] >= static_cast<u32>(submeshes.Num()))
            {
                OLO_CORE_WARN("StaticMesh::SetupStaticMesh - Invalid submesh index {} (max: {}), removing", m_Submeshes[i], submeshes.Num() - 1);
                m_Submeshes.RemoveAt(i);
            }
        }

        // Generate physics colliders from the mesh geometry if requested.
        if (m_GenerateColliders)
        {
            GenerateColliders(meshSourceAsset);
        }
    }

    void StaticMesh::GenerateColliders(const Ref<MeshSource>& meshSource)
    {
        OLO_CORE_ASSERT(meshSource, "StaticMesh::GenerateColliders called with null mesh source");

        // The mesh-collider cooker (MeshCookingFactory) consumes a Mesh asset and
        // pulls geometry from its MeshSource. A StaticMesh only holds a MeshSource
        // handle, so we wrap the source in a lightweight memory-only Mesh that the
        // generated MeshColliderAsset can reference. Cooking itself stays lazy:
        // physics-body creation / nav-mesh generation call
        // MeshColliderCache::GetMeshData(asset) on demand (the engine's established
        // path). We deliberately do not cook here — the cooker is disk-backed and
        // its async secondary-cook path is unsafe to fire during asset (re)load.
        if (meshSource->GetSubmeshes().IsEmpty())
        {
            OLO_CORE_WARN("StaticMesh::SetupStaticMesh - GenerateColliders requested but mesh source {} has no submeshes; skipping collider generation", m_MeshSource);
            return;
        }

        // Re-setup (hot-reload of the source, or a submesh-set change): keep the
        // same handles so any references stay valid, refresh the wrapper Mesh's
        // geometry, and drop stale cooked data so the next consumer re-cooks.
        if (m_GeneratedColliderHandle != 0)
        {
            if (auto wrapperMesh = AssetManager::GetAsset<Mesh>(m_GeneratedColliderMeshHandle))
            {
                wrapperMesh->SetMeshSource(meshSource);
            }
            if (auto colliderAsset = AssetManager::GetAsset<MeshColliderAsset>(m_GeneratedColliderHandle))
            {
                MeshColliderCache::GetInstance().InvalidateCache(colliderAsset);
            }
            OLO_CORE_TRACE("StaticMesh::SetupStaticMesh - Refreshed generated collider asset {}", m_GeneratedColliderHandle);
            return;
        }

        Ref<Mesh> wrapperMesh = Ref<Mesh>::Create(meshSource, 0u);
        m_GeneratedColliderMeshHandle = AssetManager::AddMemoryOnlyAsset<Mesh>(wrapperMesh);

        Ref<MeshColliderAsset> colliderAsset = Ref<MeshColliderAsset>::Create(m_GeneratedColliderMeshHandle);
        m_GeneratedColliderHandle = AssetManager::AddMemoryOnlyAsset<MeshColliderAsset>(colliderAsset);

        OLO_CORE_TRACE("StaticMesh::SetupStaticMesh - Generated collider asset {} (wrapper mesh {}) for mesh source {}",
                       m_GeneratedColliderHandle, m_GeneratedColliderMeshHandle, m_MeshSource);
    }
} // namespace OloEngine
