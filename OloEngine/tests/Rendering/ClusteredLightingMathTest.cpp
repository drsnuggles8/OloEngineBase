// OLO_TEST_LAYER: shaderpipe
//
// CPU mirrors of the clustered (froxel) light-grid math introduced by issue
// #435. These pin, without a GL context:
//   - the exponential depth-slice mapping shared by ClusteredLighting.h,
//     LightCulling.comp and ForwardPlusCommon.glsl,
//   - the flat cluster index ordering shared by the cull compute (writer)
//     and the fragment lookup (reader),
//   - near/far clip-plane extraction from projection matrices,
//   - the cluster frustum-plane construction + sphere test replicated from
//     LightCulling.comp (buildTileFrustumPlanes / sphereInClusterFrustum).
//
// The GPU-side struct sizes and the packed light-index encoding are pinned by
// the sibling LightCullingTest.cpp; the frame itself is pinned by
// ClusteredLightingVisualEvidenceTest.cpp (L8).

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred
namespace CL = OloEngine::ClusteredLighting;

// =============================================================================
// Depth-slice mapping
// =============================================================================

TEST(ClusteredLightingMath, SliceZeroStartsAtNearPlane)
{
    const f32 nearP = 0.1f;
    const f32 farP = 1000.0f;
    const auto params = CL::ComputeDepthSliceParams(CL::kClusterCountZ, nearP, farP);

    // A fragment exactly on the near plane maps to slice 0; slightly past the
    // first slice boundary maps to slice 1.
    EXPECT_EQ(CL::SliceForViewDepth(nearP, params, CL::kClusterCountZ), 0u);
    const f32 firstBoundary = CL::SliceNearDepth(1, CL::kClusterCountZ, nearP, farP);
    EXPECT_EQ(CL::SliceForViewDepth(firstBoundary * 1.001f, params, CL::kClusterCountZ), 1u);
}

TEST(ClusteredLightingMath, FarPlaneMapsToLastSlice)
{
    const f32 nearP = 0.1f;
    const f32 farP = 1000.0f;
    const auto params = CL::ComputeDepthSliceParams(CL::kClusterCountZ, nearP, farP);

    // Just inside the far plane lands in the last slice; beyond it clamps.
    EXPECT_EQ(CL::SliceForViewDepth(farP * 0.999f, params, CL::kClusterCountZ), CL::kClusterCountZ - 1);
    EXPECT_EQ(CL::SliceForViewDepth(farP * 10.0f, params, CL::kClusterCountZ), CL::kClusterCountZ - 1);
}

TEST(ClusteredLightingMath, DepthCloserThanNearClampsToSliceZero)
{
    const auto params = CL::ComputeDepthSliceParams(CL::kClusterCountZ, 0.1f, 1000.0f);
    EXPECT_EQ(CL::SliceForViewDepth(0.01f, params, CL::kClusterCountZ), 0u);
    EXPECT_EQ(CL::SliceForViewDepth(0.0f, params, CL::kClusterCountZ), 0u);
    EXPECT_EQ(CL::SliceForViewDepth(-5.0f, params, CL::kClusterCountZ), 0u);
}

TEST(ClusteredLightingMath, SliceIsMonotonicInDepth)
{
    const f32 nearP = 0.05f;
    const f32 farP = 2500.0f; // SponzaCSM's camera far
    const auto params = CL::ComputeDepthSliceParams(CL::kClusterCountZ, nearP, farP);

    u32 prevSlice = 0;
    for (f32 depth = nearP; depth <= farP; depth *= 1.15f)
    {
        const u32 slice = CL::SliceForViewDepth(depth, params, CL::kClusterCountZ);
        EXPECT_GE(slice, prevSlice) << "slice regressed at depth " << depth;
        prevSlice = slice;
    }
    EXPECT_EQ(prevSlice, CL::kClusterCountZ - 1);
}

TEST(ClusteredLightingMath, SliceBoundsRoundTrip)
{
    // The midpoint of every slice's [near, far) span must map back to that
    // slice — the writer (compute, slice bounds from k) and the reader
    // (fragment, slice from log2(viewZ)) must agree for all k.
    const f32 nearP = 0.1f;
    const f32 farP = 1000.0f;
    const auto params = CL::ComputeDepthSliceParams(CL::kClusterCountZ, nearP, farP);

    for (u32 k = 0; k < CL::kClusterCountZ; ++k)
    {
        const f32 sliceNear = CL::SliceNearDepth(k, CL::kClusterCountZ, nearP, farP);
        const f32 sliceFar = CL::SliceNearDepth(k + 1, CL::kClusterCountZ, nearP, farP);
        const f32 mid = std::sqrt(sliceNear * sliceFar); // geometric mid of an exponential span
        EXPECT_EQ(CL::SliceForViewDepth(mid, params, CL::kClusterCountZ), k)
            << "slice " << k << " midpoint " << mid << " mapped elsewhere";
    }
}

TEST(ClusteredLightingMath, SliceSpansGrowWithDepth)
{
    // Exponential distribution: each slice must be geometrically wider than
    // the previous — that is the property that stops thin depth ranges from
    // wasting clusters far away.
    const f32 nearP = 0.1f;
    const f32 farP = 1000.0f;
    f32 prevSpan = 0.0f;
    for (u32 k = 0; k < CL::kClusterCountZ; ++k)
    {
        const f32 span = CL::SliceNearDepth(k + 1, CL::kClusterCountZ, nearP, farP) -
                         CL::SliceNearDepth(k, CL::kClusterCountZ, nearP, farP);
        EXPECT_GT(span, prevSpan);
        prevSpan = span;
    }
}

// =============================================================================
// Cluster index ordering
// =============================================================================

TEST(ClusteredLightingMath, ClusterIndexRoundTrip)
{
    const u32 cx = CL::kClusterCountX;
    const u32 cy = CL::kClusterCountY;
    const u32 cz = CL::kClusterCountZ;

    // Exhaustive: every (x, y, z) maps to a unique index in [0, total) and
    // decodes back to the same coordinates.
    std::vector<bool> seen(CL::kTotalClusters, false);
    for (u32 z = 0; z < cz; ++z)
    {
        for (u32 y = 0; y < cy; ++y)
        {
            for (u32 x = 0; x < cx; ++x)
            {
                const u32 idx = CL::ClusterIndex(x, y, z, cx, cy);
                ASSERT_LT(idx, CL::kTotalClusters);
                ASSERT_FALSE(seen[idx]) << "duplicate cluster index " << idx;
                seen[idx] = true;

                // Decode (mirrors nothing in the shader — pure inverse check)
                const u32 dz = idx / (cx * cy);
                const u32 dy = (idx / cx) % cy;
                const u32 dx = idx % cx;
                EXPECT_EQ(dx, x);
                EXPECT_EQ(dy, y);
                EXPECT_EQ(dz, z);
            }
        }
    }
}

// =============================================================================
// Clip-plane extraction
// =============================================================================

TEST(ClusteredLightingMath, ExtractClipPlanesPerspective)
{
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    f32 nearP = 0.0f;
    f32 farP = 0.0f;
    CL::ExtractClipPlanes(proj, nearP, farP);
    EXPECT_NEAR(nearP, 0.1f, 1e-4f);
    // The far plane recovers through a catastrophic-cancellation-prone
    // b/(a+1) with a ≈ -1, so only ~0.1% relative accuracy is guaranteed —
    // far more than the log2 slice mapping needs.
    EXPECT_NEAR(farP, 1000.0f, 1000.0f * 2e-3f);
}

TEST(ClusteredLightingMath, ExtractClipPlanesPerspectiveSponzaRange)
{
    // SponzaCSM camera: near 0.05 (editor default) far 2500.
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.777f, 0.05f, 2500.0f);
    f32 nearP = 0.0f;
    f32 farP = 0.0f;
    CL::ExtractClipPlanes(proj, nearP, farP);
    EXPECT_NEAR(nearP, 0.05f, 1e-4f);
    EXPECT_NEAR(farP, 2500.0f, 2500.0f * 2e-3f);
}

TEST(ClusteredLightingMath, ExtractClipPlanesOrthographic)
{
    const glm::mat4 proj = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.5f, 200.0f);
    f32 nearP = 0.0f;
    f32 farP = 0.0f;
    CL::ExtractClipPlanes(proj, nearP, farP);
    EXPECT_NEAR(nearP, 0.5f, 1e-4f);
    EXPECT_NEAR(farP, 200.0f, 1e-2f);
}

TEST(ClusteredLightingMath, ExtractClipPlanesSanitisesDegenerateInputs)
{
    // Ortho with a negative near (legal GL, illegal for log slicing) must be
    // clamped so slicing never sees near <= 0; identity/zero matrices must
    // not produce NaN or inverted planes.
    const glm::mat4 orthoNegNear = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    f32 nearP = 0.0f;
    f32 farP = 0.0f;
    CL::ExtractClipPlanes(orthoNegNear, nearP, farP);
    EXPECT_GE(nearP, CL::kMinNearPlane);
    EXPECT_GT(farP, nearP);

    CL::ExtractClipPlanes(glm::mat4(0.0f), nearP, farP);
    EXPECT_TRUE(std::isfinite(nearP));
    EXPECT_TRUE(std::isfinite(farP));
    EXPECT_GT(farP, nearP);
    EXPECT_GE(nearP, CL::kMinNearPlane);
}

// =============================================================================
// Cluster frustum culling — CPU mirror of LightCulling.comp
// =============================================================================

namespace
{
    // Mirrors LightCulling.comp::ndcNearToView
    glm::vec3 NdcNearToView(const glm::mat4& invProj, glm::vec2 ndc)
    {
        const glm::vec4 view = invProj * glm::vec4(ndc, -1.0f, 1.0f);
        return glm::vec3(view) / view.w;
    }

    // Mirrors LightCulling.comp::buildTileFrustumPlanes
    std::array<glm::vec4, 4> BuildTileFrustumPlanes(const glm::mat4& invProj,
                                                    glm::vec2 ndcMin, glm::vec2 ndcMax)
    {
        const glm::vec3 topLeft = NdcNearToView(invProj, { ndcMin.x, ndcMax.y });
        const glm::vec3 topRight = NdcNearToView(invProj, { ndcMax.x, ndcMax.y });
        const glm::vec3 bottomLeft = NdcNearToView(invProj, { ndcMin.x, ndcMin.y });
        const glm::vec3 bottomRight = NdcNearToView(invProj, { ndcMax.x, ndcMin.y });

        std::array<glm::vec4, 4> planes{};
        planes[0] = glm::vec4(glm::normalize(glm::cross(bottomLeft, topLeft)), 0.0f);     // left
        planes[1] = glm::vec4(glm::normalize(glm::cross(topRight, bottomRight)), 0.0f);   // right
        planes[2] = glm::vec4(glm::normalize(glm::cross(bottomRight, bottomLeft)), 0.0f); // bottom
        planes[3] = glm::vec4(glm::normalize(glm::cross(topLeft, topRight)), 0.0f);       // top
        return planes;
    }

    // Mirrors LightCulling.comp::sphereInClusterFrustum
    bool SphereInClusterFrustum(glm::vec3 center, f32 radius,
                                const std::array<glm::vec4, 4>& planes, f32 nearZ, f32 farZ)
    {
        if (center.z + radius < farZ || center.z - radius > nearZ)
            return false;
        for (const auto& plane : planes)
        {
            const f32 d = glm::dot(glm::vec3(plane), center) + plane.w;
            if (d < -radius)
                return false;
        }
        return true;
    }

    struct ClusterTestContext
    {
        glm::mat4 Projection;
        glm::mat4 InvProjection;
        f32 NearPlane;
        f32 FarPlane;

        ClusterTestContext()
            : Projection(glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f)),
              InvProjection(glm::inverse(Projection)),
              NearPlane(0.1f),
              FarPlane(100.0f)
        {
        }

        // Cluster (x, y, z) -> planes + slice z bounds, exactly as the
        // compute shader derives them from gl_WorkGroupID.
        void ClusterBounds(u32 x, u32 y, u32 z,
                           std::array<glm::vec4, 4>& outPlanes, f32& outNearZ, f32& outFarZ) const
        {
            const glm::vec2 ndcMin = glm::vec2(static_cast<f32>(x) / CL::kClusterCountX,
                                               static_cast<f32>(y) / CL::kClusterCountY) *
                                         2.0f -
                                     1.0f;
            const glm::vec2 ndcMax = glm::vec2(static_cast<f32>(x + 1) / CL::kClusterCountX,
                                               static_cast<f32>(y + 1) / CL::kClusterCountY) *
                                         2.0f -
                                     1.0f;
            outPlanes = BuildTileFrustumPlanes(InvProjection, ndcMin, ndcMax);
            outNearZ = -CL::SliceNearDepth(z, CL::kClusterCountZ, NearPlane, FarPlane);
            outFarZ = -CL::SliceNearDepth(z + 1, CL::kClusterCountZ, NearPlane, FarPlane);
        }
    };
} // namespace

TEST(ClusteredLightingMath, LightAtClusterCenterIsFoundInThatCluster)
{
    const ClusterTestContext ctx;

    // Pick a mid-grid cluster and place a small light at its view-space
    // centroid: the containing cluster must pass the sphere test.
    const u32 cx = CL::kClusterCountX / 2;
    const u32 cy = CL::kClusterCountY / 2;
    const u32 cz = CL::kClusterCountZ / 2;

    std::array<glm::vec4, 4> planes{};
    f32 nearZ = 0.0f;
    f32 farZ = 0.0f;
    ctx.ClusterBounds(cx, cy, cz, planes, nearZ, farZ);

    // Centroid depth = geometric mid of the slice span; centre tile centroid
    // sits on the view axis.
    const f32 depth = std::sqrt(nearZ * farZ); // both negative -> product positive
    const glm::vec3 lightCenter(0.0f, 0.0f, -depth);

    EXPECT_TRUE(SphereInClusterFrustum(lightCenter, 0.05f, planes, nearZ, farZ));
}

TEST(ClusteredLightingMath, SmallLightIsRejectedByOtherSlices)
{
    const ClusterTestContext ctx;

    const u32 cx = CL::kClusterCountX / 2;
    const u32 cy = CL::kClusterCountY / 2;
    const u32 cz = CL::kClusterCountZ / 2;

    std::array<glm::vec4, 4> planes{};
    f32 nearZ = 0.0f;
    f32 farZ = 0.0f;
    ctx.ClusterBounds(cx, cy, cz, planes, nearZ, farZ);
    const f32 depth = std::sqrt(nearZ * farZ);
    const glm::vec3 lightCenter(0.0f, 0.0f, -depth);
    const f32 radius = 0.01f;

    // The same light tested against a slice two steps nearer and two steps
    // farther must be rejected purely by the depth-slab test — this is the
    // property 2D tiles lacked (a light in a thin depth slice used to land in
    // every tile it overlapped on screen regardless of depth).
    std::array<glm::vec4, 4> otherPlanes{};
    f32 otherNearZ = 0.0f;
    f32 otherFarZ = 0.0f;

    ctx.ClusterBounds(cx, cy, cz - 2, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_FALSE(SphereInClusterFrustum(lightCenter, radius, otherPlanes, otherNearZ, otherFarZ));

    ctx.ClusterBounds(cx, cy, cz + 2, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_FALSE(SphereInClusterFrustum(lightCenter, radius, otherPlanes, otherNearZ, otherFarZ));
}

TEST(ClusteredLightingMath, SmallLightIsRejectedByDistantScreenTiles)
{
    const ClusterTestContext ctx;

    const u32 cx = CL::kClusterCountX / 2;
    const u32 cy = CL::kClusterCountY / 2;
    const u32 cz = CL::kClusterCountZ / 2;

    std::array<glm::vec4, 4> planes{};
    f32 nearZ = 0.0f;
    f32 farZ = 0.0f;
    ctx.ClusterBounds(cx, cy, cz, planes, nearZ, farZ);
    const f32 depth = std::sqrt(nearZ * farZ);
    const glm::vec3 lightCenter(0.0f, 0.0f, -depth);
    const f32 radius = 0.01f;

    // A tile 4 columns to the right / 4 rows up must reject the axis light.
    std::array<glm::vec4, 4> otherPlanes{};
    f32 otherNearZ = 0.0f;
    f32 otherFarZ = 0.0f;

    ctx.ClusterBounds(cx + 4, cy, cz, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_FALSE(SphereInClusterFrustum(lightCenter, radius, otherPlanes, otherNearZ, otherFarZ));

    ctx.ClusterBounds(cx, cy + 4, cz, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_FALSE(SphereInClusterFrustum(lightCenter, radius, otherPlanes, otherNearZ, otherFarZ));
}

TEST(ClusteredLightingMath, LargeLightSpansMultipleClusters)
{
    const ClusterTestContext ctx;

    const u32 cx = CL::kClusterCountX / 2;
    const u32 cy = CL::kClusterCountY / 2;
    const u32 cz = CL::kClusterCountZ / 2;

    std::array<glm::vec4, 4> planes{};
    f32 nearZ = 0.0f;
    f32 farZ = 0.0f;
    ctx.ClusterBounds(cx, cy, cz, planes, nearZ, farZ);
    const f32 depth = std::sqrt(nearZ * farZ);
    const glm::vec3 lightCenter(0.0f, 0.0f, -depth);

    // A radius covering the whole slice span + generous screen extent must be
    // accepted by the neighbours the small light was rejected from.
    const f32 bigRadius = depth; // sphere reaches from origin to 2x depth

    std::array<glm::vec4, 4> otherPlanes{};
    f32 otherNearZ = 0.0f;
    f32 otherFarZ = 0.0f;

    ctx.ClusterBounds(cx + 4, cy, cz, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_TRUE(SphereInClusterFrustum(lightCenter, bigRadius, otherPlanes, otherNearZ, otherFarZ));

    ctx.ClusterBounds(cx, cy, cz - 2, otherPlanes, otherNearZ, otherFarZ);
    EXPECT_TRUE(SphereInClusterFrustum(lightCenter, bigRadius, otherPlanes, otherNearZ, otherFarZ));
}

// =============================================================================
// Fragment-side tile-scale mapping (UBO TileScale contract)
// =============================================================================

TEST(ClusteredLightingMath, TileScaleMapsFragCoordToClusterCoord)
{
    // Mirrors ForwardPlusCommon.glsl::fplusClusterIndex's tile computation:
    // tileCoord = uvec2(gl_FragCoord.xy * TileScale.xy), clamped to counts-1.
    const f32 screenW = 1920.0f;
    const f32 screenH = 1080.0f;
    const glm::vec2 tileScale(static_cast<f32>(CL::kClusterCountX) / screenW,
                              static_cast<f32>(CL::kClusterCountY) / screenH);

    // Pixel (0.5, 0.5) -> tile (0, 0)
    glm::uvec2 tile(glm::vec2(0.5f, 0.5f) * tileScale);
    EXPECT_EQ(tile, glm::uvec2(0, 0));

    // Last pixel -> last tile (after the shader's min() clamp)
    tile = glm::uvec2(glm::vec2(1919.5f, 1079.5f) * tileScale);
    tile = glm::min(tile, glm::uvec2(CL::kClusterCountX - 1, CL::kClusterCountY - 1));
    EXPECT_EQ(tile, glm::uvec2(CL::kClusterCountX - 1, CL::kClusterCountY - 1));

    // Interior consistency: the pixel at the exact centre of tile (i, j)
    // maps back to (i, j) for a sweep of tiles.
    for (u32 j = 0; j < CL::kClusterCountY; j += 3)
    {
        for (u32 i = 0; i < CL::kClusterCountX; i += 5)
        {
            const glm::vec2 pixelCenter((static_cast<f32>(i) + 0.5f) * screenW / CL::kClusterCountX,
                                        (static_cast<f32>(j) + 0.5f) * screenH / CL::kClusterCountY);
            const glm::uvec2 mapped(pixelCenter * tileScale);
            EXPECT_EQ(mapped, glm::uvec2(i, j));
        }
    }
}
