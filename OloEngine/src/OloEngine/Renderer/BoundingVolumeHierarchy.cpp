#include "OloEnginePCH.h"
#include "OloEngine/Renderer/BoundingVolumeHierarchy.h"

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr f32 s_FloatMax = std::numeric_limits<f32>::max();

        OLO_FINLINE void ExpandToInclude(glm::vec3& boxMin, glm::vec3& boxMax, const glm::vec3& point)
        {
            boxMin = glm::min(boxMin, point);
            boxMax = glm::max(boxMax, point);
        }
    } // namespace

    void BoundingVolumeHierarchy::Clear()
    {
        m_Nodes.Empty();
        m_Triangles.Empty();
    }

    void BoundingVolumeHierarchy::Build(const Vertex* vertices, sizet vertexCount, const u32* indices, sizet indexCount)
    {
        OLO_PROFILE_FUNCTION();

        Clear();

        if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount < 3)
            return;

        const sizet triangleCount = indexCount / 3;
        m_Triangles.Reserve(static_cast<i32>(triangleCount));

        sizet skipped = 0;
        for (sizet tri = 0; tri < triangleCount; ++tri)
        {
            const u32 i0 = indices[tri * 3 + 0];
            const u32 i1 = indices[tri * 3 + 1];
            const u32 i2 = indices[tri * 3 + 2];

            // Drop triangles whose indices fall outside the vertex array — never
            // read out of bounds from a malformed index buffer.
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            {
                ++skipped;
                continue;
            }

            const glm::vec3& p0 = vertices[i0].Position;
            const glm::vec3& p1 = vertices[i1].Position;
            const glm::vec3& p2 = vertices[i2].Position;

            // Positions originate from imported assets / generated geometry; a
            // non-finite coordinate would poison every bound and slab test.
            if (!Math::IsFinite(p0) || !Math::IsFinite(p1) || !Math::IsFinite(p2))
            {
                ++skipped;
                continue;
            }

            BVHTriangle triangle;
            triangle.V0 = p0;
            triangle.V1 = p1;
            triangle.V2 = p2;
            triangle.Centroid = (p0 + p1 + p2) * (1.0f / 3.0f);
            triangle.SourceIndex = static_cast<u32>(tri);
            m_Triangles.Add(triangle);
        }

        if (skipped > 0)
        {
            OLO_CORE_WARN("BoundingVolumeHierarchy::Build: skipped {} of {} triangles "
                          "(out-of-range index or non-finite position)",
                          skipped, triangleCount);
        }

        BuildFromTriangles();
    }

    void BoundingVolumeHierarchy::Build(const MeshSource& meshSource)
    {
        const TArray<Vertex>& vertices = meshSource.GetVertices();
        const TArray<u32>& indices = meshSource.GetIndices();
        Build(vertices.GetData(), static_cast<sizet>(vertices.Num()),
              indices.GetData(), static_cast<sizet>(indices.Num()));
    }

    void BoundingVolumeHierarchy::BuildFromTriangles()
    {
        m_Nodes.Empty();

        const u32 triangleCount = static_cast<u32>(m_Triangles.Num());
        if (triangleCount == 0)
            return;

        // A binary tree with N leaves of >=1 triangle each has at most 2N-1 nodes.
        // Reserving the upper bound keeps the node array's storage stable, so the
        // contiguous left/right child indices computed during Subdivide stay valid.
        m_Nodes.Reserve(static_cast<i32>(2 * triangleCount));

        BVHNode root;
        root.LeftFirst = 0;
        root.TriCount = triangleCount;
        m_Nodes.Add(root);

        UpdateNodeBounds(0);
        Subdivide(0, 0);
    }

    void BoundingVolumeHierarchy::UpdateNodeBounds(u32 nodeIndex)
    {
        BVHNode& node = m_Nodes[nodeIndex];

        glm::vec3 boxMin(s_FloatMax);
        glm::vec3 boxMax(-s_FloatMax);

        const BVHTriangle* triangles = m_Triangles.GetData();
        for (u32 k = 0; k < node.TriCount; ++k)
        {
            const BVHTriangle& tri = triangles[node.LeftFirst + k];
            ExpandToInclude(boxMin, boxMax, tri.V0);
            ExpandToInclude(boxMin, boxMax, tri.V1);
            ExpandToInclude(boxMin, boxMax, tri.V2);
        }

        node.Bounds.Min = boxMin;
        node.Bounds.Max = boxMax;
    }

    void BoundingVolumeHierarchy::Subdivide(u32 nodeIndex, u32 depth)
    {
        struct StackEntry
        {
            u32 Node;
            u32 Depth;
        };

        std::vector<StackEntry> stack;
        stack.push_back({ nodeIndex, depth });

        while (!stack.empty())
        {
            const StackEntry entry = stack.back();
            stack.pop_back();

            const u32 first = m_Nodes[entry.Node].LeftFirst;
            const u32 count = m_Nodes[entry.Node].TriCount;

            // Leaf when small enough, or when the depth cap is reached (keeps the
            // fixed-size traversal stack from ever overflowing).
            if (count <= s_MaxLeafTriangles || entry.Depth >= s_MaxDepth)
                continue;

            // Split on the largest axis of the centroid bounds, at the spatial midpoint.
            glm::vec3 centroidMin(s_FloatMax);
            glm::vec3 centroidMax(-s_FloatMax);
            BVHTriangle* triangles = m_Triangles.GetData();
            for (u32 k = 0; k < count; ++k)
                ExpandToInclude(centroidMin, centroidMax, triangles[first + k].Centroid);

            const glm::vec3 extent = centroidMax - centroidMin;
            glm::length_t axis = 0;
            if (extent.y > extent.x)
                axis = 1;
            if (extent.z > extent[axis])
                axis = 2;

            // All centroids coincide on the chosen axis — no useful split exists.
            constexpr f32 epsilon = 1e-8f;
            if (extent[axis] < epsilon)
                continue;

            const f32 splitPos = 0.5f * (centroidMin[axis] + centroidMax[axis]);

            // In-place partition: triangles with centroid below splitPos move left.
            u32 i = first;
            u32 j = first + count;
            while (i < j)
            {
                if (triangles[i].Centroid[axis] < splitPos)
                {
                    ++i;
                }
                else
                {
                    --j;
                    std::swap(triangles[i], triangles[j]);
                }
            }

            u32 leftCount = i - first;

            // Spatial midpoint left one side empty (clustered centroids). Fall back
            // to an object-median split so the tree keeps making progress.
            if (leftCount == 0 || leftCount == count)
            {
                const u32 mid = count / 2;
                std::nth_element(triangles + first, triangles + first + mid, triangles + first + count,
                                 [axis](const BVHTriangle& a, const BVHTriangle& b)
                                 { return a.Centroid[axis] < b.Centroid[axis]; });
                leftCount = mid;
            }

            const u32 leftChild = static_cast<u32>(m_Nodes.Num());

            BVHNode left;
            left.LeftFirst = first;
            left.TriCount = leftCount;
            m_Nodes.Add(left);

            BVHNode right;
            right.LeftFirst = first + leftCount;
            right.TriCount = count - leftCount;
            m_Nodes.Add(right);

            // Convert the current node into an internal node referencing its children.
            m_Nodes[entry.Node].LeftFirst = leftChild;
            m_Nodes[entry.Node].TriCount = 0;

            UpdateNodeBounds(leftChild);
            UpdateNodeBounds(leftChild + 1);

            stack.push_back({ leftChild, entry.Depth + 1 });
            stack.push_back({ leftChild + 1, entry.Depth + 1 });
        }
    }

    BoundingBox BoundingVolumeHierarchy::GetBounds() const
    {
        if (m_Nodes.IsEmpty())
            return BoundingBox{};
        return m_Nodes[0].Bounds;
    }

    bool BoundingVolumeHierarchy::CastRay(const Ray& ray, RayHit& outHit) const
    {
        OLO_PROFILE_FUNCTION();

        outHit = RayHit{};

        if (m_Nodes.IsEmpty())
            return false;

        // Component-wise reciprocal; +/-inf for axis-parallel components is expected
        // and handled by the slab test's min/max ordering.
        const glm::vec3 invDir = 1.0f / ray.Direction;

        f32 closest = ray.TMax;
        RayHit best;
        bool found = false;

        u32 stack[s_TraversalStackSize];
        u32 stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0)
        {
            const BVHNode& node = m_Nodes[stack[--stackPtr]];

            // Authoritative prune: closest tightens as nearer hits are found, so a
            // node enqueued earlier may now be fully behind the best hit.
            if (f32 tNear; !RayIntersect::RayAABB(ray.Origin, invDir, node.Bounds.Min, node.Bounds.Max, ray.TMin, closest, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 k = 0; k < node.TriCount; ++k)
                {
                    const BVHTriangle& tri = m_Triangles[node.LeftFirst + k];

                    Ray clipped = ray;
                    clipped.TMax = closest;

                    f32 t = 0.0f;
                    f32 u = 0.0f;
                    f32 v = 0.0f;
                    bool frontFace = false;
                    if (!RayIntersect::RayTriangle(clipped, tri.V0, tri.V1, tri.V2, t, u, v, frontFace))
                        continue;

                    closest = t;
                    found = true;

                    best.Hit = true;
                    best.Distance = t;
                    best.U = u;
                    best.V = v;
                    best.FrontFace = frontFace;
                    best.TriangleIndex = tri.SourceIndex;
                    best.Point = ray.At(t);

                    // Geometric face normal from the winding, flipped to oppose the
                    // ray so it points back toward the origin.
                    glm::vec3 normal = glm::normalize(glm::cross(tri.V1 - tri.V0, tri.V2 - tri.V0));
                    if (glm::dot(normal, ray.Direction) > 0.0f)
                        normal = -normal;
                    best.Normal = normal;
                }

                continue;
            }

            // Internal node: descend nearer child first so `closest` tightens early
            // and prunes the farther subtree more often.
            const u32 leftIndex = node.LeftFirst;
            const u32 rightIndex = leftIndex + 1;
            const BVHNode& leftNode = m_Nodes[leftIndex];
            const BVHNode& rightNode = m_Nodes[rightIndex];

            f32 tLeft = 0.0f;
            f32 tRight = 0.0f;
            const bool hitLeft = RayIntersect::RayAABB(ray.Origin, invDir, leftNode.Bounds.Min, leftNode.Bounds.Max, ray.TMin, closest, tLeft);
            const bool hitRight = RayIntersect::RayAABB(ray.Origin, invDir, rightNode.Bounds.Min, rightNode.Bounds.Max, ray.TMin, closest, tRight);

            OLO_CORE_ASSERT(stackPtr + 2 <= s_TraversalStackSize, "BVH traversal stack overflow");

            if (hitLeft && hitRight)
            {
                // Push the farther child first so the nearer one is popped next.
                if (tLeft <= tRight)
                {
                    stack[stackPtr++] = rightIndex;
                    stack[stackPtr++] = leftIndex;
                }
                else
                {
                    stack[stackPtr++] = leftIndex;
                    stack[stackPtr++] = rightIndex;
                }
            }
            else if (hitLeft)
            {
                stack[stackPtr++] = leftIndex;
            }
            else if (hitRight)
            {
                stack[stackPtr++] = rightIndex;
            }
        }

        if (found)
            outHit = best;
        return found;
    }

    bool BoundingVolumeHierarchy::CastRayAny(const Ray& ray) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Nodes.IsEmpty())
            return false;

        const glm::vec3 invDir = 1.0f / ray.Direction;

        u32 stack[s_TraversalStackSize];
        u32 stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0)
        {
            const BVHNode& node = m_Nodes[stack[--stackPtr]];

            if (f32 tNear; !RayIntersect::RayAABB(ray.Origin, invDir, node.Bounds.Min, node.Bounds.Max, ray.TMin, ray.TMax, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 k = 0; k < node.TriCount; ++k)
                {
                    const BVHTriangle& tri = m_Triangles[node.LeftFirst + k];
                    f32 t = 0.0f;
                    f32 u = 0.0f;
                    f32 v = 0.0f;
                    bool frontFace = false;
                    if (RayIntersect::RayTriangle(ray, tri.V0, tri.V1, tri.V2, t, u, v, frontFace))
                        return true;
                }

                continue;
            }

            OLO_CORE_ASSERT(stackPtr + 2 <= s_TraversalStackSize, "BVH traversal stack overflow");
            stack[stackPtr++] = node.LeftFirst;
            stack[stackPtr++] = node.LeftFirst + 1;
        }

        return false;
    }
} // namespace OloEngine
