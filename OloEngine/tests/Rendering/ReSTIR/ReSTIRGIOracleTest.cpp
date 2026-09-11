// OLO_TEST_LAYER: plumbing
// =============================================================================
// ReSTIRGIOracleTest.cpp — does the ReSTIR GI estimator compute the right
// number? Issue #1169.
//
// WHY THIS FILE IS SEPARATE FROM THE CONTRACT TEST. The contract test pins the
// PIECES: the Jacobian is a measure change, the guards guard, the hand-off is
// exclusive. Every one of those can pass while the assembled estimator converges
// to the wrong integral, because the pieces can be individually correct and
// wired together wrongly — a Jacobian applied to the wrong term, a source
// density converted at the wrong point, an MIS weight applied on top of a 1/M
// normaliser. That is the failure #1169 was filed against: "the reconnection
// Jacobian is the term that is silently wrong when the image merely looks
// plausible."
//
// THE ORACLE IS NOT ANOTHER SAMPLER. The ground truth here is DENSE STRATIFIED
// HEMISPHERE QUADRATURE over an analytically-shaded Lambertian occluder —
// deterministic, no random numbers on the ground-truth side, computed the one
// way that cannot share a bug with the estimator. Comparing a resampler against
// another sampler is a much weaker check: they can be wrong together, and in
// #1140 they were (measure-convention doc §2b).
//
// WHAT IT DELIBERATELY DOES NOT MODEL. Textures, the versioned closure, the
// probe tail, and any vertex that is not Lambertian — all of those are other
// subsystems with their own tests, and mixing them in would make a failure here
// ambiguous between two of them. What is left is exactly the RESAMPLING.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReSTIR/ReservoirGI.h"

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
        constexpr f32 kPi = std::numbers::pi_v<f32>;

        // -------------------------------------------------------------------
        // The scene: a Lambertian receiver under a Lambertian occluder plane,
        // lit by one punctual light.
        // -------------------------------------------------------------------
        //
        // WHY A PLANE AND A POINT LIGHT. Both halves of the integrand then have
        // a CLOSED FORM: the plane's outgoing radiance at any point is
        // albedo/pi * I * cos(theta_l) / d^2 with analytic visibility, and the
        // one-bounce integral at the receiver is a hemisphere integral of a
        // function this test can evaluate exactly. So the quadrature below is
        // integrating a known function rather than sampling a scene, which is
        // what makes it an oracle and not a second estimator.
        struct Scene
        {
            // The receiver: the origin, facing +Z.
            glm::vec3 ReceiverPosition{ 0.0f };
            glm::vec3 ReceiverNormal{ 0.0f, 0.0f, 1.0f };
            glm::vec3 ReceiverAlbedo{ 0.7f, 0.6f, 0.5f };

            // The occluder: the plane z = Height, facing -Z (down, toward the
            // receiver). Bounded, so the integrand has an edge the estimator has
            // to find rather than a constant it cannot get wrong.
            f32 PlaneHeight = 2.0f;
            f32 PlaneHalfExtent = 3.0f;
            glm::vec3 PlaneNormal{ 0.0f, 0.0f, -1.0f };
            glm::vec3 PlaneAlbedo{ 0.9f, 0.4f, 0.2f };

            // The light, between the receiver and the plane so the plane's
            // underside is genuinely lit and the bounce carries real colour.
            glm::vec3 LightPosition{ 1.1f, -0.7f, 1.2f };
            glm::vec3 LightIntensity{ 12.0f, 10.0f, 8.0f };

            [[nodiscard]] bool InsidePlane(const glm::vec3& p) const
            {
                return std::abs(p.x) <= PlaneHalfExtent && std::abs(p.y) <= PlaneHalfExtent;
            }

            // The ray from `origin` along `direction`: does it hit the plane, and
            // where? Analytic, so the "trace" cannot disagree with the quadrature
            // about the geometry.
            //
            // THE ORIGIN IS A PARAMETER, and it must be. It was ReceiverPosition
            // once, which made every "neighbour" reservoir below trace from the
            // DESTINATION - so the neighbour's samples needed no shift at all, the
            // Jacobian arm came out 44% too bright and the no-Jacobian arm matched
            // quadrature exactly. The negative control caught it: an estimator bug
            // makes the correct arm wrong, a HARNESS bug makes the broken arm
            // right, and only having both arms tells the two apart.
            [[nodiscard]] bool Intersect(const glm::vec3& origin, const glm::vec3& direction,
                                         glm::vec3& hit) const
            {
                if (!(direction.z > 1.0e-6f))
                    return false;
                const f32 t = (PlaneHeight - origin.z) / direction.z;
                if (!(t > 0.0f))
                    return false;
                hit = origin + direction * t;
                return InsidePlane(hit);
            }

            // L_o at a point on the plane: the DIFFUSE lobe only, and NO
            // EMISSION — the plane does not emit, and if it did, its emission
            // would be the DIRECT tier's (design note §1.1). Direction-independent
            // by construction, which is what licenses reconnecting to it.
            [[nodiscard]] glm::vec3 PlaneOutgoingRadiance(const glm::vec3& point) const
            {
                const glm::vec3 toLight = LightPosition - point;
                const f32 distanceSq = glm::dot(toLight, toLight);
                if (!(distanceSq > 0.0f))
                    return glm::vec3(0.0f);
                const glm::vec3 l = toLight / std::sqrt(distanceSq);
                const f32 cosLight = glm::dot(PlaneNormal, l);
                if (!(cosLight > 0.0f))
                    return glm::vec3(0.0f);
                // The light is below the plane and nothing is between them in
                // this scene, so visibility is 1 analytically.
                return PlaneAlbedo * (1.0f / kPi) * LightIntensity * cosLight / distanceSq;
            }

            [[nodiscard]] glm::vec3 ReceiverBrdf() const
            {
                return ReceiverAlbedo * (1.0f / kPi);
            }

            // THE ORACLE. The one-bounce indirect irradiance at the receiver, by
            // dense stratified quadrature over the hemisphere in the CONCENTRIC
            // (cos-theta, phi) parameterisation — so the cell measure is exactly
            // dw and no Jacobian of the test's own can be wrong in the same
            // direction as the estimator's.
            [[nodiscard]] glm::vec3 QuadratureOneBounce(u32 resolution) const
            {
                glm::dvec3 total(0.0);
                const f32 cellSolidAngle = 2.0f * kPi / static_cast<f32>(resolution * resolution);
                for (u32 iu = 0; iu < resolution; ++iu)
                {
                    // Uniform in cos(theta) over [0,1] gives a uniform hemisphere
                    // measure; the integrand's own cosine stays in the integrand.
                    const f32 cosTheta =
                        (static_cast<f32>(iu) + 0.5f) / static_cast<f32>(resolution);
                    const f32 sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
                    for (u32 iv = 0; iv < resolution; ++iv)
                    {
                        const f32 phi = 2.0f * kPi * (static_cast<f32>(iv) + 0.5f) /
                                        static_cast<f32>(resolution);
                        const glm::vec3 direction(sinTheta * std::cos(phi), sinTheta * std::sin(phi),
                                                  cosTheta);
                        glm::vec3 hit;
                        if (!Intersect(ReceiverPosition, direction, hit))
                            continue;
                        const glm::vec3 incoming = PlaneOutgoingRadiance(hit);
                        total += glm::dvec3(ReceiverBrdf() * incoming * cosTheta * cellSolidAngle);
                    }
                }
                return glm::vec3(total);
            }
        };

        // The target function, in the form the shader evaluates it:
        // || f_r(x0) * L_o(x1) * cos(theta0) ||, luminance-weighted.
        [[nodiscard]] glm::vec3 Contribution(const Scene& scene, const glm::vec3& shadingPoint,
                                             const glm::vec3& shadingNormal, const GISample& sample)
        {
            const glm::vec3 toVertex = sample.Position - shadingPoint;
            const f32 distanceSq = glm::dot(toVertex, toVertex);
            if (!(distanceSq > 0.0f))
                return glm::vec3(0.0f);
            const glm::vec3 l = toVertex / std::sqrt(distanceSq);
            const f32 nDotL = glm::dot(shadingNormal, l);
            if (!(nDotL > 0.0f))
                return glm::vec3(0.0f);
            return scene.ReceiverBrdf() * sample.Radiance * nDotL;
        }

        [[nodiscard]] f32 TargetPdf(const Scene& scene, const glm::vec3& shadingPoint,
                                    const glm::vec3& shadingNormal, const GISample& sample)
        {
            const glm::vec3 c = Contribution(scene, shadingPoint, shadingNormal, sample);
            const f32 t = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            return (std::isfinite(t) && t > 0.0f) ? t : 0.0f;
        }

        // One cosine-weighted bounce, traced analytically. The SOURCE DENSITY IS
        // IN SOLID ANGLE at the shading point, which is the measure the target
        // function is in too — so the initial RIS weight needs no conversion, and
        // the area measure the reservoir stores the vertex in only becomes
        // visible at reuse, as the Jacobian.
        [[nodiscard]] bool SampleBounce(const Scene& scene, const glm::vec3& shadingPoint,
                                        const glm::vec3& shadingNormal, f32 xi0, f32 xi1,
                                        GISample& outSample, f32& outPdf)
        {
            outSample = GISample{};
            outPdf = 0.0f;

            const f32 cosTheta = std::sqrt(std::max(1.0f - xi0, 0.0f));
            const f32 sinTheta = std::sqrt(std::max(xi0, 0.0f));
            const f32 phi = 2.0f * kPi * xi1;
            // The receiver's normal is +Z in this scene, so the tangent frame is
            // the identity and the direction is the local one. Spelled out rather
            // than assumed, because a test that built a basis would be testing its
            // own basis.
            const glm::vec3 direction(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
            if (!(glm::dot(direction, shadingNormal) > 0.0f))
                return false;

            glm::vec3 hit;
            if (!scene.Intersect(shadingPoint, direction, hit))
                return false; // an escape: this scene has no environment, so it contributes nothing

            outSample.Kind = GISampleKind::SurfaceHit;
            outSample.Position = hit;
            outSample.Normal = scene.PlaneNormal;
            outSample.Radiance = scene.PlaneOutgoingRadiance(hit);
            outPdf = cosTheta / kPi;
            return outPdf > 0.0f;
        }

        // One pixel's worth of the estimator: RIS over `candidates` bounces,
        // finalised the way ReSTIR_GI_InitialSample.glsl does.
        [[nodiscard]] GIReservoir SampleInitialReservoir(const Scene& scene, const glm::vec3& shadingPoint,
                                                         const glm::vec3& shadingNormal, u32 candidates,
                                                         std::mt19937& rng)
        {
            std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
            GIReservoir reservoir{};
            for (u32 c = 0; c < candidates; ++c)
            {
                GISample candidate;
                f32 pdf = 0.0f;
                if (!SampleBounce(scene, shadingPoint, shadingNormal, uniform(rng), uniform(rng), candidate,
                                  pdf))
                {
                    // A rejected draw still counts toward M: it WAS a candidate
                    // the estimator considered, and dropping it would inflate
                    // every surviving candidate's weight by the rejection rate.
                    reservoir.M += 1.0f;
                    // The acceptance number is consumed regardless, so the two
                    // arms stay aligned in the stream.
                    (void)uniform(rng);
                    continue;
                }
                const f32 targetPdf = TargetPdf(scene, shadingPoint, shadingNormal, candidate);
                ReservoirUpdate(reservoir, candidate, targetPdf / pdf, targetPdf, uniform(rng));
            }
            FinalizeInitialCandidates(reservoir);
            return reservoir;
        }

        [[nodiscard]] glm::vec3 ResolveReservoir(const Scene& scene, const glm::vec3& shadingPoint,
                                                 const glm::vec3& shadingNormal, const GIReservoir& reservoir)
        {
            if (reservoir.IsEmpty() || !(reservoir.W > 0.0f))
                return glm::vec3(0.0f);
            return Contribution(scene, shadingPoint, shadingNormal, reservoir.Sample) * reservoir.W;
        }

        [[nodiscard]] f32 RelativeError(const glm::vec3& measured, const glm::vec3& truth)
        {
            const f32 denominator = std::max(glm::length(truth), 1.0e-6f);
            return glm::length(measured - truth) / denominator;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The integral the tier exists to estimate
    // -------------------------------------------------------------------------

    TEST(ReSTIRGIOracle, CosineBounceRISConvergesToDenseHemisphereQuadrature)
    {
        const Scene scene{};
        const glm::vec3 truth = scene.QuadratureOneBounce(512);
        ASSERT_GT(glm::length(truth), 0.0f);

        std::mt19937 rng(0x1169u);
        glm::dvec3 accumulated(0.0);
        constexpr u32 kPixels = 40000;
        for (u32 i = 0; i < kPixels; ++i)
        {
            const GIReservoir reservoir =
                SampleInitialReservoir(scene, scene.ReceiverPosition, scene.ReceiverNormal, 4, rng);
            accumulated += glm::dvec3(
                ResolveReservoir(scene, scene.ReceiverPosition, scene.ReceiverNormal, reservoir));
        }
        const glm::vec3 mean = glm::vec3(accumulated / static_cast<f64>(kPixels));

        // 2.5%. A missing normaliser is off by a factor of 4 here, and a target
        // function that ignored the BRDF or the receiver cosine is off by tens of
        // percent — this bound catches both while leaving room for the Monte
        // Carlo noise that legitimately remains at this sample count.
        EXPECT_LT(RelativeError(mean, truth), 0.025f)
            << "mean (" << mean.x << ", " << mean.y << ", " << mean.z << ") vs quadrature (" << truth.x
            << ", " << truth.y << ", " << truth.z << ")";
    }

    // -------------------------------------------------------------------------
    // Spatial reuse: the reconnection Jacobian in situ, with its negative control
    // -------------------------------------------------------------------------

    // THE TEST THE JACOBIAN EXISTS FOR. Two shading points whose views of the
    // same bounce vertex genuinely differ; the destination merges the neighbour's
    // reservoir. With the Jacobian the estimate still matches quadrature; the
    // assertion below ALSO checks that dropping it does not — because a Jacobian
    // that were silently a no-op would pass every test that only measured the
    // correct arm.
    TEST(ReSTIRGIOracle, SpatialReuseWithTheJacobianStaysUnbiasedAndWithoutItDoesNot)
    {
        Scene scene{};
        const glm::vec3 destination = scene.ReceiverPosition;
        const glm::vec3 normal = scene.ReceiverNormal;
        // Far enough out that the neighbour sees the SAME vertices at genuinely
        // different distances and angles. A neighbour a texel away would make
        // every Jacobian ~1 and the negative control vacuous.
        const glm::vec3 neighbour(1.6f, 1.1f, 0.0f);

        const glm::vec3 truth = scene.QuadratureOneBounce(512);
        ASSERT_GT(glm::length(truth), 0.0f);

        // The Jacobian must actually be doing work at this geometry, or the test
        // proves nothing. Checked here rather than assumed.
        {
            std::mt19937 probe(1u);
            const GIReservoir sample = SampleInitialReservoir(scene, neighbour, normal, 8, probe);
            ASSERT_FALSE(sample.IsEmpty());
            const f32 j = GIShiftJacobian(sample.Sample, destination, neighbour);
            ASSERT_GT(j, 0.0f);
            // The merged estimate is a MIXTURE of the centre's (correct) sample
            // and the neighbour's, so the visible bias is only a fraction of J's
            // deviation. A geometry that barely moved J would leave the negative
            // control inside the Monte Carlo noise and it would pass whether or
            // not the Jacobian is applied.
            EXPECT_GT(std::abs(j - 1.0f), 0.25f)
                << "geometry too symmetric for this test to mean anything, J=" << j;
        }

        std::mt19937 rng(0x5EEDu);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
        glm::dvec3 withJacobian(0.0);
        glm::dvec3 withoutJacobian(0.0);
        constexpr u32 kPixels = 40000;

        for (u32 i = 0; i < kPixels; ++i)
        {
            // Both reservoirs are built independently, as the initial draw does.
            const GIReservoir own = SampleInitialReservoir(scene, destination, normal, 4, rng);
            const GIReservoir other = SampleInitialReservoir(scene, neighbour, normal, 4, rng);

            const auto merge = [&](bool applyJacobian)
            {
                GIReservoir merged{};
                // w_i = M_i * pHat_dest(y_i) * W_i under 1/M: the reservoir
                // stands for M_i candidates, and FinalizeCombined's summed-M
                // denominator divides them back out. The centre's own shift is
                // the identity, so its J is 1.
                const f32 ownTarget = TargetPdf(scene, destination, normal, own.Sample);
                ReservoirUpdate(merged, own.Sample, own.M * ownTarget * own.W, ownTarget, uniform(rng));

                // w = m * pHat_dest(y) * (W * J), with pHat LEFT ALONE. The
                // Jacobian multiplies the CONTRIBUTION WEIGHT — see
                // ShiftedContributionWeight's derivation in ReservoirCore.h. The
                // broken arm simply omits it, which is what this test is the
                // negative control for.
                const f32 otherTarget = TargetPdf(scene, destination, normal, other.Sample);
                f32 otherW = other.W;
                if (applyJacobian)
                {
                    const f32 j = GIShiftJacobian(other.Sample, destination, neighbour);
                    otherW = ShiftedContributionWeight(other.W, j);
                }
                ReservoirUpdate(merged, other.Sample, other.M * otherTarget * otherW, otherTarget,
                                uniform(rng));

                FinalizeCombined(merged, BiasMode::Biased, own.M + other.M);
                return ResolveReservoir(scene, destination, normal, merged);
            };

            withJacobian += glm::dvec3(merge(true));
            withoutJacobian += glm::dvec3(merge(false));
        }

        const glm::vec3 meanWith = glm::vec3(withJacobian / static_cast<f64>(kPixels));
        const glm::vec3 meanWithout = glm::vec3(withoutJacobian / static_cast<f64>(kPixels));

        const f32 errorWith = RelativeError(meanWith, truth);
        const f32 errorWithout = RelativeError(meanWithout, truth);

        EXPECT_LT(errorWith, 0.04f) << "with the Jacobian: mean (" << meanWith.x << ", " << meanWith.y
                                    << ", " << meanWith.z << ") vs quadrature (" << truth.x << ", "
                                    << truth.y << ", " << truth.z << ")";
        // THE NEGATIVE CONTROL. If this ever passes, the Jacobian is not being
        // applied where the test thinks it is and the arm above proves nothing.
        EXPECT_GT(errorWithout, errorWith * 2.0f)
            << "dropping the Jacobian did NOT measurably bias the estimate (with " << errorWith
            << ", without " << errorWithout
            << ") - the test's geometry or its wiring is wrong, not the estimator";
    }

    // -------------------------------------------------------------------------
    // Temporal reuse, the M cap and the AGE cap
    // -------------------------------------------------------------------------

    // A reservoir merged into itself frame after frame, capped at M, must not
    // drift. This is the arithmetic behind "the indirect light is stable" — and a
    // cap that rescaled M without WeightSum would make this test brighten every
    // iteration, which is the failure nobody would attribute to a cap.
    TEST(ReSTIRGIOracle, TemporalReuseUnderTheMCapDoesNotDrift)
    {
        const Scene scene{};
        const glm::vec3 point = scene.ReceiverPosition;
        const glm::vec3 normal = scene.ReceiverNormal;
        const glm::vec3 truth = scene.QuadratureOneBounce(512);
        ASSERT_GT(glm::length(truth), 0.0f);

        constexpr f32 kMCap = 20.0f;
        constexpr u32 kFrames = 300;
        constexpr u32 kPixels = 1500;

        std::mt19937 rng(0xFEEDu);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);

        std::vector<GIReservoir> history(kPixels);
        glm::dvec3 accumulatedLateFrames(0.0);
        u32 lateFrameSamples = 0;

        for (u32 frame = 0; frame < kFrames; ++frame)
        {
            for (u32 pixel = 0; pixel < kPixels; ++pixel)
            {
                const GIReservoir fresh = SampleInitialReservoir(scene, point, normal, 1, rng);
                GIReservoir& previous = history[pixel];

                GIReservoir merged{};
                const f32 freshTarget = TargetPdf(scene, point, normal, fresh.Sample);
                const f32 previousTarget = TargetPdf(scene, point, normal, previous.Sample);
                ReservoirUpdate(merged, fresh.Sample, fresh.M * freshTarget * fresh.W, freshTarget,
                                uniform(rng));
                if (!previous.IsEmpty())
                {
                    // The shading point did not move, so the shift is the
                    // identity and J is exactly 1 — which is what the temporal
                    // draw computes when the reprojection lands on the same
                    // point, rather than what it assumes.
                    const f32 j = GIShiftJacobian(previous.Sample, point, point);
                    EXPECT_FLOAT_EQ(j, 1.0f);
                    ReservoirUpdate(merged, previous.Sample,
                                    previous.M * previousTarget * ShiftedContributionWeight(previous.W, j),
                                    previousTarget, uniform(rng));
                }

                FinalizeCombined(merged, BiasMode::Biased, fresh.M + previous.M);
                ApplyTemporalMCap(merged, kMCap);
                merged.Sample.Age = AdvanceSampleAge(merged.Sample.Age);
                previous = merged;

                // Measure only once the history has saturated at the cap: the
                // first frames are legitimately a different (smaller) M, and
                // averaging them in would hide a drift that starts later.
                if (frame >= kFrames / 2)
                {
                    accumulatedLateFrames += glm::dvec3(ResolveReservoir(scene, point, normal, merged));
                    ++lateFrameSamples;
                }
            }
        }

        ASSERT_GT(lateFrameSamples, 0u);
        const glm::vec3 mean = glm::vec3(accumulatedLateFrames / static_cast<f64>(lateFrameSamples));
        EXPECT_LT(RelativeError(mean, truth), 0.04f)
            << "after " << kFrames << " frames at an M cap of " << kMCap << ": mean (" << mean.x << ", "
            << mean.y << ", " << mean.z << ") vs quadrature (" << truth.x << ", " << truth.y << ", "
            << truth.z << ")";

        // And the cap actually bound. A test that never reached it would prove
        // nothing about the rescale.
        f32 maxM = 0.0f;
        for (const auto& r : history)
            maxM = std::max(maxM, r.M);
        EXPECT_NEAR(maxM, kMCap, 1.0e-3f);

        // THE AGE CAP IS A SEPARATE BOUND, and this is where the difference shows.
        // The merge above never rejects on age, and the M cap bound every pixel at
        // 20 within the first frames — yet samples far older than the DEFAULT age
        // cap survive, because a sample's age only resets when a fresh candidate
        // wins the reservoir and a sample that keeps winning keeps ageing. An
        // implementation that relied on the M cap to bound staleness would look
        // exactly like this one and would keep such a sample indefinitely.
        //
        // Not asserted as EQUAL to the frame count: the survivor's age resets
        // every time a fresh candidate wins, which is the correct behaviour, so
        // the maximum is a property of the RNG rather than of the cap. What
        // matters is that it comfortably exceeds the cap the temporal draw applies.
        u32 maxAge = 0;
        for (const auto& r : history)
            maxAge = std::max(maxAge, r.Sample.Age);
        EXPECT_GT(maxAge, ReSTIR::kDefaultMaxSampleAge)
            << "no sample outlived the default age cap, so this test says nothing about a bound the M cap "
               "does not already provide";
        EXPECT_FALSE(SampleAgeAcceptable(maxAge, ReSTIR::kDefaultMaxSampleAge))
            << "a sample this old would still be merged under the default age cap";
    }

    // -------------------------------------------------------------------------
    // The two bias modes agree where they must
    // -------------------------------------------------------------------------

    // Two reservoirs on the SAME surface DO share a domain, so 1/M and the
    // balance heuristic must reach the same number there. That identity is what
    // catches a normaliser applied twice — the classic bug the shared
    // ComputeContributionWeight exists to make visible — because applying it
    // twice breaks only one of the two arms.
    TEST(ReSTIRGIOracle, BothBiasModesAgreeOnReservoirsThatShareADomain)
    {
        const Scene scene{};
        const glm::vec3 point = scene.ReceiverPosition;
        const glm::vec3 normal = scene.ReceiverNormal;

        std::mt19937 rng(0xB1A5u);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);
        u32 compared = 0;
        for (u32 i = 0; i < 200; ++i)
        {
            const GIReservoir a = SampleInitialReservoir(scene, point, normal, 4, rng);
            const GIReservoir b = SampleInitialReservoir(scene, point, normal, 4, rng);
            if (a.IsEmpty() || b.IsEmpty())
                continue;

            const f32 targetA = TargetPdf(scene, point, normal, a.Sample);
            const f32 targetB = TargetPdf(scene, point, normal, b.Sample);
            if (!(targetA > 0.0f) || !(targetB > 0.0f))
                continue;

            const f32 xi0 = uniform(rng);
            const f32 xi1 = uniform(rng);

            GIReservoir biased{};
            ReservoirUpdate(biased, a.Sample, a.M * targetA * a.W, targetA, xi0);
            ReservoirUpdate(biased, b.Sample, b.M * targetB * b.W, targetB, xi1);
            FinalizeCombined(biased, BiasMode::Biased, a.M + b.M);

            // On one surface pHat_a and pHat_b are the SAME function, so the
            // balance heuristic reduces to the M-weighted split.
            const f32 denominator = a.M + b.M;
            GIReservoir unbiased{};
            ReservoirUpdate(unbiased, a.Sample, (a.M / denominator) * targetA * a.W, targetA, xi0);
            ReservoirUpdate(unbiased, b.Sample, (b.M / denominator) * targetB * b.W, targetB, xi1);
            FinalizeCombined(unbiased, BiasMode::UnbiasedMIS, a.M + b.M);

            ++compared;
            EXPECT_NEAR(biased.W, unbiased.W, std::max(biased.W, unbiased.W) * 1.0e-4f) << "pair " << i;
            EXPECT_EQ(biased.Sample, unbiased.Sample) << "pair " << i;
        }
        EXPECT_GT(compared, 100u) << "too few usable pairs for this identity to mean anything";
    }
} // namespace OloEngine::Tests
