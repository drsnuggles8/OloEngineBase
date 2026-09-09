// OLO_TEST_LAYER: plumbing
// =============================================================================
// ReSTIRDIOracleTest.cpp — does the ReSTIR DI estimator compute the right
// number? Issue #1140.
//
// WHY THIS FILE IS SEPARATE FROM THE CONTRACT TEST. The contract test pins the
// PIECES: the Jacobian is a measure change, the MIS weights sum to one, the
// guards guard. Every one of those can pass while the assembled estimator
// converges to the wrong integral, because the pieces can be individually
// correct and wired together wrongly — a Jacobian applied to the wrong term, a
// source density converted at the wrong point, an MIS weight applied on top of
// a 1/M normaliser. That is the failure #1140 was filed against: "a ReSTIR
// estimator that is wrong still looks plausible."
//
// So this file assembles the estimator the way the shaders do and measures it
// against an ORACLE, and the oracle is not another estimator:
//
//   * for a PUNCTUAL light set, the ground truth is the ANALYTIC sum. There is
//     no sampling error in it at all, so a systematic bias of even a percent is
//     visible against it.
//   * for an AREA emitter, the ground truth is dense stratified QUADRATURE over
//     the emitter — 512x512 deterministic samples, no random numbers — which is
//     the same integral #1055's path tracer converges to by sampling, computed
//     the one way that cannot share a bug with the sampler.
//
// It runs headless, on every CI runner, and it is the check that fails if the
// resampling is wired up wrong. The DEVICE half — that the GLSL twin computes
// what this does — is ReSTIRDIReservoirGpuParityTest and the visual-evidence
// captures; neither can substitute for this one, because a GPU that agrees with
// a wrong CPU estimator proves only that they agree.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReSTIR/ReservoirDI.h"

#include <glm/glm.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::ReSTIR;

    namespace
    {
        // A Lambertian surface, so the BRDF is a constant and every discrepancy
        // the test sees is the RESAMPLING rather than the closure. The closure
        // itself is #975's, pinned by its own parity tests; mixing it in here
        // would make a failure ambiguous between two subsystems.
        struct LambertianSurface
        {
            glm::vec3 Position{ 0.0f };
            glm::vec3 Normal{ 0.0f, 0.0f, 1.0f };
            glm::vec3 Albedo{ 0.8f, 0.6f, 0.4f };

            [[nodiscard]] glm::vec3 Brdf() const
            {
                return Albedo * (1.0f / std::numbers::pi_v<f32>);
            }
        };

        // The integrand in SOLID-ANGLE measure at the shading point:
        // f * L * cos(thetaShading). This is the target function's vector form,
        // and it is what the resolve pass multiplies by W.
        [[nodiscard]] glm::vec3 Contribution(const LambertianSurface& surface, const LightSample& sample)
        {
            glm::vec3 l;
            if (sample.Kind == LightSampleKind::Directional)
            {
                l = glm::normalize(sample.Position);
            }
            else
            {
                const glm::vec3 toLight = sample.Position - surface.Position;
                const f32 distanceSq = glm::dot(toLight, toLight);
                if (!(distanceSq > 0.0f))
                    return glm::vec3(0.0f);
                l = toLight / std::sqrt(distanceSq);
            }
            const f32 nDotL = glm::dot(surface.Normal, l);
            if (!(nDotL > 0.0f))
                return glm::vec3(0.0f);
            return surface.Brdf() * sample.Radiance * nDotL;
        }

        [[nodiscard]] f32 TargetPdf(const LambertianSurface& surface, const LightSample& sample)
        {
            const glm::vec3 c = Contribution(surface, sample);
            const f32 t = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            return (std::isfinite(t) && t > 0.0f) ? t : 0.0f;
        }

        // -------------------------------------------------------------------
        // A punctual light set, and its analytic answer
        // -------------------------------------------------------------------

        struct PunctualLightSet
        {
            std::vector<LightSample> Lights;

            // The estimator's source density: uniform over the set. That is the
            // right SOURCE pdf precisely because the target function does the
            // importance work — the same reasoning the shader's comment gives.
            [[nodiscard]] f32 SelectionPdf() const
            {
                return Lights.empty() ? 0.0f : 1.0f / static_cast<f32>(Lights.size());
            }

            // Ground truth: every light contributes deterministically, so the
            // integral IS the sum. No sampling error, which is what makes a
            // one-percent bias detectable.
            [[nodiscard]] glm::vec3 AnalyticDirectLighting(const LambertianSurface& surface) const
            {
                glm::vec3 total(0.0f);
                for (const auto& light : Lights)
                    total += Contribution(surface, light);
                return total;
            }
        };

        [[nodiscard]] PunctualLightSet MakeManyPointLights(u32 count, u32 seed)
        {
            // Deterministic placement: a test whose light set moved between runs
            // could not distinguish a bias from a reseed.
            std::mt19937 rng(seed);
            std::uniform_real_distribution<f32> lateral(-6.0f, 6.0f);
            std::uniform_real_distribution<f32> height(0.5f, 5.0f);
            std::uniform_real_distribution<f32> brightness(0.2f, 4.0f);

            PunctualLightSet set;
            set.Lights.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                LightSample light{};
                light.Kind = LightSampleKind::Punctual;
                light.LightIndex = i;
                light.Position = glm::vec3(lateral(rng), lateral(rng), height(rng));
                // A wide brightness spread is the point: with equal lights every
                // resampling scheme looks fine, and a broken target function
                // only shows up when the lights differ by orders of magnitude.
                const f32 scale = brightness(rng);
                light.Radiance = glm::vec3(scale, scale * 0.7f, scale * 0.4f);
                set.Lights.push_back(light);
            }
            return set;
        }

        // One pixel's worth of the estimator: RIS over `candidates` draws from
        // the light set, finalised the way ReSTIR_DI_InitialSample.glsl does.
        [[nodiscard]] Reservoir SampleInitialReservoir(const LambertianSurface& surface,
                                                       const PunctualLightSet& set, u32 candidates,
                                                       std::mt19937& rng)
        {
            std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
            const f32 sourcePdf = set.SelectionPdf();
            Reservoir reservoir{};
            for (u32 c = 0; c < candidates; ++c)
            {
                const auto index =
                    std::min(static_cast<sizet>(uniform(rng) * static_cast<f32>(set.Lights.size())),
                             set.Lights.size() - 1);
                const LightSample& candidate = set.Lights[index];
                const f32 targetPdf = TargetPdf(surface, candidate);
                // A delta light lives in a discrete measure: the density IS the
                // selection probability, with no solid-angle factor.
                ReservoirUpdate(reservoir, candidate, targetPdf / sourcePdf, targetPdf, uniform(rng));
            }
            FinalizeInitialCandidates(reservoir);
            return reservoir;
        }

        [[nodiscard]] glm::vec3 ResolveReservoir(const LambertianSurface& surface, const Reservoir& reservoir)
        {
            if (reservoir.IsEmpty() || !(reservoir.W > 0.0f))
                return glm::vec3(0.0f);
            return Contribution(surface, reservoir.Sample) * reservoir.W;
        }

        // -------------------------------------------------------------------
        // An area emitter, and its quadrature answer
        // -------------------------------------------------------------------

        // A rectangle emitter in the plane z = Height, facing -Z.
        struct RectangleEmitter
        {
            glm::vec3 Centre{ 0.0f, 0.0f, 3.0f };
            glm::vec3 EdgeU{ 2.0f, 0.0f, 0.0f };
            glm::vec3 EdgeV{ 0.0f, 1.5f, 0.0f };
            glm::vec3 Radiance{ 3.0f, 2.0f, 1.0f };

            [[nodiscard]] glm::vec3 Normal() const
            {
                // Facing DOWN, toward the receiving plane.
                return glm::vec3(0.0f, 0.0f, -1.0f);
            }
            [[nodiscard]] f32 Area() const
            {
                return glm::length(EdgeU) * glm::length(EdgeV);
            }
            [[nodiscard]] f32 AreaPdf() const
            {
                return 1.0f / Area();
            }
            [[nodiscard]] LightSample At(f32 u, f32 v) const
            {
                LightSample s{};
                s.Kind = LightSampleKind::EmissiveTriangle;
                s.Position = Centre + EdgeU * (u - 0.5f) + EdgeV * (v - 0.5f);
                s.Normal = Normal();
                s.Radiance = Radiance;
                return s;
            }

            // DENSE STRATIFIED QUADRATURE — the oracle. No random numbers, so it
            // cannot share a bug with any sampler, and it converges to the same
            // integral #1055's path tracer converges to by sampling.
            [[nodiscard]] glm::vec3 QuadratureDirectLighting(const LambertianSurface& surface,
                                                            u32 resolution) const
            {
                const f32 cellArea = Area() / static_cast<f32>(resolution * resolution);
                glm::dvec3 total(0.0);
                for (u32 iu = 0; iu < resolution; ++iu)
                {
                    for (u32 iv = 0; iv < resolution; ++iv)
                    {
                        const f32 u = (static_cast<f32>(iu) + 0.5f) / static_cast<f32>(resolution);
                        const f32 v = (static_cast<f32>(iv) + 0.5f) / static_cast<f32>(resolution);
                        const LightSample point = At(u, v);
                        const glm::vec3 toLight = point.Position - surface.Position;
                        const f32 distanceSq = glm::dot(toLight, toLight);
                        if (!(distanceSq > 0.0f))
                            continue;
                        const glm::vec3 l = toLight / std::sqrt(distanceSq);
                        const f32 nDotL = glm::dot(surface.Normal, l);
                        const f32 cosLight = glm::dot(point.Normal, -l);
                        if (!(nDotL > 0.0f) || !(cosLight > 0.0f))
                            continue;
                        // The area-measure integrand: f * L * cos_s * cos_l / d^2.
                        const glm::vec3 term =
                            surface.Brdf() * point.Radiance * nDotL * cosLight / distanceSq * cellArea;
                        total += glm::dvec3(term);
                    }
                }
                return glm::vec3(total);
            }
        };

        // RIS over an area emitter, in the measure the shaders use: the sample is
        // drawn in AREA measure, the target function is evaluated in SOLID ANGLE,
        // and the source density is converted between them exactly once.
        [[nodiscard]] Reservoir SampleAreaReservoir(const LambertianSurface& surface,
                                                    const RectangleEmitter& emitter, u32 candidates,
                                                    std::mt19937& rng)
        {
            std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
            Reservoir reservoir{};
            for (u32 c = 0; c < candidates; ++c)
            {
                const LightSample candidate = emitter.At(uniform(rng), uniform(rng));
                const f32 solidAnglePdf =
                    AreaPdfToSolidAnglePdf(emitter.AreaPdf(), candidate, surface.Position);
                if (!(solidAnglePdf > 0.0f))
                {
                    reservoir.M += 1.0f;
                    continue;
                }
                const f32 targetPdf = TargetPdf(surface, candidate);
                ReservoirUpdate(reservoir, candidate, targetPdf / solidAnglePdf, targetPdf, uniform(rng));
            }
            FinalizeInitialCandidates(reservoir);
            return reservoir;
        }

        [[nodiscard]] f32 RelativeError(const glm::vec3& measured, const glm::vec3& truth)
        {
            const f32 denominator = std::max(glm::length(truth), 1.0e-6f);
            return glm::length(measured - truth) / denominator;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The many-light case the tier exists for
    // -------------------------------------------------------------------------

    // 512 point lights of wildly different brightness, 32 RIS candidates per
    // pixel. The estimator sees 1/16th of the lights per pixel and must still
    // land on the analytic sum of all of them. That is the whole claim of the
    // tier, and it is checked against a number with no sampling error in it.
    TEST(ReSTIRDIOracle, RISOverManyPointLightsConvergesToTheAnalyticSum)
    {
        const PunctualLightSet set = MakeManyPointLights(512, 0x1140u);
        LambertianSurface surface{};
        surface.Position = glm::vec3(0.0f);
        surface.Normal = glm::vec3(0.0f, 0.0f, 1.0f);

        const glm::vec3 truth = set.AnalyticDirectLighting(surface);
        ASSERT_GT(glm::length(truth), 0.0f);

        std::mt19937 rng(0xC0FFEEu);
        glm::dvec3 accumulated(0.0);
        // 60k rather than 20k, because the target function is a LUMINANCE and the
        // lights have varied chroma: a pixel's estimate carries the surviving
        // light's colour divided by its luminance share, so the per-sample RGB
        // variance does not shrink with the candidate count. The mean converges as
        // 1/sqrt(pixels), and 20k left the 2% bound only a couple of standard
        // errors away — close enough to flake for a reason that is not a bug.
        constexpr u32 kPixels = 60000;
        for (u32 i = 0; i < kPixels; ++i)
        {
            const Reservoir reservoir = SampleInitialReservoir(surface, set, 32, rng);
            accumulated += glm::dvec3(ResolveReservoir(surface, reservoir));
        }
        const glm::vec3 mean = glm::vec3(accumulated / static_cast<f64>(kPixels));

        // 2% over 20k samples of a 512-light set. A missing normaliser is off by
        // a factor of 32 here, and a target function that ignored the BRDF or the
        // cosine is off by tens of percent — this bound catches both while
        // leaving room for the Monte Carlo noise that legitimately remains.
        EXPECT_LT(RelativeError(mean, truth), 0.02f)
            << "mean (" << mean.x << ", " << mean.y << ", " << mean.z << ") vs truth (" << truth.x << ", "
            << truth.y << ", " << truth.z << ")";
    }

    // -------------------------------------------------------------------------
    // The area emitter, against quadrature
    // -------------------------------------------------------------------------

    TEST(ReSTIRDIOracle, RISOverAnAreaEmitterConvergesToDenseQuadrature)
    {
        const RectangleEmitter emitter{};
        LambertianSurface surface{};
        surface.Position = glm::vec3(0.3f, -0.2f, 0.0f);

        const glm::vec3 truth = emitter.QuadratureDirectLighting(surface, 512);
        ASSERT_GT(glm::length(truth), 0.0f);

        std::mt19937 rng(0xBADC0DEu);
        glm::dvec3 accumulated(0.0);
        constexpr u32 kPixels = 20000;
        for (u32 i = 0; i < kPixels; ++i)
        {
            const Reservoir reservoir = SampleAreaReservoir(surface, emitter, 8, rng);
            accumulated += glm::dvec3(ResolveReservoir(surface, reservoir));
        }
        const glm::vec3 mean = glm::vec3(accumulated / static_cast<f64>(kPixels));

        EXPECT_LT(RelativeError(mean, truth), 0.02f)
            << "mean (" << mean.x << ", " << mean.y << ", " << mean.z << ") vs quadrature (" << truth.x << ", "
            << truth.y << ", " << truth.z << ")";
    }

    // -------------------------------------------------------------------------
    // Spatial reuse: the Jacobian in situ
    // -------------------------------------------------------------------------

    // THE TEST THE JACOBIAN EXISTS FOR. Two shading points at different
    // distances and angles from the same emitter; the destination merges the
    // neighbour's reservoir. With the Jacobian the estimate still matches
    // quadrature; the assertion below ALSO checks that dropping it does not —
    // because a Jacobian that were silently a no-op would pass every test that
    // only measured the correct arm.
    TEST(ReSTIRDIOracle, SpatialReuseWithTheJacobianStaysUnbiasedAndWithoutItDoesNot)
    {
        const RectangleEmitter emitter{};
        LambertianSurface destination{};
        destination.Position = glm::vec3(0.0f, 0.0f, 0.0f);
        // A neighbour far enough away, and offset along the emitter's long edge,
        // that its view of the emitter genuinely differs — a neighbour a texel
        // away would make the Jacobian ~1 and the test vacuous.
        LambertianSurface neighbour = destination;
        // Far enough out AND off-axis that the neighbour sees the emitter at a
        // genuinely different distance and angle: at (2.0, 1.5) the shift Jacobian
        // is about 2.2. The first version used (1.4, 0.9), where J measured 1.077 —
        // the two arms were then the same measurement to within the Monte Carlo
        // noise, and the negative control below asserted nothing.
        neighbour.Position = glm::vec3(2.0f, 1.5f, 0.0f);

        const glm::vec3 truth = emitter.QuadratureDirectLighting(destination, 512);
        ASSERT_GT(glm::length(truth), 0.0f);

        // The Jacobian must actually be doing work at this geometry, or the test
        // proves nothing. Checked here rather than assumed.
        {
            std::mt19937 probe(1u);
            const Reservoir sample = SampleAreaReservoir(neighbour, emitter, 8, probe);
            ASSERT_FALSE(sample.IsEmpty());
            const f32 j = ShiftJacobian(sample.Sample, destination.Position, neighbour.Position);
            ASSERT_GT(j, 0.0f);
            // 0.25, not a token 0.05: the merged estimate is a MIXTURE of the
            // centre's (correct) sample and the neighbour's, so the visible bias
            // is only a fraction of J's deviation. A geometry that barely moves J
            // would leave the negative control below inside the Monte Carlo noise
            // and it would pass whether or not the Jacobian is applied. At this
            // configuration J is about 1.5.
            EXPECT_GT(std::abs(j - 1.0f), 0.25f) << "geometry too symmetric for this test to mean anything, J=" << j;
        }

        std::mt19937 rng(0x5EEDu);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
        glm::dvec3 withJacobian(0.0);
        glm::dvec3 withoutJacobian(0.0);
        constexpr u32 kPixels = 20000;

        for (u32 i = 0; i < kPixels; ++i)
        {
            // Both reservoirs are built independently, as the initial draw does.
            const Reservoir own = SampleAreaReservoir(destination, emitter, 8, rng);
            const Reservoir other = SampleAreaReservoir(neighbour, emitter, 8, rng);

            // The merge, twice: once with the measure conversion and once
            // without it, so the SAME candidate stream produces both arms and the
            // difference is the Jacobian alone.
            const auto merge = [&](bool applyJacobian)
            {
                Reservoir merged{};
                // w_i = M_i * pHat_dest(y_i) * W_i under 1/M: the reservoir stands
                // for M_i candidates, and FinalizeCombined's summed-M denominator
                // divides them back out. The centre's own shift is the identity,
                // so its J is 1.
                const f32 ownTarget = TargetPdf(destination, own.Sample);
                ReservoirUpdate(merged, own.Sample, own.M * ownTarget * own.W, ownTarget, uniform(rng));

                // w = m * pHat_dest(y) * (W * J), with pHat LEFT ALONE. The
                // Jacobian multiplies the CONTRIBUTION WEIGHT — see
                // ShiftedContributionWeight's derivation in ReservoirDI.h. The
                // broken arm simply omits it, which is what this test is the
                // negative control for.
                const f32 otherTarget = TargetPdf(destination, other.Sample);
                f32 otherW = other.W;
                if (applyJacobian)
                {
                    const f32 j = ShiftJacobian(other.Sample, destination.Position, neighbour.Position);
                    otherW = ShiftedContributionWeight(other.W, j);
                }
                ReservoirUpdate(merged, other.Sample, other.M * otherTarget * otherW, otherTarget,
                                uniform(rng));

                FinalizeCombined(merged, BiasMode::Biased, own.M + other.M);
                return ResolveReservoir(destination, merged);
            };

            // One shared draw sequence per arm would desynchronise the RNG, so
            // each arm is evaluated from the same two reservoirs but consumes its
            // own acceptance numbers; that is fine, both arms are unbiased
            // estimators of whatever they estimate.
            withJacobian += glm::dvec3(merge(true));
            withoutJacobian += glm::dvec3(merge(false));
        }

        const glm::vec3 meanWith = glm::vec3(withJacobian / static_cast<f64>(kPixels));
        const glm::vec3 meanWithout = glm::vec3(withoutJacobian / static_cast<f64>(kPixels));

        const f32 errorWith = RelativeError(meanWith, truth);
        const f32 errorWithout = RelativeError(meanWithout, truth);

        EXPECT_LT(errorWith, 0.03f) << "with the Jacobian: mean (" << meanWith.x << ", " << meanWith.y << ", "
                                    << meanWith.z << ") vs quadrature (" << truth.x << ", " << truth.y << ", "
                                    << truth.z << ")";
        // The negative control. If this ever passes, the Jacobian is not being
        // applied where the test thinks it is and the arm above proves nothing.
        EXPECT_GT(errorWithout, errorWith * 2.0f)
            << "dropping the Jacobian did NOT measurably bias the estimate (with " << errorWith << ", without "
            << errorWithout << ") - the test's geometry or its wiring is wrong, not the estimator";
    }

    // -------------------------------------------------------------------------
    // Temporal reuse and the M cap
    // -------------------------------------------------------------------------

    // A reservoir merged into itself frame after frame, capped at M, must not
    // drift. This is the arithmetic behind "the lighting is stable" — and a cap
    // that rescaled M without WeightSum would make this test brighten every
    // iteration, which is the failure mode nobody would attribute to a cap.
    TEST(ReSTIRDIOracle, TemporalReuseUnderTheMCapDoesNotDrift)
    {
        const PunctualLightSet set = MakeManyPointLights(64, 0x111u);
        LambertianSurface surface{};
        const glm::vec3 truth = set.AnalyticDirectLighting(surface);
        ASSERT_GT(glm::length(truth), 0.0f);

        constexpr f32 kMCap = 20.0f;
        constexpr u32 kFrames = 400;
        constexpr u32 kPixels = 2000;

        std::mt19937 rng(0xFEEDu);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);

        // Every pixel carries its own history, as the screen-space buffers do.
        std::vector<Reservoir> history(kPixels);
        glm::dvec3 accumulatedLateFrames(0.0);
        u32 lateFrameSamples = 0;

        for (u32 frame = 0; frame < kFrames; ++frame)
        {
            for (u32 pixel = 0; pixel < kPixels; ++pixel)
            {
                const Reservoir fresh = SampleInitialReservoir(surface, set, 4, rng);
                Reservoir& previous = history[pixel];

                Reservoir merged{};
                const f32 freshTarget = TargetPdf(surface, fresh.Sample);
                const f32 previousTarget = TargetPdf(surface, previous.Sample);
                ReservoirUpdate(merged, fresh.Sample, fresh.M * freshTarget * fresh.W, freshTarget,
                                uniform(rng));
                if (!previous.IsEmpty())
                    ReservoirUpdate(merged, previous.Sample, previous.M * previousTarget * previous.W,
                                    previousTarget, uniform(rng));

                FinalizeCombined(merged, BiasMode::Biased, fresh.M + previous.M);
                ApplyTemporalMCap(merged, kMCap);
                previous = merged;

                // Measure only once the history has saturated at the cap: the
                // first frames are legitimately a different (smaller) M, and
                // averaging them in would hide a drift that starts later.
                if (frame >= kFrames / 2)
                {
                    accumulatedLateFrames += glm::dvec3(ResolveReservoir(surface, merged));
                    ++lateFrameSamples;
                }
            }
        }

        ASSERT_GT(lateFrameSamples, 0u);
        const glm::vec3 mean = glm::vec3(accumulatedLateFrames / static_cast<f64>(lateFrameSamples));
        EXPECT_LT(RelativeError(mean, truth), 0.03f)
            << "after " << kFrames << " frames at an M cap of " << kMCap << ": mean (" << mean.x << ", "
            << mean.y << ", " << mean.z << ") vs truth (" << truth.x << ", " << truth.y << ", " << truth.z << ")";

        // And the cap actually bound. A test that never reached it would prove
        // nothing about the rescale.
        f32 maxM = 0.0f;
        for (const auto& r : history)
            maxM = std::max(maxM, r.M);
        EXPECT_NEAR(maxM, kMCap, 1.0e-3f);
    }
} // namespace OloEngine::Tests
