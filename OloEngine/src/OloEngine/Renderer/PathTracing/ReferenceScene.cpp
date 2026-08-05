#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace OloEngine::PathTracing
{
    namespace
    {
        // A world AABB is only usable as a traversal bound if it is finite;
        // an instance whose transform produced a NaN corner would otherwise
        // poison every slab test that touches it.
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        [[nodiscard]] bool IsFinite(const BoundingBox& box)
        {
            return IsFinite(box.Min) && IsFinite(box.Max);
        }
    } // namespace

    // =========================================================================
    // ReferenceGeometry
    // =========================================================================

    ReferenceGeometry::ReferenceGeometry(std::vector<Vertex> vertices, std::vector<u32> indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        m_BVH.Build(m_Vertices.data(), m_Vertices.size(), m_Indices.data(), m_Indices.size());
        m_LocalBounds = m_BVH.IsBuilt() ? m_BVH.GetBounds() : BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));
    }

    glm::vec3 ReferenceGeometry::InterpolateNormal(u32 triangleIndex, f32 u, f32 v) const
    {
        const sizet base = static_cast<sizet>(triangleIndex) * 3;
        if (base + 2 >= m_Indices.size())
            return glm::vec3(0.0f, 1.0f, 0.0f);

        const u32 i0 = m_Indices[base + 0];
        const u32 i1 = m_Indices[base + 1];
        const u32 i2 = m_Indices[base + 2];
        if (i0 >= m_Vertices.size() || i1 >= m_Vertices.size() || i2 >= m_Vertices.size())
            return glm::vec3(0.0f, 1.0f, 0.0f);

        const f32 w = 1.0f - u - v;
        const glm::vec3 n = m_Vertices[i0].Normal * w + m_Vertices[i1].Normal * u + m_Vertices[i2].Normal * v;

        // `!(x > eps)` so a NaN also takes the fallback — same guard shape as
        // PBRCommon.glsl's sanitizeSurfaceNormal, and for the same reason.
        const f32 lengthSq = glm::dot(n, n);
        if (!(lengthSq > 1e-20f))
        {
            const glm::vec3 e1 = m_Vertices[i1].Position - m_Vertices[i0].Position;
            const glm::vec3 e2 = m_Vertices[i2].Position - m_Vertices[i0].Position;
            const glm::vec3 geometric = glm::cross(e1, e2);
            const f32 geometricLengthSq = glm::dot(geometric, geometric);
            if (!(geometricLengthSq > 1e-20f))
                return glm::vec3(0.0f, 1.0f, 0.0f);
            return geometric * glm::inversesqrt(geometricLengthSq);
        }

        return n * glm::inversesqrt(lengthSq);
    }

    // =========================================================================
    // ReferenceScene — construction
    // =========================================================================

    u32 ReferenceScene::AddMaterial(const ReferenceMaterial& material)
    {
        m_Materials.push_back(material);
        m_Built = false;
        return static_cast<u32>(m_Materials.size() - 1);
    }

    u32 ReferenceScene::AddGeometry(std::vector<Vertex> vertices, std::vector<u32> indices)
    {
        m_Geometries.push_back(std::make_unique<ReferenceGeometry>(std::move(vertices), std::move(indices)));
        m_Built = false;
        return static_cast<u32>(m_Geometries.size() - 1);
    }

    u32 ReferenceScene::AddQuadGeometry(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3)
    {
        glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
        const f32 lengthSq = glm::dot(normal, normal);
        normal = (lengthSq > 1e-20f) ? normal * glm::inversesqrt(lengthSq) : glm::vec3(0.0f, 1.0f, 0.0f);

        std::vector<Vertex> vertices = {
            Vertex(p0, normal, glm::vec2(0.0f, 0.0f)),
            Vertex(p1, normal, glm::vec2(1.0f, 0.0f)),
            Vertex(p2, normal, glm::vec2(1.0f, 1.0f)),
            Vertex(p3, normal, glm::vec2(0.0f, 1.0f))
        };
        std::vector<u32> indices = { 0, 1, 2, 0, 2, 3 };
        return AddGeometry(std::move(vertices), std::move(indices));
    }

    u32 ReferenceScene::AddInstance(u32 geometryIndex, const glm::mat4& transform, u32 materialIndex)
    {
        constexpr u32 kInvalid = std::numeric_limits<u32>::max();

        if (geometryIndex >= m_Geometries.size())
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: geometry index {} out of range ({} geometries)",
                           geometryIndex, m_Geometries.size());
            return kInvalid;
        }
        if (materialIndex >= m_Materials.size())
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: material index {} out of range ({} materials)",
                           materialIndex, m_Materials.size());
            return kInvalid;
        }

        // Reject anything that is not rigid + uniform scale. The traversal
        // converts a running world-space TMax into instance-local units by a
        // single scalar; under non-uniform scale no such scalar exists, and the
        // resulting hit distances would be wrong in a way that still produces a
        // perfectly plausible image. Fail loudly at construction instead.
        const glm::vec3 axisX(transform[0]);
        const glm::vec3 axisY(transform[1]);
        const glm::vec3 axisZ(transform[2]);
        const f32 scaleX = glm::length(axisX);
        const f32 scaleY = glm::length(axisY);
        const f32 scaleZ = glm::length(axisZ);
        const f32 maxScale = std::max({ scaleX, scaleY, scaleZ });
        const f32 minScale = std::min({ scaleX, scaleY, scaleZ });
        if (!(minScale > 1e-6f) || (maxScale - minScale) > 1e-4f * maxScale)
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform must be rigid + uniform scale "
                           "(got per-axis scales {}, {}, {}); non-uniform scale would silently corrupt hit distances",
                           scaleX, scaleY, scaleZ);
            return kInvalid;
        }

        // A MIRRORED basis passes the test above — glm::length is never negative,
        // so a reflected axis looks like a perfectly uniform scale. It is not
        // harmless: a reflection flips triangle winding, and both places this
        // scene derives an outward direction from winding invert with it —
        // `BuildEmissiveList`'s cross(V1-V0, V2-V0) and `IntersectInstance`'s
        // geometric normal. A one-sided emitter would then emit from its back
        // face and the room would render black, which reads as an integrator
        // bug rather than a scene-setup one. Reject it as loudly as the
        // non-uniform case.
        if (glm::dot(glm::cross(axisX, axisY), axisZ) <= 0.0f)
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform mirrors the basis (non-positive determinant); "
                           "this flips triangle winding and inverts every geometric normal");
            return kInvalid;
        }

        ReferenceInstance instance;
        instance.GeometryIndex = geometryIndex;
        instance.MaterialIndex = materialIndex;
        instance.Transform = transform;
        instance.InverseTransform = glm::inverse(transform);
        instance.NormalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        instance.UniformScale = maxScale;
        instance.WorldBounds = m_Geometries[geometryIndex]->GetLocalBounds().Transform(transform);

        if (!IsFinite(instance.WorldBounds))
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform produced a non-finite world AABB");
            return kInvalid;
        }

        m_Instances.push_back(instance);
        m_Built = false;
        return static_cast<u32>(m_Instances.size() - 1);
    }

    void ReferenceScene::AddLight(const ReferenceLight& light)
    {
        m_Lights.push_back(light);
        m_Built = false;
    }

    const ReferenceMaterial& ReferenceScene::GetMaterial(u32 index) const
    {
        static const ReferenceMaterial s_Fallback{};
        if (index >= m_Materials.size())
            return s_Fallback;
        return m_Materials[index];
    }

    // =========================================================================
    // Build
    // =========================================================================

    void ReferenceScene::Build()
    {
        BuildTLAS();
        BuildEmissiveList();
        m_Built = true;
    }

    void ReferenceScene::BuildTLAS()
    {
        m_TLASNodes.clear();
        m_TLASInstanceRefs.clear();
        m_InstanceCentroids.clear();
        m_WorldBounds = BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));

        if (m_Instances.empty())
            return;

        m_TLASInstanceRefs.resize(m_Instances.size());
        std::iota(m_TLASInstanceRefs.begin(), m_TLASInstanceRefs.end(), 0u);

        m_InstanceCentroids.reserve(m_Instances.size());
        for (const ReferenceInstance& instance : m_Instances)
            m_InstanceCentroids.push_back(instance.WorldBounds.GetCenter());

        // Worst case for a binary tree over N leaves with >= 1 leaf each.
        m_TLASNodes.reserve(m_Instances.size() * 2);
        TLASNode& root = m_TLASNodes.emplace_back();
        root.LeftFirst = 0;
        root.Count = static_cast<u32>(m_TLASInstanceRefs.size());
        UpdateTLASNodeBounds(0);
        SubdivideTLAS(0, 0);

        m_WorldBounds = m_TLASNodes[0].Bounds;
    }

    void ReferenceScene::UpdateTLASNodeBounds(u32 nodeIndex)
    {
        TLASNode& node = m_TLASNodes[nodeIndex];
        glm::vec3 boundsMin(std::numeric_limits<f32>::max());
        glm::vec3 boundsMax(std::numeric_limits<f32>::lowest());
        for (u32 i = 0; i < node.Count; ++i)
        {
            const BoundingBox& instanceBounds = m_Instances[m_TLASInstanceRefs[node.LeftFirst + i]].WorldBounds;
            boundsMin = glm::min(boundsMin, instanceBounds.Min);
            boundsMax = glm::max(boundsMax, instanceBounds.Max);
        }
        node.Bounds = BoundingBox(boundsMin, boundsMax);
    }

    void ReferenceScene::SubdivideTLAS(u32 nodeIndex, u32 depth)
    {
        // Iterative to keep the recursion depth off the C stack, mirroring
        // BoundingVolumeHierarchy::Subdivide.
        struct Work
        {
            u32 NodeIndex;
            u32 Depth;
        };
        std::vector<Work> stack;
        stack.push_back({ nodeIndex, depth });

        while (!stack.empty())
        {
            const Work work = stack.back();
            stack.pop_back();

            TLASNode& node = m_TLASNodes[work.NodeIndex];
            if (node.Count <= s_MaxLeafInstances || work.Depth >= s_MaxDepth)
                continue;

            // Split on the largest axis of the CENTROID bounds at the spatial
            // midpoint, with an object-median fallback when the midpoint leaves
            // one side empty (identical policy to the bottom-level BVH).
            glm::vec3 centroidMin(std::numeric_limits<f32>::max());
            glm::vec3 centroidMax(std::numeric_limits<f32>::lowest());
            for (u32 i = 0; i < node.Count; ++i)
            {
                const glm::vec3& centroid = m_InstanceCentroids[m_TLASInstanceRefs[node.LeftFirst + i]];
                centroidMin = glm::min(centroidMin, centroid);
                centroidMax = glm::max(centroidMax, centroid);
            }

            const glm::vec3 extent = centroidMax - centroidMin;
            glm::length_t axis = 0;
            if (extent.y > extent[axis])
                axis = 1;
            if (extent.z > extent[axis])
                axis = 2;

            const auto first = m_TLASInstanceRefs.begin() + node.LeftFirst;
            const auto last = first + node.Count;

            const f32 splitPos = centroidMin[axis] + extent[axis] * 0.5f;
            auto middle = std::partition(first, last, [this, axis, splitPos](u32 instanceIndex)
                                         { return m_InstanceCentroids[instanceIndex][axis] < splitPos; });

            auto leftCount = static_cast<u32>(std::distance(first, middle));
            if (leftCount == 0 || leftCount == node.Count)
            {
                middle = first + node.Count / 2;
                std::nth_element(first, middle, last, [this, axis](u32 a, u32 b)
                                 { return m_InstanceCentroids[a][axis] < m_InstanceCentroids[b][axis]; });
                leftCount = node.Count / 2;
                if (leftCount == 0)
                    continue;
            }

            const u32 leftFirst = node.LeftFirst;
            const u32 totalCount = node.Count;
            const auto leftChild = static_cast<u32>(m_TLASNodes.size());

            m_TLASNodes.emplace_back();
            m_TLASNodes.emplace_back();

            // `node` may dangle after the emplaces reallocated the vector.
            m_TLASNodes[work.NodeIndex].LeftFirst = leftChild;
            m_TLASNodes[work.NodeIndex].Count = 0;

            m_TLASNodes[leftChild].LeftFirst = leftFirst;
            m_TLASNodes[leftChild].Count = leftCount;
            m_TLASNodes[leftChild + 1].LeftFirst = leftFirst + leftCount;
            m_TLASNodes[leftChild + 1].Count = totalCount - leftCount;

            UpdateTLASNodeBounds(leftChild);
            UpdateTLASNodeBounds(leftChild + 1);

            stack.push_back({ leftChild, work.Depth + 1 });
            stack.push_back({ leftChild + 1, work.Depth + 1 });
        }
    }

    void ReferenceScene::BuildEmissiveList()
    {
        m_EmissiveTriangles.clear();
        m_EmissiveAreaCdf.clear();
        m_TotalEmissiveArea = 0.0f;

        for (u32 instanceIndex = 0; instanceIndex < static_cast<u32>(m_Instances.size()); ++instanceIndex)
        {
            const ReferenceInstance& instance = m_Instances[instanceIndex];
            const ReferenceMaterial& material = GetMaterial(instance.MaterialIndex);
            if (!(std::max({ material.Emissive.x, material.Emissive.y, material.Emissive.z }) > 0.0f))
                continue;

            const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];
            const std::vector<Vertex>& vertices = geometry.GetVertices();
            const std::vector<u32>& indices = geometry.GetIndices();

            for (sizet triangle = 0; triangle + 2 < indices.size(); triangle += 3)
            {
                const u32 i0 = indices[triangle + 0];
                const u32 i1 = indices[triangle + 1];
                const u32 i2 = indices[triangle + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                    continue;

                EmissiveTriangle emitter;
                emitter.V0 = glm::vec3(instance.Transform * glm::vec4(vertices[i0].Position, 1.0f));
                emitter.V1 = glm::vec3(instance.Transform * glm::vec4(vertices[i1].Position, 1.0f));
                emitter.V2 = glm::vec3(instance.Transform * glm::vec4(vertices[i2].Position, 1.0f));

                const glm::vec3 cross = glm::cross(emitter.V1 - emitter.V0, emitter.V2 - emitter.V0);
                const f32 crossLength = glm::length(cross);
                if (!(crossLength > 1e-12f))
                    continue; // degenerate triangle carries no light

                emitter.Area = 0.5f * crossLength;
                emitter.Normal = cross / crossLength;
                emitter.InstanceIndex = instanceIndex;
                emitter.MaterialIndex = instance.MaterialIndex;
                emitter.TriangleIndex = static_cast<u32>(triangle / 3);

                m_TotalEmissiveArea += emitter.Area;
                m_EmissiveTriangles.push_back(emitter);
                m_EmissiveAreaCdf.push_back(m_TotalEmissiveArea);
            }
        }

        // Normalize the running sums into a CDF ending exactly at 1.
        if (m_TotalEmissiveArea > 0.0f)
        {
            for (f32& value : m_EmissiveAreaCdf)
                value /= m_TotalEmissiveArea;
            m_EmissiveAreaCdf.back() = 1.0f;
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    bool ReferenceScene::IntersectInstance(u32 instanceIndex, const Ray& ray, SurfaceInteraction& outHit) const
    {
        const ReferenceInstance& instance = m_Instances[instanceIndex];
        const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];

        const glm::vec3 localOrigin = glm::vec3(instance.InverseTransform * glm::vec4(ray.Origin, 1.0f));
        const glm::vec3 localDirRaw = glm::vec3(instance.InverseTransform * glm::vec4(ray.Direction, 0.0f));
        const f32 localDirLength = glm::length(localDirRaw);
        if (!(localDirLength > 0.0f))
            return false;
        const glm::vec3 localDir = localDirRaw / localDirLength;

        // A world distance t maps to a local distance t * localDirLength
        // (== t / UniformScale). Both interval ends convert the same way.
        Ray localRay;
        localRay.Origin = localOrigin;
        localRay.Direction = localDir;
        localRay.TMin = ray.TMin * localDirLength;
        localRay.TMax = (ray.TMax >= std::numeric_limits<f32>::max() * 0.5f)
                            ? ray.TMax
                            : ray.TMax * localDirLength;

        RayHit localHit;
        if (!geometry.GetBVH().CastRay(localRay, localHit))
            return false;

        const f32 worldDistance = localHit.Distance / localDirLength;

        outHit.Hit = true;
        outHit.Distance = worldDistance;
        outHit.Position = ray.Origin + ray.Direction * worldDistance;
        outHit.InstanceIndex = instanceIndex;
        outHit.MaterialIndex = instance.MaterialIndex;
        outHit.TriangleIndex = localHit.TriangleIndex;
        outHit.FrontFace = localHit.FrontFace;

        // RayHit::Normal is flipped to oppose the ray. Undo that so the caller
        // sees the true winding orientation (an emitter's front/back test needs
        // it, and the integrator flips it itself where it wants a shading
        // frame).
        const glm::vec3 localGeometric = localHit.FrontFace ? localHit.Normal : -localHit.Normal;
        const glm::vec3 localShading = geometry.InterpolateNormal(localHit.TriangleIndex, localHit.U, localHit.V);

        outHit.GeometricNormal = glm::normalize(instance.NormalMatrix * localGeometric);
        glm::vec3 shadingNormal = instance.NormalMatrix * localShading;
        const f32 shadingLengthSq = glm::dot(shadingNormal, shadingNormal);
        outHit.ShadingNormal = (shadingLengthSq > 1e-20f) ? shadingNormal * glm::inversesqrt(shadingLengthSq)
                                                          : outHit.GeometricNormal;

        // A shading normal that disagrees with the geometric one about which
        // side we are on produces black terminator artefacts and, worse, lets
        // NEE and BSDF sampling disagree about the hemisphere. Snap it.
        if (glm::dot(outHit.ShadingNormal, outHit.GeometricNormal) < 0.0f)
            outHit.ShadingNormal = outHit.GeometricNormal;

        return true;
    }

    bool ReferenceScene::OccludedInstance(u32 instanceIndex, const Ray& ray) const
    {
        const ReferenceInstance& instance = m_Instances[instanceIndex];
        const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];

        const glm::vec3 localOrigin = glm::vec3(instance.InverseTransform * glm::vec4(ray.Origin, 1.0f));
        const glm::vec3 localDirRaw = glm::vec3(instance.InverseTransform * glm::vec4(ray.Direction, 0.0f));
        const f32 localDirLength = glm::length(localDirRaw);
        if (!(localDirLength > 0.0f))
            return false;

        Ray localRay;
        localRay.Origin = localOrigin;
        localRay.Direction = localDirRaw / localDirLength;
        localRay.TMin = ray.TMin * localDirLength;
        localRay.TMax = ray.TMax * localDirLength;

        return geometry.GetBVH().CastRayAny(localRay);
    }

    bool ReferenceScene::Intersect(const Ray& ray, SurfaceInteraction& outHit) const
    {
        // m_Built goes false on any Add*, so this catches BOTH "never built"
        // and "mutated after Build()" — the latter would otherwise traverse a
        // stale TLAS that simply does not contain the new geometry, which is
        // invisible in the output.
        OLO_CORE_ASSERT(m_Built, "ReferenceScene::Intersect on an unbuilt or stale scene — call Build() after the last Add*()");
        outHit = SurfaceInteraction{};
        if (m_TLASNodes.empty())
            return false;

        const glm::vec3 invDir(1.0f / ray.Direction.x, 1.0f / ray.Direction.y, 1.0f / ray.Direction.z);

        Ray working = ray;
        bool anyHit = false;

        u32 stack[s_TraversalStackSize];
        u32 stackSize = 0;
        stack[stackSize++] = 0;

        while (stackSize > 0)
        {
            const u32 nodeIndex = stack[--stackSize];
            const TLASNode& node = m_TLASNodes[nodeIndex];

            f32 tNear = 0.0f;
            if (!RayIntersect::RayAABB(working.Origin, invDir, node.Bounds.Min, node.Bounds.Max,
                                       working.TMin, working.TMax, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.Count; ++i)
                {
                    const u32 instanceIndex = m_TLASInstanceRefs[node.LeftFirst + i];
                    SurfaceInteraction candidate;
                    if (IntersectInstance(instanceIndex, working, candidate))
                    {
                        outHit = candidate;
                        anyHit = true;
                        // Tighten the search interval so farther instances and
                        // whole subtrees prune out.
                        working.TMax = candidate.Distance;
                    }
                }
                continue;
            }

            // Push both children; the ordering does not affect correctness
            // because the interval is tightened on every accepted hit. The push
            // is UNCONDITIONAL: a capacity test that silently skipped the push
            // would drop a whole subtree and produce a plausible, wrong image.
            // The static_assert on s_TraversalStackSize (ReferenceScene.h) is
            // what makes overflow impossible; this asserts it at runtime too.
            OLO_CORE_ASSERT(stackSize + 2 <= s_TraversalStackSize, "TLAS traversal stack overflow");
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }

        return anyHit;
    }

    bool ReferenceScene::IsOccluded(const glm::vec3& from, const glm::vec3& to, f32 epsilon) const
    {
        OLO_CORE_ASSERT(m_Built, "ReferenceScene::IsOccluded on an unbuilt or stale scene — call Build() after the last Add*()");
        if (m_TLASNodes.empty())
            return false;

        const glm::vec3 delta = to - from;
        const f32 distance = glm::length(delta);
        if (!(distance > 2.0f * epsilon))
            return false;

        Ray ray;
        ray.Origin = from;
        ray.Direction = delta / distance;
        ray.TMin = epsilon;
        // Inset the far end so the light sample's own surface is not counted
        // as its own occluder.
        ray.TMax = distance - epsilon;

        const glm::vec3 invDir(1.0f / ray.Direction.x, 1.0f / ray.Direction.y, 1.0f / ray.Direction.z);

        u32 stack[s_TraversalStackSize];
        u32 stackSize = 0;
        stack[stackSize++] = 0;

        while (stackSize > 0)
        {
            const u32 nodeIndex = stack[--stackSize];
            const TLASNode& node = m_TLASNodes[nodeIndex];

            f32 tNear = 0.0f;
            if (!RayIntersect::RayAABB(ray.Origin, invDir, node.Bounds.Min, node.Bounds.Max,
                                       ray.TMin, ray.TMax, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.Count; ++i)
                {
                    if (OccludedInstance(m_TLASInstanceRefs[node.LeftFirst + i], ray))
                        return true;
                }
                continue;
            }

            OLO_CORE_ASSERT(stackSize + 2 <= s_TraversalStackSize, "TLAS traversal stack overflow");
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }

        return false;
    }

    // =========================================================================
    // Emissive sampling
    // =========================================================================

    f32 ReferenceScene::EmissivePdfArea() const
    {
        if (!(m_TotalEmissiveArea > 0.0f))
            return 0.0f;
        return 1.0f / m_TotalEmissiveArea;
    }

    bool ReferenceScene::SampleEmissive(f32 xiSelect, const glm::vec2& xiPoint, EmissiveSample& outSample) const
    {
        if (m_EmissiveTriangles.empty() || !(m_TotalEmissiveArea > 0.0f))
            return false;

        // Area-proportional triangle selection. Uniform-over-area (rather than
        // uniform-over-triangles) is what makes the density constant, which in
        // turn is what lets EmissivePdfArea() be a single number the MIS side
        // can reuse without re-deriving it.
        const auto it = std::lower_bound(m_EmissiveAreaCdf.begin(), m_EmissiveAreaCdf.end(), xiSelect);
        sizet triangleIndex = static_cast<sizet>(std::distance(m_EmissiveAreaCdf.begin(), it));
        if (triangleIndex >= m_EmissiveTriangles.size())
            triangleIndex = m_EmissiveTriangles.size() - 1;

        const EmissiveTriangle& emitter = m_EmissiveTriangles[triangleIndex];

        // Uniform barycentric point (Turk's square-root warp).
        const f32 sqrtU = std::sqrt(std::clamp(xiPoint.x, 0.0f, 1.0f));
        const f32 b0 = 1.0f - sqrtU;
        const f32 b1 = std::clamp(xiPoint.y, 0.0f, 1.0f) * sqrtU;
        const f32 b2 = 1.0f - b0 - b1;

        const ReferenceMaterial& material = GetMaterial(emitter.MaterialIndex);
        outSample.Position = emitter.V0 * b0 + emitter.V1 * b1 + emitter.V2 * b2;
        outSample.Normal = emitter.Normal;
        outSample.Radiance = material.Emissive;
        outSample.PdfArea = EmissivePdfArea();
        outSample.TwoSided = material.TwoSidedEmission;
        return true;
    }
} // namespace OloEngine::PathTracing
