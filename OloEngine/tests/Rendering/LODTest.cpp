#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/LOD.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// LODLevel Tests
// =============================================================================

// LODLevel::DefaultConstruction / ParameterizedConstruction /
// ParameterizedConstructionDefaultTriangles retired -- pure POD
// field-init smoke (assign value, read it back). Same for
// LODGroup::DefaultConstruction. See docs/testing.md
// section 4.1. The SelectLOD-related tests below pin the real contract.

// =============================================================================
// LODGroup Tests — Empty / Single Level
// =============================================================================

TEST(LODGroup, EmptyGroupReturnsInvalid)
{
    LODGroup group;
    EXPECT_EQ(group.SelectLOD(0.0f), -1);
    EXPECT_EQ(group.SelectLOD(100.0f), -1);
    EXPECT_EQ(group.SelectLOD(-5.0f), -1);
}

TEST(LODGroup, SingleLevelAlwaysSelected)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 100.0f, 1000);

    EXPECT_EQ(group.SelectLOD(0.0f), 0);
    EXPECT_EQ(group.SelectLOD(50.0f), 0);
    EXPECT_EQ(group.SelectLOD(100.0f), 0);
    // Beyond threshold — still returns last (lowest detail)
    EXPECT_EQ(group.SelectLOD(200.0f), 0);
}

// =============================================================================
// LODGroup Tests — Multiple Levels
// =============================================================================

TEST(LODGroup, MultipleLevelsSelectCorrectly)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 50.0f, 10000); // LOD 0: high detail
    group.Levels.emplace_back(AssetHandle(2), 150.0f, 2500); // LOD 1: medium
    group.Levels.emplace_back(AssetHandle(3), 500.0f, 500);  // LOD 2: low

    // Within LOD 0 range
    EXPECT_EQ(group.SelectLOD(0.0f), 0);
    EXPECT_EQ(group.SelectLOD(25.0f), 0);
    EXPECT_EQ(group.SelectLOD(50.0f), 0);

    // Within LOD 1 range
    EXPECT_EQ(group.SelectLOD(51.0f), 1);
    EXPECT_EQ(group.SelectLOD(100.0f), 1);
    EXPECT_EQ(group.SelectLOD(150.0f), 1);

    // Within LOD 2 range
    EXPECT_EQ(group.SelectLOD(151.0f), 2);
    EXPECT_EQ(group.SelectLOD(300.0f), 2);
    EXPECT_EQ(group.SelectLOD(500.0f), 2);

    // Beyond all thresholds — returns lowest detail (last)
    EXPECT_EQ(group.SelectLOD(1000.0f), 2);
}

TEST(LODGroup, BoundaryDistancesSelectCorrectLevel)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 200.0f);

    // Exactly at boundary selects current level (distance <= maxDistance)
    EXPECT_EQ(group.SelectLOD(100.0f), 0);
    EXPECT_EQ(group.SelectLOD(200.0f), 1);

    // Just past boundary selects next level
    EXPECT_EQ(group.SelectLOD(100.001f), 1);
}

// =============================================================================
// LODGroup Tests — Bias Factor
// =============================================================================

TEST(LODGroup, BiasOneHasNoEffect)
{
    LODGroup group;
    group.Bias = 1.0f;
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 200.0f);

    EXPECT_EQ(group.SelectLOD(50.0f), 0);
    EXPECT_EQ(group.SelectLOD(150.0f), 1);
}

TEST(LODGroup, BiasGreaterThanOneKeepsHighDetailLonger)
{
    // Bias < 1 means effectiveDistance = distance / bias is LARGER,
    // so objects switch to lower detail sooner.
    // Wait — bias < 1: effectiveDistance = distance / 0.5 = distance * 2
    // That means a distance of 75 becomes 150, pushing to LOD 1.
    // Actually for "keeps high detail longer" we want bias > 1.
    // Let's test the actual math:

    LODGroup group;
    group.Bias = 2.0f; // effectiveDistance = distance / 2.0
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 200.0f);

    // At distance 150, effective = 75, which is <= 100 → LOD 0
    EXPECT_EQ(group.SelectLOD(150.0f), 0);

    // Without bias (bias=1), distance 150 would select LOD 1
    LODGroup noBias;
    noBias.Levels = group.Levels;
    noBias.Bias = 1.0f;
    EXPECT_EQ(noBias.SelectLOD(150.0f), 1);
}

TEST(LODGroup, BiasLessThanOneFavorsLowerDetail)
{
    LODGroup group;
    group.Bias = 0.5f; // effectiveDistance = distance / 0.5 = distance * 2
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 200.0f);

    // At distance 75, effective = 150, which is > 100 → LOD 1
    EXPECT_EQ(group.SelectLOD(75.0f), 1);

    // Without bias, distance 75 would select LOD 0
    LODGroup noBias;
    noBias.Levels = group.Levels;
    noBias.Bias = 1.0f;
    EXPECT_EQ(noBias.SelectLOD(75.0f), 0);
}

TEST(LODGroup, VeryHighBiasAlwaysSelectsHighestDetail)
{
    LODGroup group;
    group.Bias = 1000.0f; // effectiveDistance ≈ 0 for reasonable distances
    group.Levels.emplace_back(AssetHandle(1), 10.0f);
    group.Levels.emplace_back(AssetHandle(2), 50.0f);
    group.Levels.emplace_back(AssetHandle(3), 200.0f);

    EXPECT_EQ(group.SelectLOD(100.0f), 0);
    EXPECT_EQ(group.SelectLOD(5000.0f), 0);
}

TEST(LODGroup, VeryLowBiasAlwaysSelectsLowestDetail)
{
    LODGroup group;
    group.Bias = 0.001f; // effectiveDistance = distance * 1000
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 200.0f);
    group.Levels.emplace_back(AssetHandle(3), 500.0f);

    // Even at distance 1, effective = 1000 → beyond all thresholds → last
    EXPECT_EQ(group.SelectLOD(1.0f), 2);
}

// =============================================================================
// LODGroup Tests — Edge Cases
// =============================================================================

TEST(LODGroup, ZeroDistanceSelectsFirstLevel)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 50.0f);
    group.Levels.emplace_back(AssetHandle(2), 150.0f);

    EXPECT_EQ(group.SelectLOD(0.0f), 0);
}

TEST(LODGroup, NegativeDistanceTreatedAsVeryClose)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 50.0f);
    group.Levels.emplace_back(AssetHandle(2), 150.0f);

    // Negative distance (shouldn't happen normally) — effective distance is negative,
    // which is <= first threshold → selects LOD 0
    EXPECT_EQ(group.SelectLOD(-10.0f), 0);
}

TEST(LODGroup, VeryLargeDistanceSelectsLastLevel)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 100.0f);
    group.Levels.emplace_back(AssetHandle(2), 500.0f);
    group.Levels.emplace_back(AssetHandle(3), 1000.0f);

    EXPECT_EQ(group.SelectLOD(999999.0f), 2);
}

TEST(LODGroup, AllLevelsSameDistanceSelectsFirst)
{
    LODGroup group;
    group.Levels.emplace_back(AssetHandle(1), 100.0f, 10000);
    group.Levels.emplace_back(AssetHandle(2), 100.0f, 5000);
    group.Levels.emplace_back(AssetHandle(3), 100.0f, 1000);

    // At distance 100, first level matches (distance <= 100)
    EXPECT_EQ(group.SelectLOD(100.0f), 0);
    // Beyond all (same) thresholds → last
    EXPECT_EQ(group.SelectLOD(101.0f), 2);
}

TEST(LODGroup, ManyLevelsCorrectSelection)
{
    LODGroup group;
    for (i32 i = 0; i < 8; ++i)
    {
        group.Levels.emplace_back(AssetHandle(i + 1), static_cast<f32>((i + 1) * 50));
    }
    // Levels at 50, 100, 150, 200, 250, 300, 350, 400

    EXPECT_EQ(group.SelectLOD(25.0f), 0);
    EXPECT_EQ(group.SelectLOD(75.0f), 1);
    EXPECT_EQ(group.SelectLOD(125.0f), 2);
    EXPECT_EQ(group.SelectLOD(375.0f), 7);
    EXPECT_EQ(group.SelectLOD(401.0f), 7); // Beyond all → last
}

// =============================================================================
// Pixel-error LOD selection (issue #711)
//
// These pin the two properties the whole scheme exists for and that a
// plausible-looking camera-plane implementation would silently break:
//
//   1. selection is independent of camera ORIENTATION, and
//   2. selection scales with render RESOLUTION, so one threshold is right at
//      both 1080p and 4K.
//
// Both are properties of EstimateProjectedPixelSize, so most of them assert on
// it directly rather than through a Mesh (which would need a GL context).
// =============================================================================

namespace
{
    // A 2 x 2 x 2 cube centred on the origin, in local space.
    BoundingBox UnitCubeBounds()
    {
        return BoundingBox(glm::vec3(-1.0f), glm::vec3(1.0f));
    }

    // A chain whose errors double per level, so a doubling of projected pixel
    // size moves the selection by exactly one level.
    LODGroup ErrorChain()
    {
        LODGroup group;
        group.Levels.emplace_back(AssetHandle(1), 0.0f, 1000u, 0.0f);
        group.Levels.emplace_back(AssetHandle(2), 0.0f, 500u, 0.01f);
        group.Levels.emplace_back(AssetHandle(3), 0.0f, 250u, 0.02f);
        group.Levels.emplace_back(AssetHandle(4), 0.0f, 125u, 0.04f);
        return group;
    }

    LODViewParams ViewAt(const glm::vec3& position, u32 screenHeight = 1080u, f32 threshold = 1.0f)
    {
        LODViewParams view;
        view.ViewPosition = position;
        view.ScreenHeight = screenHeight;
        view.TanHalfFovY = 1.0f; // 90 degrees vertical
        view.PixelErrorThreshold = threshold;
        return view;
    }
} // namespace

TEST(LODGroup, HasErrorDataDistinguishesGeneratedFromAuthored)
{
    LODGroup authored;
    authored.Levels.emplace_back(AssetHandle(1), 50.0f, 1000u);
    authored.Levels.emplace_back(AssetHandle(2), 150.0f, 200u);
    EXPECT_FALSE(authored.HasErrorData());

    EXPECT_TRUE(ErrorChain().HasErrorData());

    LODGroup empty;
    EXPECT_FALSE(empty.HasErrorData());
}

// The property the projection plane exists for: orbiting the camera around a
// static mesh at a fixed radius must not change the estimated pixel size, and
// therefore must not change the selected level. A camera-plane projection — the
// obvious-looking implementation — fails this the moment the mesh leaves the
// screen centre, and fails it hardest off-screen.
TEST(LODGroup, PixelSizeIsInvariantToCameraOrbit)
{
    const BoundingBox bounds = UnitCubeBounds();
    const glm::mat4 model(1.0f);
    constexpr f32 kRadius = 30.0f;

    const f32 reference = EstimateProjectedPixelSize(bounds, model, ViewAt(glm::vec3(kRadius, 0.0f, 0.0f)));
    ASSERT_GT(reference, 0.0f);

    // 36 azimuths x 5 elevations, including straight overhead and below.
    for (i32 azimuthStep = 0; azimuthStep < 36; ++azimuthStep)
    {
        const f32 azimuth = glm::radians(static_cast<f32>(azimuthStep) * 10.0f);
        for (i32 elevationStep = -2; elevationStep <= 2; ++elevationStep)
        {
            const f32 elevation = glm::radians(static_cast<f32>(elevationStep) * 40.0f);
            const glm::vec3 eye(kRadius * std::cos(elevation) * std::cos(azimuth),
                                kRadius * std::sin(elevation),
                                kRadius * std::cos(elevation) * std::sin(azimuth));

            const f32 pixelSize = EstimateProjectedPixelSize(bounds, model, ViewAt(eye));
            EXPECT_NEAR(pixelSize, reference, reference * 1e-4f)
                << "azimuth " << azimuthStep * 10 << " deg, elevation " << elevationStep * 40 << " deg";
        }
    }
}

// The same statement one level up: the SELECTED INDEX must not move as the
// camera orbits. This is the acceptance criterion "LOD does not switch when the
// camera rotates in place", expressed on the pure-CPU seam.
TEST(LODGroup, SelectedLevelIsInvariantToCameraOrbit)
{
    const LODGroup group = ErrorChain();
    const BoundingBox bounds = UnitCubeBounds();
    const glm::mat4 model(1.0f);
    constexpr f32 kRadius = 40.0f;

    const i32 reference = group.SelectLODByPixelError(
        EstimateProjectedPixelSize(bounds, model, ViewAt(glm::vec3(kRadius, 0.0f, 0.0f))), 1.0f);

    for (i32 azimuthStep = 0; azimuthStep < 72; ++azimuthStep)
    {
        const f32 azimuth = glm::radians(static_cast<f32>(azimuthStep) * 5.0f);
        const glm::vec3 eye(kRadius * std::cos(azimuth), 0.0f, kRadius * std::sin(azimuth));
        EXPECT_EQ(group.SelectLODByPixelError(EstimateProjectedPixelSize(bounds, model, ViewAt(eye)), 1.0f), reference)
            << "azimuth " << azimuthStep * 5 << " deg";
    }
}

// Rotating the OBJECT must not change it either. Re-fitting an axis-aligned box
// around a rotated one would grow the extent by up to sqrt(3) at 45 degrees and
// pump the selected level as the object spins.
TEST(LODGroup, PixelSizeIsInvariantToObjectRotation)
{
    const BoundingBox bounds = UnitCubeBounds();
    const LODViewParams view = ViewAt(glm::vec3(0.0f, 0.0f, 25.0f));
    const f32 reference = EstimateProjectedPixelSize(bounds, glm::mat4(1.0f), view);

    for (i32 step = 0; step < 24; ++step)
    {
        const f32 angle = glm::radians(static_cast<f32>(step) * 15.0f);
        const glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
        EXPECT_NEAR(EstimateProjectedPixelSize(bounds, model, view), reference, reference * 1e-4f)
            << "object yaw " << step * 15 << " deg";
    }
}

// Pixel size scales with render height, which is what lets one threshold serve
// every resolution. 4K is exactly 2x 1080p.
TEST(LODGroup, PixelSizeScalesWithRenderHeight)
{
    const BoundingBox bounds = UnitCubeBounds();
    const glm::mat4 model(1.0f);
    const glm::vec3 eye(0.0f, 0.0f, 50.0f);

    const f32 at1080p = EstimateProjectedPixelSize(bounds, model, ViewAt(eye, 1080u));
    const f32 at4K = EstimateProjectedPixelSize(bounds, model, ViewAt(eye, 2160u));

    ASSERT_GT(at1080p, 0.0f);
    EXPECT_NEAR(at4K, at1080p * 2.0f, at1080p * 1e-4f);
}

// The acceptance criterion, on the selection seam: the SAME scene and the SAME
// threshold pick a finer level at 4K than at 1080p, with no retuning.
TEST(LODGroup, HigherResolutionSelectsFinerLevelAtTheSameThreshold)
{
    const LODGroup group = ErrorChain();
    const BoundingBox bounds = UnitCubeBounds();
    const glm::mat4 model(1.0f);

    // Distance chosen so the two resolutions straddle a switch point rather than
    // both saturating at either end of the chain.
    const glm::vec3 eye(0.0f, 0.0f, 61.0f);

    const i32 lod1080p = group.SelectLODByPixelError(EstimateProjectedPixelSize(bounds, model, ViewAt(eye, 1080u)), 1.0f);
    const i32 lod4K = group.SelectLODByPixelError(EstimateProjectedPixelSize(bounds, model, ViewAt(eye, 2160u)), 1.0f);

    EXPECT_LT(lod4K, lod1080p) << "4K must hold more detail than 1080p at an unchanged threshold";
}

// Moving away monotonically coarsens: no distance may select a finer level than
// a nearer one did.
TEST(LODGroup, SelectedLevelCoarsensMonotonicallyWithDistance)
{
    const LODGroup group = ErrorChain();
    const BoundingBox bounds = UnitCubeBounds();
    const glm::mat4 model(1.0f);

    i32 previous = -1;
    for (i32 step = 1; step <= 200; ++step)
    {
        const glm::vec3 eye(0.0f, 0.0f, static_cast<f32>(step) * 5.0f);
        const i32 lod = group.SelectLODByPixelError(EstimateProjectedPixelSize(bounds, model, ViewAt(eye)), 1.0f);
        EXPECT_GE(lod, previous) << "distance " << step * 5;
        previous = lod;
    }
    EXPECT_EQ(previous, static_cast<i32>(group.Levels.size()) - 1) << "far away must reach the coarsest level";
}

// A larger threshold is the performance lever: it may only ever select a level
// at least as coarse as a tighter one.
TEST(LODGroup, LooserThresholdNeverSelectsFinerLevel)
{
    const LODGroup group = ErrorChain();
    constexpr f32 kPixelSize = 300.0f;

    const i32 strict = group.SelectLODByPixelError(kPixelSize, 1.0f);
    const i32 loose = group.SelectLODByPixelError(kPixelSize, 8.0f);
    EXPECT_GE(loose, strict);
    EXPECT_GT(loose, strict) << "an 8x looser budget must actually buy a coarser level on this chain";
}

TEST(LODGroup, PixelErrorSelectionHonoursBias)
{
    const LODGroup base = ErrorChain();
    constexpr f32 kPixelSize = 300.0f;

    LODGroup keepDetail = base;
    keepDetail.Bias = 4.0f; // "looks 4x bigger" — hold detail longer

    LODGroup dropDetail = base;
    dropDetail.Bias = 0.25f;

    const i32 neutral = base.SelectLODByPixelError(kPixelSize, 1.0f);
    EXPECT_LE(keepDetail.SelectLODByPixelError(kPixelSize, 1.0f), neutral);
    EXPECT_GE(dropDetail.SelectLODByPixelError(kPixelSize, 1.0f), neutral);
}

// A level with no measured error is UNMEASURED, not free. The inspector's "Add LOD
// Level" button appends exactly this — handle 0, error 0 — onto a generated chain,
// and `pixelSize * 0` satisfies every budget, so before the guard the blank level
// won at every distance. On screen that reads as "LOD stopped working", or, once a
// mesh is dropped onto it, a coarse mesh drawn point-blank.
TEST(LODGroup, UnmeasuredLevelDoesNotWinAtEveryDistance)
{
    LODGroup group = ErrorChain();
    const sizet lastMeasured = group.Levels.size() - 1;
    group.Levels.emplace_back(AssetHandle(0), 500.0f, 0u, 0.0f); // what the button appends

    // Point-blank: still LOD 0.
    EXPECT_EQ(group.SelectLODByPixelError(100000.0f, 1.0f), 0);

    // At any distance the scan must stop at the last MEASURED level and never reach
    // the blank one.
    for (i32 step = 1; step <= 400; ++step)
    {
        const f32 pixelSize = 40000.0f / static_cast<f32>(step);
        const i32 selected = group.SelectLODByPixelError(pixelSize, 1.0f);
        ASSERT_LE(selected, static_cast<i32>(lastMeasured))
            << "selected the unmeasured level at projected size " << pixelSize;
    }

    // And it is genuinely reachable in index terms — the guard is what excludes it,
    // not the loop bound.
    ASSERT_EQ(group.Levels.size(), lastMeasured + 2);
}

// A non-finite error in the middle of a chain (corrupt scene, bad hand-edit) ends
// the scan rather than propagating NaN into the comparison.
TEST(LODGroup, NonFiniteLevelErrorStopsTheScan)
{
    LODGroup group = ErrorChain();
    group.Levels[2].Error = std::numeric_limits<f32>::quiet_NaN();

    // Level 1 is still selectable; 2 and everything past it are not.
    EXPECT_EQ(group.SelectLODByPixelError(50.0f, 1.0f), 1);
}

TEST(LODGroup, PixelErrorSelectionEdgeCases)
{
    LODGroup empty;
    EXPECT_EQ(empty.SelectLODByPixelError(100.0f, 1.0f), -1);

    const LODGroup group = ErrorChain();
    // Zero / negative / non-finite projected size all mean "no measurable error",
    // which is the coarsest level the chain offers.
    EXPECT_EQ(group.SelectLODByPixelError(0.0f, 1.0f), static_cast<i32>(group.Levels.size()) - 1);
    EXPECT_EQ(group.SelectLODByPixelError(-5.0f, 1.0f), static_cast<i32>(group.Levels.size()) - 1);
    EXPECT_EQ(group.SelectLODByPixelError(std::numeric_limits<f32>::quiet_NaN(), 1.0f),
              static_cast<i32>(group.Levels.size()) - 1);

    // A non-finite or non-positive threshold falls back to 1 px rather than
    // selecting arbitrarily.
    const i32 defaulted = group.SelectLODByPixelError(300.0f, 1.0f);
    EXPECT_EQ(group.SelectLODByPixelError(300.0f, std::numeric_limits<f32>::quiet_NaN()), defaulted);
    EXPECT_EQ(group.SelectLODByPixelError(300.0f, 0.0f), defaulted);
}

TEST(LODGroup, EstimateProjectedPixelSizeRejectsDegenerateInput)
{
    const LODViewParams view = ViewAt(glm::vec3(0.0f, 0.0f, 10.0f));

    // Zero-extent bounds have no on-screen size.
    EXPECT_FLOAT_EQ(EstimateProjectedPixelSize(BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f)), glm::mat4(1.0f), view), 0.0f);

    // Non-finite transform must not produce a non-finite pixel size.
    glm::mat4 broken(1.0f);
    broken[3][0] = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FLOAT_EQ(EstimateProjectedPixelSize(UnitCubeBounds(), broken, view), 0.0f);

    LODViewParams brokenView = view;
    brokenView.ViewPosition.y = std::numeric_limits<f32>::infinity();
    EXPECT_FLOAT_EQ(EstimateProjectedPixelSize(UnitCubeBounds(), glm::mat4(1.0f), brokenView), 0.0f);
}

// A camera inside the bounds must report a huge size (hence LOD 0), not a
// division blow-up.
TEST(LODGroup, CameraInsideBoundsSelectsFinestLevel)
{
    const LODGroup group = ErrorChain();
    const f32 pixelSize = EstimateProjectedPixelSize(UnitCubeBounds(), glm::mat4(1.0f), ViewAt(glm::vec3(0.0f)));

    EXPECT_TRUE(std::isfinite(pixelSize));
    EXPECT_GT(pixelSize, 1e6f);
    EXPECT_EQ(group.SelectLODByPixelError(pixelSize, 1.0f), 0);
}

// A uniform scale on the transform scales the projected size with it — the
// estimate is about world size, not model-space size.
TEST(LODGroup, PixelSizeFollowsTransformScale)
{
    const BoundingBox bounds = UnitCubeBounds();
    const LODViewParams view = ViewAt(glm::vec3(0.0f, 0.0f, 500.0f));

    const f32 unscaled = EstimateProjectedPixelSize(bounds, glm::mat4(1.0f), view);
    const f32 scaled = EstimateProjectedPixelSize(bounds, glm::scale(glm::mat4(1.0f), glm::vec3(4.0f)), view);

    ASSERT_GT(unscaled, 0.0f);
    // Not exactly 4x: the distance is measured to the near side of the bounds,
    // which the larger box reaches sooner. Bracket it instead of pinning it.
    EXPECT_GT(scaled, unscaled * 3.9f);
    EXPECT_LT(scaled, unscaled * 4.2f);
}
