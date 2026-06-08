#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Containers/Array.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class MeshSource;

    // @brief CPU bounding-volume hierarchy over a mesh's triangle soup, for
    // ray-vs-mesh queries (closest hit + any hit).
    //
    // This is the general-purpose ray-vs-mesh accelerator the engine was missing
    // (see docs/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md §1.2). It is pure CPU/glm —
    // no GL context, no Jolt — so it can back editor tools that need world
    // raycasts: the scatter brush's mesh-surface placement, gizmo snapping,
    // click-to-select on mesh backings, debug raycast viz.
    //
    // Build it once from a MeshSource (or raw vertex/index arrays); the tree caches
    // the triangle geometry, so the source mesh need not outlive the BVH. Queries
    // are const and read-only, so a built BVH may be shared across threads.
    //
    // The build is a binary median/midpoint split over triangle centroids: each
    // node is split on the largest axis of its centroid bounds at the spatial
    // midpoint, with an object-median fallback (nth_element) when the midpoint
    // leaves one side empty. Nodes are stored in a flat array with children
    // allocated contiguously (right child == left child + 1). This is intentionally
    // simple; a SAH build can replace SplitNode later if profiling wants it.
    class BoundingVolumeHierarchy
    {
      public:
        BoundingVolumeHierarchy() = default;

        // Build over a triangle list. `vertices`/`vertexCount` supply positions
        // (only Vertex::Position is read); `indices`/`indexCount` form the triangle
        // list (every 3 indices = 1 triangle). Triangles referencing out-of-range
        // indices or non-finite positions are skipped. Re-building clears prior
        // state. Safe to call with null/zero inputs (produces an empty BVH).
        void Build(const Vertex* vertices, sizet vertexCount, const u32* indices, sizet indexCount);

        // Convenience overload: build from a MeshSource's full geometry
        // (GetVertices()/GetIndices()). The MeshSource is the authoritative geometry
        // and need not be GPU-built (Build() is not required).
        void Build(const MeshSource& meshSource);

        // Drop all nodes and triangle data; IsBuilt() becomes false.
        void Clear();

        // True once Build() has produced at least one node (i.e. the mesh had at
        // least one valid triangle).
        [[nodiscard]] bool IsBuilt() const
        {
            return !m_Nodes.IsEmpty();
        }

        // Closest-hit query. Returns true and fills `outHit` with the nearest
        // triangle intersection in [ray.TMin, ray.TMax]; returns false (and clears
        // outHit) on a miss or an unbuilt BVH.
        bool CastRay(const Ray& ray, RayHit& outHit) const;

        // Any-hit query: returns true as soon as ANY triangle is hit within
        // [ray.TMin, ray.TMax], without finding the closest. Cheaper than CastRay —
        // use for shadow / occlusion / line-of-sight tests where only hit/no-hit
        // matters.
        [[nodiscard]] bool CastRayAny(const Ray& ray) const;

        // Number of nodes in the flat tree (internal + leaf).
        [[nodiscard]] u32 GetNodeCount() const
        {
            return static_cast<u32>(m_Nodes.Num());
        }

        // Number of triangles indexed by the BVH (degenerate / out-of-range
        // triangles from the source mesh are excluded).
        [[nodiscard]] u32 GetTriangleCount() const
        {
            return static_cast<u32>(m_Triangles.Num());
        }

        // World-space AABB of the whole mesh (root node bounds). Returns a
        // zero-size box when the BVH is empty.
        [[nodiscard]] BoundingBox GetBounds() const;

      private:
        // A flat-array BVH node. A leaf has TriCount > 0 and indexes
        // m_Triangles[LeftFirst .. LeftFirst + TriCount). An internal node has
        // TriCount == 0 and LeftFirst is the index of its left child in m_Nodes;
        // the right child is LeftFirst + 1.
        struct BVHNode
        {
            BoundingBox Bounds;
            u32 LeftFirst = 0;
            u32 TriCount = 0;

            [[nodiscard]] bool IsLeaf() const
            {
                return TriCount > 0;
            }
        };

        // Precomputed triangle: the three vertex positions, its centroid (split
        // key), and the original triangle ordinal so hits can map back to the
        // source index buffer.
        struct BVHTriangle
        {
            glm::vec3 V0{ 0.0f };
            glm::vec3 V1{ 0.0f };
            glm::vec3 V2{ 0.0f };
            glm::vec3 Centroid{ 0.0f };
            u32 SourceIndex = 0;
        };

        void BuildFromTriangles();
        void UpdateNodeBounds(u32 nodeIndex);
        // Recursively (iteratively) split node ranges into children; `nodeIndex`/
        // `depth` seed the work stack.
        void Subdivide(u32 nodeIndex, u32 depth);

        TArray<BVHNode> m_Nodes;
        TArray<BVHTriangle> m_Triangles;

        // Leaf when a node holds at most this many triangles.
        static constexpr u32 s_MaxLeafTriangles = 4;
        // Hard cap on tree depth so the fixed-size traversal stack can never
        // overflow, even on adversarial (exponentially-clustered) geometry. A node
        // reaching this depth becomes a leaf regardless of triangle count.
        static constexpr u32 s_MaxDepth = 60;
        // Traversal stack size — must exceed s_MaxDepth (one slot per nesting level
        // plus the sibling pushed alongside the descent).
        static constexpr u32 s_TraversalStackSize = 64;
    };
} // namespace OloEngine
