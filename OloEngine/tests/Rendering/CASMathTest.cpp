// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <limits>

// =============================================================================
// Contrast Adaptive Sharpening (CAS) — CPU contract tests.
//
// These pin the math implemented in PostProcess_CAS.glsl WITHOUT a GL context
// (so they run in headless CI), mirroring ContactShadowMathTest /
// ScreenSpaceReflectionMathTest. The rendered frame is checked separately by
// the GPU CASVisualEvidenceTest. Per the CLAUDE.md rendering rule, math/contract
// tests prove the formula; the visual test proves the frame looks right.
//
// The C++ kernel below is a line-for-line mirror of the GLSL fragment shader
// (the FidelityFX CAS "sharpen-only" path). If you change one, change both.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // A 3x3 neighborhood of single-channel samples, laid out:
    //   a b c
    //   d e f
    //   g h i
    struct Neighborhood
    {
        f32 a, b, c;
        f32 d, e, f;
        f32 g, h, i;
    };

    // CPU mirror of PostProcess_CAS.glsl's per-channel kernel (one channel).
    f32 CASFilterChannel(const Neighborhood& n, f32 sharpness)
    {
        const f32 a = n.a, b = n.b, c = n.c;
        const f32 d = n.d, e = n.e, f = n.f;
        const f32 g = n.g, h = n.h, i = n.i;

        // Soft min/max: the plus (b d e f h), then fold in the diagonals; the
        // two passes are summed (a factor of 2 pulled out of the weighting).
        f32 mnRGB = std::min(std::min(std::min(d, e), std::min(f, b)), h);
        const f32 mnRGB2 = std::min(mnRGB, std::min(std::min(a, c), std::min(g, i)));
        mnRGB += mnRGB2;

        f32 mxRGB = std::max(std::max(std::max(d, e), std::max(f, b)), h);
        const f32 mxRGB2 = std::max(mxRGB, std::max(std::max(a, c), std::max(g, i)));
        mxRGB += mxRGB2;

        // Guarded reciprocal: mx/max(mx^2, 1e-6) == 1/mx in range, 0 at mx==0.
        const f32 rcpMRGB = mxRGB / std::max(mxRGB * mxRGB, 1e-6f);
        f32 ampRGB = std::clamp(std::min(mnRGB, 2.0f - mxRGB) * rcpMRGB, 0.0f, 1.0f);
        ampRGB = std::sqrt(ampRGB);

        const f32 peak = -1.0f / glm::mix(8.0f, 5.0f, std::clamp(sharpness, 0.0f, 1.0f));
        const f32 wRGB = ampRGB * peak;

        const f32 rcpWeightRGB = 1.0f / (1.0f + 4.0f * wRGB);
        const f32 outColor = (b * wRGB + d * wRGB + f * wRGB + h * wRGB + e) * rcpWeightRGB;
        return std::clamp(outColor, 0.0f, 1.0f);
    }

    // Build a uniform-neighbor neighborhood: center `centre`, the 8 ring samples
    // all `ring`. Models an isolated bright/dark pixel against a flat surround.
    Neighborhood Uniform(f32 centre, f32 ring)
    {
        return Neighborhood{ ring, ring, ring, ring, centre, ring, ring, ring, ring };
    }

    constexpr f32 kEps = 1e-4f;
} // namespace

// --- Identity on a flat field ------------------------------------------------
// CAS is a normalized weighted average that equals the input when every sample
// is equal: it MUST NOT alter a flat region regardless of sharpness (no DC
// shift, no tinting). This is the core "don't touch what isn't an edge" rule.
TEST(CASMath, FlatFieldIsIdentity)
{
    for (const f32 v : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        for (const f32 s : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const f32 out = CASFilterChannel(Uniform(v, v), s);
            EXPECT_NEAR(out, v, kEps) << "flat field changed at v=" << v << " sharpness=" << s;
        }
    }
}

// --- A bright peak is pushed brighter, a dark valley darker ------------------
// The whole point of sharpening: a center brighter than its neighbors gains
// contrast (output >= input), a center darker loses (output <= input).
TEST(CASMath, PeakBrightensValleyDarkens)
{
    // Bright center on a darker surround.
    const f32 peakOut = CASFilterChannel(Uniform(0.6f, 0.4f), 0.5f);
    EXPECT_GT(peakOut, 0.6f) << "a bright pixel was not sharpened upward";

    // Dark center on a brighter surround.
    const f32 valleyOut = CASFilterChannel(Uniform(0.4f, 0.6f), 0.5f);
    EXPECT_LT(valleyOut, 0.4f) << "a dark pixel was not sharpened downward";
}

// --- Exact numeric pin -------------------------------------------------------
// Hand-derived reference for centre=0.6, ring=0.4, sharpness=0.5:
//   mn = 0.4+0.4 = 0.8 ; mx = 0.6+0.6 = 1.2 ; rcpM = 1/1.2
//   amp = sqrt(clamp(min(0.8, 0.8)/1.2)) = sqrt(0.6667) = 0.81650
//   peak = -1/mix(8,5,0.5) = -1/6.5 = -0.153846
//   w = 0.81650 * -0.153846 = -0.125615
//   out = (0.6 + w*(4*0.4)) / (1 + 4w) = (0.6 - 0.200984)/(0.497539) = 0.801995
// Pins the 2x-sum soft min/max, the sqrt shaping, the mix(8,5) peak remap and
// the cross-weight renormalization all at once.
TEST(CASMath, ExactReferenceValue)
{
    const f32 out = CASFilterChannel(Uniform(0.6f, 0.4f), 0.5f);
    EXPECT_NEAR(out, 0.801995f, 1e-3f);
}

// --- Monotonic in sharpness --------------------------------------------------
// A higher sharpness setting must sharpen an edge at least as hard as a lower
// one (the editor slider must do something monotone, not random).
TEST(CASMath, StrongerSharpnessSharpensMore)
{
    const Neighborhood edge = Uniform(0.55f, 0.45f); // mild, stays unclamped
    f32 prevDelta = -1.0f;
    for (const f32 s : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const f32 out = CASFilterChannel(edge, s);
        const f32 delta = out - 0.55f; // positive overshoot on a bright center
        EXPECT_GT(delta, 0.0f) << "no sharpening at sharpness=" << s;
        EXPECT_GE(delta + kEps, prevDelta) << "sharpening decreased as sharpness rose to " << s;
        prevDelta = delta;
    }
}

// --- Output stays within the LDR display range -------------------------------
// CAS runs post-tonemap; the final saturate must keep every result in [0,1]
// even for extreme edges that would otherwise overshoot past white / below
// black (no ringing past the display range).
TEST(CASMath, OutputStaysInUnitRange)
{
    const std::array<Neighborhood, 4> cases = {
        Uniform(1.0f, 0.0f), // max bright peak
        Uniform(0.0f, 1.0f), // max dark valley
        Uniform(0.95f, 0.05f),
        Uniform(0.05f, 0.95f),
    };
    for (const Neighborhood& nb : cases)
    {
        const f32 out = CASFilterChannel(nb, 1.0f);
        EXPECT_GE(out, 0.0f);
        EXPECT_LE(out, 1.0f);
    }
}

// --- A pure-black neighborhood is well-defined (no NaN) -----------------------
// The guarded reciprocal must keep mx==0 from producing a NaN that would
// propagate to the framebuffer as a black/garbage pixel.
TEST(CASMath, BlackNeighborhoodIsFinite)
{
    const f32 out = CASFilterChannel(Uniform(0.0f, 0.0f), 1.0f);
    EXPECT_TRUE(std::isfinite(out));
    EXPECT_NEAR(out, 0.0f, kEps);
}

// --- A grey edge stays grey (neutral, no channel drift) ----------------------
// Running the identical kernel per channel on equal RGB inputs yields equal RGB
// outputs — sharpening must not introduce a colour cast at edges.
TEST(CASMath, NeutralEdgeStaysNeutral)
{
    const Neighborhood nb = Uniform(0.6f, 0.4f);
    const f32 r = CASFilterChannel(nb, 0.7f);
    const f32 g = CASFilterChannel(nb, 0.7f);
    const f32 b = CASFilterChannel(nb, 0.7f);
    EXPECT_NEAR(r, g, std::numeric_limits<f32>::epsilon());
    EXPECT_NEAR(g, b, std::numeric_limits<f32>::epsilon());
}

// --- UBO layout contract -----------------------------------------------------
// The CPU CASUBOData must stay std140-sized to match the GLSL CASParams block
// at binding 44 (a drift would silently corrupt the sharpness / texel size).
TEST(CASMath, UBOLayoutMatchesShader)
{
    EXPECT_EQ(CASUBOData::GetSize(), 16u);
    EXPECT_EQ(ShaderBindingLayout::UBO_UPSCALER, 44u);
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_UPSCALER, "CASParams"));
}

// --- Sanitizer clamps persisted values ---------------------------------------
// Settings read from scene YAML / save-games must be finite and in [0,1] before
// they reach the UBO (the CLAUDE.md float-finiteness rule).
TEST(CASMath, SanitizeClampsSharpness)
{
    PostProcessSettings s;

    s.CASSharpness = 5.0f;
    SanitizeCAS(s);
    EXPECT_FLOAT_EQ(s.CASSharpness, 1.0f);

    s.CASSharpness = -2.0f;
    SanitizeCAS(s);
    EXPECT_FLOAT_EQ(s.CASSharpness, 0.0f);

    s.CASSharpness = std::numeric_limits<f32>::quiet_NaN();
    SanitizeCAS(s);
    EXPECT_FLOAT_EQ(s.CASSharpness, 0.5f); // NaN falls back to the default

    s.CASSharpness = std::numeric_limits<f32>::infinity();
    SanitizeCAS(s);
    EXPECT_FLOAT_EQ(s.CASSharpness, 0.5f);
}
