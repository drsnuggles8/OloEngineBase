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
    namespace
    {
        // Handed out by the accessors below when the Mesh is not valid. Construction no
        // longer aborts (see the constructor), so every accessor has to be TOTAL — "fail
        // soft" that still dereferences a null MeshSource one call deeper is not a fix,
        // it just moves the crash.
        const TArray<Vertex> s_NoVertices;
        const TArray<u32> s_NoIndices;
        const Submesh s_NoSubmesh;
    } // namespace

    Mesh::Mesh(Ref<MeshSource> meshSource, u32 submeshIndex)
        : m_MeshSource(meshSource), m_SubmeshIndex(submeshIndex)
    {
        // Deliberately a warning and a not-valid state, NOT an assert (issue #694).
        //
        // A Mesh is constructed from data the engine does not control: an asset handle
        // that failed to resolve, a submesh index read straight off a binary asset pack,
        // or the stand-in the asset manager substitutes when an opt-in asset was never
        // fetched. Aborting here delegated safety to every one of the ~30 construction
        // sites, and they disagreed — MeshSerializer::TryLoadData clamped and warned while
        // MeshSerializer::DeserializeFromAssetPack (the shipped-runtime path) validated
        // nothing at all, and PlaceholderMesh built its cube over a MeshSource with no
        // submesh, so the recovery path for a missing asset WAS the crash. A fresh clone
        // that had not run scripts\Fetch-Assets.ps1 died opening VirtualGeometryStress.olo.
        //
        // The class already models the invalid state — Mesh() = default produces one,
        // IsValid() reports it, and GetIndexCount()/GetBoundingBox()/IsRigged() already
        // degraded — so failing soft into it costs nothing and makes such a mesh render as
        // nothing instead of taking the process down.
        //
        // The out-of-range index is deliberately NOT clamped to 0 the way SetMeshSource
        // clamps: silently drawing a different submesh is a wrong picture, whereas drawing
        // nothing is an honest one. A caller that genuinely wants the clamp does it at its
        // own seam, with its own message (MeshSerializer::TryLoadData does exactly that).
        if (!m_MeshSource)
        {
            OLO_CORE_WARN("Mesh: constructed with a null MeshSource - this mesh is not valid and renders nothing.");
            return;
        }

        if (const i32 submeshCount = m_MeshSource->GetSubmeshes().Num();
            m_SubmeshIndex >= static_cast<u32>(submeshCount))
        {
            OLO_CORE_WARN("Mesh: submesh index {} is out of range (the MeshSource has {} submesh(es)) - this mesh is "
                          "not valid and renders nothing.",
                          m_SubmeshIndex, submeshCount);
        }
    }

    void Mesh::SetMeshSource(Ref<MeshSource> meshSource)
    {
        // Same contract as the constructor: a null source leaves the mesh not-valid rather
        // than aborting. Assigning it is still the right move — the caller asked for it,
        // and IsValid() then reports the truth.
        if (!meshSource)
        {
            OLO_CORE_WARN("Mesh::SetMeshSource: null MeshSource - this mesh is not valid and renders nothing.");
            m_MeshSource = nullptr;
            return;
        }

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
        // Reject rather than abort (issue #694), and keep the previous index: a rejected
        // write leaves the mesh exactly as valid as it already was.
        if (!m_MeshSource)
        {
            OLO_CORE_WARN("Mesh::SetSubmeshIndex: no MeshSource - ignoring index {}.", submeshIndex);
            return;
        }

        if (const i32 submeshCount = m_MeshSource->GetSubmeshes().Num();
            submeshIndex >= static_cast<u32>(submeshCount))
        {
            OLO_CORE_ERROR("Mesh::SetSubmeshIndex: index {} out of range (the MeshSource has {} submesh(es)) - "
                           "keeping {}.",
                           submeshIndex, submeshCount, m_SubmeshIndex);
            return;
        }

        m_SubmeshIndex = submeshIndex;
    }

    const TArray<Vertex>& Mesh::GetVertices() const
    {
        if (!m_MeshSource)
            return s_NoVertices;

        return m_MeshSource->GetVertices();
    }

    const TArray<u32>& Mesh::GetIndices() const
    {
        if (!m_MeshSource)
            return s_NoIndices;

        return m_MeshSource->GetIndices();
    }

    Ref<VertexArray> Mesh::GetVertexArray() const
    {
        // Gate on IsValid(), not merely on a non-null source. The draw paths spell
        // "nothing to draw" as `if (!mesh->GetVertexArray())`, so this is where a
        // not-valid mesh has to report itself — and an out-of-range submesh index is
        // exactly as not-valid as a null source. Returning the source's VAO for one
        // would hand the renderer a real vertex array whose GetSubmesh()/GetIndexCount()
        // say zero, i.e. a draw set up to render nothing.
        if (!IsValid())
            return nullptr;

        return m_MeshSource->GetVertexArray();
    }

    const Submesh& Mesh::GetSubmesh() const
    {
        // An empty submesh (index 0, count 0) rather than a read past the end. Every
        // consumer of the returned range already treats a zero index count as "nothing to
        // draw", so a not-valid mesh renders as nothing here too — the same guarantee the
        // constructor now makes. This assert was the second copy of the ctor's, and just
        // as reachable: SceneMeshRaycast and Model call it on meshes they did not build.
        if (!m_MeshSource)
            return s_NoSubmesh;

        const auto& submeshes = m_MeshSource->GetSubmeshes();
        if (m_SubmeshIndex >= static_cast<u32>(submeshes.Num()))
            return s_NoSubmesh;

        return submeshes[m_SubmeshIndex];
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
