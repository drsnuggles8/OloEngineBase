// OLO_TEST_LAYER: L1
//
// CPU contract tests for camera-relative terrain LOD (issue #429, terrain
// slice). The terrain quadtree keeps its node bounds in *terrain-local*
// coordinates (0..worldSize) and the entity transform places them in the world,
// but LOD selection was fed the *world* camera / view-projection / frustum. Far
// from origin that is wrong twice over:
//   1. it ignores the terrain's world transform, so the camera<->node distance
//      and frustum test are evaluated against the wrong location, and
//   2. the world camera<->node subtraction cancels catastrophically in f32
//      (both operands ~45 km, true difference small) — the same jitter the whole
//      camera-relative feature exists to kill.
// The fix transforms the world camera / view-projection into the terrain's LOCAL
// space via MakeObjectLocalCameraPos / MakeObjectLocalViewProjection, evaluated
// through the grid-snapped render origin so the large-coordinate subtraction
// lands on small operands. These tests pin, without a GL context, the exact math
// the LOD path now performs, and reproduce the bug the old world-space path had:
//   * No-op near origin / identity transform (the safety gate).
//   * The local camera position + local view-projection reproduce the near-
//     origin reference at 45 km; the naive world path does not.
//   * The payoff: the quadtree's screen-space-error decision (distance-driven)
//     is reproduced far from origin, so the SAME LOD level is chosen — while the
//     naive world-camera evaluation collapses to the coarsest LOD (patches
//     "degenerate" far out, exactly the reported symptom).
// A GPU-gated end-to-end test drives the real TerrainQuadtree::SelectLOD and
// asserts identical node selection near vs far; it SKIPs cleanly without a GPU.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"

#include "PropertyTests/RenderPropertyTest.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // A camera orientation independent of position — the engine's camera
    // transform is translate(pos) * R, so its view = inverse(translate(pos)*R)
    // carries the large translation *unmixed* with the rotation. Building the
    // far view this way (rather than lookAt(farPos, farTarget), which would cancel
    // two 45 km operands while *constructing* the reference) keeps the test
    // measuring the LOD math, not glm::lookAt's own precision.
    glm::mat4 CameraRotationLookingDown()
    {
        // Tilt ~35 deg down around X so the ground plane is well in view.
        return glm::rotate(glm::mat4(1.0f), glm::radians(-35.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    glm::mat4 PerspectiveProj()
    {
        return glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100000.0f);
    }

    // World view-projection for a camera at `camWorldPos` with the fixed
    // orientation, exactly as the engine forms it: proj * inverse(translate*R).
    glm::mat4 WorldViewProjection(const glm::vec3& camWorldPos)
    {
        const glm::mat4 camTransform = glm::translate(glm::mat4(1.0f), camWorldPos) * CameraRotationLookingDown();
        return PerspectiveProj() * glm::inverse(camTransform);
    }

    // The quadtree's screen-space-error metric, replicated verbatim from
    // TerrainQuadtree::CalculateScreenSpaceError so the test pins the exact LOD
    // *decision*, not a proxy. Node bounds are terrain-LOCAL; a larger returned
    // value means "recurse for more detail", a smaller value "this LOD is fine".
    f32 ScreenSpaceError(f32 nodeMinX, f32 nodeMinZ, f32 nodeMaxX, f32 nodeMaxZ,
                         f32 worldSizeX, f32 worldSizeZ,
                         const glm::vec3& localBoundsMin, const glm::vec3& localBoundsMax,
                         const glm::vec3& cameraPos, const glm::mat4& viewProjection,
                         f32 viewportHeight)
    {
        const f32 nodeWorldSizeX = (nodeMaxX - nodeMinX) * worldSizeX;
        const f32 nodeWorldSizeZ = (nodeMaxZ - nodeMinZ) * worldSizeZ;
        const f32 geometricError = std::max(nodeWorldSizeX, nodeWorldSizeZ);

        const glm::vec3 nodeCenter = (localBoundsMin + localBoundsMax) * 0.5f;
        f32 distance = glm::length(cameraPos - nodeCenter);
        distance = std::max(distance, 0.001f);

        const f32 projScale = viewProjection[1][1] * viewportHeight * 0.5f;
        return (geometricError * projScale) / distance;
    }

    // A terrain-local node in the middle of a 1024-unit terrain.
    constexpr f32 kWorldSize = 1024.0f;
    constexpr f32 kNodeMinX = 0.40f, kNodeMinZ = 0.40f, kNodeMaxX = 0.50f, kNodeMaxZ = 0.50f;
    const glm::vec3 kNodeBoundsMin{ kNodeMinX * kWorldSize, -2.0f, kNodeMinZ* kWorldSize };
    const glm::vec3 kNodeBoundsMax{ kNodeMaxX * kWorldSize, 2.0f, kNodeMaxZ* kWorldSize };
    constexpr f32 kViewportHeight = 1080.0f;

    // A camera hovering over the terrain, expressed in terrain-LOCAL space (the
    // space the near-origin reference lives in). ~200 units up, offset back in Z.
    const glm::vec3 kLocalCameraPos{ 512.0f, 200.0f, 900.0f };
} // namespace

// -----------------------------------------------------------------------------
// Safety gate: near origin with an identity terrain transform, the local-space
// transform is a byte-identical pass-through — every existing terrain scene
// (which sits at or near the origin) selects LOD exactly as before.
// -----------------------------------------------------------------------------

TEST(TerrainCameraRelativeLOD, IdentityTransformNearOriginIsNoOp)
{
    // A camera within the first grid cell (|x|,|z| < gridSize/2 = 512) so the
    // render origin is exactly (0,0,0) and the whole transform is a pure
    // pass-through — the safety gate every existing (near-origin) terrain scene
    // relies on. (kLocalCameraPos is used by the far-origin tests below; there
    // its 512/900 components legitimately snap to a non-zero origin.)
    const glm::vec3 camWorldPos{ 100.0f, 50.0f, 200.0f }; // terrain at origin => local == world
    ASSERT_EQ(ComputeRenderOrigin(camWorldPos), glm::vec3(0.0f)) << "camera is within the first grid cell";

    const glm::mat4 worldTransform(1.0f); // identity: terrain at world origin
    const glm::mat4 vpWorld = WorldViewProjection(camWorldPos);
    const glm::vec3 origin = ComputeRenderOrigin(camWorldPos);

    const glm::vec3 localCam = MakeObjectLocalCameraPos(camWorldPos, worldTransform, origin);
    const glm::mat4 localVP = MakeObjectLocalViewProjection(vpWorld, worldTransform, origin);

    for (int i = 0; i < 3; ++i)
        EXPECT_FLOAT_EQ(localCam[i], camWorldPos[i]) << "component " << i;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            EXPECT_FLOAT_EQ(localVP[c][r], vpWorld[c][r]) << "col " << c << " row " << r;
}

// -----------------------------------------------------------------------------
// The local camera position reproduces the near-origin reference far from origin:
// terrain and camera both shifted by +45 km, the camera in terrain-local space is
// the same small coordinate as near origin. The naive path (world camera against
// local bounds, ignoring the transform) is off by the whole 45 km.
// -----------------------------------------------------------------------------

TEST(TerrainCameraRelativeLOD, LocalCameraReproducesNearReferenceFarFromOrigin)
{
    const glm::vec3 shift{ 45000.0f, 0.0f, 45000.0f };
    const glm::mat4 worldTransform = glm::translate(glm::mat4(1.0f), shift);
    const glm::vec3 camWorldPos = kLocalCameraPos + shift;
    const glm::vec3 origin = ComputeRenderOrigin(camWorldPos);
    ASSERT_NE(origin, glm::vec3(0.0f)) << "well outside the first grid cell";

    const glm::vec3 localCam = MakeObjectLocalCameraPos(camWorldPos, worldTransform, origin);

    // Reproduces the near-origin local camera to a tiny fraction of a unit
    // (the residual is the ULP of the stored 45 km coordinate, ~0.004).
    EXPECT_NEAR(localCam.x, kLocalCameraPos.x, 0.05f);
    EXPECT_NEAR(localCam.y, kLocalCameraPos.y, 0.05f);
    EXPECT_NEAR(localCam.z, kLocalCameraPos.z, 0.05f);

    // The naive path never subtracts the terrain origin, so it thinks the camera
    // is ~45 km from every (local-coordinate) node — off by the full shift.
    const glm::vec3 naiveCam = camWorldPos; // fed straight to a local-bounds test
    EXPECT_GT(glm::length(naiveCam - kLocalCameraPos), 40000.0f);
}

// -----------------------------------------------------------------------------
// The local view-projection reprojects terrain-local points to the same clip
// coordinates as the near-origin reference VP; the naive world VP (applied to the
// local point, i.e. ignoring the transform) sends them wildly off screen.
// -----------------------------------------------------------------------------

TEST(TerrainCameraRelativeLOD, LocalViewProjectionReprojectsLocalPointsIdenticallyFarFromOrigin)
{
    const glm::vec3 shift{ 45000.0f, 0.0f, 45000.0f };
    const glm::mat4 worldTransform = glm::translate(glm::mat4(1.0f), shift);
    const glm::vec3 camWorldPos = kLocalCameraPos + shift;
    const glm::vec3 origin = ComputeRenderOrigin(camWorldPos);

    const glm::mat4 vpNearRef = WorldViewProjection(kLocalCameraPos); // terrain-at-origin reference
    const glm::mat4 vpFarWorld = WorldViewProjection(camWorldPos);
    const glm::mat4 localVP = MakeObjectLocalViewProjection(vpFarWorld, worldTransform, origin);

    const glm::vec3 localPoints[] = {
        { kNodeBoundsMin.x, 0.0f, kNodeBoundsMin.z },
        { kNodeBoundsMax.x, 1.0f, kNodeBoundsMax.z },
        { 512.0f, 0.0f, 512.0f },
        { 300.0f, -1.0f, 700.0f }
    };

    for (const glm::vec3& p : localPoints)
    {
        const glm::vec4 clipRef = vpNearRef * glm::vec4(p, 1.0f);
        const glm::vec4 clipLocal = localVP * glm::vec4(p, 1.0f);
        // NDC after perspective divide is what actually lands on screen.
        const glm::vec3 ndcRef = glm::vec3(clipRef) / clipRef.w;
        const glm::vec3 ndcLocal = glm::vec3(clipLocal) / clipLocal.w;
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(ndcLocal[i], ndcRef[i], 1e-3f) << "ndc component " << i;

        // The naive world VP applied to the local point (the transform ignored)
        // projects it as if the terrain were at the origin but the camera 45 km
        // away — nowhere near the reference.
        const glm::vec4 clipNaive = vpFarWorld * glm::vec4(p, 1.0f);
        const glm::vec3 ndcNaive = glm::vec3(clipNaive) / clipNaive.w;
        EXPECT_GT(glm::length(ndcNaive - ndcRef), 0.5f) << "naive path should mis-project the local point";
    }
}

// -----------------------------------------------------------------------------
// The payoff: the quadtree's screen-space-error LOD *decision* is reproduced far
// from origin (same value => same split/keep choice => same LOD level), while the
// naive world-camera evaluation collapses the error toward zero — the node is
// always "good enough", so the tree never subdivides and terrain degenerates to
// the coarsest patch far out. This is the exact reported symptom.
// -----------------------------------------------------------------------------

TEST(TerrainCameraRelativeLOD, ScreenSpaceErrorDecisionReproducedFarFromOrigin)
{
    // Near-origin reference: terrain at origin, camera in local space.
    const f32 errNear = ScreenSpaceError(
        kNodeMinX, kNodeMinZ, kNodeMaxX, kNodeMaxZ, kWorldSize, kWorldSize,
        kNodeBoundsMin, kNodeBoundsMax, kLocalCameraPos, WorldViewProjection(kLocalCameraPos), kViewportHeight);
    ASSERT_GT(errNear, 0.0f);

    const glm::vec3 shift{ 45000.0f, 0.0f, 45000.0f };
    const glm::mat4 worldTransform = glm::translate(glm::mat4(1.0f), shift);
    const glm::vec3 camWorldPos = kLocalCameraPos + shift;
    const glm::vec3 origin = ComputeRenderOrigin(camWorldPos);

    // Fixed path (what the terrain LOD now does): local camera + local VP.
    const glm::vec3 localCam = MakeObjectLocalCameraPos(camWorldPos, worldTransform, origin);
    const glm::mat4 localVP = MakeObjectLocalViewProjection(WorldViewProjection(camWorldPos), worldTransform, origin);
    const f32 errFixed = ScreenSpaceError(
        kNodeMinX, kNodeMinZ, kNodeMaxX, kNodeMaxZ, kWorldSize, kWorldSize,
        kNodeBoundsMin, kNodeBoundsMax, localCam, localVP, kViewportHeight);

    // The fixed decision matches the near-origin reference to well within any LOD
    // threshold gap (thresholds are octave-spaced: 2,4,8,16,32 px).
    EXPECT_NEAR(errFixed, errNear, errNear * 1e-3f)
        << "fixed=" << errFixed << " near=" << errNear;

    // Naive path (the old bug): world camera against terrain-LOCAL node bounds.
    const f32 errNaive = ScreenSpaceError(
        kNodeMinX, kNodeMinZ, kNodeMaxX, kNodeMaxZ, kWorldSize, kWorldSize,
        kNodeBoundsMin, kNodeBoundsMax, camWorldPos, WorldViewProjection(camWorldPos), kViewportHeight);

    // The camera looks ~45 km away from the (local) node, so the projected error
    // collapses by roughly the distance ratio (~200 units vs ~64 km => ~300x).
    EXPECT_LT(errNaive, errNear * 0.02f)
        << "naive world-camera error should collapse far from origin; naive=" << errNaive << " near=" << errNear;
}

// -----------------------------------------------------------------------------
// End-to-end: the real TerrainQuadtree selects the identical set of leaf nodes at
// the identical LOD levels near origin and 45 km out (via the local transform),
// but a DIFFERENT set when fed the naive world camera. Needs a GL context to
// build the terrain heightmap; SKIPs cleanly without one.
// -----------------------------------------------------------------------------

namespace
{
    // Key a selected node by its stable pointer into the tree's node pool (the
    // pool is not rebuilt between SelectLOD calls) plus the chosen LOD level.
    // Pointer identity sidesteps any float comparison on the node bounds.
    struct SelectionKey
    {
        const TerrainQuadNode* Node;
        u32 LODLevel;
        bool operator==(const SelectionKey&) const = default;
    };

    std::vector<SelectionKey> CollectSelection(const TerrainQuadtree& tree)
    {
        std::vector<SelectionKey> keys;
        for (const auto* node : tree.GetSelectedNodes())
            keys.push_back({ node, node->LODLevel });
        return keys;
    }
} // namespace

TEST(TerrainCameraRelativeLOD, QuadtreeSelectsIdenticalNodesFarFromOrigin)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // Flat terrain: LOD selection here is purely distance-driven (heights don't
    // affect the screen-space-error metric), which is exactly what we're pinning.
    TerrainData terrainData;
    terrainData.CreateFlat(129, 0.0f);

    TerrainQuadtree tree;
    tree.Build(terrainData, kWorldSize, kWorldSize, 64.0f, TerrainLODConfig::MAX_LOD_LEVELS);
    ASSERT_GT(tree.GetNodeCount(), 1u);

    // Near-origin reference. (Avoid the identifiers `near`/`far` — they are
    // reserved macros from the Windows SDK.)
    const glm::mat4 vpNear = WorldViewProjection(kLocalCameraPos);
    tree.SelectLOD(Frustum(vpNear), kLocalCameraPos, vpNear, kViewportHeight);
    const std::vector<SelectionKey> nearSel = CollectSelection(tree);
    ASSERT_FALSE(nearSel.empty());

    // Far, transformed into terrain-local space (the fix).
    const glm::vec3 shift{ 45000.0f, 0.0f, 45000.0f };
    const glm::mat4 worldTransform = glm::translate(glm::mat4(1.0f), shift);
    const glm::vec3 camWorldPos = kLocalCameraPos + shift;
    const glm::vec3 origin = ComputeRenderOrigin(camWorldPos);
    const glm::vec3 localCam = MakeObjectLocalCameraPos(camWorldPos, worldTransform, origin);
    const glm::mat4 localVP = MakeObjectLocalViewProjection(WorldViewProjection(camWorldPos), worldTransform, origin);
    tree.SelectLOD(Frustum(localVP), localCam, localVP, kViewportHeight);
    const std::vector<SelectionKey> farSel = CollectSelection(tree);

    EXPECT_EQ(farSel, nearSel) << "far-with-shift selection must match the near-origin reference";

    // Far, naive world camera against local bounds (the old bug): a different
    // (coarser / mostly frustum-culled) selection.
    const glm::mat4 vpFarWorld = WorldViewProjection(camWorldPos);
    tree.SelectLOD(Frustum(vpFarWorld), camWorldPos, vpFarWorld, kViewportHeight);
    const std::vector<SelectionKey> naiveSel = CollectSelection(tree);

    EXPECT_NE(naiveSel, nearSel) << "naive world-camera selection should diverge far from origin (the bug being fixed)";
}
