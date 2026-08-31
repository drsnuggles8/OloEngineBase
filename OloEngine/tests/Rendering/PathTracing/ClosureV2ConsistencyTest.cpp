// OLO_TEST_LAYER: L1
// =============================================================================
// ClosureV2ConsistencyTest.cpp — Monte Carlo consistency of the v2 closure's
// Evaluate / Sample / Pdf triple (issue #975, acceptance criterion: "The v2
// sampler and PDF pass Monte Carlo consistency tests over representative
// materials").
//
// PBRClosureBSDF.h promises one thing above all: the three views of the lobe
// mixture agree on ONE density. A superficially plausible BSDF whose sampler
// and PDF describe *different* distributions converges beautifully to the
// wrong image — the reference-path-tracer postmortem's 2.18-vs-0.96 furnace
// (docs/agent-rules/reference-path-tracer.md §3, "A PDF describes the
// SAMPLER") is exactly that failure, and nothing image-shaped ever notices.
// So this file pins the agreement directly, four ways:
//
//   1. Sample()'s reported density is BIT-EQUAL to Pdf()'s — they must be the
//      same function, not two functions within tolerance of each other.
//   2. The importance-sampled reflectance estimate equals a plain uniform
//      quadrature of Evaluate * cos — two estimators of the same integral that
//      share NO code path for the density.
//   3. The near-mirror regime — where quadrature is blind — is checked with
//      the importance estimator alone against the Fresnel bound.
//   4. The mixture density integrates to 1 over the set of directions the
//      sampler can produce (below-horizon rejection mass accounted for).
//   5. The same agreement holds end-to-end through the real integrator via
//      the white-furnace plane.
//
// Everything is deterministic (PathSampler / cell-centered quadrature, no
// std::mt19937), headless and CPU-only.
//
// Budget note (reference-path-tracer.md §7: MSVC Debug runs this ~40x slower
// than Release): sample counts are deliberately small and fixed — 256 for the
// bit-equality sweep, 8192 for each MC estimate, 128x64 for each quadrature,
// 2^15 for the furnace pair. Measured against the existing furnace test's
// budget this whole file is a few seconds of Debug work.
//
// Classification: L1 (pure CPU math, no GL).
// =============================================================================

#include "OloEnginePCH.h"

#include "PathTracing/ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    namespace Fixtures = OloEngine::Tests::PathTracingFixtures;

    namespace
    {
        // ---------------------------------------------------------------------
        // The representative material grid (issue #975). Spans the axes the
        // mixture density actually branches on: specular probability (0.1 for
        // the dielectrics, ~0.5 half metal, 0.9 full metal), chromatic vs
        // achromatic F0, and diffuse-vs-specular energy split.
        // ---------------------------------------------------------------------
        struct GridMaterial
        {
            const char* Name;
            glm::vec3 Albedo;
            f32 Metallic;
        };

        [[nodiscard]] std::array<GridMaterial, 4> MaterialGrid()
        {
            return { GridMaterial{ "white dielectric", glm::vec3(0.9f), 0.0f },
                     GridMaterial{ "colored dielectric", glm::vec3(0.9f, 0.4f, 0.2f), 0.0f },
                     GridMaterial{ "full metal", glm::vec3(0.95f, 0.8f, 0.5f), 1.0f },
                     GridMaterial{ "half metal", glm::vec3(0.7f), 0.5f } };
        }

        constexpr std::array<f32, 4> kRoughnessGrid{ 0.05f, 0.3f, 0.7f, 1.0f };

        [[nodiscard]] ReferenceMaterial MakeClosureV2Material(const glm::vec3& albedo, f32 metallic, f32 roughness)
        {
            ReferenceMaterial material;
            material.BaseColor = albedo;
            material.Metallic = metallic;
            material.Roughness = roughness;
            material.Model = PBRModel::ClosureV2;
            return material;
        }

        // View direction at cos(theta_o) = mu, in the x-z plane about n = +Z.
        // Putting v in the x-z plane places the mirror direction at azimuth
        // exactly pi — which the cell-CENTERED quadrature below deliberately
        // straddles (phi nodes sit at 2pi(k+0.5)/128, so none lands on pi).
        // A node landing inside a near-delta specular core would turn the
        // deterministic quadrature into a wild overestimate; straddling makes
        // the unresolved-spike error one-sided and boundable.
        [[nodiscard]] glm::vec3 ViewFromMu(f32 mu)
        {
            return glm::normalize(glm::vec3(std::sqrt(std::max(0.0f, 1.0f - mu * mu)), 0.0f, mu));
        }

        constexpr glm::vec3 kNormal{ 0.0f, 0.0f, 1.0f };

        // ---------------------------------------------------------------------
        // Importance-sampled hemispherical reflectance:
        //     R = E[ Value * cos(theta_l) / Pdf ]
        // over the sampler's own draws. Uses the PathSampler with the exact
        // dimension order the integrator uses (Get1D for the lobe select, then
        // Get2D for the lobe shape — that order is part of the determinism
        // contract in PBRClosureBSDF.h).
        //
        // A failed Sample() (below-horizon or zero-density draw) counts as a
        // ZERO contribution while still counting in the denominator. Skipping
        // it and renormalizing over accepted draws would bias the estimator
        // UPWARD: the rejected directions are real probability mass of the
        // mixture, and the integrator itself terminates those paths with zero
        // energy — the estimator here must be the integrator's.
        // ---------------------------------------------------------------------
        [[nodiscard]] glm::dvec3 SamplerReflectanceEstimate(const ReferenceMaterial& material, const glm::vec3& n,
                                                            const glm::vec3& v, u32 sampleCount, u32 seedSalt)
        {
            glm::dvec3 sum(0.0);
            for (u32 i = 0; i < sampleCount; ++i)
            {
                PathSampler sampler(MakePixelSeed(seedSalt, 0u, 0x9e3779b9u), i);
                const f32 lobeXi = sampler.Get1D();
                const glm::vec2 xi = sampler.Get2D();

                BSDF::BSDFSample bsdfSample;
                if (!BSDF::Sample(material, n, v, lobeXi, xi, bsdfSample))
                    continue; // zero contribution — see the comment above

                const f32 cosL = glm::dot(n, bsdfSample.Direction);
                sum += glm::dvec3(bsdfSample.Value) *
                       (static_cast<f64>(cosL) / static_cast<f64>(bsdfSample.Pdf));
            }
            return sum / static_cast<f64>(sampleCount);
        }

        // ---------------------------------------------------------------------
        // Uniform quadrature of Evaluate * cos over the upper hemisphere.
        //
        // Grid choice: cell-centered, uniform in (cos(theta), phi) — i.e.
        // equal-SOLID-ANGLE cells, dOmega = 2pi / (kThetaCells * kPhiCells)
        // constant. Chosen over uniform-in-theta because the cos-weighted
        // integrand is smoothest in the cos(theta) variable (the cosine lobe
        // becomes exactly linear, so the midpoint rule integrates it almost
        // exactly), and cell centers keep every node strictly off both the
        // pole and the horizon.
        //
        // This estimator shares NOTHING with the sampler path — no Sample, no
        // Pdf — which is what makes agreement with SamplerReflectanceEstimate
        // a genuine cross-check of the density rather than a tautology.
        // ---------------------------------------------------------------------
        constexpr u32 kQuadraturePhiCells = 128;
        constexpr u32 kQuadratureThetaCells = 64;

        [[nodiscard]] glm::dvec3 QuadratureReflectance(const ReferenceMaterial& material, const glm::vec3& n,
                                                       const glm::vec3& v)
        {
            const f64 dOmega =
                static_cast<f64>(kTwoPi) / static_cast<f64>(kQuadraturePhiCells * kQuadratureThetaCells);

            glm::dvec3 sum(0.0);
            for (u32 j = 0; j < kQuadratureThetaCells; ++j)
            {
                const f32 cosTheta = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(kQuadratureThetaCells);
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                for (u32 k = 0; k < kQuadraturePhiCells; ++k)
                {
                    const f32 phi = kTwoPi * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(kQuadraturePhiCells);
                    const glm::vec3 l(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);

                    const glm::vec3 f = BSDF::Evaluate(material, n, v, l);
                    sum += glm::dvec3(f) * (static_cast<f64>(cosTheta) * dOmega);
                }
            }
            return sum;
        }
    } // namespace

    // =========================================================================
    // 1. Sample() reports the density Pdf() computes — BIT-equal.
    //
    // Exact float equality is normally a coding-rules smell (never == on
    // floats), but here EXACTNESS IS THE CONTRACT under test, not a sloppy
    // tolerance: PBRClosureBSDF.h promises that Sample()'s reported Pdf and
    // Pdf() are the SAME FUNCTION evaluated on the same arguments, because the
    // MIS weight built from an independently-written density is the classic
    // silently-biased integrator. Any tolerance here would bless exactly that
    // regression — a near-copy of the density that drifts by an ulp today and
    // a formula tomorrow. Compared through bit_cast so no float comparison
    // happens at all.
    // =========================================================================
    TEST(ClosureV2Consistency, SampleReportsTheSameDensityPdfReturns)
    {
        constexpr u32 kSamplesPerCase = 256;
        constexpr f32 kMuO = 0.6f;
        const glm::vec3 v = ViewFromMu(kMuO);

        const auto materials = MaterialGrid();
        for (u32 mi = 0; mi < materials.size(); ++mi)
        {
            for (u32 ri = 0; ri < kRoughnessGrid.size(); ++ri)
            {
                const ReferenceMaterial material =
                    MakeClosureV2Material(materials[mi].Albedo, materials[mi].Metallic, kRoughnessGrid[ri]);

                u32 accepted = 0;
                for (u32 i = 0; i < kSamplesPerCase; ++i)
                {
                    PathSampler sampler(MakePixelSeed(mi, ri, 0x9e3779b9u), i);
                    const f32 lobeXi = sampler.Get1D();
                    const glm::vec2 xi = sampler.Get2D();

                    BSDF::BSDFSample bsdfSample;
                    if (!BSDF::Sample(material, kNormal, v, lobeXi, xi, bsdfSample))
                        continue;
                    ++accepted;

                    const f32 recomputed = BSDF::Pdf(material, kNormal, v, bsdfSample.Direction);
                    EXPECT_EQ(std::bit_cast<u32>(bsdfSample.Pdf), std::bit_cast<u32>(recomputed))
                        << materials[mi].Name << ", roughness = " << kRoughnessGrid[ri] << ", sample " << i
                        << ": Sample() reported pdf " << bsdfSample.Pdf << " but Pdf() returns " << recomputed
                        << " for the same direction — the two are no longer the same function, and every "
                           "MIS weight the integrator builds from Pdf() is now biased against the sampler.";
                }

                // Non-vacuity floor: even the worst case here (full metal at
                // roughness 1.0, where ~a third of VNDF draws reflect below
                // the horizon) accepts well over half the draws. Zero accepted
                // would make the loop above pass while asserting nothing.
                EXPECT_GT(accepted, kSamplesPerCase / 2)
                    << materials[mi].Name << ", roughness = " << kRoughnessGrid[ri];
            }
        }
    }

    // =========================================================================
    // 2. The core MC consistency check: the importance-sampled reflectance
    //    equals the uniform quadrature of Evaluate * cos.
    //
    // The two estimators integrate the same quantity through disjoint code:
    // the sampler side exercises Sample + Pdf (Value / Pdf), the quadrature
    // side only Evaluate. If Pdf described a different distribution than
    // Sample draws from, the left side scales by the mismatch — the furnace
    // postmortem's failure — while the right side stays put.
    //
    // EXCLUSION at roughness 0.05: the specular lobe is near-delta (alpha =
    // 0.0025, core ~7e-5 sr against ~7.7e-4 sr quadrature cells), so uniform
    // quadrature CANNOT resolve it — depending on where the nodes fall it
    // sees anywhere from almost none of the spike to a gross overestimate.
    // The metallic materials are all spike there, so they are skipped
    // entirely; the near-mirror specular case is covered by test 3, whose
    // importance estimator CAN see a narrow lobe. The metallic-0 materials
    // are kept (they are diffuse-dominant, F0 = 0.04) with the unresolvable
    // spike bounded explicitly — see kSpikeAllowance below.
    // =========================================================================
    TEST(ClosureV2Consistency, SamplerEstimateMatchesUniformQuadrature)
    {
        constexpr u32 kSamples = 8192;
        constexpr std::array<f32, 2> kMuValues{ 0.35f, 0.75f };

        const auto materials = MaterialGrid();
        for (u32 mi = 0; mi < materials.size(); ++mi)
        {
            for (u32 ri = 0; ri < kRoughnessGrid.size(); ++ri)
            {
                const f32 roughness = kRoughnessGrid[ri];
                const bool nearMirror = roughness < 0.1f;

                // See the header comment: at 0.05 only the diffuse-dominant
                // (metallic 0) materials are quadrature-comparable at all.
                if (nearMirror && materials[mi].Metallic > 0.0f)
                    continue;

                const ReferenceMaterial material =
                    MakeClosureV2Material(materials[mi].Albedo, materials[mi].Metallic, roughness);

                for (u32 vi = 0; vi < kMuValues.size(); ++vi)
                {
                    const f32 mu = kMuValues[vi];
                    const glm::vec3 v = ViewFromMu(mu);

                    const glm::dvec3 sampled =
                        SamplerReflectanceEstimate(material, kNormal, v, kSamples, 100u + mi * 16u + ri * 4u + vi);
                    const glm::dvec3 quadrature = QuadratureReflectance(material, kNormal, v);

                    // At roughness 0.05 the dielectric spike the quadrature
                    // cannot resolve still carries real energy the sampler DOES
                    // see, bounded by the Fresnel at the incidence angle
                    // (F(mu) ~ 0.15 at mu 0.35, F0 = 0.04): the VNDF estimator
                    // weight G2/G1 <= 1 and the multi-scatter lobe is below the
                    // energy table's floor at alpha = 0.0025. Adding that bound
                    // (rather than dropping the case) keeps the diffuse lobe
                    // under a real assertion, and the slack still catches the
                    // factor-2-and-up density bugs this test exists for. The
                    // quadrature cannot OVERSHOOT by the spike because the grid
                    // straddles the mirror azimuth (see ViewFromMu).
                    const f64 spikeAllowance =
                        nearMirror ? static_cast<f64>(FresnelSchlick(mu, glm::vec3(kDefaultDielectricF0)).x) : 0.0;

                    for (glm::length_t channel = 0; channel < 3; ++channel)
                    {
                        const f64 rq = quadrature[channel];
                        const f64 rs = sampled[channel];
                        // max(0.02 abs, 4% rel): separates a density mismatch
                        // (the specular half scales by the whole mismatch —
                        // the postmortem's case was +127%) from the benign
                        // residuals of two honest estimators (LDS sampling
                        // noise ~0.5%, midpoint-quadrature resolution ~1-2% at
                        // roughness 0.3). Absolute floor for the dark channels
                        // where 4% relative would be tighter than the noise.
                        const f64 tolerance = std::max(0.02, 0.04 * rq) + spikeAllowance;
                        EXPECT_NEAR(rs, rq, tolerance)
                            << materials[mi].Name << ", roughness = " << roughness << ", mu_o = " << mu
                            << ", channel " << channel
                            << " — the importance-sampled estimate (Value*cos/Pdf) and the uniform quadrature "
                               "of Evaluate*cos disagree: Sample/Pdf describe a different distribution than "
                               "the one Evaluate is being divided by.";
                    }
                }
            }
        }
    }

    // =========================================================================
    // 3. Near-mirror consistency, by the estimator that can see it.
    //
    // At roughness 0.05 the lobe is ~delta and test 2's quadrature is blind,
    // so the check inverts: the importance-sampled reflectance of the full
    // metal must land on the Fresnel prediction per channel. For a near-mirror
    // metal the estimator contribution of every specular draw is
    // F(h.v) * G2/G1 (~ F0 with a percent-level Fresnel/masking correction at
    // mu_o 0.6), so R_s / F0 must sit just above 1 — IF the density matches
    // the sampler. A broken VNDF pdf explodes this ratio by orders of
    // magnitude exactly where nothing else looks: the reference-path-tracer
    // postmortem's clamped-density bug reported a directional albedo of 2.18
    // against a true 0.96 at low roughness, invisible above roughness 0.5.
    // =========================================================================
    TEST(ClosureV2Consistency, NearMirrorConsistencyViaImportanceOnly)
    {
        constexpr u32 kSamples = 8192;
        constexpr f32 kMuO = 0.6f;
        const glm::vec3 kF0(0.95f, 0.8f, 0.5f);

        const ReferenceMaterial material = MakeClosureV2Material(kF0, 1.0f, 0.05f);
        const glm::vec3 v = ViewFromMu(kMuO);

        const glm::dvec3 sampled = SamplerReflectanceEstimate(material, kNormal, v, kSamples, 3000u);

        for (glm::length_t channel = 0; channel < 3; ++channel)
        {
            const f64 ratio = sampled[channel] / static_cast<f64>(kF0[channel]);
            // [0.85, 1.08]: wide enough for the physics inside the band —
            // Schlick lifts F above F0 at mu_o 0.6 (~+1% on the red channel,
            // ~+3% on the blue) and near-mirror masking sheds a few percent —
            // and narrow enough that a density/sampler mismatch (a factor,
            // not a percent: 4x from a dropped reflection Jacobian, 100x+
            // from an evaluation-side clamp reused in the density) cannot fit.
            EXPECT_GE(ratio, 0.85) << "channel " << channel << ": R_s = " << sampled[channel]
                                   << " against F0 = " << kF0[channel]
                                   << " — the near-mirror lobe lost energy; the density over-reports "
                                      "where the sampler concentrates.";
            EXPECT_LE(ratio, 1.08) << "channel " << channel << ": R_s = " << sampled[channel]
                                   << " against F0 = " << kF0[channel]
                                   << " — energy created at near-mirror roughness: the classic signature of "
                                      "a pdf smaller than the density actually sampled from "
                                      "(reference-path-tracer.md's 2.18-vs-0.96 furnace).";
        }
    }

    // =========================================================================
    // 4. The mixture density is a probability density.
    //
    // Pdf() reports 0 below the horizon, but the raw VNDF draw has real mass
    // there — Sample() REJECTS those draws rather than redistributing them,
    // and at mu_o 0.6 that mass is anything but negligible at high roughness
    // (at roughness 1.0 the visible-normal lobe is so wide that ~37% of
    // reflected metal draws land below the horizon; the upper-hemisphere
    // integral alone would come out near 0.66 for the full metal). So the
    // normalization is asserted over the full set of directions the sampler
    // can produce: the upper-hemisphere integral of Pdf() PLUS the
    // specular-probability-weighted below-horizon VNDF mass, the latter
    // computed from the same reference density Pdf() uses (zeroed where the
    // half-vector drops below the horizon — directions no upper-hemisphere
    // half-vector can reflect into, hence outside the sampler's image).
    //
    // NOTE this deliberately deviates from the issue sketch (upper hemisphere
    // only, lower bound 0.93 justified by "small below-horizon mass at
    // mu_o 0.6"): that premise holds at roughness 0.3 but fails badly at 0.7
    // and 1.0 for the high-specular-probability materials, where the honest
    // upper-only integral sits far below any meaningful bound. Accounting for
    // the rejected mass keeps the test exact for every material instead of
    // vacuously wide.
    // =========================================================================
    TEST(ClosureV2Consistency, PdfIntegratesToOne)
    {
        constexpr f32 kMuO = 0.6f;
        const glm::vec3 v = ViewFromMu(kMuO);
        const f64 dOmega =
            static_cast<f64>(kTwoPi) / static_cast<f64>(kQuadraturePhiCells * kQuadratureThetaCells);

        const auto materials = MaterialGrid();
        for (const GridMaterial& gridMaterial : materials)
        {
            // 0.05 is excluded here like in test 2: the near-delta specular
            // density is unresolvable by this grid (its normalization at low
            // roughness is instead implied by test 3's f/pdf agreement).
            for (const f32 roughness : { 0.3f, 0.7f, 1.0f })
            {
                const ReferenceMaterial material =
                    MakeClosureV2Material(gridMaterial.Albedo, gridMaterial.Metallic, roughness);
                const f32 pSpecular = BSDF::SpecularProbability(material);
                const f32 v2Roughness = ClosureV2Roughness(roughness);
                const f32 nDotV = glm::dot(kNormal, v);

                f64 upperIntegral = 0.0;
                f64 belowHorizonSpecular = 0.0;
                for (u32 j = 0; j < kQuadratureThetaCells; ++j)
                {
                    const f32 cosTheta = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(kQuadratureThetaCells);
                    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                    for (u32 k = 0; k < kQuadraturePhiCells; ++k)
                    {
                        const f32 phi =
                            kTwoPi * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(kQuadraturePhiCells);
                        const f32 x = sinTheta * std::cos(phi);
                        const f32 y = sinTheta * std::sin(phi);

                        // Upper-hemisphere cell: the density the integrator uses.
                        const glm::vec3 lUp(x, y, cosTheta);
                        upperIntegral += static_cast<f64>(BSDF::Pdf(material, kNormal, v, lUp)) * dOmega;

                        // Mirrored lower-hemisphere cell: the rejected specular
                        // mass, via the same VNDF density Pdf()'s specular term
                        // uses. A below-horizon HALF-VECTOR means the direction
                        // is unreachable from any sampled (upper-hemisphere)
                        // visible normal, so its density is genuinely zero.
                        const glm::vec3 lDown(x, y, -cosTheta);
                        const glm::vec3 sumVL = v + lDown;
                        if (glm::dot(sumVL, sumVL) < 1.0e-12f)
                            continue; // l == -v: degenerate half-vector, measure-zero
                        const glm::vec3 h = glm::normalize(sumVL);
                        const f32 nDotH = glm::dot(kNormal, h);
                        if (nDotH <= 0.0f)
                            continue;
                        belowHorizonSpecular +=
                            static_cast<f64>(PdfGGXVNDF(nDotV, nDotH, v2Roughness)) * dOmega;
                    }
                }

                const f64 total = upperIntegral + static_cast<f64>(pSpecular) * belowHorizonSpecular;

                // [0.93, 1.03]: separates a mis-normalized density — a missing
                // or inverted 1/(4 v.h) reflection Jacobian moves the specular
                // share by ~4x, a wrong G1 by tens of percent — from the
                // midpoint-quadrature resolution error, which peaks at ~1-2%
                // on the roughness-0.3 lobe. The band is asymmetric because
                // quadrature under-resolves a peak more readily than it
                // over-resolves one on this straddling grid.
                EXPECT_GE(total, 0.93)
                    << gridMaterial.Name << ", roughness = " << roughness << " (upper = " << upperIntegral
                    << ", pSpec * belowHorizon = " << pSpecular * belowHorizonSpecular
                    << ") — the mixture density integrates below 1 over everything the sampler can draw: "
                       "Pdf() is over-reporting somewhere, so f/pdf under-estimates there.";
                EXPECT_LE(total, 1.03)
                    << gridMaterial.Name << ", roughness = " << roughness << " (upper = " << upperIntegral
                    << ", pSpec * belowHorizon = " << pSpecular * belowHorizonSpecular
                    << ") — the mixture density integrates above 1: Pdf() under-reports somewhere, and "
                       "f/pdf creates energy exactly where the sampler concentrates.";

                // The upper-hemisphere mass alone can never exceed the whole
                // density's normalization; 1.03 allows only quadrature error.
                EXPECT_LE(upperIntegral, 1.03) << gridMaterial.Name << ", roughness = " << roughness;
            }
        }
    }

    // =========================================================================
    // 5. End-to-end through the real integrator: the ClosureV2 white-furnace
    //    plane converges to the closure's own hemisphere integral.
    //
    // Same construction as PathTracerFurnace.WhiteFurnaceMatchesTheAnalyticIntegral,
    // but dispatched through Model = ClosureV2: a single convex plane under a
    // unit environment, viewed along its normal, converges to exactly the
    // directional albedo — so any disagreement is the transport machinery
    // (Sample / Pdf / throughput bookkeeping) and not the BRDF, which cancels
    // out of the comparison. The metallic case additionally pins the
    // Kulla-Conty energy compensation VISIBLE THROUGH THE FULL TRANSPORT
    // PATH: single-scatter GGX at roughness 0.8 sheds tens of percent, and
    // only the multi-scatter lobe brings a unit-F0 furnace back to ~1.
    // =========================================================================
    namespace
    {
        // The furnace plane with a ClosureV2 material. MakeFurnacePlaneScene
        // has no Model parameter and materials are immutable once added, so
        // this mirrors it via the same fixture struct + winding-verified quad
        // helper rather than open-coding a quad (reference-path-tracer.md §6:
        // the winding decides whether the scene renders black).
        [[nodiscard]] Fixtures::FurnacePlaneScene MakeClosureV2FurnacePlaneScene(f32 roughness, f32 metallic)
        {
            Fixtures::FurnacePlaneScene fixture;

            const ReferenceMaterial material = MakeClosureV2Material(glm::vec3(1.0f), metallic, roughness);
            fixture.MaterialIndex = fixture.Scene.AddMaterial(material);

            Fixtures::AddFloorQuad(fixture.Scene, 0.0f, -1000.0f, 1000.0f, -1000.0f, 1000.0f,
                                   fixture.MaterialIndex);

            ReferenceEnvironment environment;
            environment.Radiance = glm::vec3(1.0f);
            fixture.Scene.SetEnvironment(environment);

            fixture.Scene.Build();
            return fixture;
        }

        // Channel-mean of the tracer's own furnace estimate — the same shape
        // as PathTracerFurnaceTest's TracedFurnaceValue.
        [[nodiscard]] f64 TracedClosureV2FurnaceValue(const Fixtures::FurnacePlaneScene& fixture, u32 sampleCount)
        {
            PathTracerSettings settings;
            settings.SamplesPerPixel = sampleCount;
            // Two vertices: the plane, then the escape into the environment —
            // MaxBounces 1 renders a furnace black (the environment is only
            // collected when a ray escapes).
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

        // Uniform-hemisphere (equal-solid-angle stratified, radical-inverse
        // azimuth) integration of BSDF::Evaluate * cos with n = v = +Y — the
        // plane fixture's frame — through the same versioned dispatch the
        // integrator uses.
        [[nodiscard]] f64 AnalyticClosureV2Albedo(const ReferenceMaterial& material, u32 sampleCount)
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
                const glm::vec3 l(std::cos(phi) * sinTheta, cosTheta, std::sin(phi) * sinTheta);

                const glm::vec3 f = BSDF::Evaluate(material, n, v, l);
                sum += static_cast<f64>((f.x + f.y + f.z) / 3.0f) * static_cast<f64>(cosTheta) *
                       static_cast<f64>(kTwoPi);
            }
            return sum / static_cast<f64>(sampleCount);
        }
    } // namespace

    TEST(ClosureV2Consistency, PathTracedV2FurnaceMatchesTheAnalyticAlbedo)
    {
        constexpr u32 kSampleCount = 1u << 15;

        struct FurnaceCase
        {
            f32 Roughness;
            f32 Metallic;
        };
        constexpr std::array<FurnaceCase, 2> kCases{ FurnaceCase{ 0.5f, 0.0f }, FurnaceCase{ 0.8f, 1.0f } };

        for (const FurnaceCase& furnaceCase : kCases)
        {
            const Fixtures::FurnacePlaneScene fixture =
                MakeClosureV2FurnacePlaneScene(furnaceCase.Roughness, furnaceCase.Metallic);
            ASSERT_TRUE(fixture.Scene.IsBuilt());

            const ReferenceMaterial material =
                MakeClosureV2Material(glm::vec3(1.0f), furnaceCase.Metallic, furnaceCase.Roughness);
            const f64 analytic = AnalyticClosureV2Albedo(material, kSampleCount);
            const f64 traced = TracedClosureV2FurnaceValue(fixture, kSampleCount);

            // max(0.01 abs, 3% rel): both sides are stratified low-discrepancy
            // estimates whose residuals sit well under a percent, so this band
            // separates a transport/density bug (the smallest interesting one —
            // a wrong lobe-selection weight — shifts the value by several
            // percent; a density mismatch by a factor) from estimator noise.
            const f64 tolerance = std::max(0.01, 0.03 * analytic);
            EXPECT_NEAR(traced, analytic, tolerance)
                << "roughness = " << furnaceCase.Roughness << ", metallic = " << furnaceCase.Metallic
                << " — the integrator's ClosureV2 furnace disagrees with the closure's own hemisphere "
                   "integral: the transport bookkeeping (Sample / Pdf / throughput) is off, since the "
                   "BRDF itself cancels out of this comparison.";

            if (furnaceCase.Metallic > 0.5f)
            {
                // 0.9 floor: single-scatter GGX alone at roughness 0.8 loses
                // tens of percent (the legacy closure measures ~0.86 at 0.5
                // and drops from there), so only a live Kulla-Conty
                // multi-scatter lobe carried intact through the full transport
                // path keeps a unit-F0 metal furnace above this line.
                EXPECT_GE(traced, 0.9)
                    << "the metallic ClosureV2 furnace lost the multi-scatter compensation somewhere "
                       "between Evaluate and the traced image.";
            }
        }
    }
} // namespace OloEngine::Tests
