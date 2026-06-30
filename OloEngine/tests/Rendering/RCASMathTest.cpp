// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>

// =============================================================================
// FSR1 RCAS — CPU contract tests.
//
// These pin the robust contrast-adaptive sharpen implemented in
// PostProcess_RCAS.glsl WITHOUT a GL context (headless CI), mirroring
// CASMathTest. The rendered frame is checked separately by
// EASUVisualEvidenceTest. Per the CLAUDE.md rule, math/contract tests prove the
// formula; the visual test proves the frame looks right.
//
// The C++ kernel is a line-for-line mirror of the GLSL fragment shader (the FSR1
// RCAS path). If you change one, change both.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    const float kRcasLimit = 0.25f - (1.0f / 16.0f);

    float Max3(float a, float b, float c)
    {
        return std::max(a, std::max(b, c));
    }
    float Min3(float a, float b, float c)
    {
        return std::min(a, std::min(b, c));
    }
    // Luma in [0,1] (white -> 1.0) — see PostProcess_RCAS.glsl Luma() for why the
    // RCAS limiter requires the normalised [0,1] form, not FSR's "luma times 2".
    float Luma(const glm::vec3& c)
    {
        return 0.5f * c.g + 0.25f * (c.r + c.b);
    }

    // 3x3 cross neighbourhood: b (up), d (left), e (centre), f (right), h (down).
    struct Cross
    {
        glm::vec3 b, d, e, f, h;
    };

    // CPU mirror of PostProcess_RCAS.glsl main().
    glm::vec3 RcasFilter(const Cross& n, float sharpness)
    {
        const float bL = Luma(n.b), dL = Luma(n.d), eL = Luma(n.e), fL = Luma(n.f), hL = Luma(n.h);

        const float mn = std::min(Min3(bL, dL, fL), hL);
        const float mx = std::max(Max3(bL, dL, fL), hL);

        float dMax = 4.0f * mn - 4.0f;
        dMax = (dMax >= 0.0f) ? std::max(dMax, 1e-6f) : std::min(dMax, -1e-6f);
        const float hitMin = std::min(mn, eL) / std::max(4.0f * mx, 1e-6f);
        const float hitMax = (1.0f - std::max(mx, eL)) / dMax;
        const float lobeL = std::max(-hitMin, hitMax);
        const float stops = glm::mix(2.0f, 0.0f, std::clamp(sharpness, 0.0f, 1.0f));
        float lobe = std::max(-kRcasLimit, std::min(lobeL, 0.0f)) * std::exp2(-stops);

        float nz = 0.25f * (bL + dL + fL + hL) - eL;
        const float range = Max3(Max3(bL, dL, eL), fL, hL) - Min3(Min3(bL, dL, eL), fL, hL);
        nz = std::clamp(std::abs(nz) / std::max(range, 1e-6f), 0.0f, 1.0f);
        nz = 1.0f - 0.5f * nz;
        lobe *= nz;

        const float rcpL = 1.0f / (4.0f * lobe + 1.0f);
        const glm::vec3 outColor = (lobe * (n.b + n.d + n.h + n.f) + n.e) * rcpL;
        return glm::clamp(outColor, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    // Centre `centre`, the 4 ring samples all `ring` (isolated peak/valley).
    Cross Uniform(const glm::vec3& centre, const glm::vec3& ring)
    {
        return Cross{ ring, ring, centre, ring, ring };
    }

    constexpr float kEps = 1e-4f;
} // namespace

// --- flat field is identity --------------------------------------------------
// RCAS is a normalized weighted average that equals the input on a flat region:
// no sharpening, no DC shift, regardless of sharpness.
TEST(RCASMath, FlatFieldIsIdentity)
{
    for (const float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        for (const float s : { 0.0f, 0.5f, 1.0f })
        {
            const glm::vec3 out = RcasFilter(Uniform(glm::vec3(v), glm::vec3(v)), s);
            EXPECT_NEAR(out.r, v, kEps) << "flat field changed at v=" << v << " sharpness=" << s;
            EXPECT_NEAR(out.g, v, kEps);
            EXPECT_NEAR(out.b, v, kEps);
        }
    }
}

// --- a peak brightens, a valley darkens --------------------------------------
// The point of sharpening: a centre brighter than its ring gains contrast, a
// darker centre loses it.
TEST(RCASMath, PeakBrightensValleyDarkens)
{
    const glm::vec3 peak = RcasFilter(Uniform(glm::vec3(0.6f), glm::vec3(0.4f)), 1.0f);
    EXPECT_GT(peak.r, 0.6f) << "a bright pixel was not sharpened upward";

    const glm::vec3 valley = RcasFilter(Uniform(glm::vec3(0.4f), glm::vec3(0.6f)), 1.0f);
    EXPECT_LT(valley.r, 0.4f) << "a dark pixel was not sharpened downward";
}

// --- monotonic in sharpness --------------------------------------------------
// A higher sharpness setting must sharpen at least as hard as a lower one (the
// editor slider must be monotone).
TEST(RCASMath, StrongerSharpnessSharpensMore)
{
    const Cross edge = Uniform(glm::vec3(0.55f), glm::vec3(0.45f));
    float prevDelta = -1.0f;
    for (const float s : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const glm::vec3 out = RcasFilter(edge, s);
        const float delta = out.r - 0.55f;
        EXPECT_GE(delta + kEps, prevDelta) << "sharpening decreased as sharpness rose to " << s;
        prevDelta = delta;
    }
    // At maximum sharpness there must be a real (positive) overshoot.
    EXPECT_GT(prevDelta, 0.0f);
}

// --- output stays within the LDR display range -------------------------------
// RCAS runs post-tonemap; the final saturate must keep results in [0,1] even on
// extreme edges (no ringing past the display range).
TEST(RCASMath, OutputStaysInUnitRange)
{
    const std::array<Cross, 4> cases = {
        Uniform(glm::vec3(1.0f), glm::vec3(0.0f)),
        Uniform(glm::vec3(0.0f), glm::vec3(1.0f)),
        Uniform(glm::vec3(0.95f), glm::vec3(0.05f)),
        Uniform(glm::vec3(0.05f), glm::vec3(0.95f)),
    };
    for (const Cross& nb : cases)
    {
        const glm::vec3 out = RcasFilter(nb, 1.0f);
        for (int ch = 0; ch < 3; ++ch)
        {
            EXPECT_GE(out[ch], 0.0f);
            EXPECT_LE(out[ch], 1.0f);
        }
    }
}

// --- a pure-black neighbourhood is well-defined (no NaN) ----------------------
// The guarded reciprocals must keep an all-black cross from producing a NaN.
TEST(RCASMath, BlackNeighborhoodIsFinite)
{
    const glm::vec3 out = RcasFilter(Uniform(glm::vec3(0.0f), glm::vec3(0.0f)), 1.0f);
    for (int ch = 0; ch < 3; ++ch)
    {
        EXPECT_TRUE(std::isfinite(out[ch]));
        EXPECT_NEAR(out[ch], 0.0f, kEps);
    }
}

// --- UBO layout contract -----------------------------------------------------
// RCAS shares the binding-44 upscaler UBO with CAS (sharpness + texel size), so
// the same 16-byte std140 contract applies.
TEST(RCASMath, UBOLayoutMatchesShader)
{
    EXPECT_EQ(CASUBOData::GetSize(), 16u);
    EXPECT_EQ(ShaderBindingLayout::UBO_UPSCALER, 44u);
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_UPSCALER, "RCASParams"));
}
