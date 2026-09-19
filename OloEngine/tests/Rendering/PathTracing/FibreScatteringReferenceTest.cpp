#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// FibreScatteringReferenceTest — issue #1255, the FIBRE half, and the one of
// the four models that is a LOCAL scattering function.
//
// WHY "LOCAL" IS THE FIRST WORD OF THIS FILE. #1255's second acceptance
// criterion asks for local BSDF tests to be distinguished from nonlocal skin
// diffusion and coat transport, and the distinction is not a filing
// convenience: it decides which questions have answers. A fibre BCSDF is a
// function of two directions at one point, so energy, reciprocity and
// sampling consistency are all well posed and all asked here. The two nonlocal
// models get a different file and a different set of questions; asking one of
// them for reciprocity would be asserting a coincidence.
//
// WHAT MAKES THIS AN INDEPENDENT REFERENCE, and why GroomFibrePropertyTests
// does not already cover it. That file compares the shipped h-quadrature
// against the SAME model's converged quadrature — a genuine and valuable test
// of the approximation, and no test at all of the thing being approximated. A
// sign error in Chiang's attenuation recurrence would be reproduced identically
// by both arms and pass. This file's oracle is
// MaterialReference::FibreCylinderWalk: explicit 3D entry geometry, full 3D
// Snell, a term-by-term path sum and a second, stochastic walk that never forms
// the recurrence's products at all.
//
// THREE PRODUCTION FORMULAS BECOME CONSEQUENCES HERE rather than inputs. The
// incidence cosine cos(theta_o) cos(gamma_o), Bravais' modified index, and the
// closed-form geometric tail are each derived independently by the reference
// and compared. They are the three places a fibre model is quietly wrong: each
// is correct at normal incidence, so a screenshot cannot separate them.
//
// TOLERANCES. Every assertion below states its own, and the two kinds are kept
// apart on purpose:
//   * IDENTITIES — a formula that must hold exactly up to f32 rounding. 1e-5
//     relative, which is the width of a float32 mantissa over these magnitudes.
//   * ESTIMATES — a Monte Carlo or quadrature result. The tolerance is three
//     standard errors of the estimator at the sample count the test runs, and
//     the sample count is named at the site.
//
// Runtime budget: the whole file is under a second in a Debug build. Every
// estimator is seeded, so a failure reproduces.
// =============================================================================

#include "PathTracing/MaterialReference.h"

#include "OloEngine/Groom/GroomFibreScattering.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace Ref = MaterialReference;

        // The highest h-quadrature order the production model accepts. Used
        // everywhere the comparison wants the production side as converged as
        // it can be, so the residual is the model and not the rule.
        constexpr u32 kNodes = GroomFibreLimits::MaxHSamples;

        [[nodiscard]] GroomFibreAuthoring MakeAuthoring(const glm::vec3& sigmaA, f32 tiltDegrees, f32 roughness)
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Absorption;
            authored.Absorption = sigmaA;
            authored.TiltDegrees = tiltDegrees;
            authored.LongitudinalRoughness = roughness;
            authored.AzimuthalRoughness = roughness;
            authored.HSamples = kNodes;
            return authored;
        }

        static_assert(kGroomFibreLobeCount == 4,
                      "MaterialReference::FibreFoldedAttenuationMean folds the reference series into exactly the "
                      "four buckets the production model keeps; a fifth lobe would need a different fold.");

        [[nodiscard]] f64 RelativeError(f64 measured, f64 expected) noexcept
        {
            const f64 scale = std::max({ std::abs(measured), std::abs(expected), 1.0e-12 });
            return std::abs(measured - expected) / scale;
        }

        // A direction in the fibre's local frame from its longitudinal sine and
        // its azimuth about the tangent. x is the tangent — the same frame
        // GroomFibreSample and GroomFibrePdf document.
        [[nodiscard]] glm::vec3 LocalDirection(f64 sinTheta, f64 phi) noexcept
        {
            const f64 cosTheta = Ref::SafeSqrt(1.0 - (sinTheta * sinTheta));
            return glm::vec3(static_cast<f32>(sinTheta), static_cast<f32>(cosTheta * std::cos(phi)),
                             static_cast<f32>(cosTheta * std::sin(phi)));
        }
    } // namespace

    // =========================================================================
    // 0. The generator every reference in this directory is seeded from.
    // =========================================================================
    //
    // WHY THIS LIVES HERE. MaterialReference::Pcg32 is shared by all four
    // references, and every tolerance any of them states is only meaningful
    // because the estimator's numbers do not move between runs or between
    // machines. Nothing else in the suite pins it — ReferenceBRDFTest checks
    // the unrelated PathSampler — so a change to the multiplier, the increment
    // or the output permutation would silently re-roll every reference number
    // in this directory and be visible only as a tolerance that started
    // failing somewhere else. It sits in this file because this is the
    // lowest-level of the four; it is about the header, not about fibres.

    TEST(MaterialReferenceRng, TheSeededSequenceIsPinnedAndReproducible)
    {
        // Same seed, same bytes. The property the determinism claim rests on.
        Ref::Pcg32 a(7u);
        Ref::Pcg32 b(7u);
        for (u32 i = 0; i < 1000u; ++i)
            ASSERT_EQ(a.NextU32(), b.NextU32()) << "draw " << i;

        // Different seeds diverge, so the test above cannot pass on a constant.
        EXPECT_NE(Ref::Pcg32(1u).NextU32(), Ref::Pcg32(2u).NextU32());

        // THE PINNED SEQUENCE. Generated from this implementation and checked
        // against an independent reimplementation of PCG32's state transition
        // and output permutation. It is what catches a changed multiplier,
        // increment or rotation — each of which leaves a generator that still
        // looks random and still agrees with itself, so nothing above would
        // notice.
        constexpr std::array<u32, 8> kExpected{ 3026813963u, 2945997021u, 3676703653u, 4014656713u,
                                                1454119243u, 4124725499u, 1338505325u, 4263745283u };
        Ref::Pcg32 pinned(0x1255u);
        for (sizet i = 0; i < kExpected.size(); ++i)
            EXPECT_EQ(pinned.NextU32(), kExpected[i]) << "draw " << i << " of the pinned 0x1255 sequence";
    }

    TEST(MaterialReferenceRng, TheUnitFloatConversionStaysInsideItsHalfOpenRange)
    {
        // Every walk takes -log(1 - u) somewhere, so a u that could reach 1
        // would take a logarithm of zero and the estimator would return an
        // infinity rather than a number. The conversion uses 24 mantissa bits
        // precisely so that cannot happen; this is that guarantee, measured.
        Ref::Pcg32 rng(99u);
        f64 lowest = 2.0;
        f64 highest = -1.0;
        for (u32 i = 0; i < 200000u; ++i)
        {
            const f64 u = rng.NextDouble();
            ASSERT_GE(u, 0.0) << "draw " << i;
            ASSERT_LT(u, 1.0) << "draw " << i;
            lowest = std::min(lowest, u);
            highest = std::max(highest, u);
        }
        // And it does cover the range, so the bound above is not passing on a
        // generator stuck near a constant.
        EXPECT_LT(lowest, 0.001) << "lowest draw " << lowest;
        EXPECT_GT(highest, 0.999) << "highest draw " << highest;
    }

    // =========================================================================
    // A. The geometry production takes as given, derived independently.
    // =========================================================================
    //
    // These four are IDENTITIES: the production expression and the reference's
    // measurement are two ways of writing the same geometric fact, so the only
    // difference allowed is float rounding. They are first because everything
    // after them is meaningless if the two sides disagree about where the ray
    // goes.

    TEST(FibreScatteringReference, IncidenceCosineIsTheProductOfTheTwoAngleCosines)
    {
        // Production's Attenuations() takes the Fresnel at cos(theta_o)
        // cos(gamma_o), which is the standard fibre shortcut. The reference
        // instead dots the real 3D travel direction with the real cylinder
        // normal. That they agree is what licenses the shortcut.
        constexpr f64 kEta = 1.55;
        for (i32 t = -8; t <= 8; ++t)
        {
            const f64 sinThetaO = 0.11 * static_cast<f64>(t);
            const f64 cosThetaO = std::sqrt(1.0 - (sinThetaO * sinThetaO));
            for (i32 i = -9; i <= 9; ++i)
            {
                const f64 h = 0.1 * static_cast<f64>(i);
                const f64 cosGammaO = std::sqrt(1.0 - (h * h));

                const Ref::FibrePathWalk walk = Ref::FibreCylinderWalk(sinThetaO, h, kEta, glm::dvec3(0.0), 1);
                EXPECT_NEAR(walk.CosIncidence, cosThetaO * cosGammaO, 1.0e-12)
                    << "sinThetaO = " << sinThetaO << ", h = " << h;
            }
        }
    }

    TEST(FibreScatteringReference, BravaisModifiedIndexFallsOutOfFullThreeDimensionalSnell)
    {
        // etaPrime = sqrt(eta^2 - sin^2 theta_o) / cos(theta_o) is the index the
        // AZIMUTHAL problem sees once the longitudinal tilt is projected out.
        // Production writes it down; the reference measures sin(gamma_o) /
        // sin(gamma_t) from a 3D refraction it performed with the full vector
        // form. A model that dropped the Bravais correction would place the TT
        // lobe correctly head-on and wrongly at every grazing angle — which is
        // the angle hair is actually seen at.
        constexpr f64 kEta = 1.55;
        for (i32 t = 0; t <= 8; ++t)
        {
            const f64 sinThetaO = 0.1 * static_cast<f64>(t);
            const f64 cosThetaO = std::sqrt(1.0 - (sinThetaO * sinThetaO));
            const f64 expected = std::sqrt((kEta * kEta) - (sinThetaO * sinThetaO)) / cosThetaO;

            // h away from 0, where sin(gamma_t) vanishes and the ratio is 0/0.
            for (i32 i = 1; i <= 9; ++i)
            {
                const f64 h = 0.1 * static_cast<f64>(i);
                const Ref::FibrePathWalk walk = Ref::FibreCylinderWalk(sinThetaO, h, kEta, glm::dvec3(0.0), 1);
                EXPECT_LT(RelativeError(walk.EtaPrime, expected), 1.0e-9)
                    << "sinThetaO = " << sinThetaO << ", h = " << h << ", measured = " << walk.EtaPrime
                    << ", Bravais = " << expected;
            }
        }
    }

    TEST(FibreScatteringReference, InternalAndExternalFresnelAgreeByReversibility)
    {
        // Chiang's recurrence uses ONE Fresnel term for the cuticle and every
        // internal wall. That is not an approximation — a ray and its reverse
        // see the same reflectance — but it is the kind of thing that is
        // assumed rather than checked, and a model that used the wrong relative
        // index inside would be wrong only at grazing angles. The reference
        // computes the two from opposite sides with different relative indices.
        constexpr f64 kEta = 1.55;
        for (i32 t = 0; t <= 8; ++t)
        {
            for (i32 i = 0; i <= 9; ++i)
            {
                const Ref::FibrePathWalk walk =
                    Ref::FibreCylinderWalk(0.1 * static_cast<f64>(t), 0.1 * static_cast<f64>(i), kEta, glm::dvec3(0.0), 1);
                EXPECT_NEAR(walk.FresnelInternal, walk.FresnelExternal, 1.0e-12)
                    << "t = " << t << ", i = " << i;
            }
        }
    }

    TEST(FibreScatteringReference, TheInternalChordIsTwoCosGammaTOverCosThetaT)
    {
        // The Beer-Lambert path length, which is where sigma_a's UNITS live.
        // The production header calls sigma_a "per fibre diameter"; the chord
        // it multiplies is 2 cos(gamma_t) / cos(theta_t), which is 2 head-on on
        // a UNIT-RADIUS cylinder — so the unit is per RADIUS. The reference
        // derives the same chord from the circle's geometry, which is what
        // settles which of the two the code means.
        constexpr f64 kEta = 1.55;
        const Ref::FibrePathWalk headOn = Ref::FibreCylinderWalk(0.0, 0.0, kEta, glm::dvec3(0.0), 1);
        EXPECT_NEAR(headOn.ChordLength, 2.0, 1.0e-12) << "a head-on chord is the diameter, 2 radii";

        for (i32 t = 0; t <= 7; ++t)
        {
            const f64 sinThetaO = 0.1 * static_cast<f64>(t);
            const f64 cosThetaT = std::sqrt(1.0 - ((sinThetaO / kEta) * (sinThetaO / kEta)));
            const f64 cosThetaO = std::sqrt(1.0 - (sinThetaO * sinThetaO));
            const f64 etaPrime = std::sqrt((kEta * kEta) - (sinThetaO * sinThetaO)) / cosThetaO;

            for (i32 i = 0; i <= 9; ++i)
            {
                const f64 h = 0.1 * static_cast<f64>(i);
                const f64 sinGammaT = std::clamp(h / etaPrime, -1.0, 1.0);
                const f64 expected = (2.0 * std::sqrt(1.0 - (sinGammaT * sinGammaT))) / cosThetaT;

                const Ref::FibrePathWalk walk = Ref::FibreCylinderWalk(sinThetaO, h, kEta, glm::dvec3(0.0), 1);
                EXPECT_LT(RelativeError(walk.ChordLength, expected), 1.0e-9)
                    << "sinThetaO = " << sinThetaO << ", h = " << h;
            }
        }
    }

    // =========================================================================
    // B. Attenuation and energy — the reference against the shipped model.
    // =========================================================================

    TEST(FibreScatteringReference, LobeAttenuationsMatchTheCylinderWalk)
    {
        // The handle into the production attenuations is
        // GroomFibreAmbientResponse, which is documented as the mean over the
        // fibre's width of each path's attenuation, times cos(theta_o). Divide
        // the cosine out and it IS the quantity the reference computes — over
        // the same quadrature nodes, so the only thing the comparison can be
        // measuring is the recurrence.
        //
        // This is the file's central assertion. Every other test here supports
        // it or bounds what it does not cover.
        struct Case
        {
            const char* Name;
            glm::vec3 SigmaA;
        };
        const std::array<Case, 4> cases{ {
            { "no absorption", glm::vec3(0.0f) },
            { "pale blonde", glm::vec3(0.06f, 0.10f, 0.20f) },
            { "dark brown", glm::vec3(0.84f, 1.39f, 2.74f) },
            { "black", glm::vec3(4.0f, 6.0f, 12.0f) },
        } };

        for (const Case& testCase : cases)
        {
            const GroomFibreParams params = MakeGroomFibreParams(MakeAuthoring(testCase.SigmaA, 0.0f, 0.3f));

            for (i32 t = 0; t <= 7; ++t)
            {
                const f64 sinThetaO = 0.1 * static_cast<f64>(t);
                const f64 cosThetaO = std::sqrt(1.0 - (sinThetaO * sinThetaO));

                const GroomFibreLobeSet production = GroomFibreAmbientResponse(params, static_cast<f32>(sinThetaO));
                const std::array<glm::dvec3, 4> reference = Ref::FibreFoldedAttenuationMean(
                    sinThetaO, static_cast<f64>(params.Eta), glm::dvec3(params.SigmaA), kNodes);

                for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
                {
                    for (i32 c = 0; c < 3; ++c)
                    {
                        const f64 measured = static_cast<f64>(production.Lobe[p][c]) / cosThetaO;
                        // 3e-5 relative: the production side runs in f32, and
                        // the residual is dominated by the accumulation of
                        // kNodes single-precision adds, not by any difference of
                        // model.
                        EXPECT_LT(RelativeError(measured, reference[p][c]), 3.0e-5)
                            << testCase.Name << ", lobe " << ToString(static_cast<GroomFibreLobe>(p)) << ", channel "
                            << c << ", sinThetaO = " << sinThetaO << ": production " << measured << " vs reference "
                            << reference[p][c];
                    }
                }
            }
        }
    }

    TEST(FibreScatteringReference, TheFoldedTailIsTheRestOfTheSeries)
    {
        // Production folds every path from TRRT onwards into one lobe with the
        // closed form A2 * T * f / (1 - T * f). The reference sums the series
        // term by term instead, so this is the closed form checked against what
        // it is closed over. A truncation-by-mistake — say a model that kept
        // only TRRT — would pass every energy test at low absorption and lose a
        // visible fraction of a pale fibre's brightness.
        constexpr f64 kEta = 1.55;
        const glm::dvec3 sigmaA(0.06, 0.10, 0.20);

        for (i32 i = 0; i <= 9; ++i)
        {
            const f64 h = 0.1 * static_cast<f64>(i);
            const Ref::FibrePathWalk walk = Ref::FibreCylinderWalk(0.0, h, kEta, sigmaA, 48);

            const glm::dvec3 tail = walk.TailEnergy(3);
            const glm::dvec3 firstTailTerm = walk.Terms[3].Energy;
            // The tail is strictly more than its leading term wherever anything
            // gets past the third bounce, so a model that kept only TRRT is
            // distinguishable here rather than being within tolerance.
            EXPECT_GT(tail.x, firstTailTerm.x * 1.0000001) << "h = " << h;

            // And the whole series is geometric: the ratio of consecutive terms
            // past the first is exactly T * f, which is what makes the closed
            // form exact rather than fitted.
            const f64 ratio = walk.Terms[4].Energy.x / std::max(walk.Terms[3].Energy.x, 1.0e-300);
            const f64 expected = std::exp(-sigmaA.x * walk.ChordLength) * walk.FresnelInternal;
            EXPECT_LT(RelativeError(ratio, expected), 1.0e-9) << "h = " << h;
        }
    }

    TEST(FibreScatteringReference, AnUnabsorbingFibreReturnsExactlyAllOfItsLight)
    {
        // The energy statement, on both sides. The reference's path sum must
        // come to 1 because nothing removes energy; the production model's four
        // attenuations must come to 1 because its recurrence was built to.
        // Neither implies the other, and a model can satisfy its own identity
        // while placing the energy on the wrong paths — which is what the
        // per-lobe test above rules out and this one does not.
        constexpr f64 kEta = 1.55;
        for (i32 t = 0; t <= 8; ++t)
        {
            const f64 sinThetaO = 0.1 * static_cast<f64>(t);
            for (i32 i = 0; i <= 9; ++i)
            {
                const Ref::FibrePathWalk walk =
                    Ref::FibreCylinderWalk(sinThetaO, 0.1 * static_cast<f64>(i), kEta, glm::dvec3(0.0), 64);
                EXPECT_NEAR(walk.TotalEnergy().x, 1.0, 1.0e-12) << "sinThetaO = " << sinThetaO << ", i = " << i;
            }
        }

        const GroomFibreParams params = MakeGroomFibreParams(MakeAuthoring(glm::vec3(0.0f), 0.0f, 0.3f));
        for (i32 t = 0; t <= 8; ++t)
        {
            const f64 sinThetaO = 0.1 * static_cast<f64>(t);
            const f64 cosThetaO = std::sqrt(1.0 - (sinThetaO * sinThetaO));
            const GroomFibreLobeSet production = GroomFibreAmbientResponse(params, static_cast<f32>(sinThetaO));
            EXPECT_NEAR(static_cast<f64>(production.Sum().x) / cosThetaO, 1.0, 1.0e-5) << "sinThetaO = " << sinThetaO;
        }
    }

    TEST(FibreScatteringReference, AStochasticWalkReachesTheSameSplit)
    {
        // The deterministic reference and Chiang's recurrence are both
        // ENERGY-SPLITTING schemes. Two splitting schemes can share a mistake in
        // how the split is written — both could carry (1-f) once where physics
        // wants it twice, and both would then agree and both be wrong. A
        // roulette walk cannot make that mistake: it reflects or refracts, and
        // never forms the product at all.
        //
        // 200k samples per configuration. The estimator is a mean of values in
        // [0, 1], so its standard error is at most 0.5/sqrt(N) = 1.1e-3; the
        // tolerance is 4e-3, comfortably under four standard errors.
        constexpr u32 kSamples = 200000;
        constexpr f64 kEta = 1.55;
        const glm::dvec3 sigmaA(0.3, 0.5, 1.1);

        for (const f64 sinThetaO : { 0.0, 0.35, 0.7 })
        {
            for (const f64 h : { 0.0, 0.4, 0.85 })
            {
                const Ref::FibrePathWalk deterministic = Ref::FibreCylinderWalk(sinThetaO, h, kEta, sigmaA, 6);
                const std::vector<glm::dvec3> stochastic =
                    Ref::FibreCylinderStochasticEnergies(sinThetaO, h, kEta, sigmaA, 6, kSamples, 0x1255u);

                for (sizet p = 0; p + 1 < stochastic.size(); ++p)
                {
                    EXPECT_NEAR(stochastic[p].x, deterministic.Terms[p].Energy.x, 4.0e-3)
                        << "sinThetaO = " << sinThetaO << ", h = " << h << ", p = " << p;
                }
            }
        }
    }

    TEST(FibreScatteringReference, AbsorptionRemovesEnergyOnlyFromThePathsThatEnterTheFibre)
    {
        // A fibre cannot be darker than its own surface reflection: R never
        // enters, so no pigment removes it. That is the physical fact behind
        // the production header's note that a BaseColor target below about 0.04
        // saturates, and it is worth an assertion because the obvious
        // implementation of "make it darker" — scaling the whole response —
        // would break it while still producing a plausibly dark coat.
        constexpr f64 kEta = 1.55;
        for (i32 i = 0; i <= 9; ++i)
        {
            const f64 h = 0.1 * static_cast<f64>(i);
            const Ref::FibrePathWalk clear = Ref::FibreCylinderWalk(0.0, h, kEta, glm::dvec3(0.0), 48);
            const Ref::FibrePathWalk opaque = Ref::FibreCylinderWalk(0.0, h, kEta, glm::dvec3(30.0), 48);

            EXPECT_NEAR(opaque.Terms[0].Energy.x, clear.Terms[0].Energy.x, 1.0e-15) << "h = " << h;
            EXPECT_LT(opaque.TotalEnergy().x, clear.TotalEnergy().x);
            // Everything that is left at that absorption IS the surface term.
            EXPECT_NEAR(opaque.TotalEnergy().x, opaque.Terms[0].Energy.x, 1.0e-12) << "h = " << h;
        }

        // Monotone in absorption, per channel, which is what makes the pigment
        // controls behave.
        f64 previous = 2.0;
        for (const f64 sigma : { 0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 })
        {
            const Ref::FibrePathWalk walk = Ref::FibreCylinderWalk(0.0, 0.3, kEta, glm::dvec3(sigma), 48);
            EXPECT_LT(walk.TotalEnergy().x, previous) << "sigma = " << sigma;
            previous = walk.TotalEnergy().x;
        }
    }

    // =========================================================================
    // C. Reciprocity — asked because this model is local, answered honestly.
    // =========================================================================

    TEST(FibreScatteringReference, TheFarFieldIsReciprocalOnTheSpecularConeAndTheTiltBreaksIt)
    {
        // RECIPROCITY IS A MEANINGFUL QUESTION FOR THIS MODEL and the answer is
        // "on the cone, yes; off it and under a cuticle tilt, no". Both halves
        // are asserted, because only the pair is evidence:
        //
        //   * ON THE SPECULAR CONE with zero tilt (theta_i mirrors theta_o) the
        //     attenuations see the same incidence cosine from both ends and the
        //     longitudinal lobes are symmetric, so swapping the two directions
        //     must leave the value alone. A model with a handedness error in its
        //     azimuth would fail here while looking perfectly reasonable.
        //
        //   * THE CUTICLE TILT BREAKS IT, and not by accident: the tilt rotates
        //     the OUTGOING longitudinal angle only, which is what separates the
        //     R and TRT highlights on a real head and is the entire reason the
        //     parameter exists. A far-field model with a tilt is therefore not a
        //     reciprocal BCSDF, and #1255's criterion 4 wants that said out loud
        //     rather than papered over with a loose tolerance.
        const auto residual = [](f32 tiltDegrees, f64 sinThetaO, f64 sinThetaI, f64 phi) -> f64
        {
            const GroomFibreParams params =
                MakeGroomFibreParams(MakeAuthoring(glm::vec3(0.3f, 0.5f, 1.1f), tiltDegrees, 0.3f));
            const GroomFibreLobeSet forward =
                GroomFibreEvaluateFar(params, static_cast<f32>(sinThetaO), static_cast<f32>(sinThetaI),
                                      static_cast<f32>(phi), kNodes);
            const GroomFibreLobeSet reverse =
                GroomFibreEvaluateFar(params, static_cast<f32>(sinThetaI), static_cast<f32>(sinThetaO),
                                      static_cast<f32>(-phi), kNodes);
            const f64 a = static_cast<f64>(forward.Sum().x);
            const f64 b = static_cast<f64>(reverse.Sum().x);
            return std::abs(a - b) / std::max({ a, b, 1.0e-12 });
        };

        // On the cone, zero tilt. 1e-4 relative — f32 evaluation of two
        // different expression orders, not a model difference.
        for (const f64 sinThetaO : { 0.0, 0.2, 0.45, 0.7 })
        {
            for (const f64 phi : { 0.3, 1.2, 2.5 })
            {
                EXPECT_LT(residual(0.0f, sinThetaO, -sinThetaO, phi), 1.0e-4)
                    << "sinThetaO = " << sinThetaO << ", phi = " << phi;
            }
        }

        // The tilt breaks it, and worse as the tilt grows. Asserted as an
        // ORDERING over three tilts rather than as a value: a threshold on the
        // residual would pin one machine's float noise, while the ordering is
        // the statement the model actually makes.
        const f64 flat = residual(0.0f, 0.45, -0.45, 1.2);
        const f64 small = residual(2.0f, 0.45, -0.45, 1.2);
        const f64 large = residual(8.0f, 0.45, -0.45, 1.2);
        EXPECT_LT(flat, small) << "flat = " << flat << ", 2 deg = " << small;
        EXPECT_LT(small, large) << "2 deg = " << small << ", 8 deg = " << large;
        // And the break is real rather than float noise: an order of magnitude
        // above the on-cone residual at the shipped default tilt.
        EXPECT_GT(small, 10.0 * flat) << "2 deg = " << small << ", flat = " << flat;
    }

    // =========================================================================
    // D. Sampling consistency — the third question a local model owes.
    // =========================================================================

    TEST(FibreScatteringReference, TheSamplingDensityIntegratesToOneOverTheSphere)
    {
        // A pdf that does not integrate to 1 makes every estimator that divides
        // by it biased by exactly that factor — and the bias is invisible in a
        // white-furnace test that compares two estimators SHARING the pdf. So
        // this integrates the density directly, by a quadrature that has
        // nothing to do with the sampling routine.
        //
        // Parametrised by (u, phi) about the fibre tangent, where u is the
        // longitudinal sine: dw = du dphi, so the integral is a plain sum times
        // the cell area. 256 x 256 cells; the density is smooth in u away from
        // the lobe centres and the midpoint rule converges quickly.
        constexpr u32 kU = 256;
        constexpr u32 kPhi = 256;
        const GroomFibreParams params = MakeGroomFibreParams(MakeAuthoring(glm::vec3(0.3f, 0.5f, 1.1f), 0.0f, 0.3f));

        for (const f64 sinThetaO : { 0.0, 0.35, 0.65 })
        {
            for (const f64 h : { 0.0, 0.5 })
            {
                const glm::vec3 wo = LocalDirection(sinThetaO, 0.0);

                f64 integral = 0.0;
                for (u32 iu = 0; iu < kU; ++iu)
                {
                    const f64 u = -1.0 + ((2.0 * (static_cast<f64>(iu) + 0.5)) / static_cast<f64>(kU));
                    for (u32 ip = 0; ip < kPhi; ++ip)
                    {
                        const f64 phi =
                            (Ref::kTwoPi * (static_cast<f64>(ip) + 0.5)) / static_cast<f64>(kPhi);
                        integral += static_cast<f64>(
                            GroomFibrePdf(params, wo, LocalDirection(u, phi), static_cast<f32>(h)));
                    }
                }
                integral *= (2.0 / static_cast<f64>(kU)) * (Ref::kTwoPi / static_cast<f64>(kPhi));

                // 1.5% — the midpoint rule's error on a lobed density at this
                // resolution, measured by halving the grid. A normalisation
                // mistake is a factor, not a percent, so the tolerance has room
                // to spare without losing the failure it is for.
                EXPECT_NEAR(integral, 1.0, 0.015)
                    << "sinThetaO = " << sinThetaO << ", h = " << h << ", integral = " << integral;
            }
        }
    }

    TEST(FibreScatteringReference, TheSamplingTripleReproducesTheCylinderWalkAlbedo)
    {
        // The strongest statement in the file, and the one that needs all three
        // parts of the triple to be right at once. It draws directions from
        // GroomFibreSample, averages value/pdf — an estimate of the integral of
        // the BCSDF over the sphere, which is the fibre's albedo — and compares
        // it against the INDEPENDENT cylinder walk's total energy at the same h.
        //
        // What each arm can be wrong about is different: the sampling estimate
        // fails if evaluate, sample or pdf disagree with each other; the
        // reference fails if the physics is wrong. Agreement rules out both, and
        // neither GroomFibrePropertyTests' furnace nor the GPU parity test
        // covers the pair — the furnace compares the model's two estimators
        // against each other, which is silent about whether the number they
        // agree on is right.
        //
        // 60k samples. The integrand is bounded by the lobe peak, and the
        // measured standard error at this count is under 3e-3; the tolerance is
        // 1.5e-2, five standard errors, which leaves the assertion sensitive to
        // a wrong attenuation (those are tens of percent) and immune to the
        // seed.
        constexpr u32 kSamples = 60000;
        const glm::vec3 sigmaA(0.3f, 0.5f, 1.1f);
        const GroomFibreParams params = MakeGroomFibreParams(MakeAuthoring(sigmaA, 0.0f, 0.3f));

        for (const f64 sinThetaO : { 0.0, 0.3 })
        {
            for (const f64 h : { 0.0, 0.5 })
            {
                const glm::vec3 wo = LocalDirection(sinThetaO, 0.0);

                Ref::Pcg32 rng(0x1255u + static_cast<u64>(h * 100.0));
                f64 sum = 0.0;
                u32 valid = 0;
                for (u32 s = 0; s < kSamples; ++s)
                {
                    const glm::vec4 u(static_cast<f32>(rng.NextDouble()), static_cast<f32>(rng.NextDouble()),
                                      static_cast<f32>(rng.NextDouble()), static_cast<f32>(rng.NextDouble()));
                    const GroomFibreSampleResult sample = GroomFibreSample(params, wo, static_cast<f32>(h), u);
                    if (!sample.Valid || !(sample.Pdf > 0.0f))
                        continue;
                    sum += static_cast<f64>(sample.Value.Sum().x) / static_cast<f64>(sample.Pdf);
                    ++valid;
                }
                ASSERT_GT(valid, kSamples / 2u) << "the sampler rejected more than half its draws";

                const f64 sampled = sum / static_cast<f64>(valid);
                const Ref::FibrePathWalk reference =
                    Ref::FibreCylinderWalk(sinThetaO, h, static_cast<f64>(params.Eta), glm::dvec3(sigmaA), 48);

                EXPECT_NEAR(sampled, reference.TotalEnergy().x, 1.5e-2)
                    << "sinThetaO = " << sinThetaO << ", h = " << h << ": sampled albedo " << sampled
                    << " vs cylinder walk " << reference.TotalEnergy().x;
            }
        }
    }
} // namespace OloEngine::Tests
