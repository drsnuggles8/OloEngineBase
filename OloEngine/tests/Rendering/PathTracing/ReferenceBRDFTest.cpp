// OLO_TEST_LAYER: L1
// =============================================================================
// ReferenceBRDFTest.cpp — contract tests for the C++ mirror of PBRCommon.glsl's
// BRDF (issue #709).
//
// The reference path tracer is only an oracle if its BRDF is the engine's BRDF.
// There are two guards, and they catch different things:
//
//   * THIS file pins the C++ port's mathematical PROPERTIES — the ones an
//     integrator silently depends on (a normalized NDF, a positive PDF wherever
//     the BRDF is non-zero, sampling that actually reproduces the density it
//     claims). A port can be a faithful transcription of the GLSL and still be
//     unusable if, say, PdfGGX has the reflection Jacobian upside down; nothing
//     in a GLSL-vs-C++ diff would notice, because the GLSL has no PDF at all.
//
//   * ReferenceBRDFGpuParityTest pins the port against the REAL COMPILED
//     SHADER, texel for texel. That is the anti-drift guard.
//
// Neither subsumes the other. This one runs headless; that one needs a GPU.
//
// Classification: L1 (pure CPU math, no GL).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // Deterministic stratified 2D point set (Hammersley), so every estimate
        // below is reproducible and its error is a fixed number rather than a
        // flaky one.
        [[nodiscard]] glm::vec2 Hammersley(u32 i, u32 n)
        {
            return glm::vec2(static_cast<f32>(i) / static_cast<f32>(n),
                             SamplerDetail::ToUnitFloat(SamplerDetail::ReverseBits(i)));
        }

        // Uniform-hemisphere Monte Carlo estimate of the directional albedo
        //     integral of f(l, v) * (n . l) dw
        // with n = v = +Z. This is the SAME estimator PbrFurnaceProbe.glsl runs
        // on the GPU (mean of the three channels, pdf = 1/2pi), which is what
        // makes the two numbers comparable rather than merely similar.
        [[nodiscard]] f64 FurnaceIntegral(f32 roughness, f32 metallic, const glm::vec3& albedo, u32 sampleCount)
        {
            const glm::vec3 n(0.0f, 0.0f, 1.0f);
            const glm::vec3 v(0.0f, 0.0f, 1.0f);

            f64 sum = 0.0;
            for (u32 i = 0; i < sampleCount; ++i)
            {
                const glm::vec2 xi = Hammersley(i, sampleCount);
                const f32 cosTheta = xi.x;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const f32 phi = kTwoPi * xi.y;
                const glm::vec3 l(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

                const glm::vec3 f = CookTorranceBRDF(n, v, l, albedo, metallic, roughness);
                sum += static_cast<f64>((f.x + f.y + f.z) / 3.0f) * static_cast<f64>(cosTheta) * static_cast<f64>(kTwoPi);
            }
            return sum / static_cast<f64>(sampleCount);
        }
    } // namespace

    // =========================================================================
    // Fresnel — the two endpoints PbrFresnelTest asserts on the GPU, asserted
    // here on the port so a divergence is localized to one of the two.
    // =========================================================================
    TEST(ReferenceBRDF, FresnelNormalIncidenceEqualsF0)
    {
        for (u32 i = 0; i <= 20; ++i)
        {
            const f32 f0 = static_cast<f32>(i) / 20.0f;
            const glm::vec3 result = FresnelSchlick(1.0f, glm::vec3(f0));
            EXPECT_NEAR(result.x, f0, 1e-6f) << "F0 = " << f0;
        }
    }

    TEST(ReferenceBRDF, FresnelGrazingApproachesOne)
    {
        for (u32 i = 0; i <= 20; ++i)
        {
            const f32 f0 = static_cast<f32>(i) / 20.0f;
            const glm::vec3 result = FresnelSchlick(0.0f, glm::vec3(f0));
            EXPECT_NEAR(result.x, 1.0f, 1e-6f) << "F0 = " << f0;
        }
    }

    // =========================================================================
    // GGX NDF normalization: integral of D(h) * (n . h) dw over the hemisphere
    // must be 1.
    //
    // This is the single most load-bearing property for an importance-sampling
    // integrator: PdfGGX is derived from D directly, so a D that does not
    // integrate to 1 makes every specular estimate wrong by a constant factor
    // that no image-space eyeball test would ever catch.
    //
    // The engine's D is EPSILON-clamped in its denominator, which suppresses
    // the peak at low roughness — so the identity only holds where that clamp
    // is inactive, i.e. above roughness ~0.3 (see ReferenceBRDF.h's fidelity
    // notes). The clamped regime is asserted separately below, as a bound
    // rather than an identity, because reproducing the clamp is the point.
    // =========================================================================
    TEST(ReferenceBRDF, GgxNdfIntegratesToOneAwayFromTheEpsilonClamp)
    {
        constexpr u32 kSamples = 1u << 18;
        const glm::vec3 n(0.0f, 0.0f, 1.0f);

        for (const f32 roughness : { 0.35f, 0.5f, 0.7f, 1.0f })
        {
            f64 sum = 0.0;
            for (u32 i = 0; i < kSamples; ++i)
            {
                const glm::vec2 xi = Hammersley(i, kSamples);
                const f32 cosTheta = xi.x;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const f32 phi = kTwoPi * xi.y;
                const glm::vec3 h(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

                sum += static_cast<f64>(DistributionGGX(n, h, roughness)) * static_cast<f64>(cosTheta) * static_cast<f64>(kTwoPi);
            }
            const f64 integral = sum / static_cast<f64>(kSamples);
            EXPECT_NEAR(integral, 1.0, 0.02) << "roughness = " << roughness;
        }
    }

    // The EPSILON clamp regime: below roughness ~0.27 the engine's NDF peak is
    // capped, so the hemisphere integral drops BELOW 1. That is the shipped
    // behaviour and the reference reproduces it; assert the direction so a
    // future "cleanup" of the clamp shows up here rather than as a silent
    // brightness change in every mirror-like material.
    TEST(ReferenceBRDF, GgxNdfPeakIsEpsilonClampedAtLowRoughness)
    {
        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 h(0.0f, 0.0f, 1.0f); // NdotH == 1, the NDF peak

        // At the peak, D = a^2 / max(pi * a^4, EPSILON). The clamp engages once
        // pi * a^4 < 1e-4, i.e. roughness < ~0.274.
        const f32 clampedPeak = DistributionGGX(n, h, 0.05f);
        const f32 alpha = 0.05f * 0.05f;
        EXPECT_NEAR(clampedPeak, (alpha * alpha) / kEpsilon, 1e-3f)
            << "the engine's D no longer saturates at EPSILON — the reference and the raster "
               "path will now disagree on every low-roughness highlight";

        // Unclamped regime for contrast: D = 1 / (pi * a^2).
        const f32 unclampedPeak = DistributionGGX(n, h, 0.8f);
        const f32 alphaHigh = 0.8f * 0.8f;
        EXPECT_NEAR(unclampedPeak, 1.0f / (kPi * alphaHigh * alphaHigh), 1e-3f);
    }

    // =========================================================================
    // The furnace test, on the CPU port.
    //
    // Same energy band PbrBrdfTest.FurnaceIntegralWithinEnergyBounds accepts on
    // the GPU: [0.6, 1.05]. The upper bound is the real assertion — a value
    // above 1 is energy CREATED, which would make every GI comparison against
    // this reference blessing a brighter-than-physical result.
    // =========================================================================
    TEST(ReferenceBRDF, FurnaceIntegralWithinEnergyBounds)
    {
        constexpr u32 kSamples = 1u << 16;
        constexpr f64 kLowerBound = 0.6;
        constexpr f64 kUpperBound = 1.05;

        for (u32 bin = 0; bin < 32; ++bin)
        {
            const f32 roughness = std::max((static_cast<f32>(bin) + 0.5f) / 32.0f, kMinRoughness);
            const f64 integral = FurnaceIntegral(roughness, 0.0f, glm::vec3(1.0f), kSamples);
            EXPECT_GE(integral, kLowerBound) << "roughness = " << roughness;
            EXPECT_LE(integral, kUpperBound) << "roughness = " << roughness << " — the BRDF is creating energy";
        }
    }

    // =========================================================================
    // Sampling <-> density agreement.
    //
    // Draw from ImportanceSampleGGX and integrate 1/pdf over the sampled
    // directions. If the sampler and PdfGGX describe the same distribution the
    // estimate of the sampled solid angle is 2pi (the hemisphere the reflected
    // directions cover, minus the small below-horizon fraction the sampler
    // discards). Practically: the estimate must converge to a STABLE value
    // close to 2pi, and — the thing that actually catches a wrong Jacobian —
    // must not scale with roughness.
    //
    // A missing 1/(4 v.h) would make this off by a factor of ~4; an inverted
    // one, off by ~1/4. Both produce a perfectly plausible image.
    // =========================================================================
    TEST(ReferenceBRDF, GgxSamplingMatchesItsDensity)
    {
        constexpr u32 kSamples = 1u << 16;
        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 v(0.0f, 0.0f, 1.0f); // head-on: reflection about h stays in the hemisphere

        // The LOW-roughness cases are the point of this test. Above ~0.27 the
        // engine's EPSILON denominator clamp is inactive and the clamped and
        // unclamped NDFs coincide, so a PDF built on the wrong one passes. At
        // 0.05 the two differ by orders of magnitude, and a PDF that reused the
        // renderer's clamp (as a first cut of this file did) reports a solid
        // angle far above 2pi here — the signature of an estimator that
        // divides by a density smaller than the one it sampled from.
        for (const f32 roughness : { 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.8f, 1.0f })
        {
            f64 sum = 0.0;
            u32 valid = 0;
            for (u32 i = 0; i < kSamples; ++i)
            {
                const glm::vec2 xi = Hammersley(i, kSamples);
                const glm::vec3 h = ImportanceSampleGGX(xi, n, roughness);
                const glm::vec3 l = glm::reflect(-v, h);
                const f32 nDotL = glm::dot(n, l);
                if (nDotL <= 0.0f)
                    continue;

                const f32 pdf = PdfGGX(glm::dot(n, h), glm::dot(v, h), roughness);
                if (!(pdf > 0.0f))
                    continue;

                sum += 1.0 / static_cast<f64>(pdf);
                ++valid;
            }

            ASSERT_GT(valid, kSamples / 2) << "roughness = " << roughness;
            const f64 solidAngle = sum / static_cast<f64>(kSamples);
            // 2pi is the full hemisphere. The estimate sits at or just under it
            // — samples reflecting below the horizon are dropped, and at very
            // low roughness fp32 quantization of a near-unit cosine costs a
            // further few percent (measured 0.83x at roughness 0.05).
            //
            // The UPPER bound is the load-bearing one. The failure this test
            // exists for — a PDF that under-reports the density it sampled
            // from — overshoots by orders of magnitude, not percent: with the
            // EPSILON-clamped NDF as the density, roughness 0.05 reports a
            // "solid angle" ~8e5 times too large.
            EXPECT_GT(solidAngle, 0.6 * static_cast<f64>(kTwoPi)) << "roughness = " << roughness;
            EXPECT_LT(solidAngle, 1.05 * static_cast<f64>(kTwoPi))
                << "roughness = " << roughness
                << ": the sampler and PdfGGX describe different distributions — every specular "
                   "estimate is scaled by the mismatch";
        }
    }

    TEST(ReferenceBRDF, CosineSamplingMatchesItsDensity)
    {
        constexpr u32 kSamples = 1u << 16;
        const glm::vec3 n(0.0f, 1.0f, 0.0f);

        f64 sum = 0.0;
        for (u32 i = 0; i < kSamples; ++i)
        {
            const glm::vec3 l = CosineSampleHemisphere(Hammersley(i, kSamples), n);
            const f32 nDotL = glm::dot(n, l);
            ASSERT_GE(nDotL, 0.0f);
            const f32 pdf = PdfCosineHemisphere(nDotL);
            if (!(pdf > 0.0f))
                continue;
            sum += 1.0 / static_cast<f64>(pdf);
        }

        // integral of 1 dw over the hemisphere == 2pi.
        EXPECT_NEAR(sum / static_cast<f64>(kSamples), static_cast<f64>(kTwoPi), 0.05);
    }

    // =========================================================================
    // The mixture density the integrator divides by must be positive wherever
    // the BRDF is non-zero — a zero density on a lobe that carries energy is
    // a SILENT bias (the estimator simply never sees that energy).
    // =========================================================================
    TEST(ReferenceBRDF, SpecularLobeProbabilityNeverExcludesALobe)
    {
        for (const f32 metallic : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            for (const glm::vec3 albedo : { glm::vec3(0.0f), glm::vec3(0.02f), glm::vec3(0.5f), glm::vec3(1.0f) })
            {
                const f32 p = SpecularLobeProbability(albedo, metallic);
                EXPECT_GT(p, 0.0f) << "metallic = " << metallic;
                EXPECT_LT(p, 1.0f) << "metallic = " << metallic;
            }
        }
    }

    // =========================================================================
    // Attenuation port — the raster path's light falloff, not the physical one.
    // Pinned because a "fix" to inverse-square here would put a distance-shaped
    // divergence into every raster-vs-reference comparison.
    // =========================================================================
    TEST(ReferenceBRDF, AttenuationIsZeroBeyondRangeAndSmoothInside)
    {
        const glm::vec4 params(1.0f, 0.09f, 0.032f, 10.0f);
        const glm::vec3 lightPos(0.0f);

        EXPECT_FLOAT_EQ(CalculateAttenuation(lightPos, glm::vec3(0.0f, 0.0f, 10.5f), params), 0.0f);

        // Monotonically non-increasing with distance, and exactly 1 at zero
        // distance (constant term == 1).
        EXPECT_NEAR(CalculateAttenuation(lightPos, lightPos, params), 1.0f, 1e-5f);

        f32 previous = std::numeric_limits<f32>::max();
        for (u32 step = 0; step <= 100; ++step)
        {
            const f32 distance = static_cast<f32>(step) * 0.1f;
            const f32 attenuation = CalculateAttenuation(lightPos, glm::vec3(0.0f, 0.0f, distance), params);
            EXPECT_LE(attenuation, previous + 1e-6f) << "distance = " << distance;
            previous = attenuation;
        }
        // Reaches exactly zero AT the range boundary (the falloff term).
        EXPECT_NEAR(CalculateAttenuation(lightPos, glm::vec3(0.0f, 0.0f, 10.0f), params), 0.0f, 1e-6f);
    }

    // =========================================================================
    // Sampler determinism — the property the whole "bit-identical reference"
    // claim rests on. Two independently constructed samplers with the same
    // (pixel, sample) must emit the same stream, and different pixels must not.
    // =========================================================================
    TEST(ReferencePathSampler, IsStatelessAndReproducible)
    {
        PathSampler a(12345u, 7u);
        PathSampler b(12345u, 7u);
        for (u32 i = 0; i < 32; ++i)
        {
            const glm::vec2 va = a.Get2D();
            const glm::vec2 vb = b.Get2D();
            EXPECT_FLOAT_EQ(va.x, vb.x) << "dimension pair " << i;
            EXPECT_FLOAT_EQ(va.y, vb.y) << "dimension pair " << i;
        }

        PathSampler other(12346u, 7u);
        PathSampler baseline(12345u, 7u);
        u32 differing = 0;
        for (u32 i = 0; i < 32; ++i)
        {
            if (std::abs(other.Get1D() - baseline.Get1D()) > 1e-6f)
                ++differing;
        }
        EXPECT_GT(differing, 24u) << "neighbouring pixel seeds produce a nearly identical stream — "
                                     "the image will show structured (non-random) noise";
    }

    TEST(ReferencePathSampler, ValuesStayInTheUnitInterval)
    {
        for (u32 pixel = 0; pixel < 64; ++pixel)
        {
            for (u32 sample = 0; sample < 64; ++sample)
            {
                PathSampler sampler(MakePixelSeed(pixel, sample, 0x9e3779b9u), sample);
                for (u32 dimension = 0; dimension < 8; ++dimension)
                {
                    const f32 value = sampler.Get1D();
                    ASSERT_GE(value, 0.0f);
                    // Strictly below 1: several call sites divide by or take
                    // sqrt(1 - xi), which a returned 1.0 degenerates.
                    ASSERT_LT(value, 1.0f);
                }
            }
        }
    }

    // Stratification: over N samples of one pixel, the first 2D dimension pair
    // must cover the unit square roughly uniformly. A sampler that collapsed to
    // a constant (the classic "forgot to advance the index") still produces a
    // converging-looking but WRONG image, because every sample takes the same
    // path.
    TEST(ReferencePathSampler, FirstDimensionPairIsStratified)
    {
        constexpr u32 kSamples = 4096;
        constexpr u32 kBins = 8;
        std::vector<u32> histogram(kBins * kBins, 0);

        for (u32 sample = 0; sample < kSamples; ++sample)
        {
            PathSampler sampler(MakePixelSeed(11u, 23u, 0x9e3779b9u), sample);
            const glm::vec2 xi = sampler.Get2D();
            const auto bx = std::min(static_cast<u32>(xi.x * kBins), kBins - 1);
            const auto by = std::min(static_cast<u32>(xi.y * kBins), kBins - 1);
            ++histogram[by * kBins + bx];
        }

        const u32 expected = kSamples / (kBins * kBins);
        for (u32 bin = 0; bin < histogram.size(); ++bin)
        {
            EXPECT_GT(histogram[bin], expected / 2) << "bin " << bin << " under-covered";
            EXPECT_LT(histogram[bin], expected * 2) << "bin " << bin << " over-covered";
        }
    }
} // namespace OloEngine::Tests
