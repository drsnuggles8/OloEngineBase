// OLO_TEST_LAYER: L1
// =============================================================================
// BsdfSamplingDistributionTest.cpp — the engine's BSDF samplers against
// densities derived independently of them (issue #1347, acceptance criteria 1
// and 4).
//
// WHAT IS TESTED. Each engine sampler (SampleGGXVNDF, ImportanceSampleGGX,
// CosineSampleHemisphere and the one-sample lobe mixture BSDF::Sample, for
// ClosureV2 and Legacy) draws directions, and a goodness-of-fit test compares
// them with an OracleDensity (BsdfOracleChecks.h) built from the oracle's
// functions only (IndependentBsdfOracle.h: GgxD, GgxThetaCdf,
// GgxVisibleNormalDensity, ReflectionJacobian). The engine's own densities
// (PdfGGX, PdfGGXVNDF, BSDF::Pdf) are never the expectation:
// ReferenceBRDFTest's GgxSamplingMatchesItsDensity and ClosureV2ConsistencyTest
// integrate 1/pdf over Hammersley points, which shows a sampler and ITS OWN pdf
// agree — a shared wrong alpha passes that. Here the engine pdf is itself
// checked (EnginePdfEqualsTheOracleMixtureAtSampledDirections). The alpha
// convention (alpha = roughness^2: the GGX definition in ReferenceBRDF.h and
// ADR 0016) is decided by the oracle code below, not read back from the engine.
//
// STATISTICAL ASSUMPTIONS (OracleStatistics.h is the convention; this is how
// this file applies it):
//
//   * IID. Every draw's uniforms come from IidStream (std::mt19937_64), a
//     fresh generator per run, converted to f32 at the engine boundary as the
//     integrator hands them over. No Hammersley, Sobol or Owen point set is
//     ever fed to a goodness-of-fit statistic: those are stratified and fit
//     any smooth density "too well" (LowDiscrepancyPointsAreRefusedAsEvidence
//     pins that for this file's use). No reservoir M and no frame sequence is
//     used: M is a confidence weight over correlated, reused candidates, and
//     consecutive frames are correlated by temporal reuse, so neither counts
//     independent samples. Each draw here is one call of a stateless sampler.
//   * RUNS AND SEEDS. Every family is kSamplingPlan.Runs.Count = 5 runs with
//     seed_r = SplitMix64(BaseSeed ^ SplitMix64(r + 1)), BaseSeed
//     0x1347000000000001, each on a fresh generator. The seeds are fixed, so
//     the suite is deterministic; the significance level describes seeds
//     chosen before looking.
//   * SIGNIFICANCE. One check (one sampler at one grid cell, or one KS
//     marginal) is one family; it fails if any run has p < FamilyAlpha / 5
//     (Bonferroni over runs). The file runs kFamilyCount families, so it
//     applies Bonferroni again over them: FamilyAlpha = kFileAlpha /
//     kFamilyCount, kFileAlpha = 1e-2 — a correct engine trips some family in
//     this file with probability at most 1 %.
//   * BINNING AND POOLING. BsdfOracleChecks.h CheckSamplingDistribution: the
//     half-vector of (v, l) on a 24 x 24 (t, phi) grid, t = GgxThetaCdf(m.z,
//     binAlpha), plus one bin for rejected (nullopt / below-horizon) draws.
//     Adjacent bins, row-major, are pooled until each expected count is >= 5
//     (Cochran); dof = pooled bins - 1; nothing is estimated from the data.
//   * EXPECTED BIN MASSES. CheckSamplingDistribution's deposit
//     (DepositBinMasses): each lobe of the oracle density integrated in its
//     own inverse-CDF coordinates on a 240 x 240 grid (a multiple of the 24
//     bins), straddling cells split 2 x 2 down to 5 levels, the rejected bin
//     the complement of the mass above the horizon. The header says why it is
//     built that way (the per-bin midpoint rule it replaced failed correct
//     samplers at grazing views and on sharp mixtures). Its error is
//     measured, not assumed: ExpectedBinMassQuadratureErrorIsNegligible
//     doubles the resolution and bounds the resulting chi-square
//     non-centrality N sum e_i^2 / p_i by a tenth of the null standard
//     deviation sqrt(2 dof).
//   * f32 BOUNDARY. The engine samples in f32; directions are widened to f64
//     before binning. Polar cosines of f32 half-vectors are rebuilt from the
//     tangential components (tan^2 = (x^2 + y^2) / z^2), so the test adds no
//     cancellation the engine does not have.
//
// FINDING (found and fixed by this issue): ImportanceSampleGGX used to take
// sin theta = sqrt(1 - cos^2) in f32 after cos = sqrt((1 - xi) / (1 + (a^2 -
// 1) xi)). Near the pole cos^2 rounds to 1.0f, sin theta to exactly 0, and a
// fraction 2^-25 / (alpha^2 + 2^-25) of all draws landed ON the normal — 0.47 %
// at roughness 0.05 and 1.2 % at the Legacy sampling floor 0.04 — with the
// rest quantised to a few thousand distinct cosines. This file's roughness
// 0.05 families failed on it. The engine now forms sin^2 = a^2 xi / (1 + (a^2
// - 1) xi), which has no cancellation; those families pass with the rest of
// the grid, and ImportanceSampleGgxNoLongerCollapsesOntoTheNormalInF32 pins
// the fix beside the pre-#1347 form (BsdfMutationDetectionTest M10 is the
// same bug as a mutant of the chi-square check).
//
// Classification: L1 (pure CPU math; no GL, no scene).
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/Oracles/BsdfOracleChecks.h"
#include "Rendering/Oracles/EngineBsdfAdapters.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests::Oracle
{
    namespace
    {
        // ---- the grids and the plan ----------------------------------------

        constexpr std::array<f64, 4> kLobeRoughness{ 0.05, 0.3, 0.7, 1.0 };
        constexpr std::array<f64, 3> kLobeCosV{ 0.05, 0.5, 1.0 };
        constexpr std::array<f64, 3> kMixtureRoughness{ 0.1, 0.5, 1.0 };
        constexpr std::array<f64, 2> kMixtureCosV{ 0.1, 0.7 };
        constexpr std::array<f64, 2> kCosineCosV{ 1.0, 0.5 };

        struct NamedMaterial
        {
            const char* Name;
            glm::dvec3 Albedo;
            f64 Metallic;
        };
        const std::array<NamedMaterial, 4> kMixtureMaterials{ {
            { "white dielectric", glm::dvec3(0.9), 0.0 },
            { "coloured dielectric", glm::dvec3(0.8, 0.2, 0.1), 0.0 },
            { "metal", glm::dvec3(0.95, 0.64, 0.54), 1.0 },
            { "half metal", glm::dvec3(0.7), 0.5 },
        } };
        constexpr u32 kMixtureModels = 2; // ClosureV2, Legacy

        // Every goodness-of-fit family this file runs (the Bonferroni count):
        // 12 VNDF + 12 GGX + 2 cosine + 24 mixtures + 16 KS.
        constexpr u32 kFamilyCount =
            static_cast<u32>(kLobeRoughness.size() * kLobeCosV.size()) * 2u    // VNDF, GGX
            + static_cast<u32>(kCosineCosV.size())                             // cosine
            + kMixtureModels * 4u * static_cast<u32>(kMixtureRoughness.size()) // mixtures
            + static_cast<u32>(kLobeRoughness.size()) * 2u * 2u;               // KS: 2 samplers x (t, phi)
        static_assert(kFamilyCount == 66u);

        constexpr f64 kFileAlpha = 1.0e-2;

        // THE plan every sampling family in this file uses — and the plan a
        // mutation test must use for its power claim to transfer: 5 runs x
        // 200k draws, 24 x 24 half-vector bins + 1 rejected bin, family alpha
        // 1e-2 / kFamilyCount (per run a fifth of that), expected masses by a
        // 240 x 240 deposit refined 5 levels.
        constexpr SamplingPlan kSamplingPlan{
            IndependentRuns{ 5u, 0x1347'0000'0000'0001ull, kFileAlpha / static_cast<f64>(kFamilyCount) },
            200000u,
            24u,
            24u,
            240u,
            5u,
        };
        static_assert(kSamplingPlan.DepositBaseCells % kSamplingPlan.BinsT == 0u &&
                      kSamplingPlan.DepositBaseCells % kSamplingPlan.BinsPhi == 0u);

        // ---- oracle densities, from Oracle:: functions only ----------------

        // The Legacy specular SAMPLER's alpha: the sampling roughness is the
        // material roughness floored at MIN_ROUGHNESS 0.04 (the documented
        // Legacy sampling policy, PBRClosureBSDF.h SamplingRoughness), squared
        // by the GGX definition. The v2 alpha is Oracle::ClosureV2Alpha.
        [[nodiscard]] f64 LegacySamplingAlpha(f64 roughness)
        {
            const f64 r = std::clamp(roughness, 0.04, 1.0);
            return r * r;
        }

        [[nodiscard]] f64 SpecularAlpha(PBRModel model, f64 roughness)
        {
            return model == PBRModel::ClosureV2 ? ClosureV2Alpha(roughness) : LegacySamplingAlpha(roughness);
        }

        // The one-sample mixture: pS p_spec + (1 - pS) cos / pi. pS is the
        // engine's lobe-selection POLICY (Engine::SpecularProbability) — an
        // input, not a model property: the mixture is unbiased for any pS in
        // (0, 1), so there is nothing to derive. The SHAPES are the oracle's:
        // ClosureV2 samples the visible normals, Legacy the NDF.
        [[nodiscard]] OracleDensity MixtureDensity(PBRModel model, f64 pS, f64 roughness)
        {
            const Shape specular = model == PBRModel::ClosureV2 ? Shape::VisibleNormal : Shape::NdfReflection;
            return { Lobe{ specular, pS, SpecularAlpha(model, roughness) }, Lobe{ Shape::Cosine, 1.0 - pS, 1.0 } };
        }

        [[nodiscard]] const char* ModelName(PBRModel model)
        {
            return model == PBRModel::ClosureV2 ? "ClosureV2" : "Legacy";
        }

        // Runs one family through the shared check, asserts it passes, returns
        // the smallest per-run p.
        f64 ExpectDistribution(const SamplerFn& sampler, const OracleDensity& density, const glm::dvec3& v, f64 binAlpha,
                               const std::string& label)
        {
            const FamilyVerdict verdict = CheckSamplingDistribution(sampler, density, v, binAlpha, kSamplingPlan);
            EXPECT_TRUE(verdict.Pass) << label << " (bin alpha " << binAlpha << ", per-run alpha "
                                      << kSamplingPlan.Runs.PerRunAlpha() << "):\n"
                                      << verdict.Detail;
            return verdict.SmallestPValue;
        }

        // ---- f32 helpers -----------------------------------------------------

        // f64 cosine of an f32 unit vector's polar angle, rebuilt from the
        // tangential components: near the pole h.z is quantised in 2^-24
        // steps while (x, y) keep full relative precision.
        [[nodiscard]] f64 PolarCosine(const glm::vec3& h)
        {
            const f64 x = h.x;
            const f64 y = h.y;
            const f64 z = h.z;
            if (z <= 0.0)
                return 0.0;
            return 1.0 / std::sqrt(1.0 + (x * x + y * y) / (z * z));
        }

        [[nodiscard]] f64 AzimuthFraction(const glm::vec3& h)
        {
            f64 phi = std::atan2(static_cast<f64>(h.y), static_cast<f64>(h.x));
            if (phi < 0.0)
                phi += 2.0 * kPi;
            return phi / (2.0 * kPi);
        }

        [[nodiscard]] glm::vec2 NextXi(IidStream& stream)
        {
            const auto x = static_cast<f32>(stream.Next());
            const auto y = static_cast<f32>(stream.Next());
            return { x, y };
        }

        // The pre-#1347 engine form of ImportanceSampleGGX's inversion, kept
        // as the paired negative control of the regression test below (and
        // BsdfMutationDetectionTest M10). Verbatim from ReferenceBRDF.h before
        // the fix, in the n = +z frame the engine's basis reduces to:
        //   cosTheta = sqrt(max(0, (1 - xi.y) / (1 + (a^2 - 1) xi.y)));
        //   sinTheta = sqrt(max(0, 1 - cosTheta^2));   <-- the cancellation
        [[nodiscard]] glm::vec3 Pre1347GgxHalfVector(const glm::vec2& xi, f32 roughness)
        {
            const f32 a = roughness * roughness;
            const f32 phi = 2.0f * PathTracing::kPi * xi.x;
            const f32 cosTheta = std::sqrt(std::max(0.0f, (1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y)));
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            return glm::normalize(glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta));
        }

        [[nodiscard]] std::string Cell(f64 roughness, f64 cosV)
        {
            std::ostringstream s;
            s << "roughness " << roughness << ", cos v " << cosV;
            return s.str();
        }
    } // namespace

    // =========================================================================
    // 1. VNDF: SampleGGXVNDF reflected vs D_v(m) / (4 v.m)
    // =========================================================================

    TEST(BsdfSamplingDistributionTest, VndfSamplerMatchesTheVisibleNormalDensity)
    {
        f64 smallest = 1.0;
        for (f64 roughness : kLobeRoughness)
        {
            const f64 alpha = roughness * roughness; // the GGX alpha of the input roughness
            for (f64 cosV : kLobeCosV)
            {
                smallest = std::min(smallest, ExpectDistribution(Engine::VndfSampler(roughness),
                                                                 { Lobe{ Shape::VisibleNormal, 1.0, alpha } },
                                                                 Direction(cosV, 0.0), alpha,
                                                                 "SampleGGXVNDF, " + Cell(roughness, cosV)));
            }
        }
        std::cout << "[ VNDF ] smallest per-run p over the grid: " << smallest << '\n';
    }

    // =========================================================================
    // 2. ImportanceSampleGGX reflected vs D(m) cos(m) / (4 v.m)
    // =========================================================================

    TEST(BsdfSamplingDistributionTest, GgxHalfVectorSamplerMatchesTheReflectedNdf)
    {
        // The adapter passes roughness straight to ImportanceSampleGGX (the
        // Legacy mixture applies its 0.04 floor before calling it; that path
        // is test 4), so the sampler is tested at the given roughness. The
        // roughness 0.05 row failed before the #1347 fix (file header).
        f64 smallest = 1.0;
        for (f64 roughness : kLobeRoughness)
        {
            const f64 alpha = roughness * roughness;
            for (f64 cosV : kLobeCosV)
            {
                smallest = std::min(smallest, ExpectDistribution(Engine::GgxReflectionSampler(roughness),
                                                                 { Lobe{ Shape::NdfReflection, 1.0, alpha } },
                                                                 Direction(cosV, 0.0), alpha,
                                                                 "ImportanceSampleGGX, " + Cell(roughness, cosV)));
            }
        }
        std::cout << "[ GGX ] smallest per-run p over the grid: " << smallest << '\n';
    }

    // =========================================================================
    // 3. CosineSampleHemisphere vs cos(theta) / pi
    // =========================================================================

    TEST(BsdfSamplingDistributionTest, CosineSamplerMatchesLambert)
    {
        // Bin alpha 1. At v = n the half-vector sits at theta_l / 2, so
        // t = tan^2(theta_l/2) / (1 + tan^2(theta_l/2)) = sin^2(theta_l/2)
        // = (1 - cos theta_l) / 2: monotone in theta_l, filling t in [0, 1/2]
        // (the upper rows are below the horizon and correctly empty). At
        // cos v = 0.5 the same draws are binned on a tilted grid that mixes
        // theta and phi.
        f64 smallest = 1.0;
        for (f64 cosV : kCosineCosV)
        {
            smallest = std::min(smallest, ExpectDistribution(Engine::CosineSampler(), { Lobe{ Shape::Cosine, 1.0, 1.0 } },
                                                             Direction(cosV, 0.0), 1.0,
                                                             "CosineSampleHemisphere, binned at cos v " +
                                                                 std::to_string(cosV)));
        }
        std::cout << "[ cosine ] smallest per-run p: " << smallest << '\n';
    }

    // =========================================================================
    // 4. BSDF::Sample, the one-sample mixture, both models
    // =========================================================================

    namespace
    {
        void ExpectMixtures(PBRModel model)
        {
            // A Latin square over the views: one view per (material,
            // roughness), alternating, so every material and every roughness
            // meets both the grazing and the moderate view at half the cost
            // (sampling BSDF::Sample dominates the runtime; fewer cells, not
            // fewer draws). The pointwise pdf test covers the full grid.
            f64 smallest = 1.0;
            for (sizet mi = 0; mi < kMixtureMaterials.size(); ++mi)
            {
                const NamedMaterial& named = kMixtureMaterials[mi];
                for (sizet ri = 0; ri < kMixtureRoughness.size(); ++ri)
                {
                    const f64 roughness = kMixtureRoughness[ri];
                    const f64 cosV = kMixtureCosV[(mi + ri) % kMixtureCosV.size()];
                    const MaterialCase material{ named.Albedo, named.Metallic, roughness };
                    const f64 pS = Engine::SpecularProbability(material, model);
                    ASSERT_GT(pS, 0.0);
                    ASSERT_LT(pS, 1.0);
                    const OracleDensity density = MixtureDensity(model, pS, roughness);
                    // Bins follow the specular lobe: that is where a wrong
                    // alpha, Jacobian or pS shows. The diffuse share lands
                    // mostly in the last t row; its shape is test 3's job.
                    const f64 binAlpha = density.front().Alpha;
                    std::ostringstream label;
                    label << ModelName(model) << " BSDF::Sample, " << named.Name << ", " << Cell(roughness, cosV)
                          << ", pS " << pS;
                    smallest = std::min(smallest, ExpectDistribution(Engine::BsdfSampler(material, model), density,
                                                                     Direction(cosV, 0.0), binAlpha, label.str()));
                }
            }
            std::cout << "[ " << ModelName(model) << " mixture ] smallest per-run p over the grid: " << smallest
                      << '\n';
        }
    } // namespace

    TEST(BsdfSamplingDistributionTest, ClosureV2MixtureMatchesTheOracleMixture)
    {
        ExpectMixtures(PBRModel::ClosureV2);
    }

    TEST(BsdfSamplingDistributionTest, LegacyMixtureMatchesTheOracleMixture)
    {
        ExpectMixtures(PBRModel::Legacy);
    }

    // =========================================================================
    // 7. BSDF::Pdf against the oracle mixture, pointwise at sampled directions
    // =========================================================================

    TEST(BsdfSamplingDistributionTest, EnginePdfEqualsTheOracleMixtureAtSampledDirections)
    {
        // Not a statistic: at every direction the sampler actually produces,
        // the density the integrator divides by must BE the independent
        // density, to f32 precision.
        //
        // The f32 bound. D depends on the half-vector through
        // q = sin^2(theta_m) + alpha^2 cos^2(theta_m) (D = alpha^2 / (pi q^2)),
        // and the engine forms q from an f32 half-vector as 1 - c^2 (1 - a^2),
        // with an absolute error of a few ulp (2^-24). D's relative error is
        // then ~ 2 * few * 2^-24 / q: 5e-3 at the peak of an alpha = 0.01
        // lobe, 1e-6 on a rough one. The bound allows 32 * 2^-24 / q on the
        // specular share plus 1e-4 relative on the whole (G1, the Jacobian and
        // Lambert in f32).
        constexpr u32 kDrawsPerCell = 20000u;
        constexpr f64 kUlp = 1.0 / 16777216.0; // 2^-24
        for (PBRModel model : { PBRModel::ClosureV2, PBRModel::Legacy })
        {
            f64 worstRatio = 0.0; // |engine - oracle| / bound, worst over the grid
            std::string worstWhere;
            for (const NamedMaterial& named : kMixtureMaterials)
            {
                for (f64 roughness : kMixtureRoughness)
                {
                    const MaterialCase material{ named.Albedo, named.Metallic, roughness };
                    const f64 pS = Engine::SpecularProbability(material, model);
                    const OracleDensity density = MixtureDensity(model, pS, roughness);
                    const f64 alpha = density.front().Alpha;
                    const SamplerFn sampler = Engine::BsdfSampler(material, model);
                    const PdfFn enginePdf = Engine::BsdfPdf(material, model);
                    for (f64 cosV : kMixtureCosV)
                    {
                        const glm::dvec3 v = Direction(cosV, 0.0);
                        IidStream stream(kSamplingPlan.Runs.Seed(0) ^ 0x7'0000'0000ull);
                        u32 accepted = 0;
                        for (u32 i = 0; i < kDrawsPerCell; ++i)
                        {
                            const std::optional<glm::dvec3> l = sampler(v, stream);
                            if (!l)
                                continue;
                            ++accepted;
                            const f64 got = enginePdf(v, *l);
                            const f64 spec = density.front().Weight * LobePdf(density.front(), v, *l);
                            const f64 expected = DensityPdf(density, v, *l);
                            const glm::dvec3 m = HalfVector(v, *l);
                            const f64 q = (1.0 - m.z * m.z) + alpha * alpha * m.z * m.z;
                            const f64 bound = 1.0e-4 * expected + 32.0 * kUlp / q * spec;
                            const f64 ratio = std::abs(got - expected) / bound;
                            if (!std::isfinite(got) || ratio > worstRatio)
                            {
                                worstRatio = std::isfinite(got) ? ratio : std::numeric_limits<f64>::infinity();
                                std::ostringstream s;
                                s << named.Name << ", " << Cell(roughness, cosV) << ", l " << Describe(*l)
                                  << ": engine " << got << ", oracle " << expected << ", bound " << bound;
                                worstWhere = s.str();
                            }
                        }
                        EXPECT_GT(accepted, kDrawsPerCell / 2u) << ModelName(model) << ", " << named.Name;
                    }
                }
            }
            EXPECT_LE(worstRatio, 1.0) << ModelName(model) << " BSDF::Pdf differs from the oracle mixture at "
                                       << worstWhere;
            std::cout << "[ " << ModelName(model) << " pdf ] worst |engine - oracle| / f32 bound: " << worstRatio
                      << " at " << worstWhere << '\n';
        }
    }

    // =========================================================================
    // 5. KS: the theta-CDF and azimuth marginals of the half-vector samplers
    // =========================================================================

    namespace
    {
        // Two one-dimensional families per roughness: t = GgxThetaCdf(cos
        // theta_h, alpha) and phi / 2pi must each be U(0, 1) (the
        // probability-integral transform of D cos, whose theta CDF the oracle
        // has in closed form, and D's isotropy). KS sees a smooth shape error
        // — a wrong alpha bends the t marginal everywhere — that equal-width
        // chi-square bins average over. (The pre-#1347 pole collapse put a
        // point mass at t = 0 and phi = 0 and failed both at roughness 0.05.)
        template<typename DrawHalfVector>
        void ExpectUniformMarginals(DrawHalfVector&& draw, const std::string& samplerName, f64 roughness)
        {
            const auto uniformCdf = [](f64 x)
            { return std::clamp(x, 0.0, 1.0); };
            const f64 alpha = roughness * roughness;
            std::vector<f64> values;
            values.reserve(kSamplingPlan.SamplesPerRun);
            const FamilyVerdict theta =
                RunFamily(kSamplingPlan.Runs,
                          [&](u64 seed)
                          {
                              IidStream stream(seed);
                              values.clear();
                              for (u32 i = 0; i < kSamplingPlan.SamplesPerRun; ++i)
                                  values.push_back(GgxThetaCdf(PolarCosine(draw(roughness, stream)), alpha));
                              return KolmogorovSmirnovTest(values, uniformCdf, IidStream::Provenance());
                          });
            const FamilyVerdict azimuth =
                RunFamily(kSamplingPlan.Runs,
                          [&](u64 seed)
                          {
                              IidStream stream(seed ^ 0xA21Bull); // its own family, its own seeds
                              values.clear();
                              for (u32 i = 0; i < kSamplingPlan.SamplesPerRun; ++i)
                                  values.push_back(AzimuthFraction(draw(roughness, stream)));
                              return KolmogorovSmirnovTest(values, uniformCdf, IidStream::Provenance());
                          });
            EXPECT_TRUE(theta.Pass) << samplerName << ", roughness " << roughness
                                    << ": t = GgxThetaCdf(cos theta_h, alpha = r^2) is not U(0,1):\n"
                                    << theta.Detail;
            EXPECT_TRUE(azimuth.Pass) << samplerName << ", roughness " << roughness
                                      << ": azimuth / 2pi is not U(0,1):\n"
                                      << azimuth.Detail;
            std::cout << "[ KS " << samplerName << " r " << roughness << " ] smallest p: theta "
                      << theta.SmallestPValue << ", azimuth " << azimuth.SmallestPValue << '\n';
        }
    } // namespace

    TEST(BsdfSamplingDistributionTest, ImportanceSampleGgxThetaCdfAndAzimuthAreUniform)
    {
        // ImportanceSampleGGX draws m from D(m) cos(m) directly (no reflection,
        // no rejection), in the n = +z frame.
        for (f64 roughness : kLobeRoughness)
        {
            ExpectUniformMarginals([](f64 r, IidStream& stream)
                                   { return PathTracing::ImportanceSampleGGX(NextXi(stream), Engine::kNormal,
                                                                             static_cast<f32>(r)); },
                                   "ImportanceSampleGGX", roughness);
        }
    }

    TEST(BsdfSamplingDistributionTest, VndfAtNormalIncidenceThetaCdfAndAzimuthAreUniform)
    {
        // At v = n the visible-normal density reduces to D cos [Heitz18 eq. 1]:
        //   D_v(m) = G1(n) max(0, n.m) D(m) / (n.n) = D(m) cos(theta_m),
        // since G1(n) = 1 / (1 + Lambda(0)) = 1 (tan 0 = 0: nothing masks a view
        // along the normal) and n.n = 1. So the VNDF sampler at normal incidence
        // must have exactly the marginals of D cos — a second, differently
        // derived check of its alpha convention.
        for (f64 roughness : kLobeRoughness)
        {
            ExpectUniformMarginals([](f64 r, IidStream& stream)
                                   { return PathTracing::SampleGGXVNDF(Engine::kNormal, Engine::kNormal,
                                                                       static_cast<f32>(r), NextXi(stream)); },
                                   "SampleGGXVNDF at v = n", roughness);
        }
    }

    TEST(BsdfSamplingDistributionTest, ImportanceSampleGgxNoLongerCollapsesOntoTheNormalInF32)
    {
        // The #1347 fix, as a pair of predictions over the SAME uniforms.
        //
        // The pre-#1347 form X = (1 - xi) / (1 + (a^2 - 1) xi) = cos^2 theta =
        // 1 / (1 + tan^2 theta), then sin = sqrt(1 - cos^2): X rounds to 1.0f
        // when tan^2 < 2^-25 (half the f32 spacing below 1), and then cos = 1
        // and sin = 0 exactly: h == n. Under D cos that happens with
        // probability
        //   P(tan^2 < 2^-25) = GgxThetaCdf(1 / sqrt(1 + 2^-25), alpha)
        //                    = 2^-25 / (alpha^2 + 2^-25),
        // asserted within a binomial 5-sigma band over 2M draws — the negative
        // control: this test can see the collapse.
        //
        // The engine form sin^2 = a^2 xi / (1 + (a^2 - 1) xi) is a product and
        // a quotient of positive numbers, with no cancellation: sin == 0
        // exactly only if a^2 xi underflows or xi.y == 0. a^2 >= 2.56e-6 at the
        // 0.04 floor and a nonzero f32 xi.y >= 2^-149, so the product
        // underflows only for xi.y below ~2^-130; and a 24-bit f32 uniform is 0
        // with probability 2^-24 (this stream's is f64 rounded to f32, finer
        // still). The expected count on the normal is <= 2M x 2^-24 = 0.12;
        // P(Poisson(0.12) >= 3) = 2.7e-4, so <= 2 is the bound.
        constexpr u32 kDraws = 2000000u;
        for (f64 roughness : { 0.04, 0.05, 0.1 })
        {
            const f64 predicted = GgxThetaCdf(1.0 / std::sqrt(1.0 + std::ldexp(1.0, -25)), roughness * roughness);
            IidStream stream(kSamplingPlan.Runs.Seed(0));
            u32 engineOnTheNormal = 0;
            u32 oldOnTheNormal = 0;
            for (u32 i = 0; i < kDraws; ++i)
            {
                const glm::vec2 xi = NextXi(stream);
                const glm::vec3 h = PathTracing::ImportanceSampleGGX(xi, Engine::kNormal, static_cast<f32>(roughness));
                const glm::vec3 o = Pre1347GgxHalfVector(xi, static_cast<f32>(roughness));
                engineOnTheNormal += (h.x == 0.0f && h.y == 0.0f) ? 1u : 0u;
                oldOnTheNormal += (o.x == 0.0f && o.y == 0.0f) ? 1u : 0u;
            }
            const f64 oldFraction = static_cast<f64>(oldOnTheNormal) / kDraws;
            const f64 sigma = std::sqrt(predicted * (1.0 - predicted) / kDraws);
            EXPECT_LE(engineOnTheNormal, 2u)
                << "roughness " << roughness << ": ImportanceSampleGGX put " << engineOnTheNormal << " of " << kDraws
                << " half-vectors exactly on the normal — the pre-#1347 f32 collapse is back";
            EXPECT_NEAR(oldFraction, predicted, 5.0 * sigma + 1.0e-6)
                << "roughness " << roughness << ": the pre-#1347 form put " << oldOnTheNormal << " of " << kDraws
                << " on the normal; predicted " << predicted * kDraws << " — the control no longer shows the collapse";
            std::cout << "[ f32 collapse r " << roughness << " ] on the normal: engine " << engineOnTheNormal << " of "
                      << kDraws << ", pre-#1347 form " << oldFraction << " (predicted " << predicted << ")\n";
        }
    }

    // =========================================================================
    // Expected-mass quadrature error, stated as a chi-square non-centrality
    // =========================================================================

    namespace
    {
        // N sum (a_i - b_i)^2 / b_i over the bins b gives mass to.
        [[nodiscard]] f64 NonCentrality(const std::vector<f64>& a, const std::vector<f64>& b, u32& occupied)
        {
            const f64 n = static_cast<f64>(kSamplingPlan.SamplesPerRun);
            f64 lambda = 0.0;
            occupied = 0;
            for (sizet i = 0; i < b.size(); ++i)
            {
                if (!(b[i] > 0.0))
                    continue;
                ++occupied;
                const f64 e = a[i] - b[i];
                lambda += n * e * e / b[i];
            }
            return lambda;
        }
    } // namespace

    TEST(BsdfSamplingDistributionTest, ExpectedBinMassQuadratureErrorIsNegligible)
    {
        // If the expected probabilities p_i are off by e_i, a CORRECT sampler's
        // Pearson statistic gains a non-centrality lambda = N sum e_i^2 / p_i.
        // e_i is estimated as the change when the deposit doubles its base
        // grid (which halves every cell, the finest straddling ones included).
        // lambda must stay under a tenth of the null statistic's standard
        // deviation sqrt(2 dof), so it cannot move a p-value materially.
        // Probed where the error is largest: grazing views (a long horizon
        // cut), the sharpest lobe, and mixtures whose diffuse share sits in
        // bins shaped for a sharp specular lobe — where the per-bin midpoint
        // rule this check replaced measured 18 to 270 and ~5e8.
        struct Probe
        {
            const char* Name;
            OracleDensity Density;
            f64 CosV;
        };
        const std::vector<Probe> probes{
            { "VNDF r 1, cos v 0.05", { Lobe{ Shape::VisibleNormal, 1.0, 1.0 } }, 0.05 },
            { "VNDF r 0.05, cos v 0.05", { Lobe{ Shape::VisibleNormal, 1.0, 0.0025 } }, 0.05 },
            { "GGX r 1, cos v 0.05", { Lobe{ Shape::NdfReflection, 1.0, 1.0 } }, 0.05 },
            { "GGX r 0.3, cos v 0.5", { Lobe{ Shape::NdfReflection, 1.0, 0.09 } }, 0.5 },
            { "cosine, cos v 0.5", { Lobe{ Shape::Cosine, 1.0, 1.0 } }, 0.5 },
            { "v2 mixture r 0.1, cos v 0.1, pS 0.1", MixtureDensity(PBRModel::ClosureV2, 0.1, 0.1), 0.1 },
            { "Legacy mixture r 0.1, cos v 0.7, pS 0.5", MixtureDensity(PBRModel::Legacy, 0.5, 0.1), 0.7 },
            { "Legacy mixture r 0.5, cos v 0.1, pS 0.9", MixtureDensity(PBRModel::Legacy, 0.9, 0.5), 0.1 },
        };
        SamplingPlan finerPlan = kSamplingPlan;
        finerPlan.DepositBaseCells = 2u * kSamplingPlan.DepositBaseCells;
        for (const Probe& probe : probes)
        {
            const glm::dvec3 v = Direction(probe.CosV, 0.0);
            const f64 binAlpha = probe.Density.front().Alpha;
            const std::vector<f64> used = DepositBinMasses(probe.Density, v, binAlpha, kSamplingPlan);
            const std::vector<f64> finer = DepositBinMasses(probe.Density, v, binAlpha, finerPlan);
            ASSERT_FALSE(used.empty());
            ASSERT_EQ(used.size(), finer.size());
            u32 occupied = 0;
            const f64 lambda = NonCentrality(used, finer, occupied);
            const f64 limit = 0.1 * std::sqrt(2.0 * std::max(1.0, occupied - 1.0));
            EXPECT_LT(lambda, limit) << probe.Name << ": the expected-mass quadrature error adds a chi-square "
                                     << "non-centrality of " << lambda << " at N = " << kSamplingPlan.SamplesPerRun;
            std::cout << "[ quadrature " << probe.Name << " ] deposit non-centrality " << lambda << " (limit " << limit
                      << "), rejected " << finer.back() << '\n';
        }

        // A plan whose base grid does not align with the bins is refused, not
        // silently integrated with cells straddling every bin edge.
        SamplingPlan misaligned = kSamplingPlan;
        misaligned.DepositBaseCells = 250u;
        EXPECT_TRUE(DepositBinMasses({ Lobe{} }, Direction(0.5, 0.0), 1.0, misaligned).empty());
        const FamilyVerdict refused = CheckSamplingDistribution(Engine::CosineSampler(), { Lobe{} }, Direction(0.5, 0.0),
                                                                1.0, misaligned);
        EXPECT_FALSE(refused.Pass) << refused.Detail;
    }

    // =========================================================================
    // 6. The IID convention in this file's use: stratified points are refused
    // =========================================================================

    TEST(BsdfSamplingDistributionTest, LowDiscrepancyPointsAreRefusedAsEvidence)
    {
        // Hammersley points through the engine's cosine sampler, binned into
        // 16 x 16 equal-mass (sin^2 theta, phi) cells. Stratification makes
        // the histogram almost exactly the expectation — a statistic far below
        // its IID mean, p ~ 1 for ANY smooth density — which is why the helper
        // refuses it rather than returning that p-value.
        constexpr u32 kPoints = 1u << 14;
        constexpr u32 kBins = 16u;
        auto radicalInverse = [](u32 i)
        {
            i = (i << 16u) | (i >> 16u);
            i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
            i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
            i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
            i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
            return static_cast<f64>(i) * 0x1p-32;
        };
        std::vector<u64> observed(kBins * kBins, 0u);
        for (u32 i = 0; i < kPoints; ++i)
        {
            const glm::vec2 xi(static_cast<f32>((i + 0.5) / kPoints), static_cast<f32>(radicalInverse(i)));
            const glm::vec3 l = PathTracing::CosineSampleHemisphere(xi, Engine::kNormal);
            const f64 c = PolarCosine(l);
            const f64 sin2 = 1.0 - c * c; // U(0, 1) under the cosine density
            const u32 it = std::min(static_cast<u32>(sin2 * kBins), kBins - 1u);
            const u32 ip = std::min(static_cast<u32>(AzimuthFraction(l) * kBins), kBins - 1u);
            ++observed[it * kBins + ip];
        }
        const std::vector<f64> expected(kBins * kBins, 1.0 / (kBins * kBins));

        const GoodnessOfFit refused = ChiSquareTest(observed, expected, SampleProvenance::LowDiscrepancy);
        EXPECT_FALSE(refused.Valid) << "a stratified point set was accepted as IID evidence";
        EXPECT_NE(refused.WhyInvalid.find("refused"), std::string::npos) << refused.WhyInvalid;

        // What a mislabelled run would have reported: a statistic far below its
        // IID expectation (dof), i.e. a p-value that says nothing.
        const GoodnessOfFit mislabelled = ChiSquareTest(observed, expected, SampleProvenance::IidPseudoRandom);
        ASSERT_TRUE(mislabelled.Valid);
        EXPECT_LT(mislabelled.Statistic, 0.1 * mislabelled.DegreesOfFreedom)
            << "Hammersley points did not fit 'too well' — the premise of the refusal";
        std::cout << "[ Hammersley ] chi-square " << mislabelled.Statistic << " on " << mislabelled.DegreesOfFreedom
                  << " dof (IID mean = dof), p " << mislabelled.PValue << " — " << refused.WhyInvalid << '\n';

        // And a family built from such runs fails rather than passes.
        const FamilyVerdict family = RunFamily(
            kSamplingPlan.Runs, [&](u64)
            { return ChiSquareTest(observed, expected, SampleProvenance::LowDiscrepancy); });
        EXPECT_FALSE(family.Pass);
    }
} // namespace OloEngine::Tests::Oracle
