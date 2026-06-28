#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolumeHierarchy.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <unordered_map>

namespace OloEngine
{
    class Scene;
    class MeshSource;

    // @brief World-space closest-hit result of a scene-wide mesh raycast.
    //
    // Point/Normal/Distance are in world space; Distance is measured along the
    // (unit-direction) world ray, so it is directly comparable with other
    // world-space hit distances (e.g. the terrain heightmap raycast).
    struct SceneMeshRayHit
    {
        bool Hit = false;
        f32 Distance = 0.0f;      // world distance along the ray (unit Direction)
        glm::vec3 Point{ 0.0f };  // world-space hit position
        glm::vec3 Normal{ 0.0f }; // world-space geometric normal, oriented against the ray
        Entity HitEntity;         // entity that owns the struck triangle
        // Triangle ordinal RELATIVE to the struck submesh's index range (the
        // (firstIndex, indexCount) slice the BVH was built over) — i.e. the
        // triangle's first index sits at firstIndex + TriangleIndex * 3 in the
        // MeshSource index buffer. Not unique across a multi-submesh
        // MeshComponent entity, which traces one range per submesh.
        u32 TriangleIndex = 0;
    };

    // @brief World-space closest-hit raycaster over a scene's mesh entities,
    // backed by lazily built, cached per-mesh BoundingVolumeHierarchy trees.
    //
    // This is the editor-tool raycast the BVH was built for (see
    // docs/design/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md §1.2): the instance scatter
    // brush queries it every frame to place instances on mesh surfaces, and any
    // future tool that needs "what mesh does this ray hit" (gizmo snapping,
    // click-to-select on mesh backings) can share the same instance and cache.
    //
    // Candidate geometry mirrors what Scene's static render loop draws, at the
    // same flat entity transform (no parent-hierarchy walk — render parity):
    //   - MeshComponent: every submesh range of its MeshSource. Entities with a
    //     SkeletonComponent are skipped, exactly like the static draw path —
    //     their geometry is GPU-skinned, so a bind-pose raycast would report
    //     hits floating off the posed surface.
    //   - SubmeshComponent: the referenced submesh's index range (rigged
    //     submeshes skipped for the same bind-pose reason; m_Visible respected).
    //   - ModelComponent: each of the model's meshes (m_Visible respected;
    //     node transforms are baked into vertices at import).
    // InstancedMeshComponent instances are deliberately NOT raycast — the
    // scatter brush paints *into* those batches, and self-hits would stack
    // placements on top of previously painted instances.
    //
    // Queries run entirely on the CPU in mesh local space: the world ray is
    // transformed by the entity's inverse world matrix, traced through the
    // mesh's BVH, and the hit is mapped back (normal via inverse-transpose,
    // renormalized — correct under non-uniform scale).
    //
    // Cache lifecycle: entries are keyed by (MeshSource address, index range)
    // and built on first query. Each entry carries a WeakRef liveness probe
    // plus a geometry fingerprint (vertex/index counts and data pointers), so
    // entries are dropped or rebuilt when a MeshSource dies, is hot-reloaded
    // into new storage, or has its buffers re-allocated — no explicit
    // AssetReloadedEvent hook is needed. In-place vertex edits that keep the
    // buffer allocation and size unchanged are NOT detected; call ClearCache()
    // after such mutations. Not thread-safe — confine one instance to one
    // thread (the editor uses it from the main thread only).
    class SceneMeshRaycaster
    {
      public:
        SceneMeshRaycaster() = default;

        // Closest-hit query against the scene's mesh entities. The ray's
        // Direction is normalized internally, so [TMin, TMax] and the reported
        // Distance are always world-space distances (pass the terrain hit
        // distance as TMax to only accept mesh hits in front of the terrain).
        // Degenerate rays (zero/non-finite direction, NaN or swapped bounds)
        // miss. Returns true and fills `outHit` on a hit; returns false (and
        // resets outHit) on a miss.
        bool CastRay(Scene& scene, const Ray& worldRay, SceneMeshRayHit& outHit);

        // Drop every cached BVH (next query rebuilds on demand). Call after
        // in-place geometry edits the fingerprint cannot see.
        void ClearCache();

        // Number of live cached BVH entries (after the most recent prune).
        [[nodiscard]] sizet GetCacheEntryCount() const
        {
            return m_Cache.size();
        }

      private:
        struct CacheKey
        {
            const MeshSource* Source = nullptr;
            u32 FirstIndex = 0;
            u32 IndexCount = 0;

            auto operator==(const CacheKey&) const -> bool = default;
        };

        struct CacheKeyHash
        {
            sizet operator()(const CacheKey& key) const
            {
                sizet h = std::hash<const void*>{}(key.Source);
                h ^= std::hash<u64>{}((static_cast<u64>(key.FirstIndex) << 32) | key.IndexCount) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                return h;
            }
        };

        struct CacheEntry
        {
            WeakRef<MeshSource> Source; // liveness probe — entry dies with the mesh
            // Geometry fingerprint: detects a MeshSource rebuilt in place (or a
            // new MeshSource reusing a dead one's address) so a stale BVH is
            // never served for changed geometry.
            const void* VertexData = nullptr;
            const void* IndexData = nullptr;
            sizet VertexCount = 0;
            sizet IndexCount = 0;
            BoundingVolumeHierarchy BVH;
        };

        // Trace `worldRay` against one candidate range of `source` placed at
        // `worldTransform`, updating `outHit`/`bestWorldT` when a closer hit is
        // found. `candidateEntity` is recorded on a hit.
        void RaycastCandidate(const Ref<MeshSource>& source, u32 firstIndex, u32 indexCount,
                              const glm::mat4& worldTransform, Entity candidateEntity,
                              const Ray& worldRay, f32& bestWorldT, SceneMeshRayHit& outHit);

        // Fetch the BVH for a mesh range, (re)building when missing or stale.
        // Returns null for empty/invalid ranges.
        const BoundingVolumeHierarchy* GetOrBuild(const Ref<MeshSource>& source, u32 firstIndex, u32 indexCount);

        // Drop entries whose MeshSource has been destroyed.
        void PruneDeadEntries();

        std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> m_Cache;
    };
} // namespace OloEngine
