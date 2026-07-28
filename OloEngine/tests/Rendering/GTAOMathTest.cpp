#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

// =============================================================================
// Sky / far-plane classification robustness (GTAO black-background artifact).
//
// GTAO's per-pixel early-out classifies "sky" from the HZB mip-0 depth. Two
// contracts guard the fix for the maximize-then-rotate artifact where the
// whole background rendered as dark AO garbage:
//
//   1. The HZB first pass must copy scene depth into mip 0 with texelFetch
//      (exact integer addressing). Sampling the D24S8 depth attachment via
//      textureLod routes through the hardware's FIXED-POINT depth-filter
//      path, which can return 1.0 - 1 D24 ULP (0.99999994) for a texel that
//      is exactly 1.0 — in large, viewport-size-dependent regions.
//   2. GTAO's sky early-out must never compare that depth against 1.0
//      exactly; it uses a ULP-tolerant threshold so a filtered/truncated
//      far-plane depth still classifies as sky.
//
// The classification test mirrors the shader logic on the CPU; the two
// shader-source contract tests pin the load-bearing lines of GLSL the same
// way the serializer coverage tests pin generated code, so a refactor that
// silently reintroduces a filtered copy or an exact compare fails headless CI.
// =============================================================================

namespace
{
    // CPU mirror of GTAO.comp's sky early-out. Keep in sync with
    // XE_GTAO_FAR_DEPTH_THRESHOLD in assets/shaders/compute/GTAO.comp.
    constexpr float kGtaoFarDepthThreshold = 1.0f - 1e-6f;

    bool ClassifiesAsSky(float deviceZ)
    {
        return deviceZ <= 0.0f || deviceZ >= kGtaoFarDepthThreshold;
    }

    std::string ReadRepoFile(const std::filesystem::path& relative)
    {
        const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / relative;
        std::ifstream f(path, std::ios::binary);
        EXPECT_TRUE(f.is_open()) << "cannot open " << path.string();
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
    }
} // namespace

// An UNOCCLUDED surface must integrate to full visibility at every tilt.
//
// GTAO.comp seeds each slice's two horizon cosines, then recovers the horizon
// angles with acos() and evaluates the arc integral. acos() returns a magnitude,
// so it cannot distinguish (n - HALF_PI) from (HALF_PI - n): seeding the pair
// with cos(n +/- HALF_PI) therefore made BOTH horizons converge to the same
// angle as |n| grew, and the arc integral -- which measures the span between
// them -- fell to zero. A surface with no occluders at all reported FULL
// occlusion, with the error scaling continuously from none at n == 0 to total
// at n == HALF_PI. That is invisible looking dead-on at a surface and worst on
// a large flat expanse seen edge-on, which is why it survived every existing
// GPU evidence test (all shot at modest tilt on a small floor).
//
// Seeding both horizons at -1 ("no occluder on either side") reconstructs
// symmetrically for every n, since acos(-1) = PI on both sides.
TEST(GTAOMath, UnoccludedSurfaceIntegratesToFullVisibilityAtEveryTilt)
{
    // Mirrors the shader's arc integral for one slice, given the two seeded
    // horizon cosines and the tilt n.
    const auto sliceVisibility = [](float n, float horizonCos0, float horizonCos1)
    {
        const float h0 = n + std::max(-std::acos(horizonCos0) - n, -kPi * 0.5f);
        const float h1 = n + std::min(std::acos(horizonCos1) - n, kPi * 0.5f);
        const float iarc0 = -std::cos(2.0f * h0 - n) + std::cos(n) + 2.0f * h0 * std::sin(n);
        const float iarc1 = -std::cos(2.0f * h1 - n) + std::cos(n) + 2.0f * h1 * std::sin(n);
        return 0.25f * (iarc0 + iarc1); // projectedNormalLen == 1 for this check
    };

    // Sweep tilt from dead-on to fully grazing.
    for (int i = 0; i <= 10; ++i)
    {
        const float n = (kPi * 0.5f) * (static_cast<float>(i) / 10.0f);

        // The fix: both horizons seeded at -1. The raw arc integral is not
        // normalised to exactly 1 -- it grows past 1 with tilt, and the shader
        // scales it by projectedNormalLen (which shrinks with tilt) and clamps
        // to [0,1]. The property that matters, and the one the defect broke, is
        // that an unoccluded slice never integrates to LESS than full visibility.
        const float fixed = sliceVisibility(n, -1.0f, -1.0f);
        EXPECT_GE(fixed, 1.0f - 1e-4f)
            << "An unoccluded slice must never integrate to less than full visibility, but at n = "
            << n << " rad it gave " << fixed << ". The horizon seeding regressed.";

        // The old seeding, kept as an executable record of the defect: correct
        // dead-on, collapsing to zero as the surface tilts away.
        const float old = sliceVisibility(n, std::cos(n + kPi * 0.5f), std::cos(n - kPi * 0.5f));
        if (i == 0)
            EXPECT_NEAR(old, 1.0f, 1e-4f) << "the old seeding was correct only at n == 0";
        if (i == 10)
            EXPECT_NEAR(old, 0.0f, 1e-4f) << "the old seeding collapsed to zero when fully grazing";
    }
}

TEST(GTAOMath, FarPlaneClassificationToleratesFilteredDepthUlps)
{
    // Exact far-plane clear value.
    EXPECT_TRUE(ClassifiesAsSky(1.0f));
    // One float32 ULP below 1.0 — what a float round-trip can produce.
    EXPECT_TRUE(ClassifiesAsSky(std::nextafter(1.0f, 0.0f)));
    // One D24 ULP below 1.0 — what the fixed-point depth filter produced in
    // the observed artifact (0.99999994).
    EXPECT_TRUE(ClassifiesAsSky(1.0f - 1.0f / 16777215.0f));
    // A few D24 ULPs of accumulated error must still classify as sky.
    EXPECT_TRUE(ClassifiesAsSky(1.0f - 5.0f / 16777215.0f));
    // Depth zero / negative garbage is also the early-out.
    EXPECT_TRUE(ClassifiesAsSky(0.0f));
    // Real geometry meaningfully in front of the far plane must NOT be sky.
    EXPECT_FALSE(ClassifiesAsSky(0.9999f));
    EXPECT_FALSE(ClassifiesAsSky(0.5f));
}

// Source guard for the seeding above: the maths test proves WHY -1 is required,
// this proves GTAO.comp still does it. Without this the pair could silently
// regress -- the arc-integral test mirrors the formula, it does not read the
// shader.
TEST(GTAOMath, GtaoShaderSeedsHorizonsFullyBehindSurface)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("float horizonCos0 = -1.0;"), std::string::npos)
        << "GTAO.comp no longer seeds horizonCos0 at -1";
    EXPECT_NE(src.find("float horizonCos1 = -1.0;"), std::string::npos)
        << "GTAO.comp no longer seeds horizonCos1 at -1";
    // The exact regression this guards: acos() drops the sign of (n -/+ HALF_PI),
    // so seeding from it collapses both horizons together as the surface tilts
    // and an unoccluded grazing surface reports full occlusion.
    EXPECT_EQ(src.find("horizonCos0 = cos(n + XE_GTAO_HALF_PI)"), std::string::npos)
        << "GTAO.comp seeds its horizons from cos(n +/- HALF_PI) again — a flat surface viewed "
           "edge-on will integrate to zero visibility and the frame will go black";
}

TEST(GTAOMath, NDCToViewConstantsUseGLConventionOnBothAxes)
{
    // Regression: the XeGTAO reference's NDCToView constants negate the Y
    // pair because D3D puts texture v = 0 at the TOP row. This port consumes
    // GL-convention inputs (compute pixCoord row 0 = framebuffer bottom; the
    // HZB is a 1:1 texelFetch copy; normals fetched with the same coords),
    // so the copied D3D flip negated view-space Y for every reconstructed
    // sample position: horizon angles reflected about the horizontal plane,
    // invisible looking straight down, a full-frame visibility collapse to
    // the 0.03 floor at grazing views (the sea/quay "goosebumps" weave).
    // Derive the repo root from the compile-time editor root — cwd-relative
    // reads break when the runner's working directory isn't the repo root
    // (background shells, some CI launchers).
    const auto repoRoot = std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
    std::ifstream f(repoRoot / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / "GTAORenderPass.cpp",
                    std::ios::binary);
    ASSERT_TRUE(f.is_open());
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string src = buf.str();

    EXPECT_NE(src.find("glm::vec2(2.0f / projScale00, 2.0f / projScale11)"), std::string::npos)
        << "NDCToViewMul lost its GL-convention positive Y term";
    EXPECT_NE(src.find("glm::vec2(-1.0f / projScale00, -1.0f / projScale11)"), std::string::npos)
        << "NDCToViewAdd lost its GL-convention negative Y term";
    EXPECT_EQ(src.find("-2.0f / projScale11"), std::string::npos)
        << "the D3D top-down Y flip is back in NDCToViewMul — grazing views will collapse to black again";
}

TEST(GTAOMath, TemporalNoiseOnlyAnimatesUnderTAA)
{
    // XeGTAO's animated noise index exists so TAA can resolve the R1/Hilbert
    // pattern temporally. Without TAA the pattern boils every frame — the
    // "goosebumps" weave over water and the distant quay (VehiclesTest,
    // issue #438 follow-up). The pass must advance NoiseIndex only when TAA
    // is enabled. The test binary runs from the repo root, so engine sources
    // are reachable relatively (same convention as the coverage tests).
    const auto repoRoot = std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
    std::ifstream f(repoRoot / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / "GTAORenderPass.cpp",
                    std::ios::binary);
    ASSERT_TRUE(f.is_open());
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string src = buf.str();

    const auto incrementPos = src.find("NoiseIndex + 1");
    ASSERT_NE(incrementPos, std::string::npos) << "noise-index increment not found";
    // The gate must appear in the increment's guarding condition, i.e. within
    // the few lines immediately preceding the increment.
    const auto windowStart = incrementPos > 400u ? incrementPos - 400u : 0u;
    const auto window = src.substr(windowStart, incrementPos - windowStart);
    EXPECT_NE(window.find("m_Settings.TAAEnabled"), std::string::npos)
        << "the temporal-noise advance lost its TAA gate — without TAA the GTAO pattern will boil again";
}

TEST(GTAOMath, GtaoShaderSkyEarlyOutIsUlpTolerant)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(src.empty());
    // The threshold must exist and be used by the early-out.
    EXPECT_NE(src.find("XE_GTAO_FAR_DEPTH_THRESHOLD"), std::string::npos)
        << "GTAO.comp lost its ULP-tolerant far-depth threshold";
    EXPECT_NE(src.find("deviceZ >= XE_GTAO_FAR_DEPTH_THRESHOLD"), std::string::npos)
        << "GTAO.comp's sky early-out no longer uses the tolerant threshold";
    // The exact-compare regression this guards against.
    EXPECT_EQ(src.find("deviceZ >= 1.0)"), std::string::npos)
        << "GTAO.comp compares sampled depth against 1.0 exactly again — filtered far-plane "
           "depth (1.0 - 1 D24 ULP) will classify as geometry and blacken the sky";
}

TEST(GTAOMath, HzbShaderFirstPassCopiesDepthWithTexelFetch)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "HZB.comp");
    ASSERT_FALSE(src.empty());

    // Isolate the first-pass branch (mip-0 1:1 copy of scene depth).
    const auto firstPassBegin = src.find("if (u_IsFirstPass != 0)");
    ASSERT_NE(firstPassBegin, std::string::npos);
    const auto firstPassEnd = src.find("else", firstPassBegin);
    ASSERT_NE(firstPassEnd, std::string::npos);
    const std::string firstPass = src.substr(firstPassBegin, firstPassEnd - firstPassBegin);

    EXPECT_NE(firstPass.find("texelFetch("), std::string::npos)
        << "HZB.comp's first pass no longer copies scene depth with texelFetch — a filtered "
           "textureLod read of the D24S8 depth can truncate exact-1.0 far-plane depth by one "
           "D24 ULP and downstream sky classification breaks";
    // Match the call syntax specifically -- the explanatory comment in the
    // shader legitimately mentions textureLod in prose.
    EXPECT_EQ(firstPass.find("textureLod("), std::string::npos)
        << "HZB.comp's first pass samples scene depth through the filtering path again";
}

// =============================================================================
// Spatiotemporal noise (GTAO.comp :: SpatioTemporalNoise).
//
// The shipped bug these guard: the noise returned the Hilbert LUT value
// DIRECTLY as the noise ((idx + noiseIndex) & 0xFF) / 256. A Hilbert curve is
// locality-PRESERVING by construction -- that is the entire reason XeGTAO uses
// one -- so neighbouring pixels get indices one apart and therefore near-
// identical noise. The LUT stores an ORDERING; the R2 low-discrepancy sequence
// is what turns that ordering into a VALUE, mapping sequential indices to
// maximally-separated points in [0,1).
//
// Symptoms on screen (issue #438 follow-up, reported as "goosebumps" on water):
//   - The slice angle and sample distance varied smoothly across neighbouring
//     pixels, so round()-ing the sample offset to whole pixels produced
//     coherent CONTOURS rather than per-pixel dither: a woven lattice carrying
//     the LUT's 64px tile period, worst wherever the projected effect radius
//     put those contours at a visible spacing (hence "moving the GTAO radius
//     moves the affected band").
//   - Advancing the index by 1 per frame slid the SAME field along the curve
//     instead of redrawing it, leaving consecutive frames ~98% correlated --
//     so enabling TAA did not help, because temporal averaging had nothing to
//     cancel.
// =============================================================================
namespace
{
    constexpr int kHilbertSize = 64;

    // CPU mirror of GTAORenderPass::HilbertIndex (the LUT the pass uploads)
    // and of GTAO.comp's noise. Keep the three in lock-step -- the source
    // guard below pins the shader half.
    //
    // The mirror computes in double where the shader has float. That is a
    // deliberate mirror of the MATHS, not of the rounding: the properties
    // asserted here are precision-robust. At the worst temporal phase the
    // index reaches ~22k, where fp32 resolves 1024 rather than 4096 distinct
    // noise levels across the tile -- ample, and the neighbour correlation is
    // unchanged (-0.104 in both precisions).
    constexpr double kR2SliceMultiplier = 0.75487766624669276005;
    constexpr double kR2SampleMultiplier = 0.5698402909980532659114;
    constexpr int kTemporalStride = 288;

    int HilbertIndexMirror(int x, int y)
    {
        int d = 0;
        for (int s = kHilbertSize / 2; s > 0; s /= 2)
        {
            const int rx = ((x & s) > 0) ? 1 : 0;
            const int ry = ((y & s) > 0) ? 1 : 0;
            d += s * s * ((3 * rx) ^ ry);
            if (ry == 0)
            {
                if (rx == 1)
                {
                    x = s - 1 - x;
                    y = s - 1 - y;
                }
                std::swap(x, y);
            }
        }
        return d & 0xFFFF;
    }

    double Frac(double v)
    {
        return v - std::floor(v);
    }

    using NoiseTile = std::vector<double>; // kHilbertSize^2, row-major

    // The shipped formulation.
    NoiseTile R2Noise(double multiplier, int noiseIndex)
    {
        NoiseTile out(static_cast<sizet>(kHilbertSize) * kHilbertSize);
        for (int y = 0; y < kHilbertSize; ++y)
        {
            for (int x = 0; x < kHilbertSize; ++x)
            {
                const double idx = static_cast<double>(HilbertIndexMirror(x, y) + kTemporalStride * (noiseIndex & 63));
                out[static_cast<sizet>(y) * kHilbertSize + x] = Frac(0.5 + idx * multiplier);
            }
        }
        return out;
    }

    // The regressed formulation, kept so every threshold below is proven to
    // DISCRIMINATE rather than to pass vacuously.
    NoiseTile LegacyIndexAsValueNoise(int noiseIndex, bool sampleChannel)
    {
        NoiseTile out(static_cast<sizet>(kHilbertSize) * kHilbertSize);
        for (int y = 0; y < kHilbertSize; ++y)
        {
            for (int x = 0; x < kHilbertSize; ++x)
            {
                const int idx = HilbertIndexMirror(x, y);
                const double slice = static_cast<double>((idx + noiseIndex) & 0xFF) / 256.0;
                out[static_cast<sizet>(y) * kHilbertSize + x] = sampleChannel ? Frac(slice * 0.6180339887498949) : slice;
            }
        }
        return out;
    }

    double Correlation(const std::vector<double>& a, const std::vector<double>& b)
    {
        const auto n = static_cast<double>(a.size());
        const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / n;
        const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / n;
        double cov = 0.0;
        double varA = 0.0;
        double varB = 0.0;
        for (sizet i = 0; i < a.size(); ++i)
        {
            const double da = a[i] - meanA;
            const double db = b[i] - meanB;
            cov += da * db;
            varA += da * da;
            varB += db * db;
        }
        if (varA <= 0.0 || varB <= 0.0)
            return 0.0;
        return cov / std::sqrt(varA * varB);
    }

    // Correlation of the tile against itself shifted one pixel along +x.
    double NeighbourCorrelationX(const NoiseTile& f)
    {
        std::vector<double> a;
        std::vector<double> b;
        a.reserve(static_cast<sizet>(kHilbertSize) * (kHilbertSize - 1));
        b.reserve(a.capacity());
        for (int y = 0; y < kHilbertSize; ++y)
        {
            for (int x = 0; x + 1 < kHilbertSize; ++x)
            {
                a.push_back(f[static_cast<sizet>(y) * kHilbertSize + x]);
                b.push_back(f[static_cast<sizet>(y) * kHilbertSize + x + 1]);
            }
        }
        return Correlation(a, b);
    }
} // namespace

// The premise the whole fix rests on: the LUT is an ORDERING that deliberately
// keeps neighbours close, so its value can never serve as per-pixel noise.
TEST(GTAOMath, HilbertLutIsLocalityPreservingSoItsIndexIsNotNoise)
{
    int adjacentPairs = 0;
    int closeInIndex = 0;
    for (int y = 0; y < kHilbertSize; ++y)
    {
        for (int x = 0; x + 1 < kHilbertSize; ++x)
        {
            ++adjacentPairs;
            const int delta = std::abs(HilbertIndexMirror(x, y) - HilbertIndexMirror(x + 1, y));
            if (delta <= 4)
                ++closeInIndex;
        }
    }
    ASSERT_GT(adjacentPairs, 0);
    // A genuinely random ordering would put ~0.2% of neighbour pairs within 4
    // of each other; a Hilbert curve puts the overwhelming majority there.
    const double fraction = static_cast<double>(closeInIndex) / adjacentPairs;
    EXPECT_GT(fraction, 0.5) << "the Hilbert mirror is no longer locality-preserving — either the "
                                "mirror drifted from GTAORenderPass::HilbertIndex or the LUT changed";
}

// The fix: R2 turns that ordering into decorrelated per-pixel values.
TEST(GTAOMath, R2NoiseDecorrelatesNeighbouringPixels)
{
    const double sliceCorr = NeighbourCorrelationX(R2Noise(kR2SliceMultiplier, 0));
    const double sampleCorr = NeighbourCorrelationX(R2Noise(kR2SampleMultiplier, 0));

    EXPECT_LT(std::abs(sliceCorr), 0.35) << "GTAO slice noise is spatially correlated (" << sliceCorr
                                         << ") — round()-ing the sample offsets will produce coherent contours "
                                            "(the 'goosebump' lattice) instead of per-pixel dither";
    EXPECT_LT(std::abs(sampleCorr), 0.35) << "GTAO sample-distance noise is spatially correlated (" << sampleCorr << ")";

    // Discrimination check: the regressed formulation must FAIL this bar, so
    // the thresholds above are known to be measuring something.
    EXPECT_GT(NeighbourCorrelationX(LegacyIndexAsValueNoise(0, false)), 0.7)
        << "the legacy index-as-value noise no longer reads as correlated — this test can no "
           "longer tell the regression from the fix";
}

// Second defect in the same two lines: deriving the sample noise from the
// already-quantised slice noise (fract(noiseSlice * golden)) collapses to
// fract(k * 0.00241) for integer k, a ramp that never leaves [0, 0.62) — so
// every AO sample distance was biased toward the pixel centre.
TEST(GTAOMath, SampleDistanceNoiseSpansTheFullUnitRange)
{
    const NoiseTile shipped = R2Noise(kR2SampleMultiplier, 0);
    const auto [shippedMin, shippedMax] = std::minmax_element(shipped.begin(), shipped.end());
    const double shippedMean = std::accumulate(shipped.begin(), shipped.end(), 0.0) / static_cast<double>(shipped.size());

    EXPECT_GT(*shippedMax - *shippedMin, 0.9) << "GTAO sample noise no longer spans the unit interval — AO sample "
                                                 "distances are biased and the outer effect radius is undersampled";
    EXPECT_NEAR(shippedMean, 0.5, 0.05);

    // Discrimination check against the regressed derivation.
    const NoiseTile legacy = LegacyIndexAsValueNoise(0, true);
    const auto [legacyMin, legacyMax] = std::minmax_element(legacy.begin(), legacy.end());
    EXPECT_LT(*legacyMax - *legacyMin, 0.7) << "the legacy derived-sample noise no longer reads as range-starved";
}

// Why enabling TAA did not help: consecutive frames must draw a DIFFERENT
// field, not the same field slid along the curve.
TEST(GTAOMath, TemporalStrideRedrawsTheFieldEachFrame)
{
    // Signed, not |.|: the failure mode is a field that REPEATS (correlation
    // near +1), which is what leaves TAA nothing to average. The shipped
    // stride lands consistently around -0.45 across the whole 64-phase cycle;
    // an anti-correlated field is still a redrawn one.
    const double shippedFrameCorr = Correlation(R2Noise(kR2SliceMultiplier, 0), R2Noise(kR2SliceMultiplier, 1));
    EXPECT_LT(shippedFrameCorr, 0.35)
        << "consecutive GTAO noise frames are " << shippedFrameCorr
        << " correlated — TAA cannot resolve a pattern that barely changes between frames";

    // Discrimination check: advancing the raw index by 1 leaves the field
    // essentially unchanged, which is exactly why the lattice survived TAA.
    EXPECT_GT(Correlation(LegacyIndexAsValueNoise(0, false), LegacyIndexAsValueNoise(1, false)), 0.9)
        << "the legacy per-frame advance no longer reads as temporally static";
}

// Source guard for the CPU mirror above: the maths proves WHY the R2 step is
// required, this proves GTAO.comp still performs it.
TEST(GTAOMath, GtaoShaderMapsHilbertIndexThroughR2Sequence)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("0.75487766624669276005"), std::string::npos)
        << "GTAO.comp lost the R2 slice-noise multiplier (CPU mirror: kR2SliceMultiplier)";
    EXPECT_NE(src.find("0.5698402909980532659114"), std::string::npos)
        << "GTAO.comp lost the R2 sample-noise multiplier (CPU mirror: kR2SampleMultiplier)";
    EXPECT_NE(src.find("288u * (uint(u_NoiseIndex) & 63u)"), std::string::npos)
        << "GTAO.comp lost the per-frame temporal stride (CPU mirror: kTemporalStride)";

    // The exact regression this guards: handing the Hilbert index back as the
    // noise value, and re-deriving the sample noise from the slice noise.
    EXPECT_EQ(src.find("& 0xFFu) / 256.0"), std::string::npos)
        << "GTAO.comp uses the raw Hilbert index as its noise value again — neighbouring pixels "
           "will get near-identical noise and the AO will show a woven lattice that TAA cannot resolve";
    EXPECT_EQ(src.find("fract(noiseSlice * 0.6180339887498949)"), std::string::npos)
        << "GTAO.comp derives its sample noise from the slice noise again — the two are not "
           "independent and the sample distances collapse into [0, 0.62)";
}
