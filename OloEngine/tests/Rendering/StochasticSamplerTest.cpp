// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/BlueNoise.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Rendering/ShaderHarness.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <numbers>
#include <regex>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// The shared stochastic sampler (issue #706) — CPU contract tests.
//
// These pin what StochasticCommon.glsl, TemporalResolve.glsl and PBRCommon's
// VNDF block compute, WITHOUT a GL context, so they run in headless CI.
//
// -----------------------------------------------------------------------------
// EVERY THRESHOLD HERE IS PAIRED WITH A NEGATIVE
// -----------------------------------------------------------------------------
// A sampling test is unusually easy to write vacuously: almost any field passes
// "values are in [0,1] and the mean is about a half", including the white noise
// this whole issue exists to replace. So each property assertion below also
// asserts that the formulation it REPLACED fails the same threshold. If a
// future change quietly degrades the sampler back to white noise, the positive
// assertion alone might still pass — the paired negative is what makes that
// impossible. This is the discipline docs/agent-rules/glsl-shaders.md section 11
// arrived at after the GTAO "goosebumps" regression, where a field that looked
// fine by every summary statistic carried a +0.83 neighbour correlation.
//
// The properties tested, and the artifact each one's failure would produce:
//
//   spectral / neighbour correlation -> low-frequency screen noise a denoiser
//                                       and TAA cannot remove
//   Sobol' mirror == PathSampler     -> two samplers in one engine disagreeing
//                                       about what "stratified" means
//   temporal low-discrepancy         -> a resolve that averages but never
//                                       converges (or a frozen noise pattern)
//   VNDF weight                      -> a silently biased, permanently too-bright
//                                       specular estimator
//   clip-not-clamp                   -> rejected history landing on a colour the
//                                       frame never contained
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    constexpr u32 kN = BlueNoise::kTileSize;
    constexpr double kPi = std::numbers::pi;

    // ---------------------------------------------------------------------
    // GLSL mirror — StochasticCommon.glsl, transcribed
    //
    // These must stay bit-identical to the GLSL. They are also, by
    // construction, identical to PathSampler.h's SamplerDetail — which is the
    // point, and GlslSobolMirrorsPathSampler is what proves it.
    // ---------------------------------------------------------------------
    namespace Glsl
    {
        [[nodiscard]] u32 ReverseBits(u32 x)
        {
            x = (x << 16u) | (x >> 16u);
            x = ((x & 0x55555555u) << 1u) | ((x & 0xAAAAAAAAu) >> 1u);
            x = ((x & 0x33333333u) << 2u) | ((x & 0xCCCCCCCCu) >> 2u);
            x = ((x & 0x0F0F0F0Fu) << 4u) | ((x & 0xF0F0F0F0u) >> 4u);
            x = ((x & 0x00FF00FFu) << 8u) | ((x & 0xFF00FF00u) >> 8u);
            return x;
        }

        [[nodiscard]] u32 LaineKarrasPermutation(u32 x, u32 seed)
        {
            x += seed;
            x ^= x * 0x6c50b47cu;
            x ^= x * 0xb82f1e52u;
            x ^= x * 0xc7afe638u;
            x ^= x * 0x8d22f6e6u;
            return x;
        }

        [[nodiscard]] u32 NestedUniformScramble(u32 x, u32 seed)
        {
            return ReverseBits(LaineKarrasPermutation(ReverseBits(x), seed));
        }

        [[nodiscard]] u32 Sobol0(u32 index)
        {
            return ReverseBits(index);
        }

        [[nodiscard]] u32 Sobol1(u32 index)
        {
            u32 v = 0u;
            u32 direction = 0x80000000u;
            for (u32 i = 0; i < 32u; ++i)
            {
                if (index == 0u)
                    break;
                if ((index & 1u) != 0u)
                    v ^= direction;
                index >>= 1u;
                direction ^= direction >> 1u;
            }
            return v;
        }

        [[nodiscard]] u32 HashCombine(u32 seed, u32 value)
        {
            u32 x = seed ^ (value * 0x9e3779b9u);
            x ^= x >> 16u;
            x *= 0x7feb352du;
            x ^= x >> 15u;
            x *= 0x846ca68bu;
            x ^= x >> 16u;
            return x;
        }

        [[nodiscard]] f32 ToUnitFloat(u32 x)
        {
            return static_cast<f32>(x >> 8u) * (1.0f / 16777216.0f);
        }

        [[nodiscard]] glm::vec2 SobolOwen2D(u32 sampleIndex, u32 seed, u32 dimension)
        {
            const u32 scrambleX = HashCombine(seed, dimension);
            const u32 scrambleY = HashCombine(seed, dimension + 1u);
            const u32 index = NestedUniformScramble(sampleIndex, scrambleX ^ 0x51633e2du);
            return { ToUnitFloat(NestedUniformScramble(Sobol0(index), scrambleX)),
                     ToUnitFloat(NestedUniformScramble(Sobol1(index), scrambleY)) };
        }

        [[nodiscard]] f32 SobolOwen1D(u32 sampleIndex, u32 seed, u32 dimension)
        {
            const u32 scramble = HashCombine(seed, dimension);
            const u32 index = NestedUniformScramble(sampleIndex, scramble ^ 0x51633e2du);
            return ToUnitFloat(NestedUniformScramble(Sobol0(index), scramble));
        }

        // OLO_R2_ALPHA_FX_* / OLO_SOBOL_SCRAMBLE_SEED.
        //
        // FIXED POINT, mirroring the shader exactly. The first version of this
        // mirror used `double` for the advance, which made it strictly MORE
        // accurate than the GLSL it was standing in for -- so it could not have
        // detected the f32 precision collapse the shader had at large frame
        // indices. A mirror that is better than the original tests nothing.
        constexpr u32 kR2FixedX = 3242174889u;
        constexpr u32 kR2FixedY = 2447445414u;
        constexpr u32 kSobolScrambleSeed = 0x9e3779b9u;

        // OloFixedToUnit
        [[nodiscard]] f32 FixedToUnit(u32 fx)
        {
            return static_cast<f32>(fx >> 8u) * (1.0f / 16777216.0f);
        }

        [[nodiscard]] glm::vec2 TileValue(u32 x, u32 y)
        {
            const auto& tile = BlueNoise::GetTileRG();
            const sizet base = (static_cast<sizet>(y & (kN - 1)) * kN + (x & (kN - 1))) * BlueNoise::kChannels;
            return { static_cast<f32>(tile[base + 0]) / 255.0f, static_cast<f32>(tile[base + 1]) / 255.0f };
        }

        [[nodiscard]] double Fract(double v)
        {
            return v - std::floor(v);
        }

        // OloSampleRandomVector2D
        [[nodiscard]] glm::dvec2 SampleRandomVector2D(u32 x, u32 y, u32 frameIndex)
        {
            const glm::vec2 base = TileValue(x, y);
            // The uint multiply wraps mod 2^32 on both sides -- that IS the
            // fractional part, exactly, at any frame index.
            return { Fract(static_cast<double>(base.x) + FixedToUnit(frameIndex * kR2FixedX)),
                     Fract(static_cast<double>(base.y) + FixedToUnit(frameIndex * kR2FixedY)) };
        }

        // OloSampleStratified2D. Dimension 0 is STRATIFIED across the sample
        // index with a blue-noise jitter inside the stratum; dimension 1 is
        // toroidal and takes the ordinary rotation. See the shader for why
        // rotating dimension 0 as well measured worse than the noise it replaced.
        [[nodiscard]] glm::dvec2 SampleStratified2D(u32 x, u32 y, u32 frameIndex, u32 sampleIndex, u32 sampleCount,
                                                    u32 dimension)
        {
            const glm::dvec2 rot = SampleRandomVector2D(x, y, frameIndex);
            const double n = std::max(static_cast<double>(sampleCount), 1.0);
            return { (static_cast<double>(sampleIndex) + rot.x) / n,
                     Fract(static_cast<double>(SobolOwen1D(sampleIndex, kSobolScrambleSeed, dimension)) + rot.y) };
        }
    } // namespace Glsl

    // ---------------------------------------------------------------------
    // Field metrics — "test noise as a field, not as a formula"
    // (docs/agent-rules/glsl-shaders.md section 11)
    // ---------------------------------------------------------------------

    using Field = std::vector<double>; // kN * kN, row-major

    [[nodiscard]] double Mean(const Field& f)
    {
        double s = 0.0;
        for (double v : f)
            s += v;
        return s / static_cast<double>(f.size());
    }

    // Pearson correlation between a pixel and its right/down toroidal
    // neighbours. Blue noise is anti-correlated at lag 1 by construction;
    // white noise sits at ~0; a smooth or index-derived field goes strongly
    // positive (+0.83 in the GTAO regression).
    [[nodiscard]] double NeighbourCorrelation(const Field& f)
    {
        const double m = Mean(f);
        double num = 0.0;
        double den = 0.0;
        for (u32 y = 0; y < kN; ++y)
        {
            for (u32 x = 0; x < kN; ++x)
            {
                const double c = f[static_cast<sizet>(y) * kN + x] - m;
                num += c * (f[static_cast<sizet>(y) * kN + ((x + 1) % kN)] - m);
                num += c * (f[static_cast<sizet>((y + 1) % kN) * kN + x] - m);
                den += 2.0 * c * c;
            }
        }
        return (den > 0.0) ? num / den : 0.0;
    }

    // Mean power over the frequencies with radius in [kMin, kMax]. A direct
    // partial DFT rather than a full spectrum: only two bands are ever needed
    // and this keeps the test well under a second in a Debug build.
    [[nodiscard]] double BandPower(const Field& f, u32 kMin, u32 kMax)
    {
        const double m = Mean(f);
        double total = 0.0;
        u32 bins = 0;

        const auto half = static_cast<i32>(kN / 2);
        for (i32 ky = -half; ky <= half; ++ky)
        {
            for (i32 kx = -half; kx <= half; ++kx)
            {
                const double r = std::sqrt(static_cast<double>(kx * kx + ky * ky));
                if (r < static_cast<double>(kMin) || r > static_cast<double>(kMax))
                    continue;

                double re = 0.0;
                double im = 0.0;
                for (u32 y = 0; y < kN; ++y)
                {
                    for (u32 x = 0; x < kN; ++x)
                    {
                        const double ang = -2.0 * kPi *
                                           (static_cast<double>(kx * static_cast<i32>(x)) / kN +
                                            static_cast<double>(ky * static_cast<i32>(y)) / kN);
                        const double v = f[static_cast<sizet>(y) * kN + x] - m;
                        re += v * std::cos(ang);
                        im += v * std::sin(ang);
                    }
                }
                total += re * re + im * im;
                ++bins;
            }
        }
        return (bins > 0) ? total / bins : 0.0;
    }

    // The single number this whole exercise is about: how much of the field's
    // energy sits in the low spatial frequencies the eye sees and no small
    // filter removes. Blue noise drives this toward zero; white noise leaves
    // it at ~1 (energy is flat across the spectrum).
    [[nodiscard]] double LowFrequencyRatio(const Field& f)
    {
        const double low = BandPower(f, 1, kN / 8);
        const double high = BandPower(f, kN / 4, kN / 2);
        return (high > 0.0) ? low / high : std::numeric_limits<double>::infinity();
    }

    [[nodiscard]] Field BlueChannel(u32 channel)
    {
        const auto& tile = BlueNoise::GetTileRG();
        Field f(static_cast<sizet>(kN) * kN);
        for (sizet i = 0; i < f.size(); ++i)
            f[i] = static_cast<double>(tile[i * BlueNoise::kChannels + channel]) / 255.0;
        return f;
    }

    // The control: the same generator's PRNG used directly, i.e. exactly the
    // "per-pass ad-hoc hash" alternative issue #706 lists.
    [[nodiscard]] Field WhiteField(u32 seed)
    {
        BlueNoise::Detail::SplitMix32 rng(seed);
        Field f(static_cast<sizet>(kN) * kN);
        for (auto& v : f)
            v = static_cast<double>(rng.Next() >> 8) / 16777216.0;
        return f;
    }

    // ---------------------------------------------------------------------
    // GGX VNDF mirror — PBRCommon.glsl
    // ---------------------------------------------------------------------
    namespace Vndf
    {
        [[nodiscard]] double SmithLambda(double NdotX, double alpha)
        {
            const double c = std::clamp(std::abs(NdotX), 1.0e-4, 1.0);
            const double tan2 = (1.0 - c * c) / (c * c);
            return 0.5 * (-1.0 + std::sqrt(1.0 + alpha * alpha * tan2));
        }

        // ggxVNDFWeight — the G2/G1 the estimator needs.
        [[nodiscard]] double Weight(double NdotV, double NdotL, double roughness)
        {
            if (NdotL <= 0.0 || NdotV <= 0.0)
                return 0.0;
            const double alpha = roughness * roughness;
            const double lv = SmithLambda(NdotV, alpha);
            const double ll = SmithLambda(NdotL, alpha);
            return (1.0 + lv) / (1.0 + lv + ll);
        }

        [[nodiscard]] double D(double NdotH, double roughness)
        {
            const double alpha = roughness * roughness;
            const double a2 = alpha * alpha;
            const double d = NdotH * NdotH * (a2 - 1.0) + 1.0;
            return a2 / std::max(kPi * d * d, 1.0e-12);
        }

        [[nodiscard]] double G2(double NdotV, double NdotL, double roughness)
        {
            const double alpha = roughness * roughness;
            return 1.0 / (1.0 + SmithLambda(NdotV, alpha) + SmithLambda(NdotL, alpha));
        }

        // sampleGGXVNDFTangent — tangent space, z = macrosurface normal.
        [[nodiscard]] glm::dvec3 SampleTangent(glm::dvec3 Ve, double alpha, glm::dvec2 Xi)
        {
            const glm::dvec3 Vh = glm::normalize(glm::dvec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
            const double lenSq = Vh.x * Vh.x + Vh.y * Vh.y;
            const glm::dvec3 T1 = (lenSq > 0.0) ? glm::dvec3(-Vh.y, Vh.x, 0.0) / std::sqrt(lenSq)
                                                : glm::dvec3(1.0, 0.0, 0.0);
            const glm::dvec3 T2 = glm::cross(Vh, T1);

            const double r = std::sqrt(Xi.x);
            const double phi = 2.0 * kPi * Xi.y;
            const double t1 = r * std::cos(phi);
            double t2 = r * std::sin(phi);
            const double s = 0.5 * (1.0 + Vh.z);
            t2 = (1.0 - s) * std::sqrt(std::max(0.0, 1.0 - t1 * t1)) + s * t2;

            const glm::dvec3 Nh = t1 * T1 + t2 * T2 + std::sqrt(std::max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
            return glm::normalize(glm::dvec3(alpha * Nh.x, alpha * Nh.y, std::max(0.0, Nh.z)));
        }
    } // namespace Vndf

    // ---------------------------------------------------------------------
    // TemporalResolve.glsl mirror
    // ---------------------------------------------------------------------
    namespace Temporal
    {
        [[nodiscard]] glm::vec3 RGBToYCoCg(glm::vec3 c)
        {
            return { glm::dot(c, glm::vec3(0.25f, 0.5f, 0.25f)), glm::dot(c, glm::vec3(0.5f, 0.0f, -0.5f)),
                     glm::dot(c, glm::vec3(-0.25f, 0.5f, -0.25f)) };
        }

        [[nodiscard]] glm::vec3 YCoCgToRGB(glm::vec3 c)
        {
            return { c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z };
        }

        [[nodiscard]] glm::vec3 ClipToAABB(glm::vec3 history, glm::vec3 boxMin, glm::vec3 boxMax)
        {
            const glm::vec3 centre = 0.5f * (boxMax + boxMin);
            const glm::vec3 halfExtent = 0.5f * (boxMax - boxMin) + glm::vec3(1.0e-6f);
            const glm::vec3 offset = (history - centre) / halfExtent;
            const float maxUnit = std::max(std::abs(offset.x), std::max(std::abs(offset.y), std::abs(offset.z)));
            if (maxUnit <= 1.0f)
                return history;
            return centre + (history - centre) / maxUnit;
        }

        [[nodiscard]] float MotionFeedback(float feedback, glm::vec2 velocityPixels, float deadZonePx,
                                           float saturatePx, float motionFloor)
        {
            const float span = std::max(saturatePx - deadZonePx, 1.0e-4f);
            const float motion = std::clamp((glm::length(velocityPixels) - deadZonePx) / span, 0.0f, 1.0f);
            // min(): motion may only reduce feedback, never raise it.
            return glm::mix(feedback, std::min(feedback, motionFloor), motion);
        }

        [[nodiscard]] glm::vec3 Blend(glm::vec3 current, glm::vec3 clampedHistory, float feedback, float confidence)
        {
            return glm::mix(current, clampedHistory, std::clamp(feedback, 0.0f, 0.98f) * std::clamp(confidence, 0.0f, 1.0f));
        }

        [[nodiscard]] float DepthConfidence(float currentViewDepth, float prevViewDepth, float relativeTolerance)
        {
            const float reference = std::max(currentViewDepth, 1.0e-4f);
            const float relativeError = std::abs(currentViewDepth - prevViewDepth) / reference;
            const float t = std::clamp((relativeError - relativeTolerance) / relativeTolerance, 0.0f, 1.0f);
            return 1.0f - (t * t * (3.0f - 2.0f * t));
        }
    } // namespace Temporal

    // Hammersley, for the brute-force reference integrals.
    [[nodiscard]] glm::dvec2 Hammersley(u32 i, u32 n)
    {
        return { static_cast<double>(i) / static_cast<double>(n),
                 static_cast<double>(Glsl::ReverseBits(i)) * 2.3283064365386963e-10 };
    }
} // namespace

// =============================================================================
// The tile is BLUE — the property everything else rests on
// =============================================================================

// The headline claim: the tile's energy is pushed out of the low spatial
// frequencies. Both channels, and the white-noise control fails the same bar by
// three orders of magnitude — which is what stops this assertion being one that
// any field would pass.
TEST(StochasticSampler, TileSuppressesLowFrequencyEnergy)
{
    for (u32 ch = 0; ch < BlueNoise::kChannels; ++ch)
    {
        const double ratio = LowFrequencyRatio(BlueChannel(ch));
        EXPECT_LT(ratio, 0.05) << "channel " << ch << " carries low-frequency energy — it is not blue noise";
    }

    const double whiteRatio = LowFrequencyRatio(WhiteField(0x5eed1234u));
    EXPECT_GT(whiteRatio, 0.5) << "the white-noise control passed the blue-noise bar — the metric is not "
                                  "discriminating and the assertions above are vacuous";
}

// Lag-1 anti-correlation is the spatial statement of the same property, and the
// one the GTAO regression (glsl-shaders.md section 11) failed at +0.83.
TEST(StochasticSampler, TileNeighboursAreAntiCorrelated)
{
    for (u32 ch = 0; ch < BlueNoise::kChannels; ++ch)
    {
        const double corr = NeighbourCorrelation(BlueChannel(ch));
        EXPECT_LT(corr, -0.1) << "channel " << ch << " neighbour correlation " << corr
                              << " — a blue-noise field must be negative here";
    }

    EXPECT_NEAR(NeighbourCorrelation(WhiteField(0x5eed1234u)), 0.0, 0.06)
        << "the white control should sit at ~0; if it is negative the metric is measuring something else";
}

// The tile must span the full unit range with a centred mean. A field confined
// to a sub-range biases every sample it rotates — the second half of the
// section-11 regression, where a derived channel collapsed into [0, 0.62).
TEST(StochasticSampler, TileSpansTheFullUnitRangeWithACentredMean)
{
    for (u32 ch = 0; ch < BlueNoise::kChannels; ++ch)
    {
        const Field f = BlueChannel(ch);
        const auto [lo, hi] = std::minmax_element(f.begin(), f.end());
        EXPECT_LE(*lo, 0.01) << "channel " << ch << " never reaches 0";
        EXPECT_GE(*hi, 0.99) << "channel " << ch << " never reaches 1";
        EXPECT_NEAR(Mean(f), 0.5, 0.01) << "channel " << ch << " mean is off centre";
    }
}

// The two channels feed the two components of one 2D sample, so they must be
// independent. Deriving the second from the first is the mistake section 11
// documents; this is the assertion that would catch it.
TEST(StochasticSampler, TileChannelsAreIndependent)
{
    const Field r = BlueChannel(0);
    const Field g = BlueChannel(1);
    const double mr = Mean(r);
    const double mg = Mean(g);

    double num = 0.0;
    double dr = 0.0;
    double dg = 0.0;
    for (sizet i = 0; i < r.size(); ++i)
    {
        num += (r[i] - mr) * (g[i] - mg);
        dr += (r[i] - mr) * (r[i] - mr);
        dg += (g[i] - mg) * (g[i] - mg);
    }
    EXPECT_NEAR(num / std::sqrt(dr * dg), 0.0, 0.06) << "the tile's two channels are correlated";
}

// A golden or an A/B captured against this tile is only meaningful if the tile
// is a constant of the engine.
TEST(StochasticSampler, GenerationIsDeterministic)
{
    EXPECT_EQ(BlueNoise::GenerateRankTile(7u), BlueNoise::GenerateRankTile(7u));
    EXPECT_NE(BlueNoise::GenerateRankTile(7u), BlueNoise::GenerateRankTile(8u))
        << "different seeds produced the same tile — the seed is not reaching the generator";
}

// Every rank appears exactly once. A ranking with a hole or a duplicate is not
// a threshold array, and every prefix property downstream of it is void.
TEST(StochasticSampler, RankTileIsAPermutation)
{
    const auto ranks = BlueNoise::GenerateRankTile(1u);
    std::vector<u32> seen(BlueNoise::kTilePixels, 0u);
    for (u32 r : ranks)
    {
        ASSERT_LT(r, BlueNoise::kTilePixels) << "rank out of range";
        ++seen[r];
    }
    EXPECT_EQ(std::count(seen.begin(), seen.end(), 1u), static_cast<i64>(BlueNoise::kTilePixels));
}

// =============================================================================
// The GLSL tile size and the generator's must agree
// =============================================================================

// The shader wraps screen coordinates with OLO_BLUE_NOISE_TILE_MASK. If that
// period disagrees with the texture's, the shader samples a folded tile: still
// noise, no longer blue, and nothing anywhere reports it.
TEST(StochasticSampler, ShaderTileSizeMatchesTheGenerator)
{
    const auto root = Tests::ShaderHarness::ResolveShaderRoot();
    ASSERT_FALSE(root.empty()) << "could not locate OloEditor/assets/shaders";

    const std::string src = Tests::ShaderHarness::ReadWholeFile(root / "include" / "StochasticCommon.glsl");
    ASSERT_FALSE(src.empty()) << "StochasticCommon.glsl is missing or unreadable";

    std::smatch m;
    const std::regex re(R"(#define\s+OLO_BLUE_NOISE_TILE_SIZE\s+(\d+))");
    ASSERT_TRUE(std::regex_search(src, m, re)) << "OLO_BLUE_NOISE_TILE_SIZE not found in the shader header";
    EXPECT_EQ(static_cast<u32>(std::stoul(m[1].str())), BlueNoise::kTileSize)
        << "the shader's tile period and BlueNoise::kTileSize have drifted";
}

// =============================================================================
// The Sobol' half is the SAME sequence the offline path tracer uses
// =============================================================================

// The engine now has two samplers. They are allowed to differ in what they
// optimise; they are not allowed to disagree about the sequence. This is the
// assertion that makes "mirror of PathSampler.h" a fact rather than a comment.
TEST(StochasticSampler, GlslSobolMirrorsPathSampler)
{
    for (u32 seed : { 0u, 1u, 0x9e3779b9u, 0xdeadbeefu })
    {
        for (u32 i = 0; i < 64; ++i)
        {
            PathTracing::PathSampler cpu(seed, i);
            const glm::vec2 expected2D = cpu.Get2D();
            const glm::vec2 actual2D = Glsl::SobolOwen2D(i, seed, 0u);
            EXPECT_FLOAT_EQ(actual2D.x, expected2D.x) << "seed " << seed << " sample " << i << " (x)";
            EXPECT_FLOAT_EQ(actual2D.y, expected2D.y) << "seed " << seed << " sample " << i << " (y)";

            PathTracing::PathSampler cpu1D(seed, i);
            EXPECT_FLOAT_EQ(Glsl::SobolOwen1D(i, seed, 0u), cpu1D.Get1D()) << "seed " << seed << " sample " << i;
        }
    }
}

// The mirror above lives in this file, so it could drift from the .glsl the GPU
// actually runs without anything noticing. Pin the magic numbers by reading
// them out of the shader source — those constants ARE the sequence.
TEST(StochasticSampler, ShaderCarriesThePathSamplerConstants)
{
    const auto root = Tests::ShaderHarness::ResolveShaderRoot();
    ASSERT_FALSE(root.empty());
    const std::string src = Tests::ShaderHarness::ReadWholeFile(root / "include" / "StochasticCommon.glsl");
    ASSERT_FALSE(src.empty());

    // The Laine-Karras multipliers, the HashCombine constants, the index
    // scramble salt, and the 24-bit ToUnitFloat divisor. Every one of these
    // appears verbatim in PathSampler.h.
    for (const char* needle : { "0x6c50b47cu", "0xb82f1e52u", "0xc7afe638u", "0x8d22f6e6u", "0x9e3779b9u",
                                "0x7feb352du", "0x846ca68bu", "0x51633e2du", "16777216.0" })
    {
        EXPECT_NE(src.find(needle), std::string::npos)
            << "StochasticCommon.glsl no longer contains " << needle
            << " — the GPU sequence has drifted from PathSampler.h";
    }
}

// =============================================================================
// The combined sampler: stratified per pixel, blue across the screen
// =============================================================================

// The claim that matters at runtime. Integrate a smooth function once per pixel
// and look at the ERROR IMAGE: with the shared sampler its low-frequency
// content is far below what the same estimator produces from white noise. This
// is the property that makes the same ray budget look cleaner — not the values
// themselves, which are uniform either way.
TEST(StochasticSampler, SingleSampleErrorIsBlueInScreenSpace)
{
    // f(u) = cos(2 pi u.x) * u.y over the unit square; the exact integral is 0.
    const auto f = [](glm::dvec2 u)
    { return std::cos(2.0 * kPi * u.x) * u.y; };

    Field blueError(static_cast<sizet>(kN) * kN);
    Field whiteError(static_cast<sizet>(kN) * kN);
    BlueNoise::Detail::SplitMix32 rng(0xa5a5a5a5u);

    for (u32 y = 0; y < kN; ++y)
    {
        for (u32 x = 0; x < kN; ++x)
        {
            const sizet i = static_cast<sizet>(y) * kN + x;
            blueError[i] = f(Glsl::SampleStratified2D(x, y, 3u, 0u, 1u, 0u));

            const glm::dvec2 w{ static_cast<double>(rng.Next() >> 8) / 16777216.0,
                                static_cast<double>(rng.Next() >> 8) / 16777216.0 };
            whiteError[i] = f(w);
        }
    }

    const double blueRatio = LowFrequencyRatio(blueError);
    const double whiteRatio = LowFrequencyRatio(whiteError);

    // Measured 0.24 (blue) vs 1.01 (white) — a 4.2x reduction. The gap is far
    // narrower than the raw tile's 0.0002 because f is a NONLINEAR function of
    // the sample, which folds some energy back into the low frequencies. 4.1x is
    // the honest figure for what an actual estimator sees; the tile's 5000x is
    // not.
    EXPECT_LT(blueRatio, 0.40) << "the single-sample error image is not blue in screen space";
    EXPECT_GT(whiteRatio, 0.5) << "the white-noise control is not white — the comparison below means nothing";
    EXPECT_LT(blueRatio, whiteRatio * 0.5)
        << "blue " << blueRatio << " vs white " << whiteRatio
        << " — the sampler no longer beats ad-hoc hashing, which is the entire reason it exists";
}

// DIMENSION 0 MUST STAY STRATIFIED. Sample i of N has to land inside stratum
// i — [i/N, (i+1)/N) — at every pixel.
//
// This is the assertion that was missing when this header first shipped a
// Cranley-Patterson rotation on BOTH dimensions. That is the textbook
// construction and it is correct on a torus; it is wrong here because the
// caller feeds dimension 0 to a hemisphere RADIUS, which does not wrap. The
// rotated form measured WORSE than the interleaved-gradient noise it replaced,
// on the CPU model and in the rendered frame alike, and nothing in the suite
// objected. Now something does.
TEST(StochasticSampler, DimensionZeroStaysStratifiedAcrossSamples)
{
    constexpr u32 frame = 11u;
    constexpr u32 rays = 8u;
    for (const glm::uvec2 px : { glm::uvec2(3u, 5u), glm::uvec2(40u, 17u), glm::uvec2(63u, 63u) })
    {
        for (u32 i = 0; i < rays; ++i)
        {
            const double u1 = Glsl::SampleStratified2D(px.x, px.y, frame, i, rays, 0u).x;
            const double lo = static_cast<double>(i) / rays;
            const double hi = static_cast<double>(i + 1) / rays;
            EXPECT_GE(u1, lo) << "pixel (" << px.x << "," << px.y << ") sample " << i << " below its stratum";
            EXPECT_LT(u1, hi) << "pixel (" << px.x << "," << px.y << ") sample " << i << " above its stratum";
        }
    }

    // The paired negative: the rotate-both formulation this replaced does NOT
    // satisfy the above, so the assertions are not vacuous.
    u32 outside = 0;
    for (u32 i = 0; i < rays; ++i)
    {
        const glm::vec2 lds = Glsl::SobolOwen2D(i, Glsl::kSobolScrambleSeed, 0u);
        const glm::dvec2 rot = Glsl::SampleRandomVector2D(3u, 5u, frame);
        const double rotated = Glsl::Fract(static_cast<double>(lds.x) + rot.x);
        if (rotated < static_cast<double>(i) / rays || rotated >= static_cast<double>(i + 1) / rays)
            ++outside;
    }
    EXPECT_GT(outside, rays / 2u)
        << "the rotated formulation happened to stay stratified, so this test cannot detect the regression "
           "it was written for";
}

// DIMENSION 1 is the toroidal one, and there every pixel must walk the SAME
// sequence offset by its tile rotation — that is what makes the residual error
// blue across the screen rather than mutually white.
TEST(StochasticSampler, DimensionOneIsOneSequenceOffsetByTheTile)
{
    constexpr u32 frame = 11u;
    for (u32 sample = 0; sample < 8; ++sample)
    {
        const glm::dvec2 a = Glsl::SampleStratified2D(3u, 5u, frame, sample, 8u, 0u);
        const glm::dvec2 b = Glsl::SampleStratified2D(40u, 17u, frame, sample, 8u, 0u);
        const glm::dvec2 ra = Glsl::SampleRandomVector2D(3u, 5u, frame);
        const glm::dvec2 rb = Glsl::SampleRandomVector2D(40u, 17u, frame);

        EXPECT_NEAR(Glsl::Fract(a.y - ra.y), Glsl::Fract(b.y - rb.y), 1e-9) << "sample " << sample;
    }
}

// Over frames, one pixel must see a LOW-DISCREPANCY sequence — that is what
// lets a temporal resolve converge rather than merely average. Discrepancy is
// measured as the largest gap between consecutive sorted values: the R2 advance
// keeps it near the ideal 1/N, white noise leaves gaps several times larger.
//
// EXERCISED THROUGH OloSampleStratified2D, because that is the entry point SSR
// and SSGI actually call. An earlier version of this test pinned
// OloSampleRandomVector2D instead — which no shader calls — and so passed
// happily while the function in production re-seeded its Sobol' scramble from
// frameIndex and was i.i.d. white in time (measured 4.3x the ideal gap). Pin
// the call site, not the helper underneath it.
TEST(StochasticSampler, FrameAdvanceIsLowDiscrepancyPerPixel)
{
    constexpr u32 frames = 64;

    const auto largestGap = [](std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        double gap = v.front(); // from 0
        for (sizet i = 1; i < v.size(); ++i)
            gap = std::max(gap, v[i] - v[i - 1]);
        return std::max(gap, 1.0 - v.back()); // to 1
    };

    std::vector<double> r2;
    std::vector<double> white;
    BlueNoise::Detail::SplitMix32 rng(0x1234abcdu);
    for (u32 frame = 0; frame < frames; ++frame)
    {
        r2.push_back(Glsl::SampleStratified2D(9u, 13u, frame, 0u, 1u, 0u).x);
        white.push_back(static_cast<double>(rng.Next() >> 8) / 16777216.0);
    }

    const double ideal = 1.0 / frames;
    EXPECT_LT(largestGap(r2), ideal * 2.5) << "the per-pixel temporal sequence has clumped — a resolve accumulating "
                                              "it would average without converging";
    EXPECT_GT(largestGap(white), ideal * 3.0)
        << "the white control is unexpectedly well spread; the threshold above proves nothing";
}

// The advance is computed in 32-bit FIXED POINT precisely so it does not decay
// with uptime. The natural `float(frameIndex) * alpha` loses the fraction's low
// bits as the product grows: measured against the real constants, frame 262144
// (~73 min at 60 Hz) resolves only 64 distinct offsets and the 2^20 counter
// ceiling only 16 — under the tile's own 256 levels, so the sampler quietly
// stops advancing properly after about an hour.
//
// Assert the fixed-point form still spans the range near the ceiling, AND that
// the f32 form does not, so this cannot pass on the formulation it replaced.
TEST(StochasticSampler, FrameAdvanceStaysExactNearTheCounterCeiling)
{
    constexpr u32 base = 1u << 20;
    constexpr u32 frames = 256;

    std::set<u32> fixedPoint;
    std::set<u32> naiveFloat;
    for (u32 i = 0; i < frames; ++i)
    {
        const u32 frame = base - frames + i;

        // What the shader does.
        fixedPoint.insert(static_cast<u32>(Glsl::FixedToUnit(frame * Glsl::kR2FixedX) * 256.0f));

        // What it would do with the obvious formulation.
        const float product = static_cast<float>(frame) * 0.75487766624669276005f;
        naiveFloat.insert(static_cast<u32>((product - std::floor(product)) * 256.0f));
    }

    EXPECT_GT(fixedPoint.size(), 200u) << "the fixed-point advance collapsed near the counter ceiling";
    EXPECT_LT(naiveFloat.size(), 32u)
        << "the naive f32 advance did NOT collapse, so this test cannot detect the precision loss it "
           "was written for";
}

// A frozen frame index is the failure mode that looks like working code: the
// sampler still returns good values, they just never change, so the temporal
// resolve accumulates one still image's worth of noise forever. Assert the
// field genuinely moves from frame to frame.
TEST(StochasticSampler, ConsecutiveFramesRedrawTheField)
{
    Field a(static_cast<sizet>(kN) * kN);
    Field b(static_cast<sizet>(kN) * kN);
    for (u32 y = 0; y < kN; ++y)
    {
        for (u32 x = 0; x < kN; ++x)
        {
            const sizet i = static_cast<sizet>(y) * kN + x;
            a[i] = Glsl::SampleRandomVector2D(x, y, 5u).x;
            b[i] = Glsl::SampleRandomVector2D(x, y, 6u).x;
        }
    }

    const double ma = Mean(a);
    const double mb = Mean(b);
    double num = 0.0;
    double da = 0.0;
    double db = 0.0;
    for (sizet i = 0; i < a.size(); ++i)
    {
        num += (a[i] - ma) * (b[i] - mb);
        da += (a[i] - ma) * (a[i] - ma);
        db += (b[i] - mb) * (b[i] - mb);
    }
    const double corr = num / std::sqrt(da * db);
    EXPECT_LT(std::abs(corr), 0.5) << "consecutive frames correlate at " << corr
                                   << " — temporal averaging cannot remove a field that only slides";

    // ...and each frame is still blue. A temporal advance that destroyed the
    // spatial property would trade one artifact for another.
    // Measured 0.057, against 0.0002 for the raw tile and ~1.0 for white: the
    // value-space wrap in fract(tile + k) costs some of the spatial property
    // (see the note in StochasticCommon.glsl), so the bar here is deliberately
    // looser than TileSuppressesLowFrequencyEnergy's and still an order of
    // magnitude clear of white noise.
    EXPECT_LT(LowFrequencyRatio(b), 0.15) << "the R2 advance broke the spatial blue-noise property";
}

// =============================================================================
// GGX VNDF — the part where a mistake is invisible
// =============================================================================

TEST(StochasticSampler, VndfSamplesAreUnitAndAboveTheHorizon)
{
    constexpr u32 n = 4096;
    const glm::dvec3 Ve = glm::normalize(glm::dvec3(0.6, 0.2, 0.4));
    for (u32 i = 0; i < n; ++i)
    {
        const glm::dvec3 H = Vndf::SampleTangent(Ve, 0.25, Hammersley(i, n));
        EXPECT_NEAR(glm::length(H), 1.0, 1e-9) << "sample " << i;
        EXPECT_GE(H.z, 0.0) << "sample " << i << " points below the macrosurface";
    }
}

TEST(StochasticSampler, VndfWeightIsAValidRatioAndVanishesBelowTheHorizon)
{
    EXPECT_DOUBLE_EQ(Vndf::Weight(0.5, -0.1, 0.4), 0.0);
    EXPECT_DOUBLE_EQ(Vndf::Weight(-0.1, 0.5, 0.4), 0.0);
    for (double roughness : { 0.05, 0.3, 0.6, 1.0 })
    {
        for (double NdotV : { 0.05, 0.4, 0.99 })
        {
            for (double NdotL : { 0.05, 0.4, 0.99 })
            {
                const double w = Vndf::Weight(NdotV, NdotL, roughness);
                EXPECT_GT(w, 0.0);
                EXPECT_LE(w, 1.0 + 1e-12) << "G2/G1 exceeded 1 — masking cannot increase visibility";
            }
        }
    }
}

// THE bias test. VNDF-sample the specular lobe, weight by G2/G1, and compare the
// mean against a brute-force hemisphere integration of the same BRDF. They must
// agree — and the UNWEIGHTED mean, which is what dropping the weight would
// produce, must NOT. Without that second half this test would pass on the very
// mistake it exists to catch.
TEST(StochasticSampler, VndfEstimatorMatchesBruteForce)
{
    constexpr u32 nSamples = 200000;
    constexpr u32 nReference = 400000;

    for (double roughness : { 0.4, 0.7 })
    {
        for (double NdotV : { 0.9, 0.35 })
        {
            const double alpha = roughness * roughness;
            const glm::dvec3 V(std::sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
            const glm::dvec3 N(0.0, 0.0, 1.0);

            // VNDF estimator: E[G2/G1].
            double weighted = 0.0;
            double unweighted = 0.0;
            for (u32 i = 0; i < nSamples; ++i)
            {
                const glm::dvec3 H = Vndf::SampleTangent(V, alpha, Hammersley(i, nSamples));
                const glm::dvec3 L = glm::reflect(-V, H);
                if (L.z <= 0.0)
                    continue; // below the horizon: weight is 0, and so is the contribution
                weighted += Vndf::Weight(NdotV, L.z, roughness);
                unweighted += 1.0;
            }
            weighted /= nSamples;
            unweighted /= nSamples;

            // Reference: uniform hemisphere integration of D * G2 / (4 N.V),
            // i.e. the single-scattering GGX directional albedo with F = 1.
            double reference = 0.0;
            for (u32 i = 0; i < nReference; ++i)
            {
                const glm::dvec2 u = Hammersley(i, nReference);
                const double cosTheta = u.x; // uniform over [0,1]
                const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
                const double phi = 2.0 * kPi * u.y;
                const glm::dvec3 L(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
                const glm::dvec3 H = glm::normalize(V + L);
                if (H.z <= 0.0)
                    continue;
                reference += Vndf::D(H.z, roughness) * Vndf::G2(NdotV, L.z, roughness) / (4.0 * NdotV);
            }
            reference *= 2.0 * kPi / nReference; // uniform hemisphere pdf = 1/(2 pi)

            // Measured agreement is to five decimal places; 2% is headroom for
            // platform float variance, not slack in the claim.
            EXPECT_NEAR(weighted, reference, 0.02 * std::max(reference, 0.05))
                << "roughness " << roughness << ", N.V " << NdotV
                << ": the VNDF estimator disagrees with brute force — the sample or its weight is wrong";

            // The paired negative. G2/G1 <= 1 always, so dropping it can only
            // ever overestimate — assert the direction everywhere...
            EXPECT_GT(unweighted, weighted)
                << "roughness " << roughness << ", N.V " << NdotV
                << ": dropping the G2/G1 weight did NOT change the answer, so this test cannot detect the "
                   "one mistake it was written for";

            // ...and the MAGNITUDE where it is large. Measured: +17% to +19% at
            // roughness 0.7, but only +1% to +7% at 0.4. That gradient is the
            // reason the mistake survives review — on smooth surfaces the weight
            // is nearly 1 and omitting it looks like it works.
            if (roughness >= 0.7)
            {
                EXPECT_GT(unweighted, reference * 1.15)
                    << "roughness " << roughness << ", N.V " << NdotV
                    << ": the unweighted estimator is no longer clearly biased on a rough surface";
            }
        }
    }
}

// =============================================================================
// Temporal resolve
// =============================================================================

TEST(StochasticSampler, YCoCgRoundTrips)
{
    for (glm::vec3 c : { glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.2f, 0.7f, 0.4f), glm::vec3(3.5f, 0.1f, 2.2f) })
    {
        const glm::vec3 back = Temporal::YCoCgToRGB(Temporal::RGBToYCoCg(c));
        EXPECT_NEAR(back.x, c.x, 1e-5f);
        EXPECT_NEAR(back.y, c.y, 1e-5f);
        EXPECT_NEAR(back.z, c.z, 1e-5f);
    }
}

TEST(StochasticSampler, ClipLeavesAnInsideHistoryUntouched)
{
    const glm::vec3 h(0.4f, 0.5f, 0.6f);
    EXPECT_EQ(Temporal::ClipToAABB(h, glm::vec3(0.0f), glm::vec3(1.0f)), h);
}

// The property a componentwise clamp does NOT have: the clipped point stays on
// the segment joining the history to the box centre, so a rejected history
// desaturates toward a colour the neighbourhood actually contains instead of
// sliding to a corner it never did.
TEST(StochasticSampler, ClipStaysOnTheSegmentToTheBoxCentre)
{
    const glm::vec3 boxMin(0.0f, 0.0f, 0.0f);
    const glm::vec3 boxMax(1.0f, 0.4f, 1.0f);
    const glm::vec3 centre = 0.5f * (boxMin + boxMax);
    const glm::vec3 history(3.0f, 2.0f, -1.0f); // well outside, violating several axes

    const glm::vec3 clipped = Temporal::ClipToAABB(history, boxMin, boxMax);

    // Collinear with (centre -> history): the cross product of the two offsets
    // must vanish.
    const glm::vec3 cross = glm::cross(clipped - centre, history - centre);
    EXPECT_NEAR(glm::length(cross), 0.0f, 1e-4f) << "the clip left the segment — this is a clamp, not a clip";

    // And it moved strictly toward the centre.
    EXPECT_LT(glm::length(clipped - centre), glm::length(history - centre));

    // A componentwise clamp would land somewhere else entirely; assert that so
    // the collinearity check above cannot pass on a clamp by accident.
    const glm::vec3 clamped = glm::clamp(history, boxMin, boxMax);
    EXPECT_GT(glm::length(clamped - clipped), 0.1f)
        << "clip and clamp agreed on a case chosen to separate them — the test is not exercising the difference";
}

// The sub-pixel dead zone. Any jittered pass moves ~1px per frame by
// construction; without the dead zone a stationary camera reads as motion,
// feedback collapses, and the resolve visibly shakes.
TEST(StochasticSampler, MotionFeedbackIgnoresSubPixelJitter)
{
    constexpr float feedback = 0.9f;
    constexpr float floorValue = 0.5f;
    EXPECT_FLOAT_EQ(Temporal::MotionFeedback(feedback, glm::vec2(0.0f), 1.0f, 5.0f, floorValue), feedback);
    EXPECT_FLOAT_EQ(Temporal::MotionFeedback(feedback, glm::vec2(0.9f, 0.0f), 1.0f, 5.0f, floorValue), feedback)
        << "sub-pixel motion reduced feedback — this is the TAA shake";

    // Real motion does reduce it, all the way to the floor.
    EXPECT_LT(Temporal::MotionFeedback(feedback, glm::vec2(3.0f, 0.0f), 1.0f, 5.0f, floorValue), feedback);
    EXPECT_FLOAT_EQ(Temporal::MotionFeedback(feedback, glm::vec2(50.0f, 0.0f), 1.0f, 5.0f, floorValue), floorValue);
}

// motionFloor is a FLOOR. A caller who deliberately picks a low feedback (the
// editor slider goes down to 0.0) must not have it RAISED by motion — which is
// what mixing toward a bare constant does, and what the inline TAA code this was
// lifted from did. TAA's own 0.9/0.5 pairing never hit it; a shared header would
// have carried it to every future caller.
TEST(StochasticSampler, MotionNeverRaisesFeedback)
{
    constexpr float lowFeedback = 0.2f;
    constexpr float floorValue = 0.5f;
    for (float speed : { 0.0f, 2.0f, 3.5f, 50.0f })
    {
        const float out = Temporal::MotionFeedback(lowFeedback, glm::vec2(speed, 0.0f), 1.0f, 5.0f, floorValue);
        EXPECT_LE(out, lowFeedback + 1e-6f)
            << "motion at " << speed << " px raised feedback from " << lowFeedback << " to " << out;
    }
}

// Confidence 0 means "there is no history for this pixel", and the only correct
// answer is the current frame exactly — not a mostly-current blend.
TEST(StochasticSampler, ZeroConfidenceReturnsTheCurrentFrameExactly)
{
    const glm::vec3 current(0.25f, 0.5f, 0.75f);
    const glm::vec3 history(9.0f, -3.0f, 4.0f);
    const glm::vec3 out = Temporal::Blend(current, history, 0.95f, 0.0f);
    EXPECT_FLOAT_EQ(out.x, current.x);
    EXPECT_FLOAT_EQ(out.y, current.y);
    EXPECT_FLOAT_EQ(out.z, current.z);
}

// The disocclusion test must be scale-free: the same relative depth step has to
// read the same at 5 units and at 5000, or the rejection only works at one
// distance — which is worse than not testing at all, because it looks like it
// works while you are standing near the thing you tested it on.
TEST(StochasticSampler, DepthConfidenceIsRelativeNotAbsolute)
{
    constexpr float tolerance = 0.02f;
    // (nearConfidence / farConfidence, not near / far — windows.h defines both
    // as macros and the expansion is an unhelpful parse error.)
    const float nearConfidence = Temporal::DepthConfidence(5.0f, 5.0f * 1.05f, tolerance);
    const float farConfidence = Temporal::DepthConfidence(5000.0f, 5000.0f * 1.05f, tolerance);
    EXPECT_NEAR(nearConfidence, farConfidence, 1e-5f) << "the same relative depth step gave different confidence at different ranges";

    EXPECT_FLOAT_EQ(Temporal::DepthConfidence(10.0f, 10.0f, tolerance), 1.0f) << "identical depth must be full confidence";
    EXPECT_FLOAT_EQ(Temporal::DepthConfidence(10.0f, 20.0f, tolerance), 0.0f) << "a doubled depth must be fully rejected";
}

// =============================================================================
// The binding-slot constraint
// =============================================================================

// TEX_BLUE_NOISE (17) is also UBO_FOG and SSBO_INSTANCE_DRAW_INDIRECT. That is
// fine across GL's disjoint namespaces, and fine on Vulkan too — except inside
// ONE shader, where the single-set model makes it a real collision. It is the
// same trap that moved TEX_DDGI_VISIBILITY off 57 in issue #691 (ADR item A2),
// and it was found there by a shader that pulled vertices while including a
// header. So check the tree rather than trusting anyone to remember.
TEST(StochasticSampler, NoShaderCollidesWithTheBlueNoiseSlot)
{
    namespace fs = std::filesystem;
    const auto root = Tests::ShaderHarness::ResolveShaderRoot();
    ASSERT_FALSE(root.empty());

    // Transitively collect a shader's own text plus everything it includes.
    const std::regex includeRe(R"(#include\s+\"([^\"]+)\")");
    const std::function<void(const fs::path&, std::string&, std::vector<std::string>&)> collect =
        [&](const fs::path& file, std::string& out, std::vector<std::string>& visited)
    {
        const std::string key = file.generic_string();
        if (std::find(visited.begin(), visited.end(), key) != visited.end())
            return;
        visited.push_back(key);

        const std::string src = Tests::ShaderHarness::ReadWholeFile(file);
        if (src.empty())
            return;
        out += src;

        for (auto it = std::sregex_iterator(src.begin(), src.end(), includeRe); it != std::sregex_iterator(); ++it)
        {
            const std::string rel = (*it)[1].str();
            // Includes are written either root-relative ("include/Foo.glsl") or
            // relative to the including file ("Foo.glsl" from inside include/).
            for (const fs::path candidate : { root / rel, file.parent_path() / rel })
            {
                std::error_code ec;
                if (fs::exists(candidate, ec))
                {
                    collect(candidate, out, visited);
                    break;
                }
            }
        }
    };

    // Every `layout(... binding = 17 ...) [qualifiers] uniform|buffer <name>`.
    // A sampler/image declared that way is the one we are deliberately adding;
    // anything else at 17 is a uniform or storage BLOCK, i.e. the collision.
    const std::regex blockAt17(
        "layout[^)]*binding[ \t]*=[ \t]*17[^)]*\\)[ \t\r\n]*"
        "(?:(?:readonly|writeonly|restrict|coherent|volatile)[ \t\r\n]+)*"
        "(uniform|buffer)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)");

    u32 checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".glsl" && ext != ".comp")
            continue;

        const std::string own = Tests::ShaderHarness::ReadWholeFile(entry.path());
        if (own.find("OLO_BLUE_NOISE_GLOBAL_SAMPLER") == std::string::npos)
            continue; // this shader does not sample the tile

        ++checked;

        std::string full;
        std::vector<std::string> visited;
        collect(entry.path(), full, visited);

        for (auto it = std::sregex_iterator(full.begin(), full.end(), blockAt17); it != std::sregex_iterator(); ++it)
        {
            const std::string keyword = (*it)[1].str();
            const std::string typeName = (*it)[2].str();
            const bool isOpaque = typeName.rfind("sampler", 0) == 0 || typeName.rfind("image", 0) == 0 ||
                                  typeName.rfind("texture", 0) == 0 || typeName.rfind("usampler", 0) == 0 ||
                                  typeName.rfind("isampler", 0) == 0;
            if (keyword == "uniform" && isOpaque)
                continue; // the blue-noise sampler itself

            ADD_FAILURE() << entry.path().filename().string() << " samples TEX_BLUE_NOISE (17) and also declares a "
                          << keyword << " block at binding 17 (" << typeName
                          << "). On Vulkan's single-set model that is a real within-shader collision — see ADR "
                             "item A2.";
        }
    }

    EXPECT_GT(checked, 0u) << "no shader opts into the blue-noise sampler — this test is passing vacuously";
}

// The slot number itself, so a renumber has to come here and think.
TEST(StochasticSampler, BlueNoiseSlotIsWhereTheShaderExpects)
{
    EXPECT_EQ(ShaderBindingLayout::TEX_BLUE_NOISE, 17u);
    // Taking a slot BELOW the shader-graph base is what keeps
    // MAX_ENGINE_TEXTURE_SLOTS — and therefore HEAP_IMAGE_SLOT_BASE — where it
    // was. Issue #702's drift came from a slot added above it.
    EXPECT_LT(ShaderBindingLayout::TEX_BLUE_NOISE, ShaderBindingLayout::TEX_SHADER_GRAPH_0);
}
