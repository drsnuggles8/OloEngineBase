#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneMeshRaycast.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    namespace
    {
        // A local-space direction shorter than this after the inverse-transform
        // means the entity's scale is (near-)zero along the ray — untraceable.
        constexpr f32 s_DegenerateDirEpsilon = 1e-12f;

        // True when the box has been meaningfully computed: non-inverted with
        // at least one positive extent. The default zero box (a hand-built
        // MeshSource before CalculateBounds, or an empty submesh) fails this
        // and callers then skip the early-out rather than wrongly culling.
        bool IsUsableBounds(const BoundingBox& box)
        {
            return box.Min.x <= box.Max.x && box.Min.y <= box.Max.y && box.Min.z <= box.Max.z &&
                   (box.Max.x > box.Min.x || box.Max.y > box.Min.y || box.Max.z > box.Min.z);
        }
    } // namespace

    bool SceneMeshRaycaster::CastRay(Scene& scene, const Ray& worldRay, SceneMeshRayHit& outHit)
    {
        OLO_PROFILE_FUNCTION();

        outHit = SceneMeshRayHit{};

        if (!Math::IsFinite(worldRay.Origin) || !Math::IsFinite(worldRay.Direction))
            return false;

        PruneDeadEntries();

        f32 bestWorldT = worldRay.TMax;

        // MeshComponent — every submesh range of the source, drawn at the flat
        // entity transform. SkeletonComponent entities are skipped to mirror
        // the static render path (their geometry is GPU-skinned).
        {
            auto view = scene.GetAllEntitiesWith<TransformComponent, MeshComponent>();
            for (auto entityHandle : view)
            {
                Entity entity(entityHandle, &scene);
                if (entity.HasComponent<SkeletonComponent>())
                    continue;

                const auto [transform, mesh] = view.get<TransformComponent, MeshComponent>(entityHandle);
                if (!mesh.m_MeshSource)
                    continue;

                const auto& submeshes = mesh.m_MeshSource->GetSubmeshes();
                const i32 submeshCount = submeshes.Num();
                for (i32 i = 0; i < submeshCount; ++i)
                {
                    const Submesh& submesh = submeshes[i];
                    RaycastCandidate(mesh.m_MeshSource, submesh.m_BaseIndex, submesh.m_IndexCount,
                                     transform.GetTransform(), entity, worldRay, bestWorldT, outHit);
                }
            }
        }

        // SubmeshComponent — one submesh range at the submesh entity's own
        // transform. Rigged submeshes deform on the GPU; skip them.
        {
            auto view = scene.GetAllEntitiesWith<TransformComponent, SubmeshComponent>();
            for (auto entityHandle : view)
            {
                const auto [transform, submeshComp] = view.get<TransformComponent, SubmeshComponent>(entityHandle);
                if (!submeshComp.m_Mesh || !submeshComp.m_Visible || !submeshComp.m_Mesh->IsValid())
                    continue;
                if (submeshComp.m_Mesh->IsRigged())
                    continue;

                const Submesh& submesh = submeshComp.m_Mesh->GetSubmesh();
                RaycastCandidate(submeshComp.m_Mesh->GetMeshSource(), submesh.m_BaseIndex, submesh.m_IndexCount,
                                 transform.GetTransform(), Entity(entityHandle, &scene), worldRay, bestWorldT, outHit);
            }
        }

        // ModelComponent — every mesh of the model at the entity transform
        // (node transforms are pre-baked into vertices at import).
        {
            auto view = scene.GetAllEntitiesWith<TransformComponent, ModelComponent>();
            for (auto entityHandle : view)
            {
                const auto [transform, model] = view.get<TransformComponent, ModelComponent>(entityHandle);
                if (!model.m_Model || !model.m_Visible)
                    continue;

                for (const auto& mesh : model.m_Model->GetMeshes())
                {
                    if (!mesh || !mesh->IsValid())
                        continue;

                    const Submesh& submesh = mesh->GetSubmesh();
                    RaycastCandidate(mesh->GetMeshSource(), submesh.m_BaseIndex, submesh.m_IndexCount,
                                     transform.GetTransform(), Entity(entityHandle, &scene), worldRay, bestWorldT, outHit);
                }
            }
        }

        return outHit.Hit;
    }

    void SceneMeshRaycaster::RaycastCandidate(const Ref<MeshSource>& source, u32 firstIndex, u32 indexCount,
                                              const glm::mat4& worldTransform, Entity candidateEntity,
                                              const Ray& worldRay, f32& bestWorldT, SceneMeshRayHit& outHit)
    {
        if (!source || indexCount < 3)
            return;

        if (!Math::IsFinite(worldTransform))
            return;

        const glm::mat4 invWorld = glm::inverse(worldTransform);
        if (!Math::IsFinite(invWorld))
            return; // singular transform (zero scale) — entity is unrenderable anyway

        // Transform the world ray into mesh local space. The local direction's
        // length is the world→local distance scale along the ray, so world
        // parametric distances map as localT = worldT * dirScale.
        const auto localOrigin = glm::vec3(invWorld * glm::vec4(worldRay.Origin, 1.0f));
        const glm::vec3 localDirRaw = glm::mat3(invWorld) * worldRay.Direction;
        const f32 dirScale = glm::length(localDirRaw);
        if (!(dirScale > s_DegenerateDirEpsilon) || !Math::IsFinite(localOrigin))
            return;
        const glm::vec3 localDir = localDirRaw / dirScale;

        const Ray localRay(localOrigin, localDir, worldRay.TMin * dirScale, bestWorldT * dirScale);

        // Cheap reject before paying for a BVH build: the MeshSource's local
        // bounds are precomputed at construction, so a ray that never enters
        // them can skip this candidate without building anything. Degenerate
        // (never-computed) bounds fall through to the BVH, which decides.
        if (const BoundingBox& localBounds = source->GetBoundingBox(); IsUsableBounds(localBounds))
        {
            f32 tNear = 0.0f;
            if (!RayIntersect::RayAABB(localRay.Origin, 1.0f / localRay.Direction,
                                       localBounds.Min, localBounds.Max, localRay.TMin, localRay.TMax, tNear))
                return;
        }

        const BoundingVolumeHierarchy* bvh = GetOrBuild(source, firstIndex, indexCount);
        if (bvh == nullptr)
            return;

        RayHit localHit;
        if (!bvh->CastRay(localRay, localHit))
            return;

        const f32 worldT = localHit.Distance / dirScale;
        if (worldT >= bestWorldT)
            return;

        // Map the hit back to world space. The point comes from the world-ray
        // equation directly (cheaper and more robust than re-transforming the
        // local point); the normal needs the inverse-transpose to stay
        // perpendicular under non-uniform scale.
        glm::vec3 worldNormal = glm::transpose(glm::mat3(invWorld)) * localHit.Normal;
        const f32 normalLen = glm::length(worldNormal);
        if (normalLen > s_DegenerateDirEpsilon)
            worldNormal /= normalLen;
        else
            worldNormal = -worldRay.Direction;
        // A negative-determinant (mirrored) transform flips orientation —
        // re-assert the "normal opposes the ray" contract in world space.
        if (glm::dot(worldNormal, worldRay.Direction) > 0.0f)
            worldNormal = -worldNormal;

        bestWorldT = worldT;
        outHit.Hit = true;
        outHit.Distance = worldT;
        outHit.Point = worldRay.At(worldT);
        outHit.Normal = worldNormal;
        outHit.HitEntity = candidateEntity;
        outHit.TriangleIndex = localHit.TriangleIndex;
    }

    const BoundingVolumeHierarchy* SceneMeshRaycaster::GetOrBuild(const Ref<MeshSource>& source, u32 firstIndex, u32 indexCount)
    {
        const TArray<Vertex>& vertices = source->GetVertices();
        const TArray<u32>& indices = source->GetIndices();
        const auto totalIndices = static_cast<sizet>(indices.Num());

        // Reject ranges that overrun the index buffer (malformed submesh data).
        if (indexCount < 3 || firstIndex > totalIndices || indexCount > totalIndices - firstIndex)
            return nullptr;

        const CacheKey key{ source.get(), firstIndex, indexCount };

        if (auto it = m_Cache.find(key); it != m_Cache.end())
        {
            CacheEntry& entry = it->second;
            const bool fresh = entry.Source.IsValid() &&
                               entry.VertexData == vertices.GetData() && entry.IndexData == indices.GetData() &&
                               entry.VertexCount == static_cast<sizet>(vertices.Num()) && entry.IndexCount == totalIndices;
            if (fresh)
                return entry.BVH.IsBuilt() ? &entry.BVH : nullptr;
            m_Cache.erase(it);
        }

        CacheEntry entry;
        entry.Source = WeakRef<MeshSource>(source);
        entry.VertexData = vertices.GetData();
        entry.IndexData = indices.GetData();
        entry.VertexCount = static_cast<sizet>(vertices.Num());
        entry.IndexCount = totalIndices;
        entry.BVH.Build(vertices.GetData(), entry.VertexCount, indices.GetData() + firstIndex, indexCount);

        auto [it, inserted] = m_Cache.emplace(key, std::move(entry));
        return it->second.BVH.IsBuilt() ? &it->second.BVH : nullptr;
    }

    void SceneMeshRaycaster::PruneDeadEntries()
    {
        for (auto it = m_Cache.begin(); it != m_Cache.end();)
        {
            if (!it->second.Source.IsValid())
                it = m_Cache.erase(it);
            else
                ++it;
        }
    }

    void SceneMeshRaycaster::ClearCache()
    {
        m_Cache.clear();
    }
} // namespace OloEngine
