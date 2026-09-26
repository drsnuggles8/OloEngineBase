#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
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

// =============================================================================
// Issue #1463: an unoccluded flat plane reads full visibility from every view
// elevation, and a contact crease still reads occluded.
//
// GtaoPixelVisibility below is a per-pixel CPU mirror of GTAO.comp's main():
// slice loop, step distribution, texel-centre rounding, falloff, horizon
// update, h0/h1 reconstruction, arc integral and power curve. It traces an
// analytic scene (an infinite floor at y = 0, optionally a wall standing on it)
// through the same GL-convention NDCToView unprojection GTAORenderPass
// uploads, in place of the HZB. It does not model HZB mips: those reduce with
// max(), so they return a FARTHER depth, which can only lower a horizon.
//
// The mirror carries both horizon conventions, so the pre-fix arm is an
// executable record of the defect: it reproduces the issue's live
// olo_render_probe_pixel readings on DDGITest.olo's floor (1.00 / 0.71 / 0.34)
// to within a few hundredths, which is what licenses trusting the fixed arm.
// The GPU half is GTAOVisualEvidenceTest's elevation sweep.
// =============================================================================

namespace
{
    enum class HorizonConvention
    {
        // GTAO.comp since #1463: horizon cosines against viewVec, seeded at the
        // low horizon cos(n +/- HALF_PI), sample 0 (+omega) bounds h1, falloff
        // fades towards the low horizon, samples at least 2 px out.
        XeGTAO,
        // GTAO.comp before #1463: horizon cosines against viewNormal, seeded
        // at -1, sample 0 bounds h0, falloff mixed from the running horizon,
        // samples at least 1 px out.
        Pre1463,
    };

    struct GtaoMirrorCamera
    {
        static constexpr int kWidth = 1024;
        static constexpr int kHeight = 768;

        glm::vec3 Eye{ 0.0f };
        glm::mat4 View{ 1.0f };
        glm::mat4 InvView{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::vec2 NDCToViewMul{ 1.0f };
        glm::vec2 NDCToViewAdd{ 0.0f };
        // Vulkan's row order: the projection seam flips clip y, so memory row 0
        // (uv v = 0) is the TOP of the view there and the bottom on GL.
        bool TopDownRows = false;

        GtaoMirrorCamera(const glm::vec3& eye, const glm::vec3& target, bool topDownRows = false)
            : Eye(eye), TopDownRows(topDownRows)
        {
            const glm::vec3 forward = glm::normalize(target - eye);
            const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            View = glm::lookAt(eye, target, up);
            InvView = glm::inverse(View);
            Projection = glm::perspective(glm::radians(60.0f), static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.05f, 1000.0f);
            // GTAORenderPass::UploadGTAOUniforms: the seam's reconstruction
            // projection, whose proj11 is negated on Vulkan's top-down rows.
            const float proj11 = topDownRows ? -Projection[1][1] : Projection[1][1];
            NDCToViewMul = glm::vec2(2.0f / Projection[0][0], 2.0f / proj11);
            NDCToViewAdd = glm::vec2(-1.0f / Projection[0][0], -1.0f / proj11);
        }

        // Screen position, in pixels, of a world-space point.
        [[nodiscard]] glm::vec2 PixelOf(const glm::vec3& world) const
        {
            const glm::vec4 clip = Projection * View * glm::vec4(world, 1.0f);
            glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
            if (TopDownRows)
                uv.y = 1.0f - uv.y;
            return uv * glm::vec2(kWidth, kHeight);
        }
    };

    struct MirrorHit
    {
        float Depth = 1.0e6f; // positive view distance; the far default is sky
        glm::vec3 WorldNormal{ 0.0f, 1.0f, 0.0f };
        bool Hit = false;
    };

    // Floor y = 0, plus (when hasWall) a wall z = wallZ facing +z that stands on it.
    MirrorHit TraceMirrorScene(const GtaoMirrorCamera& camera, const glm::vec2& screenUV, bool hasWall, float wallZ)
    {
        // A view-space ray scaled to unit depth: ComputeViewspacePosition(uv, 1).
        const glm::vec3 rayView(camera.NDCToViewMul * screenUV + camera.NDCToViewAdd, -1.0f);
        const glm::vec3 rayWorld = glm::mat3(camera.InvView) * rayView;
        MirrorHit best;
        if (rayWorld.y < 0.0f)
        {
            const float t = -camera.Eye.y / rayWorld.y;
            const glm::vec3 p = camera.Eye + t * rayWorld;
            if (!hasWall || p.z >= wallZ)
                best = { t, glm::vec3(0.0f, 1.0f, 0.0f), true };
        }
        if (hasWall && rayWorld.z < 0.0f)
        {
            const float t = (wallZ - camera.Eye.z) / rayWorld.z;
            const glm::vec3 p = camera.Eye + t * rayWorld;
            if (p.y >= 0.0f && (!best.Hit || t < best.Depth))
                best = { t, glm::vec3(0.0f, 0.0f, 1.0f), true };
        }
        return best;
    }

    struct GtaoMirrorInputs
    {
        HorizonConvention Convention = HorizonConvention::XeGTAO;
        float NoiseSlice = 0.5f;
        float NoiseSample = 0.5f;
        bool HasWall = false;
        float WallZ = 0.0f;
        // Take +omega's view direction from the unpack's signs (GTAO.comp since
        // #1463's Vulkan follow-up). False is omega verbatim, which is only
        // right on GL's bottom-up rows.
        bool UnpackSigns = true;
    };

    // Production defaults (PostProcessSettings.h): GTAORadius 0.5, GTAOPower
    // 2.2, GTAOFalloffRange 0.615, GTAOSampleDistribution 2.0,
    // GTAOThinCompensation 0.
    float GtaoPixelVisibility(const GtaoMirrorCamera& camera, const glm::vec2& pixel, const GtaoMirrorInputs& in)
    {
        constexpr int kSlices = 9;
        constexpr int kSteps = 3;
        constexpr float kHalfPi = kPi * 0.5f;
        constexpr float kRadius = 0.5f;
        constexpr float kFalloffRange = 0.615f;
        constexpr float kSampleDistributionPower = 2.0f;
        constexpr float kFinalValuePower = 2.2f;
        const bool xe = in.Convention == HorizonConvention::XeGTAO;

        const glm::vec2 pixelSize(1.0f / GtaoMirrorCamera::kWidth, 1.0f / GtaoMirrorCamera::kHeight);
        const glm::vec2 uv = pixel * pixelSize;
        const auto viewspacePosition = [&camera](const glm::vec2& screenPos, float depth)
        { return glm::vec3((camera.NDCToViewMul * screenPos + camera.NDCToViewAdd) * depth, -depth); };

        const MirrorHit centre = TraceMirrorScene(camera, uv, in.HasWall, in.WallZ);
        EXPECT_TRUE(centre.Hit) << "mirror pixel (" << pixel.x << ", " << pixel.y << ") sees no geometry";
        const glm::vec3 pixCenterPos = viewspacePosition(uv, centre.Depth);
        const glm::vec3 viewVec = glm::normalize(-pixCenterPos);
        const glm::vec3 viewNormal = glm::normalize(glm::mat3(camera.View) * centre.WorldNormal);

        const float pixelRadius = kRadius * (1.0f / camera.NDCToViewMul.x) * GtaoMirrorCamera::kWidth / centre.Depth;
        if (pixelRadius < 2.0f)
            return 1.0f;

        float visibility = 0.0f;
        for (int slice = 0; slice < kSlices; ++slice)
        {
            const float sliceAngle = (static_cast<float>(slice) + in.NoiseSlice) * (kPi / kSlices);
            const glm::vec2 omega(std::cos(sliceAngle), std::sin(sliceAngle));
            const glm::vec3 directionVS(in.UnpackSigns ? omega * glm::sign(camera.NDCToViewMul) : omega, 0.0f);
            const glm::vec3 orthoDirectionVS = directionVS - glm::dot(directionVS, viewVec) * viewVec;
            const glm::vec3 axisVS = glm::normalize(glm::cross(directionVS, viewVec));
            const glm::vec3 projectedNormal = viewNormal - axisVS * glm::dot(viewNormal, axisVS);
            const float projectedNormalLen = glm::length(projectedNormal);
            const float signN = glm::sign(glm::dot(orthoDirectionVS, projectedNormal));
            const float cosN = glm::clamp(glm::dot(projectedNormal, viewVec) / std::max(projectedNormalLen, 1e-6f), 0.0f, 1.0f);
            const float n = signN * std::acos(cosN);

            const float lowHorizonCos0 = std::cos(n + kHalfPi);
            const float lowHorizonCos1 = std::cos(n - kHalfPi);
            float horizonCos0 = xe ? lowHorizonCos0 : -1.0f;
            float horizonCos1 = xe ? lowHorizonCos1 : -1.0f;

            for (int step = 0; step < kSteps; ++step)
            {
                const float stepNoise = std::pow((static_cast<float>(step) + in.NoiseSample) / kSteps, kSampleDistributionPower);
                const float sampleOffsetPixels = std::max(stepNoise * pixelRadius, xe ? 2.0f : 1.0f);
                const glm::vec2 sampleOffset = glm::round(omega * sampleOffsetPixels) * pixelSize;

                std::array<float, 2> sampleCos{};
                std::array<float, 2> weight{};
                for (int side = 0; side < 2; ++side)
                {
                    const glm::vec2 sampleUV = side == 0 ? uv + sampleOffset : uv - sampleOffset;
                    const glm::vec3 samplePos = viewspacePosition(sampleUV, TraceMirrorScene(camera, sampleUV, in.HasWall, in.WallZ).Depth);
                    const glm::vec3 delta = samplePos - pixCenterPos;
                    const float dist = glm::length(delta);
                    const glm::vec3 horizonVec = delta / std::max(dist, 1e-6f);
                    weight[side] = glm::clamp(1.0f - (dist / kRadius - (1.0f - kFalloffRange)) / kFalloffRange, 0.0f, 1.0f);
                    sampleCos[side] = glm::dot(horizonVec, xe ? viewVec : viewNormal);
                }

                if (xe)
                {
                    horizonCos0 = std::max(horizonCos0, glm::mix(lowHorizonCos0, sampleCos[0], weight[0]));
                    horizonCos1 = std::max(horizonCos1, glm::mix(lowHorizonCos1, sampleCos[1], weight[1]));
                }
                else
                {
                    horizonCos0 = std::max(horizonCos0, glm::mix(horizonCos0, sampleCos[0], weight[0]));
                    horizonCos1 = std::max(horizonCos1, glm::mix(horizonCos1, sampleCos[1], weight[1]));
                }
            }

            float h0 = 0.0f;
            float h1 = 0.0f;
            if (xe)
            {
                h0 = -std::acos(glm::clamp(horizonCos1, -1.0f, 1.0f));
                h1 = std::acos(glm::clamp(horizonCos0, -1.0f, 1.0f));
                h0 = n + glm::clamp(h0 - n, -kHalfPi, kHalfPi);
                h1 = n + glm::clamp(h1 - n, -kHalfPi, kHalfPi);
            }
            else
            {
                h0 = n + std::max(-std::acos(glm::clamp(horizonCos0, -1.0f, 1.0f)) - n, -kHalfPi);
                h1 = n + std::min(std::acos(glm::clamp(horizonCos1, -1.0f, 1.0f)) - n, kHalfPi);
            }

            const float iarc0 = -std::cos(2.0f * h0 - n) + std::cos(n) + 2.0f * h0 * std::sin(n);
            const float iarc1 = -std::cos(2.0f * h1 - n) + std::cos(n) + 2.0f * h1 * std::sin(n);
            visibility += 0.25f * (iarc0 + iarc1) * projectedNormalLen;
        }

        visibility = glm::clamp(visibility / kSlices, 0.03f, 1.0f);
        return glm::clamp(std::pow(visibility, kFinalValuePower), 0.03f, 1.0f);
    }

    // Mean and minimum over an 8x8 grid of (slice, sample) noise values: the
    // spread the R2 noise covers across a denoise footprint.
    struct NoiseStats
    {
        float Mean = 0.0f;
        float Min = 1.0f;
    };

    NoiseStats GtaoOverNoise(const GtaoMirrorCamera& camera, const glm::vec2& pixel, GtaoMirrorInputs in)
    {
        constexpr int kGrid = 8;
        NoiseStats stats;
        for (int i = 0; i < kGrid; ++i)
        {
            for (int j = 0; j < kGrid; ++j)
            {
                in.NoiseSlice = (static_cast<float>(i) + 0.5f) / kGrid;
                in.NoiseSample = (static_cast<float>(j) + 0.5f) / kGrid;
                const float v = GtaoPixelVisibility(camera, pixel, in);
                stats.Mean += v / static_cast<float>(kGrid * kGrid);
                stats.Min = std::min(stats.Min, v);
            }
        }
        return stats;
    }

    // Camera 6 m from a floor point, `zenithDeg` away from the floor normal
    // (0 = straight down, 75 = 15 degrees above the floor).
    GtaoMirrorCamera FloorCameraAtZenith(float zenithDeg)
    {
        const float zenith = glm::radians(zenithDeg);
        return GtaoMirrorCamera(glm::vec3(0.0f, 6.0f * std::cos(zenith), 6.0f * std::sin(zenith)), glm::vec3(0.0f));
    }
} // namespace

// THE #1463 CONTRACT, CPU half: an unoccluded plane reads AO >= 0.97 from 0 to
// 75 degrees, at the screen centre and off-centre (where viewVec departs from
// the camera axis). The minimum over noise has its own looser floor: one R2
// draw is one pixel before the denoise averages its neighbours.
TEST(GTAOMath, UnoccludedPlaneIsFullyVisibleFromEveryElevation)
{
    for (float zenithDeg : { 0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 75.0f })
    {
        const GtaoMirrorCamera camera = FloorCameraAtZenith(zenithDeg);
        for (const glm::vec2& at : { glm::vec2(0.5f, 0.5f), glm::vec2(0.25f, 0.3f), glm::vec2(0.8f, 0.35f) })
        {
            const glm::vec2 pixel = at * glm::vec2(GtaoMirrorCamera::kWidth, GtaoMirrorCamera::kHeight);
            const NoiseStats stats = GtaoOverNoise(camera, pixel, {});
            EXPECT_GE(stats.Mean, 0.97f) << "unoccluded floor, " << zenithDeg << " degrees from the normal, screen ("
                                         << at.x << ", " << at.y << "): mean AO " << stats.Mean;
            EXPECT_GE(stats.Min, 0.94f) << "unoccluded floor, " << zenithDeg << " degrees, screen (" << at.x << ", "
                                        << at.y << "): one noise draw reads " << stats.Min;
        }
    }
}

// Negative control for the mirror, and the executable record of #1463: the
// pre-fix conventions reproduce the issue's live readings of DDGITest.olo's
// floor at [0,0,7] from its three cameras. If this arm stopped matching them,
// the mirror would no longer be evidence about the shader.
TEST(GTAOMath, Pre1463ConventionsReproduceTheMeasuredFloorDarkening)
{
    struct Reading
    {
        glm::vec3 Eye;
        float Measured;
    };
    const glm::vec3 floorPoint(0.0f, 0.0f, 7.0f);
    const glm::vec2 centre(GtaoMirrorCamera::kWidth * 0.5f, GtaoMirrorCamera::kHeight * 0.5f);
    for (const Reading& r : { Reading{ { 0.0f, 4.0f, 7.0f }, 1.00f }, Reading{ { 0.0f, 4.0f, 3.0f }, 0.71f },
                              Reading{ { 0.0f, 1.0f, 2.0f }, 0.34f } })
    {
        const GtaoMirrorCamera camera(r.Eye, floorPoint);
        GtaoMirrorInputs pre;
        pre.Convention = HorizonConvention::Pre1463;
        EXPECT_NEAR(GtaoOverNoise(camera, centre, pre).Mean, r.Measured, 0.04f)
            << "the pre-#1463 arm no longer reproduces the live reading from eye (" << r.Eye.x << ", " << r.Eye.y
            << ", " << r.Eye.z << ")";
        EXPECT_GE(GtaoOverNoise(camera, centre, {}).Mean, 0.97f)
            << "the fixed conventions still darken the floor from eye (" << r.Eye.x << ", " << r.Eye.y << ", "
            << r.Eye.z << ")";
    }
}

// THE SAME CONTRACT ON VULKAN'S ROW ORDER. Memory row 0 is the top of the
// view there, so screen +y is view -y, and GTAO.comp takes +omega's view
// direction from sign(u_NDCToViewMul). With omega taken verbatim every slice
// with a vertical component has its sides swapped: the pre-follow-up arm below
// reproduces the live Vulkan reading of the issue's 45-degree floor (0.216).
TEST(GTAOMath, UnoccludedPlaneIsFullyVisibleOnVulkanRowOrder)
{
    for (float zenithDeg : { 0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 75.0f })
    {
        const float zenith = glm::radians(zenithDeg);
        const GtaoMirrorCamera camera(glm::vec3(0.0f, 6.0f * std::cos(zenith), 6.0f * std::sin(zenith)), glm::vec3(0.0f),
                                      /*topDownRows*/ true);
        const glm::vec2 centre(GtaoMirrorCamera::kWidth * 0.5f, GtaoMirrorCamera::kHeight * 0.5f);
        const NoiseStats stats = GtaoOverNoise(camera, centre, {});
        EXPECT_GE(stats.Mean, 0.97f) << "unoccluded floor on top-down rows, " << zenithDeg << " degrees: AO "
                                     << stats.Mean;
    }

    const GtaoMirrorCamera issueCamera(glm::vec3(0.0f, 4.0f, 3.0f), glm::vec3(0.0f, 0.0f, 7.0f), /*topDownRows*/ true);
    const glm::vec2 centre(GtaoMirrorCamera::kWidth * 0.5f, GtaoMirrorCamera::kHeight * 0.5f);
    GtaoMirrorInputs verbatim;
    verbatim.UnpackSigns = false;
    EXPECT_NEAR(GtaoOverNoise(issueCamera, centre, verbatim).Mean, 0.216f, 0.04f)
        << "omega taken verbatim on top-down rows no longer reproduces the live Vulkan reading";
    EXPECT_GE(GtaoOverNoise(issueCamera, centre, {}).Mean, 0.97f);

    GtaoMirrorInputs crease;
    crease.HasWall = true;
    crease.WallZ = -2.0f;
    const GtaoMirrorCamera creaseCamera(glm::vec3(0.0f, 3.0f, 3.0f), glm::vec3(0.0f, 0.0f, -2.0f), /*topDownRows*/ true);
    EXPECT_LT(GtaoOverNoise(creaseCamera, creaseCamera.PixelOf({ 0.0f, 0.0f, -1.95f }), crease).Mean, 0.65f)
        << "the crease is not occluded on top-down rows";
}

// Positive control: fixing the flat plane must not flatten real occlusion. A
// floor point 5 cm from a wall reads clearly occluded, and the occlusion fades
// back to full visibility beyond the 0.5 m radius.
TEST(GTAOMath, ContactCreaseStaysOccluded)
{
    const GtaoMirrorCamera camera(glm::vec3(0.0f, 3.0f, 3.0f), glm::vec3(0.0f, 0.0f, -2.0f));
    GtaoMirrorInputs in;
    in.HasWall = true;
    in.WallZ = -2.0f;

    const float nearFloor = GtaoOverNoise(camera, camera.PixelOf({ 0.0f, 0.0f, -1.95f }), in).Mean;
    const float nearWall = GtaoOverNoise(camera, camera.PixelOf({ 0.0f, 0.05f, -2.0f }), in).Mean;
    const float openFloor = GtaoOverNoise(camera, camera.PixelOf({ 0.0f, 0.0f, -1.0f }), in).Mean;
    EXPECT_LT(nearFloor, 0.65f) << "floor 5 cm from the wall is not occluded (AO " << nearFloor << ")";
    EXPECT_LT(nearWall, 0.65f) << "wall 5 cm above the floor is not occluded (AO " << nearWall << ")";
    EXPECT_GE(openFloor, 0.97f) << "floor 1 m from the wall, twice the radius, is still occluded (AO " << openFloor << ")";
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

// Source guard for the conventions the mirror above assumes. One find per
// load-bearing line, so reverting any one of them fails here: the mirror
// proves WHY, this proves GTAO.comp still does it.
TEST(GTAOMath, GtaoShaderUsesXeGtaoHorizonConventions)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(src.empty());
    for (const char* line : {
             "const float lowHorizonCos0 = cos(n + XE_GTAO_HALF_PI);",
             "const float lowHorizonCos1 = cos(n - XE_GTAO_HALF_PI);",
             "float horizonCos0 = lowHorizonCos0;",
             "float horizonCos1 = lowHorizonCos1;",
             "mix(lowHorizonCos0, dot(sampleHorizon0, viewVec), weight0)",
             "mix(lowHorizonCos1, dot(sampleHorizon1, viewVec), weight1)",
             "float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));",
             "float h1 = acos(clamp(horizonCos0, -1.0, 1.0));",
             "#define XE_GTAO_MIN_SAMPLE_OFFSET_PIXELS 2.0",
             "max(stepNoise * pixelRadius, XE_GTAO_MIN_SAMPLE_OFFSET_PIXELS)",
             "vec3 directionVS = vec3(omega * sign(u_NDCToViewMul), 0.0);",
         })
    {
        EXPECT_NE(src.find(line), std::string::npos) << "GTAO.comp lost: " << line;
    }
    // The pre-#1463 form: horizons measured against the normal.
    EXPECT_EQ(src.find("dot(sampleHorizon0, viewNormal)"), std::string::npos)
        << "GTAO.comp measures horizons against viewNormal again; n and h0/h1 are angles from viewVec (#1463)";
    EXPECT_EQ(src.find("dot(sampleHorizon1, viewNormal)"), std::string::npos)
        << "GTAO.comp measures horizons against viewNormal again; n and h0/h1 are angles from viewVec (#1463)";
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
    const std::string src = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / "GTAORenderPass.cpp");
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("glm::vec2(2.0f / projScale00, 2.0f / projScale11)"), std::string::npos)
        << "NDCToViewMul lost its GL-convention positive Y term";
    EXPECT_NE(src.find("glm::vec2(-1.0f / projScale00, -1.0f / projScale11)"), std::string::npos)
        << "NDCToViewAdd lost its GL-convention negative Y term";
    EXPECT_EQ(src.find("-2.0f / projScale11"), std::string::npos)
        << "the D3D top-down Y flip is back in NDCToViewMul — grazing views will collapse to black again";
}

// ...and the scale comes from the seam's RECONSTRUCTION projection, which is
// the row flip Vulkan's top-down rows need (identity on GL). Both passes that
// unpack screen uv this way must take it.
TEST(GTAOMath, ScreenUnpackCarriesTheVulkanRowFlip)
{
    for (const char* pass : { "GTAORenderPass.cpp", "SphereProxyAORenderPass.cpp" })
    {
        SCOPED_TRACE(pass);
        const std::string src = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / pass);
        ASSERT_FALSE(src.empty());
        EXPECT_NE(src.find("RHI::AdjustProjectionForShaderReconstruction(m_Projection)"), std::string::npos)
            << pass << " unpacks screen uv with the raw projection again: on Vulkan every reconstructed position is "
                       "mirrored about the horizontal";
        EXPECT_EQ(src.find("projScale11 = m_Projection[1][1]"), std::string::npos) << pass;
    }
}

TEST(GTAOMath, TemporalNoiseOnlyAnimatesUnderTAA)
{
    // XeGTAO's animated noise index exists so TAA can resolve the R1/Hilbert
    // pattern temporally. Without TAA the pattern boils every frame — the
    // "goosebumps" weave over water and the distant quay (VehiclesTest,
    // issue #438 follow-up). The pass must advance NoiseIndex only when TAA
    // is enabled. Read through ReadRepoFile, which anchors on the compile-time
    // OLO_TEST_EDITOR_ROOT rather than the working directory — a cwd-relative
    // read breaks whenever the runner isn't launched from the repo root.
    const std::string src = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / "GTAORenderPass.cpp");
    ASSERT_FALSE(src.empty());

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

// An off-screen horizon sample never reads depth from the opposite screen
// edge. The HZB is sampled through the texture's own sampler (Repeat by
// default), so the viewport clamp alone left a linear fetch at uv 0, and at
// every edge of a power-of-two viewport, half in the texel on the far side:
// VulkanPassSuite.GtaoIsOpenOnUniformDepthAndDarkensACrease (128 x 128) read an
// unoccluded top row at 103/255 against a control of 240. The border read is
// per mip level (one clamp for both trilinear levels moves the fine read inward
// and darkened a receding floor's border live, 173 against 176) and built from
// texelFetch (GL's pooled HZB has no mipmap min filter, so a clamped textureLod
// reads level 0 at the coarse level's inset). The device test needs Vulkan
// hardware and runs on no CI job; this pin runs everywhere.
TEST(GTAOMath, GtaoHzbFetchNeverWrapsToTheOppositeEdge)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(src.empty());
    const auto bodyOf = [&src](const std::string& signature) -> std::string
    {
        const auto begin = src.find(signature);
        if (begin == std::string::npos)
            return {};
        const auto end = src.find("\n}", begin);
        return end == std::string::npos ? std::string{} : src.substr(begin, end - begin);
    };
    const auto count = [](const std::string& haystack, const std::string& needle)
    {
        int n = 0;
        for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
            ++n;
        return n;
    };

    const std::string level = bodyOf("float FetchHZBLevelClamped(");
    ASSERT_FALSE(level.empty()) << "GTAO.comp lost FetchHZBLevelClamped";
    EXPECT_EQ(count(level, "texelFetch(u_HZBDepth, "), 4) << "the per-level border read is no longer a four-tap texelFetch";
    EXPECT_EQ(count(level, "clamp(i0, ivec2(0), size - 1)"), 1) << "the first tap is no longer clamped to the level";
    EXPECT_EQ(count(level, "clamp(i0 + 1, ivec2(0), size - 1)"), 1) << "the second tap is no longer clamped to the level";
    EXPECT_EQ(count(level, "texture"), 0) << "the border read samples through the sampler again";

    const std::string depth = bodyOf("float SampleHZBDepth(");
    ASSERT_FALSE(depth.empty()) << "GTAO.comp lost SampleHZBDepth";
    EXPECT_EQ(count(depth, "FetchHZBLevelClamped(hzbUV, lo)"), 1) << "the border path no longer reads the fine level clamped";
    EXPECT_EQ(count(depth, "FetchHZBLevelClamped(hzbUV, hi)"), 1) << "the border path no longer reads the coarse level clamped";
    // Exactly one unclamped trilinear fetch: the interior fast path. A second
    // one is a border read that can wrap again.
    EXPECT_EQ(count(depth, "textureLod(u_HZBDepth, hzbUV, mipLevel)"), 1)
        << "SampleHZBDepth has an unclamped trilinear fetch outside its interior fast path";
    EXPECT_EQ(count(depth, "vec2(1.0) - halfTexelHi"), 1) << "the fast path no longer excludes the coarse level's border";
}

// Issue #771, layer 1 — the AO CONSUMER must follow the graph, not the setting.
//
// `ActiveAOTechnique` selects which AO pass RegisterSceneAndLightingNodes adds
// to the graph, and that switch runs at TOPOLOGY-BUILD time. So the field has
// two readers on different clocks: the topology (recorded in
// `ActiveGraphAOTechnique`, updated only by ConfigureRenderGraph) and the
// per-frame pipeline hook. While they disagreed, PopulateBlackboard declared
// AOBuffer and enabled AOApplyPass for a producer that was NOT in the graph —
// so AOApplyPass multiplied the whole frame by a transient nobody wrote. The
// graph said so outright: its "AO/Post order" trace read `GTAO=n/a` and the
// submission plan contained no GTAOPass at all.
//
// Keying the consumer off the graph makes an unapplied technique change
// degrade to "no AO" instead of a black frame.
TEST(GTAOMath, AoConsumersKeyOffTheGraphsTechniqueNotTheRequestedOne)
{
    const std::string src = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" /
                                         "Renderer" / "RenderPipeline.cpp");
    ASSERT_FALSE(src.empty());

    // The AOApply enable gate.
    EXPECT_NE(src.find("data.ActiveGraphAOTechnique == AOTechnique::SSAO && data.PostProcess.SSAOEnabled"),
              std::string::npos)
        << "the AOApply enable gate no longer keys off the technique the GRAPH was built with — a "
           "requested-but-unapplied AO technique will enable the apply pass with no producer "
           "registered, and the frame gets multiplied by an unwritten AO buffer (#771)";
    EXPECT_NE(src.find("data.ActiveGraphAOTechnique == AOTechnique::GTAO && data.PostProcess.GTAOEnabled"),
              std::string::npos)
        << "the AOApply enable gate no longer keys off the graph's technique for GTAO (#771)";

    // ...and the AOBuffer declaration + the two scratch-declaration gates,
    // which are the other half: declaring the buffer is what gives the consumer
    // something to sample in the first place. Counted rather than matched
    // line-by-line so reformatting cannot silently retire the check.
    u32 graphKeyedGates = 0;
    for (std::size_t at = src.find("data.ActiveGraphAOTechnique"); at != std::string::npos;
         at = src.find("data.ActiveGraphAOTechnique", at + 1))
    {
        ++graphKeyedGates;
    }
    EXPECT_GE(graphKeyedGates, 5u)
        << "expected the AOApply enable gate (x2), the AOBuffer declaration (x2) and the SSAO/GTAO "
           "scratch declarations to key off ActiveGraphAOTechnique; found "
        << graphKeyedGates
        << " site(s). A gate that fell back to PostProcess.ActiveAOTechnique re-opens #771.";

    // The precise regression: an AO gate comparing the REQUESTED technique.
    EXPECT_EQ(src.find("data.PostProcess.ActiveAOTechnique == AOTechnique::"), std::string::npos)
        << "an AO gate compares PostProcess.ActiveAOTechnique again — that is the setting the caller "
           "asked for, not the one the graph has a pass registered for (#771)";

    // BOTH ends of the seam. The producers bail out of Setup/Execute on their own
    // m_Settings.ActiveAOTechnique, so they must be handed the graph's technique
    // too — otherwise the identical black frame reappears with the roles swapped
    // (graph wired for GTAO, request flipped to SSAO, nobody writes AOBuffer).
    EXPECT_NE(src.find("aoProducerSettings.ActiveAOTechnique = data.ActiveGraphAOTechnique"), std::string::npos)
        << "the AO producers are no longer handed the graph's technique — SSAORenderPass / "
           "GTAORenderPass will decline to run while the consumer side still enables AOApply (#771)";
    EXPECT_EQ(src.find("SSAO->SetSettings(data.PostProcess)"), std::string::npos)
        << "SSAORenderPass is fed the requested technique again (#771)";
    EXPECT_EQ(src.find("GTAO->SetSettings(data.PostProcess)"), std::string::npos)
        << "GTAORenderPass is fed the requested technique again (#771)";
}

// Issue #771, layer 2 — the AO target must never reach AOApplyPass unwritten.
//
// PostProcess_SSAOApply computes `sceneColor * mix(1.0, ao, intensity)`, so an
// AO texel of 0 multiplies the scene to EXACTLY black. GTAO.comp cannot produce
// 0: every path either stores full visibility (sky / no-normal / degenerate
// early-outs) or falls through the `clamp(visibility, 0.03, 1.0)` floor below.
// So an AO texel of 0 arriving at the apply pass ALWAYS means storage nobody
// wrote. Layer 1 above stops the graph from ever asking for that; this is the
// belt-and-braces half — GTAORenderPass itself must not leave its own output
// unwritten when it skips a frame for any other reason.
//
// This pins BOTH halves, since either alone is silently useless: the
// shader-side floor (0 is not producible) and the pass-side guarantee (every
// early return past the AO resolve publishes the no-occlusion identity).
TEST(GTAOMath, GtaoAoTargetIsNeverLeftUnwrittenForTheApplyPass)
{
    const std::string shaderSrc = ReadRepoFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "GTAO.comp");
    ASSERT_FALSE(shaderSrc.empty());
    EXPECT_NE(shaderSrc.find("clamp(visibility, 0.03, 1.0)"), std::string::npos)
        << "GTAO.comp lost its positive visibility floor — AO == 0 becomes a value the pass can "
           "legitimately produce, and 'the AO target was never written' stops being diagnosable";

    // The identity itself lives in the shared helper both AO producers call, so
    // the two cannot drift apart.
    const std::string identitySrc = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" /
                                                 "Renderer" / "Passes" / "AOTargetIdentity.h");
    ASSERT_FALSE(identitySrc.empty());
    EXPECT_NE(identitySrc.find("ClearTextureFloat(aoTarget, 0, glm::vec4(1.0f))"), std::string::npos)
        << "PublishAOTargetAsFullyVisible no longer clears the AO target to 1.0 (fully visible)";

    // SSAO is the other producer of the same resource and has the same duty: its
    // clear-to-white covers only its own SSAORaw/SSAOBlur scratch and sits after
    // its early returns, so AOBuffer needs the explicit publish too.
    const std::string ssaoSrc = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" /
                                             "Renderer" / "Passes" / "SSAORenderPass.cpp");
    ASSERT_FALSE(ssaoSrc.empty());
    EXPECT_NE(ssaoSrc.find("PublishAOTargetAsFullyVisible("), std::string::npos)
        << "SSAORenderPass no longer publishes the no-occlusion identity on its early returns — the "
           "same unwritten-AOBuffer black frame as #771, on the SSAO technique";

    const std::string passSrc = ReadRepoFile(std::filesystem::path{ ".." } / "OloEngine" / "src" / "OloEngine" /
                                             "Renderer" / "Passes" / "GTAORenderPass.cpp");
    ASSERT_FALSE(passSrc.empty());

    // EVERY early exit from the moment the AO target is RESOLVED (i.e. from the
    // moment AOApplyPass is guaranteed to sample it this frame) must leave the
    // identity published. Two spellings satisfy that, because #1013 moved the
    // body out of Execute() and into PrepareParallelRecording(): a bare
    // `return;` preceded by a PublishNoOcclusion() call, and a
    // `return PrepareNoOcclusion(...);` whose prepared body records the clear.
    //
    // The LAST return in the window is the success path — it hands back the
    // real prepared pass, which writes the AO target for real — so it is
    // exempt. Anything textually after it is a different (void) function, so a
    // newly added early-out cannot hide in that exemption: it would no longer
    // be last.
    const auto resolvePos = passSrc.find("aoOutputTexID = context.ResolveTextureHandle(");
    ASSERT_NE(resolvePos, std::string::npos) << "AO target resolve not found in GTAORenderPass";
    const auto executeEnd = passSrc.find("void GTAORenderPass::PublishNoOcclusion");
    ASSERT_NE(executeEnd, std::string::npos) << "PublishNoOcclusion definition not found";
    ASSERT_GT(executeEnd, resolvePos);

    const std::string tail = passSrc.substr(resolvePos, executeEnd - resolvePos);
    std::vector<std::size_t> returns;
    for (std::size_t at = tail.find("return"); at != std::string::npos; at = tail.find("return", at + 1))
        returns.push_back(at);
    ASSERT_FALSE(returns.empty()) << "no returns found to check — the scan anchor probably moved";

    u32 guardedReturns = 0;
    for (std::size_t i = 0; i + 1 < returns.size(); ++i)
    {
        const std::size_t at = returns[i];
        const std::size_t statementEnd = std::min(tail.find(';', at) + 1u, tail.size());
        const std::string statement = tail.substr(at, statementEnd - at);
        const std::size_t windowStart = at > 400u ? at - 400u : 0u;
        const std::string before = tail.substr(windowStart, at - windowStart);
        const bool publishesInline = before.find("PublishNoOcclusion(") != std::string::npos;
        const bool returnsThePreparedIdentity = statement.find("PrepareNoOcclusion(") != std::string::npos;
        EXPECT_TRUE(publishesInline || returnsThePreparedIdentity)
            << "GTAORenderPass has an early return after the AO target is resolved that does NOT leave "
               "the no-occlusion identity published — AOApplyPass will multiply the scene by whatever "
               "the transient pool handed us, and on fresh (zeroed) storage that is an exactly black "
               "frame (#771). Offending statement: "
            << statement;
        ++guardedReturns;
    }
    EXPECT_GT(guardedReturns, 0u) << "no early returns found to check — the scan anchor probably moved";
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
    //
    // Walk EVERY consecutive pair in the cycle, including the 63 -> 0 wrap:
    // the index is used as (noiseIndex & 63), so the wrap is a real frame
    // transition the renderer performs once per 64 frames. Checking only
    // 0 -> 1 would leave a stride that decorrelates for one pair and repeats
    // for another (or specifically at the wrap) completely undetected.
    constexpr int kNoisePhaseCount = 64;
    for (int phase = 0; phase < kNoisePhaseCount; ++phase)
    {
        const int nextPhase = (phase + 1) % kNoisePhaseCount;
        const double frameCorr = Correlation(R2Noise(kR2SliceMultiplier, phase), R2Noise(kR2SliceMultiplier, nextPhase));
        EXPECT_LT(frameCorr, 0.35)
            << "GTAO noise phases " << phase << " -> " << nextPhase << " are " << frameCorr
            << " correlated — TAA cannot resolve a pattern that barely changes between frames";
    }

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
