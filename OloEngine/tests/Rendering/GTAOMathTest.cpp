#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>

// =============================================================================
// GTAO — CPU contract tests.
//
// OLO_TEST_LAYER: shaderpipe
//
// Pins the per-slice tangent-elevation-angle ("n") math implemented in
// GTAO.comp (XeGTAO-style horizon-based AO) WITHOUT a GL context (so this
// runs in headless CI — the GPU-dependent check is GTAOVisualEvidenceTest).
// Per the CLAUDE.md rendering rule, math/contract tests prove the formula;
// the visual test proves the frame looks right.
//
// This guards the fix for issue #533 (GTAO-lit scenes render too dark), which
// had TWO layered bugs in the same few lines:
//
//   1. axisVS was built as `cross(directionVS, viewNormal)`. Crossing
//      anything with `viewNormal` is, by definition of the cross product,
//      always perpendicular to `viewNormal` — so `dot(viewNormal, axisVS)`
//      was identically 0, collapsing `projectedNormal` to `viewNormal` and
//      `n` to exactly 0 for EVERY slice on EVERY pixel, regardless of the
//      surface's real tilt. Any surface not exactly face-on to the camera
//      then measured its horizon against the wrong (untilted) baseline and
//      self-occluded — the near-black composite the issue reports.
//   2. Basing axisVS on the camera's view axis instead fixed that, but
//      `cosN` still dotted `projectedNormal` against `viewNormal` — and for
//      an orthogonal projection `P = V - axis*dot(V,axis)`,
//      `dot(P, V) == |P|^2` is a plain linear-algebra identity, so that
//      `cosN` always reduced back to `projectedNormalLen`: non-degenerate,
//      but still not the intended "elevation relative to the slice plane's
//      reference axis" quantity — a quieter, second self-occlusion source
//      that left surfaces away from dead-on darker than they should be and
//      washed out real, localised occlusion (e.g. a contact crease).
//
// Both are fixed by using the per-pixel view vector
// (`viewVec = normalize(-pixCenterPos)`, i.e. from the shaded point toward
// the camera) — not `viewNormal`, not a globally-fixed screen axis — for
// `orthoDirectionVS`, `axisVS`, and `cosN`'s dot target.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    constexpr float kPi = 3.14159265358979f;

    struct TangentBasis
    {
        float N = 0.0f;
        float ProjectedNormalLen = 0.0f;
    };

    // CPU mirror of GTAO.comp's per-slice tangent-elevation computation (the
    // orthoDirectionVS/axisVS/projectedNormal/signN/cosN/n block), current
    // (fully-fixed) form: everything measured relative to the per-pixel
    // view vector.
    TangentBasis ComputeTangentBasis(const glm::vec3& viewNormal, const glm::vec3& viewVec, float sliceAngle)
    {
        const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
        const glm::vec3 directionVS(omega.x, omega.y, 0.0f);
        const glm::vec3 orthoDirectionVS = directionVS - glm::dot(directionVS, viewVec) * viewVec;
        const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, viewVec));
        const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
        const float projectedNormalLen = glm::length(projectedNormal);

        const float signN = glm::sign(glm::dot(orthoDirectionVS, projectedNormal));
        const float cosN = glm::clamp(glm::dot(projectedNormal, viewVec) / projectedNormalLen, 0.0f, 1.0f);
        const float n = signN * std::acos(cosN);
        return { n, projectedNormalLen };
    }

    // Bug #1 (pre-#533-fix): axisVS built from the surface normal instead of
    // a view-related axis. Kept only so the regression test below can prove
    // it was degenerate — do not use this outside this test.
    TangentBasis ComputeTangentBasis_Bug1_AxisFromNormal(const glm::vec3& viewNormal, float sliceAngle)
    {
        const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
        const glm::vec3 directionVS(omega.x, omega.y, 0.0f);
        const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, viewNormal));
        const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
        const float projectedNormalLen = glm::length(projectedNormal);
        const float cosN = glm::clamp(glm::dot(projectedNormal, viewNormal) / projectedNormalLen, 0.0f, 1.0f);
        return { std::acos(cosN), projectedNormalLen };
    }

    // Bug #2 (intermediate, still-wrong fix): axisVS correctly built from a
    // view axis, but cosN still dots against viewNormal instead of the view
    // vector — a linear-algebra identity then forces cosN == projectedNormalLen
    // always (non-degenerate, but not the intended quantity). Kept only so
    // the regression test below can prove it. Uses the fixed screen axis
    // (0,0,-1) for axisVS to match the exact intermediate state that shipped.
    TangentBasis ComputeTangentBasis_Bug2_CosNDotsNormal(const glm::vec3& viewNormal, float sliceAngle)
    {
        const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
        const glm::vec3 directionVS(omega.x, omega.y, 0.0f);
        const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, glm::vec3(0.0f, 0.0f, -1.0f)));
        const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
        const float projectedNormalLen = glm::length(projectedNormal);
        const float cosN = glm::clamp(glm::dot(projectedNormal, viewNormal) / projectedNormalLen, 0.0f, 1.0f);
        return { std::acos(cosN), projectedNormalLen };
    }
} // namespace

// BUG #1: crossing with viewNormal is ALWAYS perpendicular to viewNormal (a
// pure cross-product identity), so this formula collapses n to 0 for every
// slice angle and every surface tilt — the tangent-elevation term never did
// anything. If this test starts failing, the old degenerate formula has been
// reintroduced.
TEST(GTAOMath, Bug1FormulaWasDegenerateRegardlessOfTilt)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f)); // grazing-angle floor
    for (float angleDeg : { 0.0f, 20.0f, 40.0f, 73.0f, 110.0f, 160.0f })
    {
        const float angle = angleDeg * kPi / 180.0f;
        const auto basis = ComputeTangentBasis_Bug1_AxisFromNormal(tiltedNormal, angle);
        EXPECT_NEAR(basis.N, 0.0f, 1e-5f) << "bug#1 formula unexpectedly produced nonzero n at angle=" << angleDeg;
        EXPECT_NEAR(basis.ProjectedNormalLen, 1.0f, 1e-5f);
    }
}

// BUG #2: dot(projectedNormal, viewNormal) / projectedNormalLen is a linear-
// algebra identity that always equals projectedNormalLen (a projection P onto
// a plane satisfies dot(P, V) == |P|^2 for the projected vector V). So this
// intermediate formula's cosN — and therefore n — collapses to a value that
// only depends on projectedNormalLen, never on the true elevation relative to
// the slice-plane reference axis. If this test starts failing (i.e. cosN
// stops tracking projectedNormalLen), the still-buggy dot target may have
// been reintroduced in a way that no longer reproduces the identity — but the
// real guard is GTAOMath.CosNIsNotTautologicallyProjectedNormalLen below,
// which proves the CURRENT formula escapes this identity.
TEST(GTAOMath, Bug2FormulaCollapsesCosNToProjectedNormalLen)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f));
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis_Bug2_CosNDotsNormal(tiltedNormal, angle);
        const float cosN = std::cos(std::abs(basis.N)); // n = signN * acos(cosN) -> recover cosN
        EXPECT_NEAR(cosN, basis.ProjectedNormalLen, 1e-4f)
            << "bug#2 formula's cosN no longer tracks projectedNormalLen — the identity this test documents no "
               "longer holds for the intermediate formula (informational; the real guard is the test below)";
    }
}

// THE FULL FIX: cosN (dotted against the per-pixel view vector, not
// viewNormal) is NOT tautologically equal to projectedNormalLen — it is a
// genuinely different, slice-varying quantity. This is what bug #2 broke.
TEST(GTAOMath, CosNIsNotTautologicallyProjectedNormalLen)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f));
    const glm::vec3 viewVec(0.0f, 0.0f, 1.0f); // camera-facing pixel: viewVec ~= fixed axis here
    bool foundDivergence = false;
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(tiltedNormal, viewVec, angle);
        const float cosN = std::cos(std::abs(basis.N));
        if (std::abs(cosN - basis.ProjectedNormalLen) > 1e-3f)
            foundDivergence = true;
    }
    EXPECT_TRUE(foundDivergence)
        << "cosN tracked projectedNormalLen across every slice -- the #533 bug#2 tautology regression";
}

// THE FIX: for a surface tilted away from the camera, n now varies with the
// slice angle (nonzero for most slices) instead of being pinned at 0.
TEST(GTAOMath, TiltedSurfaceProducesNonZeroElevationForMostSlices)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f));
    const glm::vec3 viewVec(0.0f, 0.0f, 1.0f);
    int nonZeroCount = 0;
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(tiltedNormal, viewVec, angle);
        if (std::abs(basis.N) > 1e-3f)
            ++nonZeroCount;
    }
    EXPECT_GT(nonZeroCount, 0) << "tangent elevation angle stayed at 0 for every slice -- the #533 regression";
}

// A surface facing the camera dead-on (viewNormal == viewVec) has zero tilt
// in EVERY slice -- n == 0 is the CORRECT answer here, not a sign of the bug
// (the bug was n == 0 for grazing surfaces too, which is what the tests above
// and below distinguish).
TEST(GTAOMath, FaceOnSurfaceHasZeroElevationInEverySlice)
{
    const glm::vec3 faceOnNormal(0.0f, 0.0f, 1.0f);
    const glm::vec3 viewVec(0.0f, 0.0f, 1.0f);
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(faceOnNormal, viewVec, angle);
        EXPECT_NEAR(basis.N, 0.0f, 1e-4f);
        EXPECT_NEAR(basis.ProjectedNormalLen, 1.0f, 1e-4f);
    }
}

// projectedNormalLen must stay in (0, 1] -- it weights each slice's
// contribution in the final visibility sum (GTAO.comp's
// `localVisibility * projectedNormalLen`), so a value outside this range
// would over/under-weight a slice.
TEST(GTAOMath, ProjectedNormalLengthStaysInUnitRange)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.2f, 0.5f, 0.85f));
    const glm::vec3 viewVec(0.0f, 0.0f, 1.0f);
    constexpr int kSlices = 16;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (2.0f * kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(tiltedNormal, viewVec, angle);
        EXPECT_GT(basis.ProjectedNormalLen, 0.0f);
        EXPECT_LE(basis.ProjectedNormalLen, 1.0f + 1e-5f);
    }
}

// Off-centre pixels have a viewVec that genuinely diverges from a fixed
// screen axis under perspective -- using the per-pixel viewVec must produce a
// different tangent basis than a fixed (0,0,-1) axis would, proving the fix
// is not just cosmetically different but numerically load-bearing away from
// screen centre.
TEST(GTAOMath, OffCentreViewVecDivergesFromFixedAxis)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f));
    const glm::vec3 fixedAxis(0.0f, 0.0f, 1.0f);
    // A pixel well off screen centre: view-space position with large XY
    // relative to depth, so normalize(-pixCenterPos) diverges from fixedAxis.
    const glm::vec3 pixCenterPos(8.0f, 4.0f, -20.0f);
    const glm::vec3 viewVec = glm::normalize(-pixCenterPos);

    bool foundDivergence = false;
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto onAxis = ComputeTangentBasis(tiltedNormal, fixedAxis, angle);
        const auto offAxis = ComputeTangentBasis(tiltedNormal, viewVec, angle);
        if (std::abs(onAxis.N - offAxis.N) > 1e-3f || std::abs(onAxis.ProjectedNormalLen - offAxis.ProjectedNormalLen) > 1e-3f)
            foundDivergence = true;
    }
    EXPECT_TRUE(foundDivergence)
        << "per-pixel viewVec produced the same basis as a fixed screen axis for an off-centre pixel -- the "
           "perspective-tracking fix has no effect";
}
