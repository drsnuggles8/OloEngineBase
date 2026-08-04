// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ShaderDebugDrawExpansionTest.cpp — issue #725
//
// `DebugDrawPrimitives.glsl` turns one appended entry into N line segments. That
// math is mirrored in C++ (`Renderer/Debug/ShaderDebugDrawExpansion.cpp`) for the
// same reason `GPUFrustumCullParityTest` mirrors the cull compute: the shader
// cannot be executed headlessly, so the alternative to a mirrored implementation
// is no coverage at all.
//
// A mirror only pays for itself if it is checked against something other than
// itself, so the assertions here are ANALYTIC — every point of a circle is at
// `radius` from the centre and in the plane, an AABB's 12 edges are exactly the
// axis-aligned edges of its corner set, a cone's spokes end on its own base ring
// — rather than "the output equals this recorded blob". A recorded blob would
// pass for a wrong-but-stable implementation; these do not.
//
// The GLSL/C++ agreement itself is pinned separately, by
// ShaderDebugDrawContractTest reading the shader's segment-count literals.
//
// Classification: L1 / shaderpipe (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Debug/ShaderDebugDrawExpansion.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <vector>

namespace OloEngine::Tests
{
    using ShaderDebugDrawExpansion::Segment;

    namespace
    {
        constexpr f32 kEps = 1e-4f;

        [[nodiscard]] bool NearlyEqual(const glm::vec3& a, const glm::vec3& b, f32 eps = kEps)
        {
            return glm::length(a - b) <= eps;
        }

        // Does `point` coincide with any of `candidates`?
        [[nodiscard]] bool IsOneOf(const glm::vec3& point, const std::vector<glm::vec3>& candidates, f32 eps = kEps)
        {
            return std::ranges::any_of(candidates, [&](const glm::vec3& c)
                                       { return NearlyEqual(point, c, eps); });
        }

        // Collect every distinct endpoint across a segment list.
        [[nodiscard]] std::vector<glm::vec3> DistinctEndpoints(const std::vector<Segment>& segments, f32 eps = kEps)
        {
            std::vector<glm::vec3> unique;
            for (const auto& segment : segments)
            {
                for (const glm::vec3& point : { segment.A, segment.B })
                {
                    if (!IsOneOf(point, unique, eps))
                        unique.push_back(point);
                }
            }
            return unique;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Line
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawExpansion, LineIsOneSegmentEndToEnd)
    {
        ShaderDebugDrawLine entry;
        entry.Start = { 1.0f, 2.0f, 3.0f };
        entry.End = { -4.0f, 5.0f, 6.0f };

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        ASSERT_EQ(segments.size(), 1u);
        EXPECT_TRUE(NearlyEqual(segments[0].A, entry.Start));
        EXPECT_TRUE(NearlyEqual(segments[0].B, entry.End));
    }

    // -------------------------------------------------------------------------
    // AABB / Box — the primitive the issue's first acceptance criterion names.
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawExpansion, AABBDrawsExactlyTheTwelveAxisAlignedEdgesOfItsCornerSet)
    {
        ShaderDebugDrawAABB entry;
        entry.Min = { -1.0f, -2.0f, -3.0f };
        entry.Max = { 4.0f, 5.0f, 6.0f };

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        ASSERT_EQ(segments.size(), 12u);

        const auto corners = ShaderDebugDrawExpansion::AABBCorners(entry.Min, entry.Max);
        const std::vector<glm::vec3> cornerList(corners.begin(), corners.end());

        // All eight corners appear, and nothing else does.
        const auto endpoints = DistinctEndpoints(segments);
        EXPECT_EQ(endpoints.size(), 8u);
        for (const auto& endpoint : endpoints)
            EXPECT_TRUE(IsOneOf(endpoint, cornerList)) << "an endpoint is not a corner of the box";

        // Every edge runs along exactly one axis, with a length equal to that
        // axis's full extent. This is what rules out a corner-indexing bug that
        // connects diagonals — which still draws a closed-looking wireframe.
        const glm::vec3 extent = entry.Max - entry.Min;
        std::array<u32, 3> perAxis{ 0, 0, 0 };
        for (const auto& segment : segments)
        {
            const glm::vec3 delta = glm::abs(segment.B - segment.A);
            u32 varyingAxes = 0;
            u32 axis = 0;
            for (u32 i = 0; i < 3; ++i)
            {
                if (delta[static_cast<i32>(i)] > kEps)
                {
                    ++varyingAxes;
                    axis = i;
                }
            }
            ASSERT_EQ(varyingAxes, 1u) << "a box edge is not axis-aligned — the corner indexing is wrong";
            EXPECT_NEAR(delta[static_cast<i32>(axis)], extent[static_cast<i32>(axis)], kEps);
            ++perAxis[axis];
        }
        // A cube has four edges along each axis; anything else means the edge
        // table double-counts one direction.
        EXPECT_EQ(perAxis[0], 4u);
        EXPECT_EQ(perAxis[1], 4u);
        EXPECT_EQ(perAxis[2], 4u);
    }

    TEST(ShaderDebugDrawExpansion, AABBEdgesAreAllDistinct)
    {
        ShaderDebugDrawAABB entry;
        entry.Min = { 0.0f, 0.0f, 0.0f };
        entry.Max = { 1.0f, 1.0f, 1.0f };

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        std::set<std::pair<i64, i64>> seen;
        for (const auto& segment : segments)
        {
            // Quantise so the pair key is stable, and order it so A-B and B-A
            // collide (a duplicated edge drawn backwards is still a duplicate).
            const auto key = [](const glm::vec3& v)
            {
                return static_cast<i64>(std::llround(v.x * 1000.0f)) * 1000000 +
                       static_cast<i64>(std::llround(v.y * 1000.0f)) * 1000 +
                       static_cast<i64>(std::llround(v.z * 1000.0f));
            };
            auto a = key(segment.A);
            auto b = key(segment.B);
            if (a > b)
                std::swap(a, b);
            EXPECT_TRUE(seen.insert({ a, b }).second) << "duplicate edge — an edge of the box is never drawn";
        }
        EXPECT_EQ(seen.size(), 12u);
    }

    TEST(ShaderDebugDrawExpansion, BoxWithAABBCornersProducesTheSameEdgesAsTheAABBPrimitive)
    {
        // The two primitives exist for different reasons (Box takes arbitrary
        // corners), but they MUST agree where their inputs agree — otherwise a
        // CPU bound drawn as an AABB and its GPU counterpart drawn as a Box
        // would not overlay, which is the exact comparison the feature is for.
        const glm::vec3 mn{ -2.0f, 1.0f, 0.5f };
        const glm::vec3 mx{ 3.0f, 4.0f, 7.5f };

        ShaderDebugDrawAABB aabbEntry;
        aabbEntry.Min = mn;
        aabbEntry.Max = mx;

        ShaderDebugDrawBox boxEntry;
        const auto corners = ShaderDebugDrawExpansion::AABBCorners(mn, mx);
        for (u32 i = 0; i < 8; ++i)
            boxEntry.Corners[i] = glm::vec4(corners[i], 1.0f);

        const auto aabbSegments = ShaderDebugDrawExpansion::ExpandToVector(aabbEntry);
        const auto boxSegments = ShaderDebugDrawExpansion::ExpandToVector(boxEntry);
        ASSERT_EQ(aabbSegments.size(), boxSegments.size());
        for (sizet i = 0; i < aabbSegments.size(); ++i)
        {
            EXPECT_TRUE(NearlyEqual(aabbSegments[i].A, boxSegments[i].A)) << "segment " << i;
            EXPECT_TRUE(NearlyEqual(aabbSegments[i].B, boxSegments[i].B)) << "segment " << i;
        }
    }

    TEST(ShaderDebugDrawExpansion, InvertedAABBIsDrawnAsGivenNotSilentlyReordered)
    {
        // A swapped Min/Max is usually a bug in the PUSHING shader, and seeing
        // the inside-out box is how you find it. Silently sorting would hide it.
        ShaderDebugDrawAABB entry;
        entry.Min = { 1.0f, 1.0f, 1.0f };
        entry.Max = { -1.0f, -1.0f, -1.0f };

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        ASSERT_EQ(segments.size(), 12u);
        const auto endpoints = DistinctEndpoints(segments);
        EXPECT_EQ(endpoints.size(), 8u);
        EXPECT_TRUE(IsOneOf(glm::vec3(1.0f), endpoints));
        EXPECT_TRUE(IsOneOf(glm::vec3(-1.0f), endpoints));
    }

    // -------------------------------------------------------------------------
    // Circle / Sphere
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawExpansion, CircleIsAClosedRingInItsOwnPlane)
    {
        ShaderDebugDrawCircle entry;
        entry.Center = { 3.0f, -1.0f, 2.0f };
        entry.Normal = { 0.3f, 0.9f, -0.2f }; // deliberately not unit length
        entry.Radius = 2.5f;

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        ASSERT_EQ(segments.size(), ShaderDebugDrawContract::kCircleSegments);

        const glm::vec3 unitNormal = glm::normalize(entry.Normal);
        for (const auto& segment : segments)
        {
            for (const glm::vec3& point : { segment.A, segment.B })
            {
                const glm::vec3 offset = point - entry.Center;
                EXPECT_NEAR(glm::length(offset), entry.Radius, 1e-3f);
                EXPECT_NEAR(glm::dot(offset, unitNormal), 0.0f, 1e-3f)
                    << "a ring point left the circle's plane — the basis is not orthogonal to the normal";
            }
        }

        // Consecutive segments must share an endpoint, and the last must close
        // back onto the first: a ring that does not close reads as a dotted
        // circle, which looks like a rendering artefact rather than a maths one.
        for (sizet i = 0; i + 1 < segments.size(); ++i)
            EXPECT_TRUE(NearlyEqual(segments[i].B, segments[i + 1].A, 1e-3f)) << "gap after segment " << i;
        EXPECT_TRUE(NearlyEqual(segments.back().B, segments.front().A, 1e-3f)) << "the ring does not close";
    }

    TEST(ShaderDebugDrawExpansion, SphereIsThreeAxisAlignedGreatCircles)
    {
        ShaderDebugDrawSphere entry;
        entry.Center = { -5.0f, 2.0f, 1.0f };
        entry.Radius = 3.0f;

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        constexpr u32 ringSegments = ShaderDebugDrawContract::kSphereRingSegments;
        ASSERT_EQ(segments.size(), 3u * ringSegments);

        // Every point is on the sphere.
        for (const auto& segment : segments)
        {
            EXPECT_NEAR(glm::length(segment.A - entry.Center), entry.Radius, 1e-3f);
            EXPECT_NEAR(glm::length(segment.B - entry.Center), entry.Radius, 1e-3f);
        }

        // Ring `r` lies in the plane normal to world axis `r` — this is the
        // property that makes the sphere read correctly from any angle, and it
        // is what a ring-index/axis mix-up would break.
        const std::array<glm::vec3, 3> normals{ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                                                glm::vec3(0.0f, 0.0f, 1.0f) };
        for (u32 ring = 0; ring < 3; ++ring)
        {
            for (u32 i = 0; i < ringSegments; ++i)
            {
                const auto& segment = segments[(ring * ringSegments) + i];
                EXPECT_NEAR(glm::dot(segment.A - entry.Center, normals[ring]), 0.0f, 1e-3f)
                    << "ring " << ring << " segment " << i;
            }
            // Each ring closes on itself independently.
            EXPECT_TRUE(NearlyEqual(segments[(ring * ringSegments) + ringSegments - 1].B,
                                    segments[ring * ringSegments].A, 1e-3f))
                << "ring " << ring << " does not close";
        }
    }

    // -------------------------------------------------------------------------
    // Rectangle / Cone
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawExpansion, RectangleIsAClosedLoopThroughItsFourCorners)
    {
        ShaderDebugDrawRectangle entry;
        entry.Center = { 1.0f, 0.0f, -2.0f };
        entry.AxisU = { 2.0f, 0.0f, 0.0f };
        entry.AxisV = { 0.0f, 0.0f, 3.0f };

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        ASSERT_EQ(segments.size(), 4u);

        const std::vector<glm::vec3> expectedCorners{
            entry.Center - entry.AxisU - entry.AxisV, entry.Center + entry.AxisU - entry.AxisV,
            entry.Center + entry.AxisU + entry.AxisV, entry.Center - entry.AxisU + entry.AxisV
        };
        const auto endpoints = DistinctEndpoints(segments);
        EXPECT_EQ(endpoints.size(), 4u);
        for (const auto& corner : expectedCorners)
            EXPECT_TRUE(IsOneOf(corner, endpoints));

        for (sizet i = 0; i < segments.size(); ++i)
        {
            EXPECT_TRUE(NearlyEqual(segments[i].B, segments[(i + 1) % segments.size()].A))
                << "the rectangle's outline is not a closed loop at segment " << i;
        }
    }

    TEST(ShaderDebugDrawExpansion, ConeIsABaseRingPlusSpokesThatLandOnIt)
    {
        ShaderDebugDrawCone entry;
        entry.Apex = { 0.0f, 5.0f, 0.0f };
        entry.Axis = { 0.0f, -4.0f, 0.0f }; // apex -> base, height 4
        entry.Radius = 1.5f;

        const auto segments = ShaderDebugDrawExpansion::ExpandToVector(entry);
        constexpr u32 ringSegments = ShaderDebugDrawContract::kConeRingSegments;
        constexpr u32 sideLines = ShaderDebugDrawContract::kConeSideLines;
        ASSERT_EQ(segments.size(), ringSegments + sideLines);

        const glm::vec3 baseCenter = entry.Apex + entry.Axis;
        const glm::vec3 unitAxis = glm::normalize(entry.Axis);

        std::vector<glm::vec3> ringPoints;
        for (u32 i = 0; i < ringSegments; ++i)
        {
            for (const glm::vec3& point : { segments[i].A, segments[i].B })
            {
                EXPECT_NEAR(glm::length(point - baseCenter), entry.Radius, 1e-3f);
                EXPECT_NEAR(glm::dot(point - baseCenter, unitAxis), 0.0f, 1e-3f)
                    << "a base-ring point left the base plane";
                if (!IsOneOf(point, ringPoints, 1e-3f))
                    ringPoints.push_back(point);
            }
        }
        EXPECT_TRUE(NearlyEqual(segments[ringSegments - 1].B, segments[0].A, 1e-3f))
            << "the base ring does not close";

        // Each spoke starts at the apex and ENDS ON A RING VERTEX, not at a
        // chord midpoint — the property that makes the silhouette look like a
        // cone rather than a slightly-collapsed one.
        for (u32 i = 0; i < sideLines; ++i)
        {
            const auto& spoke = segments[ringSegments + i];
            EXPECT_TRUE(NearlyEqual(spoke.A, entry.Apex));
            EXPECT_TRUE(IsOneOf(spoke.B, ringPoints, 1e-3f))
                << "cone spoke " << i << " does not end on the base ring";
        }
    }

    TEST(ShaderDebugDrawExpansion, EveryPrimitiveWritesExactlyItsContractedSegmentCount)
    {
        // The count the CPU writes into DrawArraysIndirectCommand.count is
        // derived from SegmentCount(); if an expansion wrote fewer, the tail of
        // every instance would draw uninitialised segments.
        // EXPECT_EQ, not EXPECT_TRUE(a == b): a drifted count is exactly the
        // case where the expected-vs-actual numbers are the whole diagnosis, and
        // EXPECT_TRUE would print only "false".
        const auto expanded = [](auto entry)
        { return ShaderDebugDrawExpansion::ExpandToVector(entry).size(); };

        EXPECT_EQ(expanded(ShaderDebugDrawLine{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Line));
        EXPECT_EQ(expanded(ShaderDebugDrawCircle{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Circle));
        EXPECT_EQ(expanded(ShaderDebugDrawRectangle{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Rectangle));
        EXPECT_EQ(expanded(ShaderDebugDrawAABB{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::AABB));
        EXPECT_EQ(expanded(ShaderDebugDrawBox{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Box));
        EXPECT_EQ(expanded(ShaderDebugDrawCone{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Cone));
        EXPECT_EQ(expanded(ShaderDebugDrawSphere{}),
                  ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Sphere));
    }
} // namespace OloEngine::Tests
