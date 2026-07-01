// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// =============================================================================
// FSR1 EASU — CPU contract tests.
//
// These pin the directional-Lanczos resolve implemented in PostProcess_EASU.glsl
// WITHOUT a GL context (so they run in headless CI), mirroring CASMathTest. The
// rendered frame is checked separately by the GPU EASUVisualEvidenceTest. Per
// the CLAUDE.md rendering rule, math/contract tests prove the formula; the visual
// test proves the frame looks right.
//
// The C++ kernel below is a line-for-line mirror of the GLSL EasuSet / EasuTap /
// resolve (operating on the reversible tonemap proxy). If you change one, change
// both. Texture sampling/filtering is intentionally NOT mirrored here — these
// tests feed the 12 proxy-colour taps directly, exactly as CASMathTest feeds a
// synthetic neighbourhood, so the contract is the kernel math, not GL filtering.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // --- reversible per-channel tonemap proxy (mirror of ToProxy / FromProxy) --
    glm::vec3 ToProxy(const glm::vec3& c)
    {
        return c / (glm::vec3(1.0f) + glm::max(c, glm::vec3(0.0f)));
    }
    glm::vec3 FromProxy(const glm::vec3& t)
    {
        return t / glm::max(glm::vec3(1.0f) - t, glm::vec3(1e-6f));
    }

    float Rcp(float x)
    {
        return 1.0f / std::max(x, 1e-6f);
    }
    float Rsqrt(float x)
    {
        return 1.0f / std::sqrt(std::max(x, 1e-6f));
    }
    float Luma(const glm::vec3& c)
    {
        return c.g + 0.5f * (c.r + c.b);
    }

    void EasuTap(glm::vec3& aC, float& aW, glm::vec2 off, glm::vec2 dir, glm::vec2 len,
                 float lob, float clp, const glm::vec3& c)
    {
        glm::vec2 v;
        v.x = (off.x * dir.x) + (off.y * dir.y);
        v.y = (off.x * (-dir.y)) + (off.y * dir.x);
        v *= len;
        float d2 = v.x * v.x + v.y * v.y;
        d2 = std::min(d2, clp);
        float wB = (2.0f / 5.0f) * d2 - 1.0f;
        float wA = lob * d2 - 1.0f;
        wB *= wB;
        wA *= wA;
        wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
        float w = wB * wA;
        aC += c * w;
        aW += w;
    }

    void EasuSet(glm::vec2& dir, float& len, glm::vec2 pp, float w,
                 float lA, float lB, float lC, float lD, float lE)
    {
        float dc = lD - lC;
        float cb = lC - lB;
        float lenX = std::max(std::abs(dc), std::abs(cb));
        lenX = Rcp(lenX);
        float dirX = lD - lB;
        lenX = std::clamp(std::abs(dirX) * lenX, 0.0f, 1.0f);
        lenX *= lenX;

        float ec = lE - lC;
        float ca = lC - lA;
        float lenY = std::max(std::abs(ec), std::abs(ca));
        lenY = Rcp(lenY);
        float dirY = lE - lA;
        lenY = std::clamp(std::abs(dirY) * lenY, 0.0f, 1.0f);
        lenY *= lenY;

        dir += glm::vec2(dirX, dirY) * w;
        len += (lenX + lenY) * w;
        (void)pp;
    }

    // 12 HDR taps in the FSR layout (b c / e f g h / i j k l / n o), fractional
    // position pp in [0,1]^2. Returns the resolved HDR colour.
    struct Taps
    {
        glm::vec3 b, c, e, f, g, h, i, j, k, l, n, o;
    };

    glm::vec3 EasuResolve(const Taps& t, glm::vec2 pp)
    {
        const glm::vec3 b = ToProxy(t.b), c = ToProxy(t.c), e = ToProxy(t.e), f = ToProxy(t.f);
        const glm::vec3 g = ToProxy(t.g), h = ToProxy(t.h), i = ToProxy(t.i), j = ToProxy(t.j);
        const glm::vec3 k = ToProxy(t.k), l = ToProxy(t.l), n = ToProxy(t.n), o = ToProxy(t.o);

        const float bL = Luma(b), cL = Luma(c);
        const float eL = Luma(e), fL = Luma(f), gL = Luma(g), hL = Luma(h);
        const float iL = Luma(i), jL = Luma(j), kL = Luma(k), lL = Luma(l);
        const float nL = Luma(n), oL = Luma(o);

        glm::vec2 dir(0.0f);
        float len = 0.0f;
        EasuSet(dir, len, pp, (1.0f - pp.x) * (1.0f - pp.y), bL, eL, fL, gL, jL);
        EasuSet(dir, len, pp, pp.x * (1.0f - pp.y), cL, fL, gL, hL, kL);
        EasuSet(dir, len, pp, (1.0f - pp.x) * pp.y, fL, iL, jL, kL, nL);
        EasuSet(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);

        glm::vec2 dir2 = dir * dir;
        float dirR = dir2.x + dir2.y;
        bool zro = dirR < (1.0f / 32768.0f);
        dirR = Rsqrt(dirR);
        dirR = zro ? 1.0f : dirR;
        dir.x = zro ? 1.0f : dir.x;
        dir *= glm::vec2(dirR);

        len = len * 0.5f;
        len *= len;

        float stretch = (dir.x * dir.x + dir.y * dir.y) * Rcp(std::max(std::abs(dir.x), std::abs(dir.y)));
        glm::vec2 len2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
        float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
        float clp = Rcp(lob);

        glm::vec3 aC(0.0f);
        float aW = 0.0f;
        EasuTap(aC, aW, glm::vec2(0.0f, -1.0f) - pp, dir, len2, lob, clp, b);
        EasuTap(aC, aW, glm::vec2(1.0f, -1.0f) - pp, dir, len2, lob, clp, c);
        EasuTap(aC, aW, glm::vec2(-1.0f, 1.0f) - pp, dir, len2, lob, clp, i);
        EasuTap(aC, aW, glm::vec2(0.0f, 1.0f) - pp, dir, len2, lob, clp, j);
        EasuTap(aC, aW, glm::vec2(0.0f, 0.0f) - pp, dir, len2, lob, clp, f);
        EasuTap(aC, aW, glm::vec2(-1.0f, 0.0f) - pp, dir, len2, lob, clp, e);
        EasuTap(aC, aW, glm::vec2(1.0f, 1.0f) - pp, dir, len2, lob, clp, k);
        EasuTap(aC, aW, glm::vec2(2.0f, 1.0f) - pp, dir, len2, lob, clp, l);
        EasuTap(aC, aW, glm::vec2(2.0f, 0.0f) - pp, dir, len2, lob, clp, h);
        EasuTap(aC, aW, glm::vec2(1.0f, 0.0f) - pp, dir, len2, lob, clp, g);
        EasuTap(aC, aW, glm::vec2(1.0f, 2.0f) - pp, dir, len2, lob, clp, o);
        EasuTap(aC, aW, glm::vec2(0.0f, 2.0f) - pp, dir, len2, lob, clp, n);

        glm::vec3 proxy = aC / std::max(aW, 1e-6f);
        glm::vec3 mn = glm::min(glm::min(f, g), glm::min(j, k));
        glm::vec3 mx = glm::max(glm::max(f, g), glm::max(j, k));
        proxy = glm::clamp(proxy, mn, mx);
        return FromProxy(proxy);
    }

    Taps Uniform(const glm::vec3& v)
    {
        return Taps{ v, v, v, v, v, v, v, v, v, v, v, v };
    }

    constexpr float kEps = 2e-3f;
} // namespace

// --- proxy tonemap is invertible --------------------------------------------
// The HDR<->[0,1) proxy round-trip must reconstruct the original colour (the
// whole point of running EASU in the bounded proxy and expanding back).
TEST(EASUMath, ProxyTonemapRoundTrips)
{
    for (const glm::vec3 c : { glm::vec3(0.0f), glm::vec3(0.25f), glm::vec3(1.0f),
                               glm::vec3(4.0f, 2.0f, 0.5f), glm::vec3(20.0f) })
    {
        const glm::vec3 back = FromProxy(ToProxy(c));
        EXPECT_NEAR(back.r, c.r, kEps + c.r * 1e-3f);
        EXPECT_NEAR(back.g, c.g, kEps + c.g * 1e-3f);
        EXPECT_NEAR(back.b, c.b, kEps + c.b * 1e-3f);
    }
}

// --- a flat field upsamples to itself ---------------------------------------
// EASU is a normalized directional average; on a uniform neighbourhood it must
// reproduce the input exactly at any sub-pixel position (no DC shift, no tint),
// including HDR values well above 1.0.
TEST(EASUMath, FlatFieldIsIdentity)
{
    const std::array<glm::vec3, 3> colors = { glm::vec3(0.5f), glm::vec3(0.1f, 0.7f, 0.3f), glm::vec3(8.0f, 4.0f, 2.0f) };
    const std::array<glm::vec2, 4> positions = { glm::vec2(0.0f), glm::vec2(0.5f), glm::vec2(0.25f, 0.75f), glm::vec2(0.99f, 0.01f) };
    for (const glm::vec3& col : colors)
    {
        for (const glm::vec2& pp : positions)
        {
            const glm::vec3 out = EasuResolve(Uniform(col), pp);
            EXPECT_NEAR(out.r, col.r, kEps + col.r * 2e-3f) << "flat field shifted r at pp=" << pp.x << "," << pp.y;
            EXPECT_NEAR(out.g, col.g, kEps + col.g * 2e-3f) << "flat field shifted g";
            EXPECT_NEAR(out.b, col.b, kEps + col.b * 2e-3f) << "flat field shifted b";
        }
    }
}

// --- de-ring keeps the result inside the local 2x2 envelope ------------------
// The dering clamp guarantees EASU never overshoots beyond the nearest 2x2
// (f,g,j,k) range — this is what prevents upscale ringing on hard edges, and it
// must hold in HDR (clamp to local min/max, not a [0,1] saturate).
TEST(EASUMath, OutputStaysWithinLocalEnvelope)
{
    // A hard checkerboard edge across the centre 2x2, plus HDR highlights.
    Taps t = Uniform(glm::vec3(0.2f));
    t.f = glm::vec3(6.0f);
    t.k = glm::vec3(6.0f);
    t.g = glm::vec3(0.05f);
    t.j = glm::vec3(0.05f);
    for (const glm::vec2 pp : { glm::vec2(0.5f), glm::vec2(0.2f, 0.8f), glm::vec2(0.9f, 0.1f) })
    {
        const glm::vec3 out = EasuResolve(t, pp);
        // Local envelope in HDR space is [0.05, 6.0] on every channel.
        for (int ch = 0; ch < 3; ++ch)
        {
            EXPECT_GE(out[ch], 0.05f - kEps) << "EASU undershot local min";
            EXPECT_LE(out[ch], 6.0f + kEps) << "EASU overshot local max (ringing)";
            EXPECT_TRUE(std::isfinite(out[ch]));
        }
    }
}

// --- a monotone gradient stays monotone -------------------------------------
// On a smooth horizontal ramp, sampling left-of-centre must be darker than
// right-of-centre (the reconstruction follows the signal, doesn't invert it).
TEST(EASUMath, GradientRemainsOrdered)
{
    // Build a horizontal ramp: columns increase in brightness.
    auto ramp = [](float x)
    { return glm::vec3(x); };
    Taps t;
    // b c (row -1), e f g h (row 0), i j k l (row 1), n o (row 2)
    t.b = ramp(0.30f);
    t.c = ramp(0.50f);
    t.e = ramp(0.10f);
    t.f = ramp(0.30f);
    t.g = ramp(0.50f);
    t.h = ramp(0.70f);
    t.i = ramp(0.10f);
    t.j = ramp(0.30f);
    t.k = ramp(0.50f);
    t.l = ramp(0.70f);
    t.n = ramp(0.30f);
    t.o = ramp(0.50f);

    const glm::vec3 leftBiased = EasuResolve(t, glm::vec2(0.15f, 0.5f));
    const glm::vec3 rightBiased = EasuResolve(t, glm::vec2(0.85f, 0.5f));
    EXPECT_LT(leftBiased.r, rightBiased.r) << "EASU inverted a monotone gradient";
    // and both stay inside the central f..g span (+ envelope tolerance).
    EXPECT_GT(leftBiased.r, 0.10f - kEps);
    EXPECT_LT(rightBiased.r, 0.70f + kEps);
}

// --- preset -> render-scale mapping ------------------------------------------
// The editor dropdown maps each FSR1 preset to its render-scale factor; Off is
// native, and stronger presets render at progressively lower scale.
TEST(EASUMath, PresetRenderScaleOrdering)
{
    EXPECT_FLOAT_EQ(UpscaleModeToRenderScale(UpscaleMode::Off), 1.0f);
    const float q = UpscaleModeToRenderScale(UpscaleMode::Quality);
    const float b = UpscaleModeToRenderScale(UpscaleMode::Balanced);
    const float p = UpscaleModeToRenderScale(UpscaleMode::Performance);
    const float u = UpscaleModeToRenderScale(UpscaleMode::UltraPerformance);
    EXPECT_GT(1.0f, q);
    EXPECT_GT(q, b);
    EXPECT_GT(b, p);
    EXPECT_GT(p, u);
    EXPECT_GT(u, 0.0f);
}

// --- UBO layout contract -----------------------------------------------------
// The CPU EASUUBOData must stay std140-sized to match the GLSL EASUParams block
// at binding 45 (a drift would silently corrupt the input size / texel / bounds).
TEST(EASUMath, UBOLayoutMatchesShader)
{
    EXPECT_EQ(EASUUBOData::GetSize(), 32u);
    EXPECT_EQ(ShaderBindingLayout::UBO_EASU, 45u);
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_EASU, "EASUParams"));
}

// --- sanitizer clamps persisted values ---------------------------------------
// Settings read from scene YAML / save-games must carry a valid enum and a
// finite, in-range RCAS sharpness before they reach the GPU.
TEST(EASUMath, SanitizeClampsUpscale)
{
    PostProcessSettings s;

    s.Upscale = static_cast<UpscaleMode>(99);
    SanitizeUpscale(s);
    EXPECT_EQ(s.Upscale, UpscaleMode::Off);

    s.Upscale = static_cast<UpscaleMode>(-3);
    SanitizeUpscale(s);
    EXPECT_EQ(s.Upscale, UpscaleMode::Off);

    s.Upscale = UpscaleMode::Performance;
    s.RCASSharpness = 9.0f;
    SanitizeUpscale(s);
    EXPECT_EQ(s.Upscale, UpscaleMode::Performance);
    EXPECT_FLOAT_EQ(s.RCASSharpness, 1.0f);

    s.RCASSharpness = std::numeric_limits<f32>::quiet_NaN();
    SanitizeUpscale(s);
    EXPECT_FLOAT_EQ(s.RCASSharpness, 0.5f);
}
