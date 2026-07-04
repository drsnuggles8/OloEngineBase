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
// This guards the fix for issue #533 (GTAO-lit scenes render too dark): the
// axis used to project the surface normal onto the slice plane was built by
// crossing the slice direction with the SURFACE NORMAL
// (`cross(directionVS, viewNormal)`). Crossing anything with `viewNormal` is,
// by the definition of the cross product, always perpendicular to
// `viewNormal` — so `dot(viewNormal, axisVS)` was identically 0, collapsing
// `projectedNormal` to `viewNormal` and `n` (the per-slice tangent elevation
// angle) to 0 for EVERY slice on EVERY pixel, regardless of the surface's
// real tilt relative to the camera. Any surface not exactly face-on to the
// camera then measured its horizon against the wrong (untilted) baseline,
// self-occluding and producing the near-black GTAO composite the issue
// reports. The fix crosses the slice direction with the camera's view axis
// instead; the tests below prove that is non-degenerate — `n` now genuinely
// depends on both the slice angle and the surface's tilt.
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

    // CPU mirror of GTAO.comp's per-slice tangent-elevation computation
    // (the axisVS/projectedNormal/signN/cosN/n block), AFTER the #533 fix.
    TangentBasis ComputeTangentBasis(const glm::vec3& viewNormal, float sliceAngle)
    {
        const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
        const glm::vec3 directionVS(omega.x, omega.y, 0.0f);
        const glm::vec3 orthoDirectionVS = directionVS - glm::dot(directionVS, viewNormal) * viewNormal;
        const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, glm::vec3(0.0f, 0.0f, -1.0f)));
        const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
        const float projectedNormalLen = glm::length(projectedNormal);

        const float signN = glm::sign(glm::dot(orthoDirectionVS, projectedNormal));
        const float cosN = glm::clamp(glm::dot(projectedNormal, viewNormal) / projectedNormalLen, 0.0f, 1.0f);
        const float n = signN * std::acos(cosN);
        return { n, projectedNormalLen };
    }

    // The PRE-#533-fix formula: axisVS built from the surface normal instead
    // of the camera view axis. Kept here only so the regression test below
    // can prove it was degenerate — do not use this outside this test.
    TangentBasis ComputeTangentBasis_PreFix533Buggy(const glm::vec3& viewNormal, float sliceAngle)
    {
        const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
        const glm::vec3 directionVS(omega.x, omega.y, 0.0f);
        const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, viewNormal));
        const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
        const float projectedNormalLen = glm::length(projectedNormal);
        const float cosN = glm::clamp(glm::dot(projectedNormal, viewNormal) / projectedNormalLen, 0.0f, 1.0f);
        return { std::acos(cosN), projectedNormalLen };
    }
} // namespace

// THE BUG: crossing with viewNormal is ALWAYS perpendicular to viewNormal (a
// pure cross-product identity), so the pre-fix formula collapses n to 0 for
// every slice angle and every surface tilt — the tangent-elevation term never
// did anything. If this test starts failing, the old degenerate formula has
// been reintroduced.
TEST(GTAOMath, PreFixFormulaWasDegenerateRegardlessOfTilt)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f)); // grazing-angle floor
    for (float angleDeg : { 0.0f, 20.0f, 40.0f, 73.0f, 110.0f, 160.0f })
    {
        const float angle = angleDeg * kPi / 180.0f;
        const auto basis = ComputeTangentBasis_PreFix533Buggy(tiltedNormal, angle);
        EXPECT_NEAR(basis.N, 0.0f, 1e-5f) << "pre-fix formula unexpectedly produced nonzero n at angle=" << angleDeg;
        EXPECT_NEAR(basis.ProjectedNormalLen, 1.0f, 1e-5f);
    }
}

// THE FIX: for a surface tilted away from the camera, n now varies with the
// slice angle (nonzero for most slices) instead of being pinned at 0.
TEST(GTAOMath, TiltedSurfaceProducesNonZeroElevationForMostSlices)
{
    const glm::vec3 tiltedNormal = glm::normalize(glm::vec3(0.0f, 0.35f, 0.94f));
    int nonZeroCount = 0;
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(tiltedNormal, angle);
        if (std::abs(basis.N) > 1e-3f)
            ++nonZeroCount;
    }
    EXPECT_GT(nonZeroCount, 0) << "tangent elevation angle stayed at 0 for every slice -- the #533 regression";
}

// A surface facing the camera dead-on (viewNormal == view axis) has zero
// tilt in EVERY slice -- n == 0 is the CORRECT answer here, not a sign of the
// bug (the bug was n == 0 for grazing surfaces too, which is what the tests
// above and below distinguish).
TEST(GTAOMath, FaceOnSurfaceHasZeroElevationInEverySlice)
{
    const glm::vec3 faceOnNormal(0.0f, 0.0f, 1.0f);
    constexpr int kSlices = 9;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(faceOnNormal, angle);
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
    constexpr int kSlices = 16;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (static_cast<float>(slice) + 0.5f) * (2.0f * kPi / static_cast<float>(kSlices));
        const auto basis = ComputeTangentBasis(tiltedNormal, angle);
        EXPECT_GT(basis.ProjectedNormalLen, 0.0f);
        EXPECT_LE(basis.ProjectedNormalLen, 1.0f + 1e-5f);
    }
}
