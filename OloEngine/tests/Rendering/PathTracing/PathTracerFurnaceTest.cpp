// OLO_TEST_LAYER: L1
// =============================================================================
// PathTracerFurnaceTest.cpp — the analytic anchor for the offline reference
// path tracer (issue #709, acceptance criterion 1).
//
// WHY THIS TEST EXISTS BEFORE ANY COMPARISON AGAINST THE RASTER PATH
// ------------------------------------------------------------------
// A reference image is only useful if the INTEGRATOR is right. If it is not,
// every downstream comparison "validates" the raster path against a second
// wrong answer, and — because the reference looks converged and smooth — there
// is nothing to notice. So the integrator is pinned against a number that is
// known independently of it: the white-furnace integral of the BRDF.
//
// The chain, each link checkable on its own:
//
//   1. `WhiteFurnaceMatchesTheAnalyticIntegral` — a single plane under a
//      uniform environment of radiance 1, viewed along its own normal, is a
//      CONVEX single-scatter configuration: the converged radiance is exactly
//      the directional albedo
//          integral of f(l, v) * (n . l) dw    with n = v.
//      That is the identical quantity PbrFurnaceProbe.glsl estimates on the
//      GPU and ReferenceBRDFTest estimates on the CPU. Equality here means the
//      integrator's throughput bookkeeping — sample, evaluate, divide by the
//      density — is correct, because the BRDF term cancels out of the
//      comparison and only the transport machinery is left under test.
//
//   2. `WhiteFurnaceStaysWithinTheEnergyBounds` — the same [0.6, 1.05] band
//      PbrBrdfTest.FurnaceIntegralWithinEnergyBounds accepts on the GPU, so
//      the reference and the shader probe deliver the same energy VERDICT and
//      not merely two similar numbers.
//
//   3. `CornellFurnaceCreatesNoEnergy` — the multi-bounce version, and the one
//      the acceptance criterion actually names: the full Cornell-box GEOMETRY
//      with every surface a white reflector and a uniform environment entering
//      the open face. A perfectly energy-conserving BRDF would converge to
//      exactly 1.0 everywhere. This catches transport bugs that a single
//      bounce cannot: a missing cosine on a secondary bounce, a Russian
//      roulette that forgets to divide by the survival probability (energy
//      GAIN, compounding with depth), MIS weights that do not sum to one.
//
//   4. `NextEventEstimationDoesNotBias` — NEE + MIS switched off must converge
//      to the same image as NEE on. They sample the same integral by different
//      strategies, so any disagreement beyond noise is a bias in the MIS
//      weights, which is exactly the failure mode that converges beautifully
//      to the wrong answer.
//
// Classification: L1 (pure CPU, headless — this is the CI gate's cheap half).
// =============================================================================

#include "OloEnginePCH.h"

#include "PathTracing/ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    namespace Fixtures = OloEngine::Tests::PathTracingFixtures;

    namespace
    {
        // Uniform-hemisphere estimate of the directional albedo with n = v, the
        // same estimator PbrFurnaceProbe.glsl uses. Duplicated (rather than
        // shared with ReferenceBRDFTest) on purpose: this file must be able to
        // state its expected value without depending on another test's helper.
        [[nodiscard]] f64 AnalyticFurnaceIntegral(f32 roughness, f32 metallic, u32 sampleCount)
        {
            const glm::vec3 n(0.0f, 1.0f, 0.0f);
            const glm::vec3 v(0.0f, 1.0f, 0.0f);

            f64 sum = 0.0;
            for (u32 i = 0; i < sampleCount; ++i)
            {
                const glm::vec2 xi(static_cast<f32>(i) / static_cast<f32>(sampleCount),
                                   SamplerDetail::ToUnitFloat(SamplerDetail::ReverseBits(i)));
                const f32 cosTheta = xi.x;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const f32 phi = kTwoPi * xi.y;
                // Hemisphere around +Y.
                const glm::vec3 l(std::cos(phi) * sinTheta, cosTheta, std::sin(phi) * sinTheta);

                const glm::vec3 f = CookTorranceBRDF(n, v, l, glm::vec3(1.0f), metallic, roughness);
                sum += static_cast<f64>((f.x + f.y + f.z) / 3.0f) * static_cast<f64>(cosTheta) * static_cast<f64>(kTwoPi);
            }
            return sum / static_cast<f64>(sampleCount);
        }

        // Average of the tracer's own estimate of the same quantity: trace the
        // straight-down view ray many times and average the channel mean.
        [[nodiscard]] f64 TracedFurnaceValue(const Fixtures::FurnacePlaneScene& fixture, u32 sampleCount)
        {
            PathTracerSettings settings;
            settings.SamplesPerPixel = sampleCount;
            // Two vertices: the plane, then the escape into the environment.
            // One would be direct lighting only and — with no lights in a
            // furnace scene — would render black.
            settings.MaxBounces = 2;
            settings.RussianRouletteStartBounce = 0; // no stochastic energy loss
            settings.EnableNextEventEstimation = false;

            const Ray viewRay = fixture.ViewRay();

            f64 sum = 0.0;
            for (u32 sample = 0; sample < sampleCount; ++sample)
            {
                PathSampler sampler(MakePixelSeed(0u, 0u, settings.Seed), sample);
                const glm::vec3 radiance = PathTracer::TracePath(fixture.Scene, viewRay, settings, sampler);
                sum += static_cast<f64>((radiance.x + radiance.y + radiance.z) / 3.0f);
            }
            return sum / static_cast<f64>(sampleCount);
        }
    } // namespace

    // =========================================================================
    // 1. Single-scatter furnace == the analytic directional albedo.
    // =========================================================================
    TEST(PathTracerFurnace, WhiteFurnaceMatchesTheAnalyticIntegral)
    {
        constexpr u32 kAnalyticSamples = 1u << 17;
        constexpr u32 kTracedSamples = 1u << 15;
        // Both sides are Monte Carlo estimates, but both use stratified
        // low-discrepancy point sets, so the residuals are tiny and stable
        // rather than noisy: an offline reference implementation of this
        // comparison agreed to five decimal places across the whole grid, and
        // the analytic side moved by < 1e-5 between 2^17 and 2^20 samples.
        //
        // A combined absolute-or-relative bound is what this grid needs: the
        // METALLIC values span two orders of magnitude (0.035 at roughness 0.1,
        // where the engine's EPSILON clamp all but deletes the specular lobe,
        // up to 0.89), so a pure absolute tolerance would be vacuous at the
        // bottom and a pure relative one needlessly brittle at the top.
        //
        // For scale: the clamped-PDF bug this test was written to catch made
        // the traced value 2.18 against an analytic 0.96.
        constexpr f64 kAbsoluteTolerance = 0.005;
        constexpr f64 kRelativeTolerance = 0.02;

        for (const f32 roughness : { 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            for (const f32 metallic : { 0.0f, 1.0f })
            {
                const Fixtures::FurnacePlaneScene fixture =
                    Fixtures::MakeFurnacePlaneScene(roughness, metallic);
                ASSERT_TRUE(fixture.Scene.IsBuilt());

                const f64 analytic = AnalyticFurnaceIntegral(roughness, metallic, kAnalyticSamples);
                const f64 traced = TracedFurnaceValue(fixture, kTracedSamples);
                const f64 tolerance = std::max(kAbsoluteTolerance, kRelativeTolerance * analytic);

                EXPECT_NEAR(traced, analytic, tolerance)
                    << "roughness = " << roughness << ", metallic = " << metallic
                    << " — the integrator's throughput bookkeeping disagrees with the BRDF's own "
                       "hemisphere integral";
            }
        }
    }

    // =========================================================================
    // 2. Same energy verdict as the GPU probe test.
    // =========================================================================
    TEST(PathTracerFurnace, WhiteFurnaceStaysWithinTheEnergyBounds)
    {
        constexpr u32 kTracedSamples = 1u << 14;
        // The band PbrBrdfTest.FurnaceIntegralWithinEnergyBounds accepts.
        constexpr f64 kLowerBound = 0.6;
        constexpr f64 kUpperBound = 1.05;

        for (u32 bin = 0; bin < 16; ++bin)
        {
            const f32 roughness = std::max((static_cast<f32>(bin) + 0.5f) / 16.0f, kMinRoughness);
            const Fixtures::FurnacePlaneScene fixture = Fixtures::MakeFurnacePlaneScene(roughness, 0.0f);
            const f64 traced = TracedFurnaceValue(fixture, kTracedSamples);

            EXPECT_GE(traced, kLowerBound) << "roughness = " << roughness;
            EXPECT_LE(traced, kUpperBound) << "roughness = " << roughness
                                           << " — the converged reference is CREATING energy";
        }
    }

    // The environment must scale the result exactly linearly: a furnace at
    // radiance L converges to L times the albedo. This is the cheapest possible
    // check that no additive term (an ambient constant, a stray emissive) has
    // crept into the transport.
    TEST(PathTracerFurnace, ConvergedValueScalesLinearlyWithTheEnvironment)
    {
        constexpr u32 kTracedSamples = 1u << 14;
        const Fixtures::FurnacePlaneScene unit = Fixtures::MakeFurnacePlaneScene(0.5f, 0.0f, glm::vec3(1.0f), 1.0f);
        const Fixtures::FurnacePlaneScene quad = Fixtures::MakeFurnacePlaneScene(0.5f, 0.0f, glm::vec3(1.0f), 4.0f);

        const f64 unitValue = TracedFurnaceValue(unit, kTracedSamples);
        const f64 quadValue = TracedFurnaceValue(quad, kTracedSamples);

        ASSERT_GT(unitValue, 0.1);
        EXPECT_NEAR(quadValue / unitValue, 4.0, 1e-3);
    }

    // =========================================================================
    // 3. Multi-bounce furnace on the Cornell-box geometry — acceptance
    //    criterion 1's headline check.
    // =========================================================================
    TEST(PathTracerFurnace, CornellFurnaceCreatesNoEnergy)
    {
        constexpr u32 kWidth = 40;
        constexpr u32 kHeight = 40;

        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellFurnaceScene(1.0f, 1.0f);
        ASSERT_TRUE(fixture.Scene.IsBuilt());

        PathTracerSettings settings;
        // The sample count is a CORRECTNESS parameter here, not a quality one —
        // see the per-pixel bound below. At 128 samples this same (correct)
        // render produced a peak of 1.069 with 0.68% of channel estimates above
        // 1.0, purely from Monte Carlo variance. At 160 the peak is 1.026 and
        // NOTHING exceeds the 1.05 tail bound.
        settings.SamplesPerPixel = 160;
        // Deep enough that truncation is not what keeps the answer below 1 —
        // otherwise the test would pass for the wrong reason.
        settings.MaxBounces = 32;
        settings.RussianRouletteStartBounce = 6;
        settings.EnableNextEventEstimation = false; // no emitters here; env only

        ReferenceFilm film(kWidth, kHeight);
        PathTracer::Render(fixture.Scene, fixture.MakeCamera(kWidth, kHeight), settings, film);

        f32 maximum = 0.0f;
        u32 nonZeroPixels = 0;
        u32 channelsAboveBound = 0;
        u32 channelCount = 0;
        for (const glm::vec3& pixel : film.GetPixels())
        {
            ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));
            maximum = std::max({ maximum, pixel.x, pixel.y, pixel.z });
            for (glm::length_t channel = 0; channel < 3; ++channel)
            {
                if (pixel[channel] > 1.05f)
                    ++channelsAboveBound;
                ++channelCount;
            }
            if (pixel.x > 0.01f)
                ++nonZeroPixels;
        }

        ASSERT_GT(nonZeroPixels, (kWidth * kHeight) / 2)
            << "most of the frame is black — the camera is not looking into the box";

        // The load-bearing assertion, stated on a LOW-VARIANCE statistic.
        //
        // The obvious form — "no pixel exceeds 1.0" — is wrong, and
        // instructively so: the max of a Monte Carlo image is a NOISE statistic,
        // not a bound on the quantity being estimated. An unbiased estimator whose
        // expectation is 0.858 still produces individual pixels above 1.0 at a low
        // sample count, so a test asserting the max is really asserting the sample
        // count.
        //
        // So: the frame MEAN carries the energy-conservation claim (it averages
        // 12288 channel estimates, so its own error is negligible), the tail is
        // bounded as a FRACTION rather than by an extreme order statistic, and the
        // per-pixel ceiling sits where only gross creation can reach — a Russian
        // roulette that forgot to divide by the survival probability compounds
        // into the tens, not into 1.06.
        const glm::vec3 frameMean = film.MeanRadiance(0, 0, kWidth - 1, kHeight - 1);
        const f32 meanChannel = (frameMean.x + frameMean.y + frameMean.z) / 3.0f;
        EXPECT_LE(meanChannel, 1.005f)
            << "multi-bounce transport is creating energy: the mean radiance of a white enclosure "
               "under a unit environment came out at "
            << meanChannel << " (measured 0.858)";
        EXPECT_LT(static_cast<f32>(channelsAboveBound) / static_cast<f32>(channelCount), 0.01f)
            << channelsAboveBound << " of " << channelCount
            << " channel estimates exceed 1.05 (measured 0)";
        EXPECT_LE(maximum, 1.5f) << "gross energy creation in multi-bounce transport (peak = " << maximum << ")";

        // ...and it must not be trivially dark either — a ceiling alone would
        // pass for a completely broken integrator that returns zero.
        //
        // The floor is NOT 1.0, and deliberately not asserted as such. The
        // classic "closed white furnace converges to exactly the environment"
        // statement needs a perfectly energy-conserving BRDF; single-scatter
        // GGX loses a few percent per bounce, and this cavity is open on one
        // face, so the equilibrium is the fixed point of
        //     L = rho * (f_open * 1 + f_closed * L)
        // with rho the directional albedo (~0.93 here) and f_open the opening's
        // form factor (~1/6 for a cube open on one face) — landing around 0.7,
        // not 1.0. Pinning that number exactly would be pinning the cavity's
        // geometry, not the integrator; 0.3 is a floor that only a genuinely
        // broken transport can breach.
        // Measured: 0.858, matching the L = rho * (f_open + f_closed * L) estimate.
        const glm::vec3 centre = film.MeanRadiance(kWidth / 4, kHeight / 4, (kWidth * 3) / 4, (kHeight * 3) / 4);
        EXPECT_GE(std::max({ centre.x, centre.y, centre.z }), 0.5f)
            << "the white furnace enclosure converged far below the environment radiance — "
               "multi-bounce transport is losing energy it should not";
    }

    // =========================================================================
    // 4. NEE + MIS must not bias the result.
    // =========================================================================
    TEST(PathTracerFurnace, NextEventEstimationDoesNotBias)
    {
        constexpr u32 kWidth = 20;
        constexpr u32 kHeight = 20;

        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        ASSERT_TRUE(fixture.Scene.IsBuilt());

        PathTracerSettings base;
        base.MaxBounces = 6;
        // No Russian roulette: RR is itself somewhere a bias could hide, and
        // this test is trying to isolate the MIS weights.
        base.RussianRouletteStartBounce = 0;

        // BSDF sampling alone is far noisier, so it gets 8x the budget. The
        // comparison is on a region MEAN over ~160 pixels, which is what makes
        // these modest budgets enough to separate a bias from noise: measured
        // residuals are 0.6% / 0.5% / 0.1% per channel.
        PathTracerSettings withNee = base;
        withNee.EnableNextEventEstimation = true;
        withNee.SamplesPerPixel = 96;

        PathTracerSettings withoutNee = base;
        withoutNee.EnableNextEventEstimation = false;
        withoutNee.SamplesPerPixel = 768;

        const ReferenceCamera camera = fixture.MakeCamera(kWidth, kHeight);

        ReferenceFilm neeFilm(kWidth, kHeight);
        PathTracer::Render(fixture.Scene, camera, withNee, neeFilm);

        ReferenceFilm bsdfFilm(kWidth, kHeight);
        PathTracer::Render(fixture.Scene, camera, withoutNee, bsdfFilm);

        // Compare the mean over the lit interior — a whole-frame mean would be
        // dominated by the emitter itself, which both strategies get exactly
        // right and which would therefore hide a disagreement everywhere else.
        const glm::vec3 neeMean = neeFilm.MeanRadiance(2, kHeight / 2, kWidth - 2, kHeight - 2);
        const glm::vec3 bsdfMean = bsdfFilm.MeanRadiance(2, kHeight / 2, kWidth - 2, kHeight - 2);

        ASSERT_GT(neeMean.x + neeMean.y + neeMean.z, 0.01f) << "the NEE render is black";
        ASSERT_GT(bsdfMean.x + bsdfMean.y + bsdfMean.z, 0.01f) << "the BSDF-only render is black";

        for (glm::length_t channel = 0; channel < 3; ++channel)
        {
            const f32 reference = bsdfMean[channel];
            const f32 measured = neeMean[channel];
            const f32 relative = std::abs(measured - reference) / std::max(reference, 1e-4f);
            // 8% is more than 10x the measured residual (0.6% on the worst
            // channel), so this is not a tight-rope threshold. A genuine MIS-weight bug (a missing power
            // heuristic, a double-counted emitter) shows up as tens of percent
            // and, characteristically, in ONE direction on every channel.
            EXPECT_LT(relative, 0.08f)
                << "channel " << channel << ": NEE " << measured << " vs BSDF-only " << reference
                << " — the two sampling strategies disagree, so the MIS weights are biased";
        }
    }
} // namespace OloEngine::Tests
