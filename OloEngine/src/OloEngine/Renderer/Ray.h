#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

namespace OloEngine
{
    // @brief A pure-CPU ray for geometric queries (ray-vs-mesh, ray-vs-AABB, picking).
    //
    // Mirrors the conventions of Physics3D's RayCastInfo (origin + direction +
    // distance limit) so a future unified raycast API reads naturally, but is a
    // standalone glm structure with no Jolt dependency.
    //
    // Direction is expected to be **normalized**: the parametric `t` returned by
    // intersection tests is then a world-space distance. The [TMin, TMax] interval
    // bounds the valid hit range — TMin > 0 lets callers skip self-intersections at
    // the origin, TMax bounds the search distance (and is tightened during a
    // closest-hit traversal as nearer hits are found).
    struct Ray
    {
        glm::vec3 Origin{ 0.0f };
        glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
        f32 TMin = 0.0f;
        f32 TMax = std::numeric_limits<f32>::max();

        Ray() = default;
        Ray(const glm::vec3& origin, const glm::vec3& direction,
            f32 tMin = 0.0f, f32 tMax = std::numeric_limits<f32>::max())
            : Origin(origin), Direction(direction), TMin(tMin), TMax(tMax)
        {
        }

        // Build a ray spanning two world points. Direction is normalized and TMax is
        // set to the distance between them, so a hit only counts if it lies on the
        // segment [from, to]. For coincident points the segment is a single point:
        // a stable +Z unit direction is chosen but TMax is 0, so the result is a
        // zero-length segment (only a hit exactly at `from` could qualify) rather
        // than an unbounded ray.
        [[nodiscard]] static Ray FromPoints(const glm::vec3& from, const glm::vec3& to)
        {
            glm::vec3 delta = to - from;
            f32 distance = glm::length(delta);
            constexpr f32 epsilon = 1e-8f;
            glm::vec3 dir = (distance > epsilon) ? (delta / distance) : glm::vec3(0.0f, 0.0f, 1.0f);
            return Ray(from, dir, 0.0f, (distance > epsilon) ? distance : 0.0f);
        }

        // Point at parametric distance t along the ray.
        [[nodiscard]] glm::vec3 At(f32 t) const
        {
            return Origin + Direction * t;
        }
    };

    // @brief Result of a closest-hit ray-vs-mesh query.
    //
    // Normal is the geometric (per-face) normal derived from the triangle winding,
    // re-oriented to oppose the incoming ray direction so it always points back
    // toward the ray origin (the convention picking / surface-placement tools want).
    // FrontFace records whether the un-flipped winding faced the ray. U/V are the
    // barycentric coordinates of the hit, so a caller can interpolate vertex
    // attributes (smooth normals, UVs) from the mesh using TriangleIndex.
    struct RayHit
    {
        bool Hit = false;
        f32 Distance = 0.0f;      // parametric t == world distance (for a unit Direction)
        glm::vec3 Point{ 0.0f };  // Origin + Direction * Distance
        glm::vec3 Normal{ 0.0f }; // geometric face normal, oriented against the ray
        u32 TriangleIndex = 0;    // ordinal of the hit triangle (index-buffer offset / 3)
        f32 U = 0.0f;             // barycentric weight of vertex 1
        f32 V = 0.0f;             // barycentric weight of vertex 2 (weight of vertex 0 == 1 - U - V)
        bool FrontFace = false;   // true if the triangle's winding faced the ray
    };

    namespace RayIntersect
    {
        // @brief Möller–Trumbore ray-vs-triangle intersection (two-sided).
        //
        // Reference: Möller & Trumbore, "Fast, Minimum Storage Ray/Triangle
        // Intersection", Journal of Graphics Tools 2(1), 1997.
        // https://en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm
        //
        // On a hit, writes the parametric distance to `outT`, the barycentric
        // coordinates to `outU`/`outV`, and whether the winding faced the ray to
        // `outFrontFace`, then returns true. A hit only counts when `outT` lies in
        // [ray.TMin, ray.TMax]. Rays parallel to the triangle plane (|det| below
        // epsilon) miss. No floating-point `==`; an epsilon guards the determinant
        // (cpp-coding-quality §2).
        [[nodiscard]] inline bool RayTriangle(const Ray& ray,
                                              const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                              f32& outT, f32& outU, f32& outV, bool& outFrontFace)
        {
            constexpr f32 epsilon = 1e-8f;

            const glm::vec3 edge1 = v1 - v0;
            const glm::vec3 edge2 = v2 - v0;
            const glm::vec3 pvec = glm::cross(ray.Direction, edge2);
            const f32 det = glm::dot(edge1, pvec);

            // Ray parallel to the triangle plane (or a degenerate, zero-area triangle).
            if (std::abs(det) < epsilon)
                return false;

            const f32 invDet = 1.0f / det;

            const glm::vec3 tvec = ray.Origin - v0;
            const f32 u = glm::dot(tvec, pvec) * invDet;
            if (u < 0.0f || u > 1.0f)
                return false;

            const glm::vec3 qvec = glm::cross(tvec, edge1);
            const f32 v = glm::dot(ray.Direction, qvec) * invDet;
            if (v < 0.0f || (u + v) > 1.0f)
                return false;

            const f32 t = glm::dot(edge2, qvec) * invDet;
            if (t < ray.TMin || t > ray.TMax)
                return false;

            outT = t;
            outU = u;
            outV = v;
            // det = -dot(Direction, cross(edge1, edge2)); det > 0 means the geometric
            // normal opposes the ray, i.e. the ray strikes the front (CCW) face.
            outFrontFace = det > 0.0f;
            return true;
        }

        // @brief Ray-vs-AABB slab test used for BVH node pruning.
        //
        // Reference: Kay & Kajiya slab method; the `invDir` form is the one
        // popularised by Williams et al. ("An Efficient and Robust Ray-Box
        // Intersection Algorithm", 2005) and Jacco Bikker's BVH series.
        // `invDir` is the component-wise reciprocal of the (normalized) ray
        // direction — +/-inf for axis-parallel components (a zero direction
        // component) is expected and handled explicitly: see below. Returns true
        // when the ray overlaps the box within [tMin, tMax], writing the entry
        // distance (clamped to tMin) to `outTNear`. A ray originating inside the
        // box reports a hit with outTNear == tMin. Box bounds are inclusive, so a
        // ray grazing a face counts as a hit.
        [[nodiscard]] inline bool RayAABB(const glm::vec3& origin, const glm::vec3& invDir,
                                          const glm::vec3& boxMin, const glm::vec3& boxMax,
                                          f32 tMin, f32 tMax, f32& outTNear)
        {
            f32 tEnter = tMin;
            f32 tExit = tMax;
            for (glm::length_t axis = 0; axis < 3; ++axis)
            {
                f32 tNearAxis;
                f32 tFarAxis;
                if (std::isinf(invDir[axis]))
                {
                    // Ray parallel to this axis (direction component is zero, so
                    // invDir is +/-inf). The naive (box - origin) * invDir would
                    // evaluate 0 * inf == NaN whenever the origin lies exactly on a
                    // slab plane, and glm::min/max then poison tFar/tEnter and report
                    // a spurious miss. Handle it directly: the ray can only intersect
                    // if its origin is within the slab; if it is, the axis places no
                    // bound on t (treat as the full -inf..+inf range).
                    if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis])
                        return false;
                    tNearAxis = -std::numeric_limits<f32>::infinity();
                    tFarAxis = std::numeric_limits<f32>::infinity();
                }
                else
                {
                    const f32 t1 = (boxMin[axis] - origin[axis]) * invDir[axis];
                    const f32 t2 = (boxMax[axis] - origin[axis]) * invDir[axis];
                    tNearAxis = glm::min(t1, t2);
                    tFarAxis = glm::max(t1, t2);
                }
                tEnter = glm::max(tEnter, tNearAxis);
                tExit = glm::min(tExit, tFarAxis);
            }

            outTNear = tEnter;
            return tExit >= tEnter;
        }
    } // namespace RayIntersect
} // namespace OloEngine
