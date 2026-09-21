#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomDeformationTest — issue #1249, acceptance criteria 2 and 3.
//
// "Guides and rendered hairs follow bending limbs and facial morphs without
//  floating roots or gross coat collapse."
// "Previous-frame data, bounds and invalidation cover LOD, topology, teleport
//  and animation reset."
//
// Those two sentences are the tests, and they are testable HERE — with no GPU,
// no scene and no editor — precisely because the deformation produces CPU
// positions rather than only pixels. That is the whole reason the rigid root
// transfer lives in a pure function: "the root did not float" is
// `EXPECT_NEAR(root, surfacePoint, 1e-5)` against a number, not an eyeball on a
// screenshot.
//
// WHAT EACH CRITERION LOOKS LIKE AS AN ASSERTION:
//
//   no floating roots  — the deformed root sits on the deformed triangle, to
//                        within the offset it was authored with. Checked after
//                        a bend, so it is about the DEFORMED surface rather
//                        than about the bind pose the binding was built from.
//   no coat collapse   — every strand's length is preserved EXACTLY under the
//                        deformation. A rigid transfer cannot change it, so a
//                        tolerance here would be hiding a bug rather than
//                        absorbing float noise.
//   previous frame     — prev positions come from the previous PALETTE, so a
//                        pose that moved produces a non-zero delta and a pose
//                        that did not produces a bit-for-bit zero one.
//   invalidation       — with history rejected, prev == current EXACTLY, so the
//                        emitted motion is zero rather than approximately zero.
// =============================================================================

#include <gtest/gtest.h>

#include <optional>
#include <set>

#include "GroomBindingFixture.h"

#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomBindingTest;

namespace
{
    // The bind-pose palette: two bones, both identity. Deforming with it must
    // reproduce the bind pose exactly, which is the baseline every bend below
    // is measured against.
    [[nodiscard]] std::vector<glm::mat4> RestPalette()
    {
        return { glm::mat4(1.0f), glm::mat4(1.0f) };
    }

    // Bone 1 rotated about the Z axis through the hinge line at x = 0.5, so the
    // half of the grid weighted to it lifts. A real limb bend, expressed as the
    // one matrix the shared deformation output would carry.
    [[nodiscard]] std::vector<glm::mat4> BentPalette(f32 degrees)
    {
        const glm::vec3 hinge{ 0.5f, 0.0f, 0.0f };
        glm::mat4 bend = glm::translate(glm::mat4(1.0f), hinge);
        bend = glm::rotate(bend, glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
        bend = glm::translate(bend, -hinge);
        return { glm::mat4(1.0f), bend };
    }

    struct Bound
    {
        Ref<GroomAsset> Groom;
        Ref<GroomBindingAsset> Binding;
        GroomBindingBuildStats Stats;
    };

    [[nodiscard]] Bound BindCoat(const GridSurface& grid, u32 strands = 16u, u32 points = 4u,
                                 f32 rootOffsetY = 0.0f)
    {
        Bound bound;
        bound.Groom = MakeCoat(strands, points, 0.1f, rootOffsetY);
        EXPECT_TRUE(bound.Groom);
        std::string reason;
        const bool built = GroomBindingBuilder::Build(*bound.Groom, grid.View(2u), "TestBody",
                                                      GroomBindingBuildSettings{}, bound.Binding, bound.Stats,
                                                      reason);
        EXPECT_TRUE(built) << reason;
        return bound;
    }
} // namespace

// ── The baseline: an unbent body does not move the coat ─────────────────────

TEST(GroomDeformation, TheBindPosePaletteReproducesTheAuthoredCoat)
{
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid);
    ASSERT_TRUE(bound.Binding);

    const auto palette = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);

    EXPECT_EQ(stats.RootsDeformed, bound.Groom->GetCurveCount());
    EXPECT_EQ(stats.RootsHeldDegenerate, 0u);
    EXPECT_EQ(stats.RootsSkippedOutOfRange, 0u);

    const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);
    const auto& authored = bound.Groom->GetPoints();
    ASSERT_EQ(deformed.size(), authored.size());
    for (sizet i = 0; i < deformed.size(); ++i)
    {
        // A tight absolute tolerance rather than equality: the round trip goes
        // through a quaternion, so the arithmetic is not the identity even
        // where the transform is. 1e-5 on a unit-scale body is a micrometre.
        EXPECT_NEAR(deformed[i].x, authored[i].x, 1.0e-5f) << "point " << i;
        EXPECT_NEAR(deformed[i].y, authored[i].y, 1.0e-5f) << "point " << i;
        EXPECT_NEAR(deformed[i].z, authored[i].z, 1.0e-5f) << "point " << i;
    }
}

// ── Criterion 2: no floating roots ──────────────────────────────────────────

TEST(GroomDeformation, EveryRootStaysOnTheDeformedSurfaceThroughABend)
{
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    // Rooted ON the surface, so "did the root float" is a question about the
    // surface point itself rather than about an authored offset.
    const Bound bound = BindCoat(grid, 24u, 4u, 0.0f);
    ASSERT_TRUE(bound.Binding);

    const auto rest = RestPalette();
    const auto bent = BentPalette(40.0f);

    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(bent, rest, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    ASSERT_EQ(stats.RootsDeformed, bound.Groom->GetCurveCount());

    const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        // Where the surface actually IS after the bend: the deformed triangle,
        // sampled at the barycentric the binding recorded. Computed from the
        // skinned corners here rather than taken from the transform, so the
        // assertion is against the surface and not against the thing under
        // test restating itself.
        const GroomRootBinding& record = bound.Binding->GetRoot(curve);
        const glm::uvec3 corners = inputs.Surface.TriangleIndices(record.TriangleIndex);
        bool weighted = false;
        const glm::vec3 c0 = SkinGroomSurfaceVertex(inputs.Skinning, corners.x, grid.Positions[corners.x], bent,
                                                    weighted);
        const glm::vec3 c1 = SkinGroomSurfaceVertex(inputs.Skinning, corners.y, grid.Positions[corners.y], bent,
                                                    weighted);
        const glm::vec3 c2 = SkinGroomSurfaceVertex(inputs.Skinning, corners.z, grid.Positions[corners.z], bent,
                                                    weighted);
        const glm::vec3 surfacePoint =
            c0 * record.Barycentric.x + c1 * record.Barycentric.y + c2 * record.Barycentric.z;

        const u32 first = bound.Groom->GetCurveFirstPoint(curve);
        const f32 drift = glm::length(deformed[first] - surfacePoint);
        // The authored root sits ON the surface in this fixture, so the only
        // distance between it and the deformed surface point is the binding's
        // own rest offset, which is zero to within the projection's precision.
        EXPECT_LT(drift, 1.0e-4f) << "curve " << curve << " floated " << drift << " off the deformed surface";
    }
}

TEST(GroomDeformation, ARootAuthoredAboveTheSurfaceKeepsItsOffsetThroughABend)
{
    // The other half of "no floating roots": a coat authored with a shell
    // offset must keep that offset, not be snapped down to the surface. A
    // binder that silently projected roots onto the mesh would pass the test
    // above and fail this one.
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    constexpr f32 kOffset = 0.02f;
    const Bound bound = BindCoat(grid, 12u, 4u, kOffset);
    ASSERT_TRUE(bound.Binding);

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        EXPECT_NEAR(bound.Binding->GetRoot(curve).RestDistance, kOffset, 1.0e-4f);
    }

    const auto bent = BentPalette(35.0f);
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(bent, bent, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        const u32 first = bound.Groom->GetCurveFirstPoint(curve);
        const GroomRootTransform& transform = transforms[curve];
        ASSERT_TRUE(transform.Valid);
        const f32 distance = glm::length(deformed[first] - transform.Origin);
        EXPECT_NEAR(distance, kOffset, 1.0e-4f) << "curve " << curve << " lost its authored shell offset";
    }
}

// ── Criterion 2: no gross coat collapse ─────────────────────────────────────

TEST(GroomDeformation, EveryStrandKeepsItsLengthThroughABend)
{
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 32u, 6u);
    ASSERT_TRUE(bound.Binding);

    const auto& authored = bound.Groom->GetPoints();

    // Several bend angles, including one past 90 degrees: a collapse shows up
    // as a length that shrinks with the angle, which one sample cannot see.
    for (const f32 degrees : { 10.0f, 45.0f, 90.0f, 135.0f })
    {
        const auto bent = BentPalette(degrees);
        GroomDeformationInputs inputs;
        inputs.Surface = grid.View(2u);
        inputs.Skinning = grid.Skinning(bent, bent, true);
        inputs.HasHistory = true;

        TArray<GroomRootTransform> transforms;
        const GroomDeformationStats stats =
            EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
        ASSERT_EQ(stats.RootsHeldDegenerate, 0u) << "at " << degrees << " degrees";

        const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);
        for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
        {
            const f32 authoredLength = CurveLength(*bound.Groom, curve, authored);
            const f32 deformedLength = CurveLength(*bound.Groom, curve, deformed);
            ASSERT_GT(authoredLength, 0.0f);
            // A rigid transfer preserves length exactly; the tolerance is float
            // noise through a quaternion, nothing else. A relative bound so the
            // assertion says the same thing for a long coat as for a short one.
            EXPECT_NEAR(deformedLength / authoredLength, 1.0f, 1.0e-4f)
                << "curve " << curve << " at " << degrees << " degrees collapsed from " << authoredLength << " to "
                << deformedLength;
        }
    }
}

TEST(GroomDeformation, ABentCoatActuallyMoves)
{
    // The negative control for every "nothing changed" assertion above: if the
    // deformation quietly did nothing, the length and offset tests would still
    // pass. This is what makes them mean something.
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 24u, 4u);
    ASSERT_TRUE(bound.Binding);

    const auto rest = RestPalette();
    const auto bent = BentPalette(60.0f);

    GroomDeformationInputs restInputs;
    restInputs.Surface = grid.View(2u);
    restInputs.Skinning = grid.Skinning(rest, rest, true);
    restInputs.HasHistory = true;
    TArray<GroomRootTransform> restTransforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, restInputs, std::nullopt, restTransforms);

    GroomDeformationInputs bentInputs = restInputs;
    bentInputs.Skinning = grid.Skinning(bent, bent, true);
    TArray<GroomRootTransform> bentTransforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, bentInputs, std::nullopt, bentTransforms);

    const auto restPoints = DeformAllPoints(*bound.Groom, *bound.Binding, restTransforms);
    const auto bentPoints = DeformAllPoints(*bound.Groom, *bound.Binding, bentTransforms);

    u32 moved = 0;
    f32 maxMove = 0.0f;
    for (sizet i = 0; i < restPoints.size(); ++i)
    {
        const f32 delta = glm::length(bentPoints[i] - restPoints[i]);
        maxMove = std::max(maxMove, delta);
        if (delta > 1.0e-3f)
        {
            ++moved;
        }
    }
    // The hinge lifts half the grid, so roughly half the coat must move, and it
    // must move by a distance a person would see.
    EXPECT_GT(moved, restPoints.size() / 4u) << "only " << moved << " of " << restPoints.size() << " points moved";
    EXPECT_GT(maxMove, 0.05f) << "the largest movement was " << maxMove << ", which is not a 60-degree bend";
}

// ── Criterion 2: facial morphs ──────────────────────────────────────────────

TEST(GroomDeformation, AMorphedSurfaceCarriesTheCoatWithNoSkeletonAtAll)
{
    // Morph targets are applied on the CPU straight into the vertex array, so a
    // morph reaches the coat through the POSITIONS with no morph-specific code
    // in the groom. This is that claim as a test: the same binding, the same
    // (absent) palette, a surface whose vertices moved.
    GridSurface grid = MakeGrid(8u);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    GroomDeformationInputs neutral;
    neutral.Surface = grid.View();
    neutral.HasHistory = true;
    TArray<GroomRootTransform> neutralTransforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, neutral, std::nullopt, neutralTransforms);
    const auto neutralPoints = DeformAllPoints(*bound.Groom, *bound.Binding, neutralTransforms);

    // "An expression": a smooth bulge in +Y across the middle of the surface,
    // written into the positions exactly as MorphTargetSystem would.
    GridSurface expressed = grid;
    for (auto& position : expressed.Positions)
    {
        const f32 falloff = std::exp(-16.0f * ((position.x - 0.5f) * (position.x - 0.5f) +
                                               (position.z - 0.5f) * (position.z - 0.5f)));
        position.y += 0.08f * falloff;
    }

    GroomDeformationInputs morphed;
    morphed.Surface = expressed.View();
    morphed.HasHistory = true;
    TArray<GroomRootTransform> morphedTransforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, morphed, std::nullopt, morphedTransforms);
    EXPECT_EQ(stats.RootsDeformed, bound.Groom->GetCurveCount());

    const auto morphedPoints = DeformAllPoints(*bound.Groom, *bound.Binding, morphedTransforms);

    f32 maxMove = 0.0f;
    for (sizet i = 0; i < neutralPoints.size(); ++i)
    {
        maxMove = std::max(maxMove, glm::length(morphedPoints[i] - neutralPoints[i]));
    }
    EXPECT_GT(maxMove, 0.01f) << "the coat did not follow the expression";

    // ...and it followed it without collapsing.
    const auto& authored = bound.Groom->GetPoints();
    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        EXPECT_NEAR(CurveLength(*bound.Groom, curve, morphedPoints) / CurveLength(*bound.Groom, curve, authored),
                    1.0f, 1.0e-4f);
    }
}

// ── Criterion 3: previous-frame data ────────────────────────────────────────

TEST(GroomDeformation, PreviousPositionsComeFromThePreviousPose)
{
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    const auto previousPose = BentPalette(20.0f);
    const auto currentPose = BentPalette(40.0f);

    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(currentPose, previousPose, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    EXPECT_TRUE(stats.HasHistory);

    const auto current = DeformAllPoints(*bound.Groom, *bound.Binding, transforms, false);
    const auto previous = DeformAllPoints(*bound.Groom, *bound.Binding, transforms, true);

    // The previous positions must be the 20-degree pose, which is a REAL
    // difference from the 40-degree one on the half of the coat the hinge
    // moves. A previous position derived from the current pose would make every
    // delta zero, which is the ghosting-free-but-wrong failure.
    f32 maxDelta = 0.0f;
    for (sizet i = 0; i < current.size(); ++i)
    {
        maxDelta = std::max(maxDelta, glm::length(current[i] - previous[i]));
    }
    EXPECT_GT(maxDelta, 0.01f) << "previous positions did not come from a different pose";

    // And a cross-check: deforming with the PREVIOUS palette as the current one
    // must reproduce those previous positions.
    GroomDeformationInputs asPrevious;
    asPrevious.Surface = grid.View(2u);
    asPrevious.Skinning = grid.Skinning(previousPose, previousPose, true);
    asPrevious.HasHistory = true;
    TArray<GroomRootTransform> previousTransforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, asPrevious, std::nullopt, previousTransforms);
    const auto reference = DeformAllPoints(*bound.Groom, *bound.Binding, previousTransforms);
    for (sizet i = 0; i < previous.size(); ++i)
    {
        EXPECT_NEAR(previous[i].x, reference[i].x, 1.0e-5f) << "point " << i;
        EXPECT_NEAR(previous[i].y, reference[i].y, 1.0e-5f) << "point " << i;
        EXPECT_NEAR(previous[i].z, reference[i].z, 1.0e-5f) << "point " << i;
    }
}

TEST(GroomDeformation, RejectedHistoryMakesPreviousExactlyEqualToCurrent)
{
    // The invalidation contract, and the reason it is an EQUALITY rather than a
    // tolerance: holding prev equal to current is what makes the emitted motion
    // exactly zero. "Almost zero" is a smear at 4x upscale.
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    // Named, not temporaries: GroomSkinningView borrows them as spans, and the
    // fixture deletes the rvalue overload precisely so this cannot be written
    // the short way.
    const auto current = BentPalette(40.0f);
    const auto previous = BentPalette(10.0f);

    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(current, previous, true);
    inputs.HasHistory = false; // the caller detected a discontinuity

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    EXPECT_FALSE(stats.HasHistory);

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        const GroomRootTransform& transform = transforms[curve];
        ASSERT_TRUE(transform.Valid);
        EXPECT_EQ(std::bit_cast<u32>(transform.PrevOrigin.x), std::bit_cast<u32>(transform.Origin.x));
        EXPECT_EQ(std::bit_cast<u32>(transform.PrevOrigin.y), std::bit_cast<u32>(transform.Origin.y));
        EXPECT_EQ(std::bit_cast<u32>(transform.PrevOrigin.z), std::bit_cast<u32>(transform.Origin.z));
        EXPECT_EQ(std::bit_cast<u32>(transform.PrevRotation.w), std::bit_cast<u32>(transform.Rotation.w));
    }

    const auto currentPoints = DeformAllPoints(*bound.Groom, *bound.Binding, transforms, false);
    const auto previousPoints = DeformAllPoints(*bound.Groom, *bound.Binding, transforms, true);
    for (sizet i = 0; i < currentPoints.size(); ++i)
    {
        EXPECT_EQ(std::bit_cast<u32>(currentPoints[i].x), std::bit_cast<u32>(previousPoints[i].x)) << "point " << i;
        EXPECT_EQ(std::bit_cast<u32>(currentPoints[i].y), std::bit_cast<u32>(previousPoints[i].y)) << "point " << i;
        EXPECT_EQ(std::bit_cast<u32>(currentPoints[i].z), std::bit_cast<u32>(previousPoints[i].z)) << "point " << i;
    }
}

TEST(GroomDeformation, ASkeletonWithNoBoneHistoryEmitsZeroMotionEvenWhenTheCallerAsksForHistory)
{
    // The second gate. The caller can be perfectly happy — no teleport, no LOD
    // switch — while the SKELETON has just been reset, and the previous palette
    // then describes a pose it was never in. Both gates have to hold, because
    // either alone leaves one discontinuity uncovered.
    GridSurface grid = MakeGrid(4u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 8u, 3u);
    ASSERT_TRUE(bound.Binding);

    const auto current = BentPalette(40.0f);
    const auto previous = BentPalette(10.0f);

    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(current, previous, /*hasPreviousPose*/ false);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    for (const auto& transform : transforms)
    {
        ASSERT_TRUE(transform.Valid);
        EXPECT_EQ(std::bit_cast<u32>(transform.PrevOrigin.y), std::bit_cast<u32>(transform.Origin.y));
    }
}

// ── The groom and the body are two entities with two transforms ────────

TEST(GroomDeformation, ABodyScaledRelativeToItsGroomStillBindsAndDeformsCorrectly)
{
    // The case the FIRST version of this feature got wrong, and it is not
    // exotic: Scenes/GroomStrandCoat.olo authors its body sphere at scale 0.088
    // and its coat at scale 1, because the groom asset is already at world size.
    // Bound in the body's object space, every root of that coat sat at a tenth
    // of the sphere's radius and bound Distant to whatever triangle faced the
    // origin -- a coat visibly detached from the body it grows on.
    //
    // Here the body is a grid HALF the size the coat is authored against, so an
    // implementation that ignores the relative transform cannot pass: the roots
    // would miss the surface by half the grid.
    // The transform has a TRANSLATION as well as a scale, and the translation is
    // what makes this test discriminate. A scale alone moves a plane through the
    // origin nowhere, so a binder that ignored the transform entirely would
    // still find the roots sitting on it and this test would pass while the bug
    // was present -- which is what its first version did. Verified the other way
    // too, by forcing SurfaceToGroom to identity: max rest distance becomes 0.35
    // (exactly the lift), every root is Distant, and the coat stops moving.
    constexpr f32 kBodyScale = 0.5f;
    constexpr f32 kBodyLift = 0.35f;
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid, 0.5f);

    // The body's own object space is the unit grid at y = 0. Its world transform
    // lifts it to y = kBodyLift and halves it, so in the GROOM's space it spans
    // [0, 0.5] x [0, 0.5] at y = 0.35 -- and the coat is authored there, which
    // is what an artist fitting a groom to a placed character produces.
    Ref<GroomAsset> groom = MakeCoat(24u, 4u, 0.05f, kBodyLift, kBodyScale);
    ASSERT_TRUE(groom);

    // groomWorld = identity, targetWorld = translate(kBodyLift) * scale(kBodyScale).
    const glm::mat4 targetWorld = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kBodyLift, 0.0f)),
                                             glm::vec3(kBodyScale));
    const glm::mat4 surfaceToGroom = MakeGroomSurfaceToGroomMatrix(glm::mat4(1.0f), targetWorld);

    // The bug this pins, stated as a number: ignoring the transform puts the
    // body at y = 0 while the roots are at y = 0.35, which is past the default
    // search radius, so every root would be Distant.
    ASSERT_GT(kBodyLift, GroomBindingBuildSettings{}.SearchRadius);

    GroomBindingBuildSettings settings;
    settings.SurfaceToGroom = surfaceToGroom;

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(2u), "ScaledBody", settings, binding, stats, reason))
        << reason;

    // THE ASSERTION THAT WOULD HAVE CAUGHT THE BUG: with the transform honoured
    // the roots sit ON the surface; without it they would be half a grid away
    // and every one of them Distant.
    EXPECT_EQ(stats.RootsDistant, 0u) << "the roots missed the scaled body entirely";
    EXPECT_LT(stats.MaxRestDistance, 1.0e-3f)
        << "the roots are " << stats.MaxRestDistance << " from a surface they are authored to sit on";

    // ...and it deforms: bending the scaled body carries the coat.
    const auto rest = RestPalette();
    const auto bent = BentPalette(50.0f);

    GroomDeformationInputs restInputs;
    restInputs.Surface = grid.View(2u);
    restInputs.Skinning = grid.Skinning(rest, rest, true);
    restInputs.SurfaceToGroom = surfaceToGroom;
    restInputs.HasHistory = true;
    TArray<GroomRootTransform> restTransforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, restInputs, std::nullopt, restTransforms);

    // The PREVIOUS pose gets the same space conversion, and this is asserted
    // rather than assumed: the first version of this test set SurfaceToGroom,
    // offered history, and left PrevSurfaceToGroom unset -- so last frame's
    // pose was mapped by the identity while this frame's was mapped correctly,
    // and nothing here noticed, because every other assertion in this test is
    // about positions and this defect is only in velocities. The field is an
    // optional now so that omitting it means "the same matrix" instead of
    // "the identity", and the arm below is what says so.
    GroomDeformationInputs previousFrame = restInputs;
    previousFrame.PrevSurfaceToGroom = surfaceToGroom;
    TArray<GroomRootTransform> previousFrameTransforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, previousFrame, std::nullopt, previousFrameTransforms);
    ASSERT_EQ(previousFrameTransforms.Num(), restTransforms.Num());
    for (sizet i = 0; i < restTransforms.Num(); ++i)
    {
        if (!restTransforms[i].Valid)
        {
            continue;
        }
        // Unset and set-to-the-same-matrix must be bit-identical, and both must
        // put the previous origin exactly where the current one is, since the
        // pose is the rest pose on both sides here.
        EXPECT_EQ(std::bit_cast<u32>(restTransforms[i].PrevOrigin.x),
                  std::bit_cast<u32>(previousFrameTransforms[i].PrevOrigin.x));
        EXPECT_EQ(std::bit_cast<u32>(restTransforms[i].PrevOrigin.x),
                  std::bit_cast<u32>(restTransforms[i].Origin.x));
        EXPECT_EQ(std::bit_cast<u32>(restTransforms[i].PrevOrigin.y),
                  std::bit_cast<u32>(restTransforms[i].Origin.y));
        EXPECT_EQ(std::bit_cast<u32>(restTransforms[i].PrevOrigin.z),
                  std::bit_cast<u32>(restTransforms[i].Origin.z));
    }

    GroomDeformationInputs bentInputs = restInputs;
    bentInputs.Skinning = grid.Skinning(bent, bent, true);
    TArray<GroomRootTransform> bentTransforms;
    const GroomDeformationStats bentStats =
        EvaluateGroomRootTransforms(*groom, *binding, bentInputs, std::nullopt, bentTransforms);
    EXPECT_EQ(bentStats.RootsDeformed, groom->GetCurveCount());

    const auto restPoints = DeformAllPoints(*groom, *binding, restTransforms);
    const auto bentPoints = DeformAllPoints(*groom, *binding, bentTransforms);

    f32 maxMove = 0.0f;
    for (sizet i = 0; i < restPoints.size(); ++i)
    {
        maxMove = std::max(maxMove, glm::length(bentPoints[i] - restPoints[i]));
    }
    EXPECT_GT(maxMove, 0.01f) << "the coat did not follow the scaled body";

    // ...without collapsing. The scale is in the SPACE conversion, not in the
    // strand: a rigid transfer preserves the authored length whatever the body
    // is scaled by, and a conversion applied twice would shrink every strand.
    const auto& authored = groom->GetPoints();
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        EXPECT_NEAR(CurveLength(*groom, curve, bentPoints) / CurveLength(*groom, curve, authored), 1.0f, 1.0e-3f)
            << "curve " << curve;
    }
}

// ── Refusals, counted rather than silent ────────────────────────────────────

TEST(GroomDeformation, ADegenerateTriangleHoldsItsStrandsAtRestAndIsCounted)
{
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    // Collapse the whole surface to a line, which makes every triangle
    // degenerate. A pose can do this to a real character's mesh, and the
    // requirement is that the strand does not fly off.
    GridSurface collapsed = grid;
    for (auto& position : collapsed.Positions)
    {
        position.z = 0.0f;
    }

    GroomDeformationInputs inputs;
    inputs.Surface = collapsed.View();
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);

    EXPECT_EQ(stats.RootsDeformed, 0u);
    EXPECT_EQ(stats.RootsHeldDegenerate, bound.Groom->GetCurveCount())
        << "a collapsed surface must be counted, not silently drawn somewhere plausible";

    // Held at rest means exactly the authored positions, not NaNs and not the
    // origin.
    const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);
    const auto& authored = bound.Groom->GetPoints();
    for (sizet i = 0; i < deformed.size(); ++i)
    {
        EXPECT_TRUE(std::isfinite(deformed[i].x) && std::isfinite(deformed[i].y) && std::isfinite(deformed[i].z));
        EXPECT_EQ(std::bit_cast<u32>(deformed[i].x), std::bit_cast<u32>(authored[i].x)) << "point " << i;
        EXPECT_EQ(std::bit_cast<u32>(deformed[i].y), std::bit_cast<u32>(authored[i].y)) << "point " << i;
        EXPECT_EQ(std::bit_cast<u32>(deformed[i].z), std::bit_cast<u32>(authored[i].z)) << "point " << i;
    }
}

TEST(GroomDeformation, AnUnweightedVertexIsUsedUnskinnedAndCounted)
{
    // MeshSource pre-allocates one BoneInfluence per vertex, so an unweighted
    // vertex is the NORMAL state of a partially rigged mesh rather than an
    // exotic one. Collapsing it to the origin — which dividing by a zero weight
    // sum would do — would drag every strand rooted there to the model's pivot.
    GridSurface grid = MakeGrid(2u);
    // Deliberately weight nothing: every influence stays all-zero.
    const Bound bound = BindCoat(grid, 4u, 3u);
    ASSERT_TRUE(bound.Binding);

    const auto palette = BentPalette(90.0f);
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);

    EXPECT_GT(stats.VerticesUnweighted, 0u) << "an unweighted vertex must be counted";
    EXPECT_EQ(stats.RootsDeformed, bound.Groom->GetCurveCount());

    // Unskinned means the rest surface, so the coat is exactly where it was
    // authored despite a 90-degree palette.
    const auto deformed = DeformAllPoints(*bound.Groom, *bound.Binding, transforms);
    const auto& authored = bound.Groom->GetPoints();
    for (sizet i = 0; i < deformed.size(); ++i)
    {
        EXPECT_NEAR(deformed[i].y, authored[i].y, 1.0e-5f) << "point " << i;
    }
}

TEST(GroomDeformation, EvaluatingOnlyTheSelectedCurvesLeavesTheRestInvalid)
{
    // The budget path: the ribbon build strides over the groom, so only the
    // strands that will be drawn are deformed. Every OTHER entry must still be
    // addressable — the build indexes this array by curve — and must be
    // Valid == false so those strands are drawn at rest rather than at
    // whatever was in memory.
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 16u, 3u);
    ASSERT_TRUE(bound.Binding);

    GroomStrandBuildSettings build;
    build.MaxStrands = 4u;
    TArray<u32> selected;
    SelectGroomStrandCurves(*bound.Groom, build, selected);
    ASSERT_FALSE(selected.IsEmpty());
    ASSERT_LT(selected.Num(), bound.Groom->GetCurveCount());

    const auto palette = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::span<const u32>{ selected.GetData(), static_cast<sizet>(selected.Num()) }, transforms);

    EXPECT_EQ(transforms.Num(), bound.Groom->GetCurveCount()) << "the array must span the whole groom";
    EXPECT_EQ(stats.RootsDeformed, static_cast<u32>(selected.Num()));

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        const bool wasSelected = std::find(selected.begin(), selected.end(), curve) != selected.end();
        EXPECT_EQ(transforms[curve].Valid, wasSelected) << "curve " << curve;
    }
}

// ── The ribbon build, deformed ──────────────────────────────────────────────

TEST(GroomDeformation, TheRibbonBuildMovesItsVerticesAndItsBounds)
{
    // The seam between the deformation and the geometry: the build must APPLY
    // the transforms, and its reported bounds must be the deformed box rather
    // than the asset's bind-pose one. A culler handed the bind-pose box would
    // cull a raised limb's fur.
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    const auto bent = BentPalette(80.0f);
    const auto rest = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(bent, rest, true);
    inputs.HasHistory = true;

    GroomStrandBuildSettings build;
    TArray<u32> selected;
    SelectGroomStrandCurves(*bound.Groom, build, selected);
    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::span<const u32>{ selected.GetData(), static_cast<sizet>(selected.Num()) }, transforms);

    GroomStrandDeformation deformation;
    deformation.Binding = bound.Binding.Raw();
    deformation.RootTransforms = std::span{ transforms.GetData(), static_cast<sizet>(transforms.Num()) };
    ASSERT_TRUE(deformation.IsUsable(bound.Groom->GetCurveCount()));

    std::vector<GroomStrandVertex> restVertices;
    std::vector<GroomStrandVertex> bentVertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats restStats =
        BuildGroomStrandMesh(*bound.Groom, build, restVertices, indices, nullptr);
    const GroomStrandMeshStats bentStats =
        BuildGroomStrandMesh(*bound.Groom, build, bentVertices, indices, &deformation);

    ASSERT_EQ(restStats.VertexCount, bentStats.VertexCount)
        << "deforming a strand moves its points and must not change how many segments it has";
    ASSERT_TRUE(restStats.BoundsValid);
    ASSERT_TRUE(bentStats.BoundsValid);

    u32 moved = 0;
    for (sizet i = 0; i < restVertices.size(); ++i)
    {
        if (glm::length(bentVertices[i].Position - restVertices[i].Position) > 1.0e-3f)
        {
            ++moved;
        }
    }
    EXPECT_GT(moved, 0u) << "the deformation was accepted but changed nothing";

    // The deformed box must differ from the rest box, and must contain every
    // deformed vertex.
    EXPECT_NE(std::bit_cast<u32>(bentStats.BoundsMax.y), std::bit_cast<u32>(restStats.BoundsMax.y));
    for (const auto& vertex : bentVertices)
    {
        EXPECT_GE(vertex.Position.x, bentStats.BoundsMin.x - 1.0e-4f);
        EXPECT_LE(vertex.Position.x, bentStats.BoundsMax.x + 1.0e-4f);
        EXPECT_GE(vertex.Position.y, bentStats.BoundsMin.y - 1.0e-4f);
        EXPECT_LE(vertex.Position.y, bentStats.BoundsMax.y + 1.0e-4f);
        EXPECT_GE(vertex.Position.z, bentStats.BoundsMin.z - 1.0e-4f);
        EXPECT_LE(vertex.Position.z, bentStats.BoundsMax.z + 1.0e-4f);
    }

    // And the previous positions rode along: the previous palette is the REST
    // one here, so PrevPosition must differ from Position on the moving half.
    u32 withMotion = 0;
    for (const auto& vertex : bentVertices)
    {
        if (glm::length(vertex.PrevPosition - vertex.Position) > 1.0e-3f)
        {
            ++withMotion;
        }
    }
    EXPECT_GT(withMotion, 0u) << "a bent coat emitted no strand motion at all";
}

// ── Review round 2 (PR #1312): the regressions the review found ─────────────
//
// Each of these failed before the fix it is named for. They are grouped rather
// than scattered because they share one property worth stating once: every one
// of them is a case where the code was ALREADY checking something adjacent, and
// the adjacent check is what made the gap survive a first reading.

TEST(GroomDeformation, AnEmptySelectionDeformsNothingWhileNulloptDeformsEverything)
{
    // These are different requests and the implementation used to conflate
    // them. It matters because a selection really does come back empty --
    // GuidesOnly on a groom with no guides is the reachable case -- and reading
    // that as "all of them" turns the cheapest possible frame into the most
    // expensive one: every root of the coat evaluated, at full cost, to build
    // no geometry at all.
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 16u, 3u);
    ASSERT_TRUE(bound.Binding);

    const auto palette = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;

    const std::vector<u32> nothing;
    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats none =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::span<const u32>(nothing), transforms);
    EXPECT_EQ(none.RootsDeformed, 0u);
    EXPECT_EQ(transforms.Num(), bound.Groom->GetCurveCount()) << "still fully addressable";
    for (const auto& transform : transforms)
    {
        EXPECT_FALSE(transform.Valid);
    }

    const GroomDeformationStats all =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    EXPECT_EQ(all.RootsDeformed, bound.Groom->GetCurveCount());
}

TEST(GroomDeformation, TheSelectionStopsOnTheSameCurveTheBuildStopsOn)
{
    // The selection used to return every stride-selected curve, while the build
    // stops dead at the segment cap. Each surplus curve was then deformed -- a
    // root evaluation and a surface frame -- to build geometry that was never
    // emitted, and the binding preview drew a frame at a strand the viewport
    // did not contain.
    //
    // The case that shows it is a SMALL budget against LONG strands: the stride
    // is derived from the average segments per curve, so for a uniform coat it
    // is self-correcting and the surplus is at most the truncated last curve,
    // while a budget under one strand's length is where the two can diverge by
    // the whole groom.
    //
    // THE PREMISE MOVED IN #1251, and the movement is the reason to record it
    // here rather than quietly re-fit the numbers. The old selection only
    // widened the stride while at least one WHOLE curve was affordable and left
    // it at 1 below that, so this budget selected every one of the 20 curves and
    // built one of them. The per-role solver has no such floor -- it solves for
    // a retained fraction and turns it into a stride whatever the budget -- so
    // the same case now strides to 40 and names one curve. That is strictly
    // better, and the invariant under test is unchanged: the selection and the
    // build must name the same curves. Only the stride the premise asserts had
    // to move.
    GridSurface grid = MakeGrid(6u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 20u, 21u); // 20 curves of 20 segments
    ASSERT_TRUE(bound.Binding);

    GroomStrandBuildSettings build;
    build.MaxStrands = 64u;  // not the binding constraint here
    build.MaxSegments = 10u; // half of ONE strand
    TArray<u32> selected;
    SelectGroomStrandCurves(*bound.Groom, build, selected);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*bound.Groom, build, vertices, indices);
    ASSERT_TRUE(stats.SegmentBudgetLimited) << "this case is only interesting under a real budget";
    ASSERT_GT(stats.Stride, 1u) << "the premise: the budget is tight enough to have widened the stride";
    ASSERT_LT(selected.Num(), static_cast<sizet>(bound.Groom->GetCurveCount()))
        << "the premise: the budget excluded some curves";

    // Four vertices per emitted segment.
    EXPECT_EQ(static_cast<u32>(vertices.size() / 4u), stats.SegmentCount);
    EXPECT_EQ(stats.SegmentCount, build.MaxSegments);

    // The property, stated directly: every curve the selection names puts at
    // least one segment in the mesh, and no curve outside it puts any.
    // GroomSegmentIdentity is a one-way mix, so the question is asked forwards
    // -- each candidate identity is looked for -- rather than by decoding what
    // was built.
    std::set<u32> built;
    for (const auto& vertex : vertices)
    {
        built.insert(std::bit_cast<u32>(vertex.SegmentId));
    }
    const std::set<u32> named(selected.begin(), selected.end());
    ASSERT_LT(named.size(), bound.Groom->GetCurveCount()) << "the budget must have excluded something";

    for (u32 curve = 0; curve < bound.Groom->GetCurveCount(); ++curve)
    {
        const u32 points = bound.Groom->GetCurvePointCount(curve);
        const u32 segments = points > 1u ? points - 1u : 0u;
        u32 present = 0;
        for (u32 segment = 0; segment < segments; ++segment)
        {
            present += built.contains(GroomSegmentIdentity(curve, segment)) ? 1u : 0u;
        }
        if (named.contains(curve))
        {
            EXPECT_GT(present, 0u) << "curve " << curve << " was deformed but never built";
        }
        else
        {
            EXPECT_EQ(present, 0u) << "curve " << curve << " was built but never deformed";
        }
    }
}

TEST(GroomDeformation, TheEmittedBoundsContainTheRibbonAndNotOnlyItsCentreline)
{
    // GroomStrand.glsl expands every segment sideways by Radius, so a
    // centreline box is smaller than the thing drawn from it. A culler handed
    // it removes strands that are visibly on screen, and it does so worst at
    // grazing angles -- where the expansion is largest and a coat losing its
    // silhouette is most obvious.
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 16u, 4u);
    ASSERT_TRUE(bound.Binding);

    GroomStrandBuildSettings build;
    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*bound.Groom, build, vertices, indices);
    ASSERT_TRUE(stats.BoundsValid);
    ASSERT_FALSE(vertices.empty());

    f32 maxRadius = 0.0f;
    for (const auto& vertex : vertices)
    {
        maxRadius = std::max(maxRadius, vertex.Radius);
    }
    ASSERT_GT(maxRadius, 0.0f) << "a zero-radius groom cannot show this";

    // Every vertex, expanded by its OWN radius in every direction, is inside
    // the published box. That is the shape the shader can draw, so it is the
    // shape a culler has to be given.
    for (const auto& vertex : vertices)
    {
        const glm::vec3 expand{ vertex.Radius };
        const glm::vec3 low = vertex.Position - expand;
        const glm::vec3 high = vertex.Position + expand;
        EXPECT_GE(low.x, stats.BoundsMin.x - 1.0e-4f);
        EXPECT_GE(low.y, stats.BoundsMin.y - 1.0e-4f);
        EXPECT_GE(low.z, stats.BoundsMin.z - 1.0e-4f);
        EXPECT_LE(high.x, stats.BoundsMax.x + 1.0e-4f);
        EXPECT_LE(high.y, stats.BoundsMax.y + 1.0e-4f);
        EXPECT_LE(high.z, stats.BoundsMax.z + 1.0e-4f);
    }
}

TEST(GroomDeformation, ASkinnedSurfaceWithNoPreviousPoseReportsNoHistoryRatherThanClaimingIt)
{
    // HasHistory was echoed back from the INPUT, so a caller that offered
    // history to a skeleton that has none was told it was used. The evaluation
    // had already written prev == current for every root, so the frame carried
    // no motion -- and GroomRenderPass counts a groom with HasHistory false as
    // history-rejected, which is the number someone reads when a coat ghosts.
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 8u, 3u);
    ASSERT_TRUE(bound.Binding);

    const auto palette = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, false); // skinned, no previous pose
    inputs.HasHistory = true;

    TArray<GroomRootTransform> transforms;
    const GroomDeformationStats stats =
        EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    EXPECT_FALSE(stats.HasHistory) << "the caller offered history; the skeleton could not supply it";
    EXPECT_GT(stats.RootsDeformed, 0u) << "and the CURRENT pose still deformed normally";
}

TEST(GroomDeformation, ThePreviousPoseIsMappedWithThePreviousFrameMatrix)
{
    // Both halves of a previous position move: the body's pose and the two
    // world transforms that put the body and the groom where they were.
    // Skinning last frame's pose and then mapping it with THIS frame's matrix
    // mixes the two, and the error is the entity's own motion -- which is a
    // smeared coat on a walking character and nothing at all on a standing
    // one, so it survives every static test.
    GridSurface grid = MakeGrid(4u);
    WeightAllToBone0(grid);
    const Bound bound = BindCoat(grid, 8u, 3u);
    ASSERT_TRUE(bound.Binding);

    const auto palette = RestPalette();
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    // The POSE is identical in both frames, so every difference below is the
    // matrix and nothing else.
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;
    inputs.SurfaceToGroom = glm::mat4(1.0f);
    inputs.PrevSurfaceToGroom = glm::translate(glm::mat4(1.0f), glm::vec3{ 0.0f, 0.0f, -2.0f });

    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);

    u32 withMotion = 0;
    for (const auto& transform : transforms)
    {
        if (transform.Valid && glm::length(transform.Origin - transform.PrevOrigin) > 1.0e-3f)
        {
            ++withMotion;
        }
    }
    EXPECT_GT(withMotion, 0u) << "the body moved 2 units between frames and the coat emitted no motion";

    // And the control: the same matrix on both sides emits exactly zero, which
    // is what makes the assertion above about the MATRIX rather than about any
    // difference between the two code paths.
    inputs.PrevSurfaceToGroom = inputs.SurfaceToGroom;
    (void)EvaluateGroomRootTransforms(*bound.Groom, *bound.Binding, inputs, std::nullopt, transforms);
    for (const auto& transform : transforms)
    {
        if (transform.Valid)
        {
            EXPECT_EQ(std::bit_cast<u32>(transform.Origin.z), std::bit_cast<u32>(transform.PrevOrigin.z));
        }
    }
}
