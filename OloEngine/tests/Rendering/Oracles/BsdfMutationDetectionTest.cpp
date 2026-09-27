// OLO_TEST_LAYER: L1
// =============================================================================
// BsdfMutationDetectionTest.cpp — the oracle checks can FAIL (issue #1347,
// acceptance criterion 2).
//
// "Known mutations such as missing/wrong Jacobians, mismatched alpha
// conventions and double denominators are detected; not every oracle calls the
// same implementation under test."
//
// Every TEST below is one mutation and a PAIRED NEGATIVE CONTROL: a test-local
// copy of an engine function with exactly one line changed (the changed line
// is quoted beside it) goes through a check from BsdfOracleChecks.h, and the
// unmutated engine function goes through the IDENTICAL check with identical
// parameters. The mutant must fail and the engine must pass. A check that no
// mutant fails is not evidence; a mutant that survives is a finding and would
// be documented here as such, never deleted. Engine code is never edited — the
// mutants live in this file only. Each mutant copies the CURRENT engine body
// (M1 and M2 the vector overloads PdfGGXVNDF(n, v, h, r) / PdfGGX(n, v, h, r)
// that BSDF::Pdf calls, M9 BSDF::Pdf itself), so it differs from its engine
// control in the quoted line and nowhere else.
//
//  mutation                          | real-world bug it models                         | caught by                               | why that oracle is independent of the engine
//  ----------------------------------+--------------------------------------------------+-----------------------------------------+---------------------------------------------
//  M1 VNDF pdf without G1(v)         | a VNDF pdf copied from D_v without its           | CheckPdfNormalisation vs 1 -            | the below-horizon mass is integrated from
//     (PdfGGXVNDF(n, v, h, r))       | normaliser (pdf no longer integrates to the      | VndfBelowHorizonMass (the oracle's      | Heitz18 eq. 1 in f64 (tan-form D, Lambda
//                                    | accepted-draw fraction; #975 contract)           | below-horizon D_v mass)                 | form G1), not from PdfGGXVNDF
//  M2 PdfGGX without 1/(4 v.h)       | the missing reflection Jacobian (half-vector     | CheckPdfNormalisation vs the oracle's   | the accepted mass is an f64 quadrature of
//     (PdfGGX(n, v, h, r))           | density used as a direction density)             | accepted D cos mass                     | D cos over half-vectors (Walter07 eq. 33)
//  M3 VNDF sampler alpha = roughness | #706 / #904: roughness passed where alpha is     | CheckSamplingDistribution (chi-square)  | expected bin masses are deposited from the
//                                    | expected (the ALPHA LEDGER in PBRCommon.glsl)    | vs a VisibleNormal lobe at alpha = r^2  | oracle's D_v and D cos CDF (Heitz18, Walter07)
//  M4 ClosureV2 D at alpha = r       | #706 / #904: NDF on the wrong alpha convention   | CheckEvaluationAgainstModel vs          | Oracle::ClosureV2Brdf: tan-form D, Lambda
//                                    |                                                  | Oracle::ClosureV2Brdf                   | form G2, energies by f64 quadrature
//  M5 D Vis F / (4 NdotV NdotL)      | #904: the double divide — Vis already folds the  | CheckEvaluationAgainstModel + a white   | same model; the furnace integrates the
//                                    | Cook-Torrance denominator in                     | furnace directional albedo > 1          | engine with the oracle's own quadrature
//  M6 Fresnel at NdotV, not VdotH    | a "cheap" Fresnel that breaks Helmholtz          | CheckReciprocity                        | reciprocity is a law, not an implementation:
//                                    | reciprocity (Legacy and ClosureV2 closures)      |                                         | it needs no second copy of the formula
//  M7a-d reconnection Jacobian       | #1140 / #1169: inverted J, missing cosine or     | CheckReconnectionJacobian vs the ratio  | Van Oosterom-Strackee exact solid angles of a
//        inverted / no cos / no d^2  | distance ratio, "no Jacobian" (J = 1)            | of two measured solid angles            | finite patch — no cos/d^2 transcription
//        / J = 1                     |                                                  |                                         |
//  M7e shift without its J = 1 arm   | design note §4.3: an environment (GI) or delta   | the oracle's solid-angle ratio in the   | the same measured solid angles, taken to the
//                                    | light (DI) sample fed to the geometric core      | distant limit (-> 1)                    | distant limit numerically
//  M8a/b Kulla-Conty at alpha /      | the energy table looked up with the wrong        | white furnace (f64 quadrature of f cos, | quadrature and the physical statement
//        omitted                     | convention; compensation dropped                 | albedo 1, metallic 1) must equal 1      | "F = 1 loses no energy" — not the table
//  M9 mixture pdf with pS = 0.5      | a MIS density written independently of the      | CheckSamplingDistribution vs the        | the chi-square counts the SAMPLER's draws;
//     (BSDF::Pdf)                    | sampler it describes (PBRClosureBSDF.h header)   | oracle mixture Pdf() claims (pinned     | the claimed density is only what is tested
//                                    |                                                  | pointwise to the mutant Pdf)            |
//  M10 pre-#1347 ImportanceSampleGGX | A REAL BUG, found and fixed by #1347: sin theta  | CheckSamplingDistribution vs an         | expected bin masses are deposited from the
//      sin = sqrt(1 - cos^2) in f32  | = sqrt(1 - cos^2) in f32 put 2^-25/(a^2+2^-25)   | NdfReflection lobe at alpha = r^2,      | oracle's D cos CDF in f64; the f32
//                                    | of all draws exactly on the normal (1.2 % at the | roughness 0.04 and 0.05                 | cancellation exists only in the sampler
//                                    | Legacy 0.04 floor)                               |                                         |
//
// Sample-size conventions are OracleStatistics.h's: 5 independent runs,
// Bonferroni at family alpha 1e-3, IID pseudo-random draws only.
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/Oracles/BsdfOracleChecks.h"
#include "Rendering/Oracles/EngineBsdfAdapters.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirCore.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirDI.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirGI.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OloEngine::Tests::Oracle
{
    namespace
    {
        using Engine::kNormal;
        using Engine::ToF32;
        using Engine::ToF64;

        // Every mutation test shows what the check saw, pass or fail, so a
        // reader of the log can see the MARGIN, not just the verdict.
        void Report(const char* who, const Verdict& v)
        {
            std::printf("  [%s] %s: worst %.6g vs bound %.6g\n      %s\n", who, v.Pass ? "pass" : "FAIL", v.Worst,
                        v.Bound, v.Detail.c_str());
        }

        void Report(const char* who, const FamilyVerdict& v)
        {
            std::printf("  [%s] %s: smallest p %.6g\n%s", who, v.Pass ? "pass" : "FAIL", v.SmallestPValue,
                        v.Detail.c_str());
        }

        // ---- oracle models (f64, no engine code) ----------------------------

        // Oracle::ClosureV2Brdf with its energies — E and the Schlick moment S
        // for the Kulla-Conty lobe and the coupled Lambert — from the oracle's
        // own quadrature, memoised per cosine (the direction grid has five of
        // them).
        [[nodiscard]] BrdfFn OracleClosureV2(const MaterialCase& m)
        {
            const f64 alpha = ClosureV2Alpha(m.Roughness);
            const f64 eAvg = GgxAverageAlbedo(alpha).Value;
            const f64 sAvg = GgxAverageSchlickMoment(alpha).Value;
            auto cache = std::make_shared<std::map<f64, glm::dvec2>>();
            return [m, alpha, eAvg, sAvg, cache](const glm::dvec3& v, const glm::dvec3& l) -> glm::dvec3
            {
                if (v.z <= 0.0 || l.z <= 0.0)
                    return glm::dvec3(0.0);
                auto moments = [&](f64 mu)
                {
                    if (const auto it = cache->find(mu); it != cache->end())
                        return it->second;
                    const glm::dvec2 value(GgxDirectionalAlbedo(mu, alpha).Value,
                                           GgxDirectionalSchlickMoment(mu, alpha).Value);
                    cache->emplace(mu, value);
                    return value;
                };
                const glm::dvec2 mv = moments(v.z);
                const glm::dvec2 ml = moments(l.z);
                ClosureV2Energies energies;
                energies.EV = mv.x;
                energies.EL = ml.x;
                energies.EAvg = eAvg;
                energies.SV = mv.y;
                energies.SL = ml.y;
                energies.SAvg = sAvg;
                energies.MultiScatter = 1.0 - eAvg >= 1.0e-4; // ADR 0016 §5's gate
                return ClosureV2Brdf(v, l, m.Albedo, m.Metallic, m.Roughness, energies);
            };
        }

        // The white furnace. Albedo 1, metallic 1 makes F0 = 1, so Schlick's F
        // is exactly 1 and there is no diffuse lobe: a surface that absorbs
        // nothing. ClosureV2 claims (ADR 0016, Kulla-Conty) that its
        // single-scattering loss is put back, so int f cos dw = 1 at every
        // view angle. The integral is the oracle's quadrature of the
        // implementation; the bound is max(floor, 4 x its error estimate).
        using ClosureFactory = std::function<BrdfFn(const MaterialCase&)>;

        // The grid reaches the table's edges: since #1478 the energy table is
        // node-centred with both endpoints as nodes, so roughness 1 and a
        // grazing cosine are read by interpolation, not by an edge clamp (which
        // cost 3.8 % at roughness 1, cos v 1 before it). The engine's worst
        // cell on this grid is measured by the engine-furnace verdict below.
        inline const std::vector<f64> kFurnaceRoughness{ 0.3, 0.5, 0.75, 1.0 };
        inline const std::vector<f64> kFurnaceCosines{ 0.02, 0.1, 0.35, 0.7, 1.0 };

        [[nodiscard]] Verdict CheckWhiteFurnace(const ClosureFactory& make, f64 floorTol, const std::string& label)
        {
            Verdict verdict;
            for (f64 roughness : kFurnaceRoughness)
            {
                const MaterialCase material{ glm::dvec3(1.0), 1.0, roughness };
                const BrdfFn f = make(material);
                for (f64 mu : kFurnaceCosines)
                {
                    const Quadrature q = DirectionalAlbedo(f, Direction(mu, 0.0), ClosureV2Alpha(roughness), 0);
                    const f64 err = std::abs(q.Value - 1.0);
                    const f64 bound = std::max(floorTol, 4.0 * q.ErrorEstimate);
                    verdict.Record(err <= bound, err, bound,
                                   label + ": roughness " + std::to_string(roughness) + ", cos v " +
                                       std::to_string(mu) + ": albedo " + std::to_string(q.Value) +
                                       " (quadrature error estimate " + std::to_string(q.ErrorEstimate) + ")");
                }
            }
            return verdict;
        }

        [[nodiscard]] ClosureFactory EngineClosureV2Factory()
        {
            return [](const MaterialCase& m)
            { return Engine::ClosureV2Brdf(m); };
        }

        // =====================================================================
        // THE MUTANTS. Each is the engine body with exactly one change; the
        // changed line is quoted. Signatures match the engine's.
        // =====================================================================

        // ---- M1: PathTracing::PdfGGXVNDF(n, v, h, r) without G1(v) ----------
        // engine: return g1V * DistributionGGXSamplingDensity(n, h, roughness) / (4.0f * nDotV);
        // mutant: return       DistributionGGXSamplingDensity(n, h, roughness) / (4.0f * nDotV);
        [[nodiscard]] f32 MutantPdfGGXVNDF_NoG1(const glm::vec3& n, const glm::vec3& v, const glm::vec3& h,
                                                f32 roughness) noexcept
        {
            const f32 nDotV = glm::dot(n, v);
            if (nDotV <= 0.0f)
                return 0.0f;
            const f32 alpha = roughness * roughness;
            [[maybe_unused]] const f32 g1V = 1.0f / (1.0f + PathTracing::GgxSmithLambda(nDotV, alpha));
            return PathTracing::DistributionGGXSamplingDensity(n, h, roughness) / (4.0f * nDotV);
        }

        // ---- M2: PathTracing::PdfGGX(n, v, h, r) without the reflection Jacobian
        // engine: return DistributionGGXSamplingDensity(n, h, roughness) * std::max(glm::dot(n, h), 0.0f) / (4.0f * vDotH);
        // mutant: return DistributionGGXSamplingDensity(n, h, roughness) * std::max(glm::dot(n, h), 0.0f);
        [[nodiscard]] f32 MutantPdfGGX_NoReflectionJacobian(const glm::vec3& n, const glm::vec3& v, const glm::vec3& h,
                                                            f32 roughness) noexcept
        {
            const f32 vDotH = glm::dot(v, h);
            if (vDotH <= 0.0f)
                return 0.0f;
            return PathTracing::DistributionGGXSamplingDensity(n, h, roughness) * std::max(glm::dot(n, h), 0.0f);
        }

        // Adapters: Engine::VndfPdf / Engine::GgxReflectionPdf with the mutant
        // substituted for the engine call and nothing else changed.
        [[nodiscard]] PdfFn MutantVndfPdf(f64 roughness)
        {
            return [roughness](const glm::dvec3& v, const glm::dvec3& l)
            {
                const glm::vec3 vf = ToF32(v);
                const glm::vec3 lf = ToF32(l);
                if (lf.z <= 0.0f)
                    return 0.0;
                const glm::vec3 h = glm::normalize(vf + lf);
                return static_cast<f64>(MutantPdfGGXVNDF_NoG1(kNormal, vf, h, static_cast<f32>(roughness)));
            };
        }

        [[nodiscard]] PdfFn MutantGgxReflectionPdf(f64 roughness)
        {
            return [roughness](const glm::dvec3& v, const glm::dvec3& l)
            {
                const glm::vec3 vf = ToF32(v);
                const glm::vec3 lf = ToF32(l);
                if (lf.z <= 0.0f)
                    return 0.0;
                const glm::vec3 h = glm::normalize(vf + lf);
                return static_cast<f64>(MutantPdfGGX_NoReflectionJacobian(kNormal, vf, h, static_cast<f32>(roughness)));
            };
        }

        // ---- M3: PathTracing::SampleGGXVNDF stretching by ROUGHNESS ---------
        // engine: const f32 alpha = roughness * roughness;
        // mutant: const f32 alpha = roughness;
        [[nodiscard]] glm::vec3 MutantSampleGGXVNDF_AlphaIsRoughness(const glm::vec3& n, const glm::vec3& v, f32 roughness,
                                                                     const glm::vec2& xi) noexcept
        {
            glm::vec3 tangent;
            glm::vec3 bitangent;
            PathTracing::OrthonormalBasis(n, tangent, bitangent);

            const glm::vec3 ve(glm::dot(v, tangent), glm::dot(v, bitangent), glm::dot(v, n));
            const f32 alpha = roughness;
            const glm::vec3 h = PathTracing::SampleGGXVNDFTangent(ve, alpha, alpha, xi);
            return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
        }

        // Engine::VndfSampler with the mutant substituted.
        [[nodiscard]] SamplerFn MutantVndfSampler(f64 roughness)
        {
            return [roughness](const glm::dvec3& v, IidStream& stream) -> std::optional<glm::dvec3>
            {
                const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
                const glm::vec3 vf = ToF32(v);
                const glm::vec3 h = MutantSampleGGXVNDF_AlphaIsRoughness(kNormal, vf, static_cast<f32>(roughness), xi);
                const glm::vec3 l = glm::reflect(-vf, h);
                if (l.z <= 0.0f)
                    return std::nullopt;
                return ToF64(l);
            };
        }

        // ---- M4/M5/M6b/M8: PathTracing::ClosureV2Evaluate variants ----------
        // One body, one switch per mutation, so each variant differs from the
        // engine function (reproduced line for line under `None`) by exactly
        // the line its case names.
        enum class ClosureV2Mutation
        {
            None,                 // the engine body, verbatim (control for the copy itself)
            NdfAlphaIsRoughness,  // M4
            DoubleDenominator,    // M5
            FresnelAtNdotV,       // M6
            EnergyLookupAtAlpha,  // M8a
            NoEnergyCompensation, // M8b
        };

        [[nodiscard]] glm::vec3 MutantClosureV2Evaluate(ClosureV2Mutation mutation, const glm::vec3& n, const glm::vec3& v,
                                                        const glm::vec3& l, const glm::vec3& albedo, f32 metallic,
                                                        f32 roughness) noexcept
        {
            using namespace PathTracing;
            const f32 r = ClosureV2Roughness(roughness);
            const glm::vec3 h = glm::normalize(v + l);
            const f32 nDotV = std::max(glm::dot(n, v), 0.0f);
            const f32 nDotL = std::max(glm::dot(n, l), 0.0f);

            const glm::vec3 f0 = glm::mix(glm::vec3(kDefaultDielectricF0), albedo, metallic);

            // engine: const f32 d = DistributionGGXSamplingDensity(n, h, r);
            // M4:     const f32 d = DistributionGGXSamplingDensity(n, h, std::sqrt(r));   (alpha = r, not r^2)
            const f32 d = mutation == ClosureV2Mutation::NdfAlphaIsRoughness
                              ? DistributionGGXSamplingDensity(n, h, std::sqrt(r))
                              : DistributionGGXSamplingDensity(n, h, r);
            const f32 vis = VisibilitySmithGGXCorrelated(nDotV, nDotL, r);

            // engine: const glm::vec3 f = FresnelSchlick(std::max(glm::dot(h, v), 0.0f), f0);
            // M6:     const glm::vec3 f = FresnelSchlick(std::max(glm::dot(n, v), 0.0f), f0);
            const glm::vec3 f = mutation == ClosureV2Mutation::FresnelAtNdotV
                                    ? FresnelSchlick(std::max(glm::dot(n, v), 0.0f), f0)
                                    : FresnelSchlick(std::max(glm::dot(h, v), 0.0f), f0);

            // engine: const ClosureV2EnergyTerms energy = ClosureV2Energy(nDotV, nDotL, roughness, f0);
            //         const glm::vec3 specular = d * vis * f + energy.MultiScatter;
            // M5:     const glm::vec3 specular = d * vis * f / (4.0f * nDotV * nDotL) + energy.MultiScatter;
            // M8a:    const ClosureV2EnergyTerms energy = ClosureV2Energy(nDotV, nDotL, r * r, f0);
            // M8b:    const glm::vec3 specular = d * vis * f;
            const ClosureV2EnergyTerms energy =
                ClosureV2Energy(nDotV, nDotL, mutation == ClosureV2Mutation::EnergyLookupAtAlpha ? r * r : roughness, f0);
            glm::vec3 specular;
            switch (mutation)
            {
                case ClosureV2Mutation::DoubleDenominator:
                    specular = d * vis * f / (4.0f * nDotV * nDotL) + energy.MultiScatter;
                    break;
                case ClosureV2Mutation::NoEnergyCompensation:
                    specular = d * vis * f;
                    break;
                default:
                    specular = d * vis * f + energy.MultiScatter;
                    break;
            }

            const glm::vec3 kD = energy.DiffuseCoupling * (1.0f - metallic);
            return kD * albedo * kInvPi + specular;
        }

        // Engine::ClosureV2Brdf with the mutant substituted.
        [[nodiscard]] BrdfFn MutantClosureV2Brdf(ClosureV2Mutation mutation, const MaterialCase& m)
        {
            return [mutation, m](const glm::dvec3& v, const glm::dvec3& l)
            {
                return ToF64(MutantClosureV2Evaluate(mutation, kNormal, ToF32(v), ToF32(l), ToF32(m.Albedo),
                                                     static_cast<f32>(m.Metallic), static_cast<f32>(m.Roughness)));
            };
        }

        [[nodiscard]] ClosureFactory MutantClosureV2Factory(ClosureV2Mutation mutation)
        {
            return [mutation](const MaterialCase& m)
            { return MutantClosureV2Brdf(mutation, m); };
        }

        // ---- M6 (Legacy): PathTracing::CookTorranceBRDF, Fresnel at NdotV ---
        // engine: const glm::vec3 f = FresnelSchlick(std::max(glm::dot(h, v), 0.0f), f0);
        // mutant: const glm::vec3 f = FresnelSchlick(std::max(glm::dot(n, v), 0.0f), f0);
        [[nodiscard]] glm::vec3 MutantCookTorranceBRDF_FresnelAtNdotV(const glm::vec3& n, const glm::vec3& v,
                                                                      const glm::vec3& l, const glm::vec3& albedo,
                                                                      f32 metallic, f32 roughness) noexcept
        {
            using namespace PathTracing;
            const glm::vec3 h = glm::normalize(v + l);

            glm::vec3 f0 = glm::vec3(kDefaultDielectricF0);
            f0 = glm::mix(f0, albedo, metallic);

            const f32 ndf = DistributionGGX(n, h, roughness);
            const f32 g = GeometrySmith(n, v, l, roughness);
            const glm::vec3 f = FresnelSchlick(std::max(glm::dot(n, v), 0.0f), f0);

            const glm::vec3 numerator = ndf * g * f;
            const f32 denominator = 4.0f * std::max(glm::dot(n, v), 0.0f) * std::max(glm::dot(n, l), 0.0f) + kEpsilon;
            const glm::vec3 specular = numerator / denominator;

            glm::vec3 kD = glm::vec3(1.0f) - f;
            kD *= 1.0f - metallic;

            return kD * albedo * kInvPi + specular;
        }

        [[nodiscard]] BrdfFn MutantLegacyBrdf_FresnelAtNdotV(const MaterialCase& m)
        {
            return [m](const glm::dvec3& v, const glm::dvec3& l)
            {
                return ToF64(MutantCookTorranceBRDF_FresnelAtNdotV(kNormal, ToF32(v), ToF32(l), ToF32(m.Albedo),
                                                                   static_cast<f32>(m.Metallic),
                                                                   static_cast<f32>(m.Roughness)));
            };
        }

        // ---- M7: ReSTIR::ReconnectionJacobian variants ----------------------
        enum class JacobianMutation
        {
            None,            // the engine body, verbatim
            Inverted,        // M7a
            NoCosineRatio,   // M7b
            NoDistanceRatio, // M7c
            One,             // M7d
        };

        [[nodiscard]] f32 MutantReconnectionJacobian(JacobianMutation mutation, const glm::vec3& vertexPosition,
                                                     const glm::vec3& vertexNormal, const glm::vec3& destShadingPoint,
                                                     const glm::vec3& sourceShadingPoint)
        {
            const glm::vec3 toDest = destShadingPoint - vertexPosition;
            const glm::vec3 toSource = sourceShadingPoint - vertexPosition;
            const f32 destDistanceSq = glm::dot(toDest, toDest);
            const f32 sourceDistanceSq = glm::dot(toSource, toSource);
            if (!(destDistanceSq > 0.0f) || !(sourceDistanceSq > 0.0f))
                return 0.0f;

            const f32 normalLengthSq = glm::dot(vertexNormal, vertexNormal);
            if (!(normalLengthSq > 0.0f))
                return 0.0f;
            const glm::vec3 n = vertexNormal / std::sqrt(normalLengthSq);

            const f32 cosDest = std::abs(glm::dot(n, toDest / std::sqrt(destDistanceSq)));
            const f32 cosSource = std::abs(glm::dot(n, toSource / std::sqrt(sourceDistanceSq)));
            if (!(cosSource > ReSTIR::kMinimumShiftCosine))
                return 0.0f;

            // engine: const f32 jacobian = (cosDest / cosSource) * (sourceDistanceSq / destDistanceSq);
            // M7a:    const f32 jacobian = (cosSource / cosDest) * (destDistanceSq / sourceDistanceSq);
            // M7b:    const f32 jacobian = (sourceDistanceSq / destDistanceSq);
            // M7c:    const f32 jacobian = (cosDest / cosSource);
            // M7d:    const f32 jacobian = 1.0f;
            f32 jacobian = 0.0f;
            switch (mutation)
            {
                case JacobianMutation::Inverted:
                    jacobian = (cosSource / cosDest) * (destDistanceSq / sourceDistanceSq);
                    break;
                case JacobianMutation::NoCosineRatio:
                    jacobian = sourceDistanceSq / destDistanceSq;
                    break;
                case JacobianMutation::NoDistanceRatio:
                    jacobian = cosDest / cosSource;
                    break;
                case JacobianMutation::One:
                    jacobian = 1.0f;
                    break;
                case JacobianMutation::None:
                    jacobian = (cosDest / cosSource) * (sourceDistanceSq / destDistanceSq);
                    break;
            }
            return std::isfinite(jacobian) && jacobian >= 0.0f ? jacobian : 0.0f;
        }

        [[nodiscard]] JacobianFn MutantJacobian(JacobianMutation mutation)
        {
            return [mutation](const glm::dvec3& vertex, const glm::dvec3& normal, const glm::dvec3& destination,
                              const glm::dvec3& source)
            {
                return static_cast<f64>(MutantReconnectionJacobian(mutation, ToF32(vertex), ToF32(normal),
                                                                   ToF32(destination), ToF32(source)));
            };
        }

        // ---- M7e: the domain wrappers without their J = 1 arm ---------------
        // engine (GI): if (IsDistantSample(sample.Kind)) return 1.0f;   -- mutant: line removed
        [[nodiscard]] f32 MutantGIShiftJacobian_NoEnvironmentArm(const ReSTIR::GISample& sample,
                                                                 const glm::vec3& destShadingPoint,
                                                                 const glm::vec3& sourceShadingPoint)
        {
            if (sample.Kind == ReSTIR::GISampleKind::None)
                return 0.0f;
            return ReSTIR::ReconnectionJacobian(sample.Position, sample.Normal, destShadingPoint, sourceShadingPoint);
        }

        // engine (DI): if (IsDeltaLight(sample.Kind)) return 1.0f;       -- mutant: line removed
        [[nodiscard]] f32 MutantShiftJacobian_NoDeltaArm(const ReSTIR::LightSample& sample,
                                                         const glm::vec3& destShadingPoint,
                                                         const glm::vec3& sourceShadingPoint)
        {
            return ReSTIR::ReconnectionJacobian(sample.Position, sample.Normal, destShadingPoint, sourceShadingPoint);
        }

        // ---- M9: PathTracing::BSDF::Pdf with its own lobe probability -------
        // engine: const f32 pSpecular = SpecularProbability(material);
        // mutant: const f32 pSpecular = 0.5f;
        [[nodiscard]] f32 MutantBsdfPdf_FixedLobeProbability(const PathTracing::ReferenceMaterial& material,
                                                             const glm::vec3& n, const glm::vec3& v,
                                                             const glm::vec3& l) noexcept
        {
            using namespace PathTracing;
            using namespace PathTracing::BSDF;
            const f32 nDotL = glm::dot(n, l);
            if (nDotL <= 0.0f)
                return 0.0f;

            const f32 pdfDiffuse = PdfCosineHemisphere(nDotL);
            const f32 pSpecular = 0.5f;
            if (!(pSpecular > 0.0f))
                return pdfDiffuse;

            const glm::vec3 h = glm::normalize(v + l);

            f32 pdfSpecular = 0.0f;
            if (material.Model == PBRModel::ClosureV2)
            {
                pdfSpecular = PdfGGXVNDF(n, v, h, ClosureV2Roughness(material.Roughness));
            }
            else
            {
                pdfSpecular = PdfGGX(n, v, h, SamplingRoughness(material.Roughness));
            }

            return pSpecular * pdfSpecular + (1.0f - pSpecular) * pdfDiffuse;
        }

        [[nodiscard]] PdfFn MutantBsdfPdf(const MaterialCase& m, PBRModel model)
        {
            const PathTracing::ReferenceMaterial material = Engine::ToReferenceMaterial(m, model);
            return [material](const glm::dvec3& v, const glm::dvec3& l)
            { return static_cast<f64>(MutantBsdfPdf_FixedLobeProbability(material, kNormal, ToF32(v), ToF32(l))); };
        }

        // ---- M10: PathTracing::ImportanceSampleGGX before the #1347 fix ------
        // engine: const f32 sinTheta = std::sqrt(std::max(0.0f, a * a * xi.y / denom));
        // mutant: const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        // Not an invented mutant: this was the shipped line until #1347 (see
        // BsdfSamplingDistributionTest's FINDING).
        [[nodiscard]] glm::vec3 MutantImportanceSampleGGX_Pre1347(const glm::vec2& xi, const glm::vec3& n,
                                                                  f32 roughness) noexcept
        {
            using namespace PathTracing;
            const f32 a = roughness * roughness;

            const f32 phi = 2.0f * kPi * xi.x;
            const f32 denom = 1.0f + (a * a - 1.0f) * xi.y;
            const f32 cosTheta = std::sqrt(std::max(0.0f, (1.0f - xi.y) / denom));
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

            const glm::vec3 h(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

            glm::vec3 tangent;
            glm::vec3 bitangent;
            OrthonormalBasis(n, tangent, bitangent);
            return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
        }

        // Engine::GgxReflectionSampler with the mutant substituted.
        [[nodiscard]] SamplerFn MutantGgxReflectionSampler(f64 roughness)
        {
            return [roughness](const glm::dvec3& v, IidStream& stream) -> std::optional<glm::dvec3>
            {
                const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
                const glm::vec3 vf = ToF32(v);
                const glm::vec3 h = MutantImportanceSampleGGX_Pre1347(xi, kNormal, static_cast<f32>(roughness));
                if (glm::dot(vf, h) <= 0.0f)
                    return std::nullopt;
                const glm::vec3 l = glm::reflect(-vf, h);
                if (l.z <= 0.0f)
                    return std::nullopt;
                return ToF64(l);
            };
        }

        // ---- shared parameters ----------------------------------------------

        // CheckPdfNormalisation's floor. The engine densities are f32 and the
        // quadrature puts a kink (the horizon) inside cells; 2e-3 is the
        // stated floor beside the rule's own 4x error estimate.
        constexpr f64 kPdfMassFloor = 2.0e-3;

        // CheckReconnectionJacobian's bound: f32 arithmetic on O(1) geometry
        // (relative 1e-6) plus the patch discretisation O(h^2) = 1e-6, with
        // two decades of headroom.
        constexpr f64 kJacobianRelTol = 1.0e-4;
    } // namespace

    // =========================================================================
    // M1 — VNDF pdf without G1(v)
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M1_VndfPdfWithoutG1FailsPdfNormalisation)
    {
        // A grazing view, cos(theta_v) = 0.1, at roughness 0.5 (alpha 0.25):
        // G1(v) ~ 0.54 there, so the mutant's density integrates to ~1.8x the
        // accepted mass. At normal incidence G1 = 1 and the mutant is
        // invisible — the grazing view is what this check needs.
        constexpr f64 roughness = 0.5;
        const f64 alpha = roughness * roughness;
        const glm::dvec3 v = Direction(0.1, 0.0);
        // The expected mass is 1 - (the D_v mass whose reflection falls below
        // the horizon), BsdfOracleChecks.h VndfBelowHorizonMass: 0.03081 here
        // against 0.03083 converged (its header comment has the measurement;
        // before IntegrateOverNdfOnce's s = sqrt(1 - t) rule it returned
        // 0.0192, and this test used a uniform solid-angle rule instead).
        const Quadrature below = VndfBelowHorizonMass(v, alpha);
        const f64 expectedMass = 1.0 - below.Value;
        std::printf("  [oracle] VNDF below-horizon mass %.6f (error estimate %.2g)\n", below.Value, below.ErrorEstimate);

        const Verdict engine = CheckPdfNormalisation(Engine::VndfPdf(roughness), v, alpha, expectedMass, kPdfMassFloor,
                                                     "engine PdfGGXVNDF");
        const Verdict mutant = CheckPdfNormalisation(MutantVndfPdf(roughness), v, alpha, expectedMass, kPdfMassFloor,
                                                     "mutant PdfGGXVNDF without G1");
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;
    }

    // =========================================================================
    // M2 — GGX reflection pdf without 1/(4 v.h)
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M2_GgxPdfWithoutReflectionJacobianFailsPdfNormalisation)
    {
        // ImportanceSampleGGX draws m from D cos over the whole hemisphere and
        // the adapter rejects v.m <= 0 and below-horizon reflections; the pdf
        // over l must integrate to the D cos mass that survives, integrated
        // here by the oracle over half-vectors.
        constexpr f64 roughness = 0.5;
        const f64 alpha = roughness * roughness;
        const glm::dvec3 v = Direction(0.7, 0.0);
        const Quadrature accepted = IntegrateOverNdf(alpha, 1024, 256,
                                                     [&](const glm::dvec3& m) -> f64
                                                     {
                                                         if (glm::dot(v, m) <= 0.0)
                                                             return 0.0;
                                                         return Reflect(v, m).z > 0.0 ? 1.0 : 0.0;
                                                     });

        const Verdict engine = CheckPdfNormalisation(Engine::GgxReflectionPdf(roughness), v, alpha, accepted.Value,
                                                     kPdfMassFloor, "engine PdfGGX");
        const Verdict mutant = CheckPdfNormalisation(MutantGgxReflectionPdf(roughness), v, alpha, accepted.Value,
                                                     kPdfMassFloor, "mutant PdfGGX without 1/(4 v.h)");
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;
    }

    // =========================================================================
    // M3 — VNDF sampler stretched by roughness instead of alpha
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M3_VndfSamplerWithRoughnessAsAlphaFailsChiSquare)
    {
        // The default SamplingPlan (5 IID runs x 200k draws, 24 x 24
        // half-vector bins + a rejected bin, Bonferroni at family alpha 1e-3)
        // — the same plan the sampling-distribution suite uses. Roughness 0.6:
        // the mutant samples alpha 0.6 where the density says 0.36.
        constexpr f64 roughness = 0.6;
        const f64 alpha = roughness * roughness;
        const glm::dvec3 v = Direction(0.5, 0.0);
        const SamplingPlan plan{};
        const OracleDensity density{ Lobe{ Shape::VisibleNormal, 1.0, alpha } };

        const FamilyVerdict engine = CheckSamplingDistribution(Engine::VndfSampler(roughness), density, v, alpha, plan);
        const FamilyVerdict mutant = CheckSamplingDistribution(MutantVndfSampler(roughness), density, v, alpha, plan);
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;
    }

    // =========================================================================
    // M4 — ClosureV2 NDF on alpha = roughness
    // =========================================================================

    namespace
    {
        // The ClosureV2 evaluation case. The engine's D Vis F agrees with the
        // oracle's D G2 F / (4 mu_v mu_l) to f32 precision; its two table
        // terms — the Kulla-Conty lobe and the coupled Lambert (#1479) — agree
        // only to the 16 x 16 table's resolution, since the oracle feeds them
        // TRUE energies (BsdfIdentityOracleTest test 2 measures that cost). On
        // a METAL the Kulla-Conty lobe is a large share of f at grazing and
        // the resolution shows at the 20 % level. On a dielectric F_ms is
        // ~0.007 and the coupled Lambert's table error is a few percent of a
        // term that is small wherever it is large, so the engine's measured
        // worst is 3.9e-3 relative; 8e-3 is the bound, the same 2x margin as the
        // furnace bound below. Every mutant below
        // changes the D / Vis / F part, which a dielectric shows as plainly
        // as a metal.
        const MaterialCase kV2EvaluationMaterial{ glm::dvec3(0.9, 0.6, 0.3), 0.0, 0.3 };
        constexpr f64 kV2EvaluationRelTol = 8.0e-3;
        constexpr f64 kV2EvaluationAbsTol = 1.0e-6;
    } // namespace

    TEST(BsdfMutationDetectionTest, M4_ClosureV2NdfOnRoughnessFailsEvaluationAgainstModel)
    {
        const BrdfFn model = OracleClosureV2(kV2EvaluationMaterial);
        const Verdict engine = CheckEvaluationAgainstModel(Engine::ClosureV2Brdf(kV2EvaluationMaterial), model,
                                                           kV2EvaluationRelTol, kV2EvaluationAbsTol,
                                                           "engine ClosureV2Evaluate");
        const Verdict mutant = CheckEvaluationAgainstModel(
            MutantClosureV2Brdf(ClosureV2Mutation::NdfAlphaIsRoughness, kV2EvaluationMaterial), model,
            kV2EvaluationRelTol, kV2EvaluationAbsTol, "mutant ClosureV2 D at alpha = r");
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;

        // The switch-based copy with no mutation IS the engine function: if it
        // were not, every mutant above would be a mutant of something else.
        const Verdict copy = CheckEvaluationAgainstModel(
            MutantClosureV2Brdf(ClosureV2Mutation::None, kV2EvaluationMaterial),
            Engine::ClosureV2Brdf(kV2EvaluationMaterial), 0.0, 0.0, "unmutated copy vs engine");
        EXPECT_TRUE(copy.Pass) << copy.Detail;
    }

    // =========================================================================
    // M5 — the #904 double denominator
    // =========================================================================

    namespace
    {
        // The white-furnace bound. The engine's residual is the energy table's
        // bilinear interpolation error, measured at 0.48 % worst on the
        // furnace grid (roughness 0.3-1, cos v 0.02-1; roughness 0.5,
        // cos v 1). 1 % keeps a 2x margin over it and is an order of magnitude
        // inside the energy mutants' departures (M8a: 23 %, M8b: 69 %).
        constexpr f64 kFurnaceFloor = 1.0e-2;
    } // namespace

    TEST(BsdfMutationDetectionTest, M5_ClosureV2DoubleDenominatorFailsModelAndWhiteFurnace)
    {
        const BrdfFn model = OracleClosureV2(kV2EvaluationMaterial);
        const Verdict engine = CheckEvaluationAgainstModel(Engine::ClosureV2Brdf(kV2EvaluationMaterial), model,
                                                           kV2EvaluationRelTol, kV2EvaluationAbsTol,
                                                           "engine ClosureV2Evaluate");
        const Verdict mutant = CheckEvaluationAgainstModel(
            MutantClosureV2Brdf(ClosureV2Mutation::DoubleDenominator, kV2EvaluationMaterial), model,
            kV2EvaluationRelTol, kV2EvaluationAbsTol, "mutant ClosureV2 D Vis F / (4 NdotV NdotL)");
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;

        // The white furnace: the doubled denominator CREATES energy.
        const Verdict engineFurnace = CheckWhiteFurnace(EngineClosureV2Factory(), kFurnaceFloor, "engine furnace");
        const Verdict mutantFurnace = CheckWhiteFurnace(MutantClosureV2Factory(ClosureV2Mutation::DoubleDenominator),
                                                        kFurnaceFloor, "mutant furnace");
        Report("engine furnace", engineFurnace);
        Report("mutant furnace", mutantFurnace);
        EXPECT_TRUE(engineFurnace.Pass) << engineFurnace.Detail;
        EXPECT_FALSE(mutantFurnace.Pass) << "SURVIVED: " << mutantFurnace.Detail;
        // Not just "different from 1": ABOVE 1, energy created.
        const Quadrature grazing = DirectionalAlbedo(
            MutantClosureV2Brdf(ClosureV2Mutation::DoubleDenominator, { glm::dvec3(1.0), 1.0, 0.5 }),
            Direction(0.1, 0.0), ClosureV2Alpha(0.5), 0);
        EXPECT_GT(grazing.Value, 1.0 + kFurnaceFloor) << "the double divide should create energy at cos v 0.1";
    }

    // =========================================================================
    // M6 — Fresnel at NdotV breaks reciprocity (Legacy and ClosureV2)
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M6_FresnelAtNdotVFailsReciprocity)
    {
        // Both shipped closures take F at v.h, which is symmetric in (v, l),
        // and every other factor is symmetric too — so the engine is
        // reciprocal to f32 rounding (in fact bit-exactly: each product and
        // sum only swaps its operands). F at n.v is not symmetric; with a
        // dielectric F0 of 0.04 it varies 20x between a grazing and a normal
        // direction.
        constexpr f64 relTol = 1.0e-5;
        constexpr f64 absTol = 1.0e-7;
        const MaterialCase material{ glm::dvec3(0.8, 0.5, 0.2), 0.0, 0.5 };

        const Verdict legacyEngine = CheckReciprocity(Engine::LegacyBrdf(material), relTol, absTol, "engine Legacy");
        const Verdict legacyMutant =
            CheckReciprocity(MutantLegacyBrdf_FresnelAtNdotV(material), relTol, absTol, "mutant Legacy F(n.v)");
        const Verdict v2Engine = CheckReciprocity(Engine::ClosureV2Brdf(material), relTol, absTol, "engine ClosureV2");
        const Verdict v2Mutant = CheckReciprocity(MutantClosureV2Brdf(ClosureV2Mutation::FresnelAtNdotV, material),
                                                  relTol, absTol, "mutant ClosureV2 F(n.v)");
        Report("engine Legacy", legacyEngine);
        Report("mutant Legacy", legacyMutant);
        Report("engine ClosureV2", v2Engine);
        Report("mutant ClosureV2", v2Mutant);
        EXPECT_TRUE(legacyEngine.Pass) << legacyEngine.Detail;
        EXPECT_TRUE(v2Engine.Pass) << v2Engine.Detail;
        EXPECT_FALSE(legacyMutant.Pass) << "SURVIVED: " << legacyMutant.Detail;
        EXPECT_FALSE(v2Mutant.Pass) << "SURVIVED: " << v2Mutant.Detail;
    }

    // =========================================================================
    // M7 — reconnection Jacobian mutants
    // =========================================================================

    namespace
    {
        void ExpectJacobianMutantDetected(JacobianMutation mutation, const char* label)
        {
            const Verdict engine =
                CheckReconnectionJacobian(Engine::ReconnectionJacobian(), kJacobianRelTol, "engine ReconnectionJacobian");
            const Verdict copy = CheckReconnectionJacobian(MutantJacobian(JacobianMutation::None), kJacobianRelTol,
                                                           "unmutated copy");
            const Verdict mutant = CheckReconnectionJacobian(MutantJacobian(mutation), kJacobianRelTol, label);
            Report("engine", engine);
            Report("mutant", mutant);
            EXPECT_TRUE(engine.Pass) << engine.Detail;
            EXPECT_TRUE(copy.Pass) << copy.Detail;
            EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;
        }
    } // namespace

    TEST(BsdfMutationDetectionTest, M7a_InvertedReconnectionJacobianFailsSolidAngleRatio)
    {
        ExpectJacobianMutantDetected(JacobianMutation::Inverted, "mutant J inverted (source/dest swapped)");
    }

    TEST(BsdfMutationDetectionTest, M7b_ReconnectionJacobianWithoutCosineRatioFailsSolidAngleRatio)
    {
        ExpectJacobianMutantDetected(JacobianMutation::NoCosineRatio, "mutant J without cosine ratio");
    }

    TEST(BsdfMutationDetectionTest, M7c_ReconnectionJacobianWithoutDistanceRatioFailsSolidAngleRatio)
    {
        ExpectJacobianMutantDetected(JacobianMutation::NoDistanceRatio, "mutant J without distance ratio");
    }

    TEST(BsdfMutationDetectionTest, M7d_ReconnectionJacobianAppliedAsOneFailsSolidAngleRatio)
    {
        ExpectJacobianMutantDetected(JacobianMutation::One, "mutant J = 1 (no Jacobian)");
    }

    TEST(BsdfMutationDetectionTest, M7e_ShiftWithoutItsDistantArmIsNotOneInTheDistantLimit)
    {
        // docs/design/restir-gi-reconnection-shift.md §4.3: put the vertex at
        // x0 + t w with its normal opposed to w and let t -> infinity; then
        // J -> 1 exactly. The oracle takes that limit NUMERICALLY, with the
        // same measured solid angles CheckReconnectionJacobian uses (patch
        // scaled with t so its angular size stays 1e-3), and the ratio must
        // approach 1 as O(|dest - source| / t).
        const glm::dvec3 source{ 0.0, 0.0, 0.0 };
        const glm::dvec3 destination{ 0.6, -0.3, 0.2 };
        const glm::dvec3 w = glm::normalize(glm::dvec3(0.3, 0.2, 1.0));
        const f64 offset = glm::length(destination - source);
        for (f64 t : { 1.0e2, 1.0e3, 1.0e4 })
        {
            const f64 ratio = ReconnectionJacobianBySolidAngles(source + t * w, -w, destination, source, 1.0e-3 * t);
            EXPECT_NEAR(ratio, 1.0, 4.0 * offset / t) << "oracle distant limit at t = " << t;
            std::printf("  [oracle] distant limit t = %g: solid-angle ratio %.9f\n", t, ratio);
        }

        // The engine's GI wrapper: an Environment sample stores the unit
        // DIRECTION in Position and no normal. J == 1 exactly, per §4.3.
        ReSTIR::GISample environment;
        environment.Kind = ReSTIR::GISampleKind::Environment;
        environment.Position = ToF32(w);
        environment.Normal = glm::vec3(0.0f);
        const glm::vec3 dst = ToF32(destination);
        const glm::vec3 src = ToF32(source);
        const f32 engineGI = ReSTIR::GIShiftJacobian(environment, dst, src);
        const f32 mutantGI = MutantGIShiftJacobian_NoEnvironmentArm(environment, dst, src);
        std::printf("  [engine] GIShiftJacobian(Environment) = %.9g\n  [mutant] without the arm = %.9g\n",
                    static_cast<f64>(engineGI), static_cast<f64>(mutantGI));
        // "Exactly 1" is the claim (the arm returns the literal 1.0f), so the
        // comparison is on the bits, not within a tolerance.
        EXPECT_EQ(std::bit_cast<u32>(engineGI), std::bit_cast<u32>(1.0f)) << "GIShiftJacobian = " << engineGI;
        EXPECT_GT(std::abs(static_cast<f64>(mutantGI) - 1.0), 1.0e-2)
            << "SURVIVED: the core fed an environment direction returned " << mutantGI;

        // The engine's DI wrapper: a Directional light is the same distant
        // limit (Position stores the direction toward the light); a Punctual
        // light is a delta in a discrete measure — no area, nothing to convert
        // (ReservoirDI.h header). Both return exactly 1.
        for (ReSTIR::LightSampleKind kind : { ReSTIR::LightSampleKind::Directional, ReSTIR::LightSampleKind::Punctual })
        {
            ReSTIR::LightSample light;
            light.Kind = kind;
            light.Position = kind == ReSTIR::LightSampleKind::Directional ? ToF32(w) : glm::vec3(0.5f, 2.0f, 1.0f);
            light.Normal = glm::vec3(0.0f);
            const f32 engineDI = ReSTIR::ShiftJacobian(light, dst, src);
            const f32 mutantDI = MutantShiftJacobian_NoDeltaArm(light, dst, src);
            std::printf("  [engine] ShiftJacobian(%s) = %.9g, [mutant] without the arm = %.9g\n",
                        std::string(ReSTIR::ToString(kind)).c_str(), static_cast<f64>(engineDI),
                        static_cast<f64>(mutantDI));
            EXPECT_EQ(std::bit_cast<u32>(engineDI), std::bit_cast<u32>(1.0f))
                << ReSTIR::ToString(kind) << ": ShiftJacobian = " << engineDI;
            EXPECT_GT(std::abs(static_cast<f64>(mutantDI) - 1.0), 1.0e-2)
                << "SURVIVED: " << ReSTIR::ToString(kind) << " through the core returned " << mutantDI;
        }
    }

    // =========================================================================
    // M8 — Kulla-Conty compensation on the wrong convention, or omitted
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M8_KullaContyAtWrongAlphaOrOmittedFailsWhiteFurnace)
    {
        const Verdict engine = CheckWhiteFurnace(EngineClosureV2Factory(), kFurnaceFloor, "engine furnace");
        const Verdict atAlpha = CheckWhiteFurnace(MutantClosureV2Factory(ClosureV2Mutation::EnergyLookupAtAlpha),
                                                  kFurnaceFloor, "mutant energy lookup at alpha = r^2");
        const Verdict omitted = CheckWhiteFurnace(MutantClosureV2Factory(ClosureV2Mutation::NoEnergyCompensation),
                                                  kFurnaceFloor, "mutant without Kulla-Conty");
        Report("engine", engine);
        Report("mutant at alpha", atAlpha);
        Report("mutant omitted", omitted);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(atAlpha.Pass) << "SURVIVED: " << atAlpha.Detail;
        EXPECT_FALSE(omitted.Pass) << "SURVIVED: " << omitted.Detail;
    }

    // =========================================================================
    // M9 — a mixture density that does not describe its sampler
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M9_MixturePdfWithFixedLobeProbabilityFailsChiSquare)
    {
        // A dielectric with albedo 0.8: the engine's lobe policy picks the
        // specular lobe with probability 0.1 (the floor of its clamp); the
        // mutant Pdf claims 0.5. The SAMPLER is the engine's in both arms —
        // only the density the check is told to expect differs.
        //
        // The check takes an OracleDensity, so each arm's density is the
        // mixture its Pdf CLAIMS — a VisibleNormal lobe at the v2 alpha with
        // the Pdf's pSpecular, Lambert with the rest — and that claim is first
        // pinned pointwise to the Pdf function itself, so the chi-square below
        // tests the function, not a restatement of it.
        const MaterialCase material{ glm::dvec3(0.8), 0.0, 0.5 };
        const glm::dvec3 v = Direction(0.5, 0.0);
        const f64 alpha = ClosureV2Alpha(material.Roughness);
        const SamplingPlan plan{};
        const f64 pS = Engine::SpecularProbability(material, PBRModel::ClosureV2);
        std::printf("  engine specular-lobe probability %.4f, mutant claims 0.5\n", pS);
        const OracleDensity engineClaim{ Lobe{ Shape::VisibleNormal, pS, alpha }, Lobe{ Shape::Cosine, 1.0 - pS, 1.0 } };
        const OracleDensity mutantClaim{ Lobe{ Shape::VisibleNormal, 0.5, alpha }, Lobe{ Shape::Cosine, 0.5, 1.0 } };

        // f32 densities against f64 mixtures on a rough lobe: 1e-4 relative.
        const auto claimMatches = [](const PdfFn& pdf, const OracleDensity& claim, const char* who)
        {
            f64 worst = 0.0;
            for (const auto& [dv, dl] : DirectionPairs())
            {
                const f64 got = pdf(dv, dl);
                const f64 expected = DensityPdf(claim, dv, dl);
                worst = std::max(worst, std::abs(got - expected) / std::max(expected, 1.0e-6));
            }
            std::printf("  [%s] Pdf vs its claimed mixture: worst relative difference %.3g\n", who, worst);
            return worst <= 1.0e-4;
        };
        ASSERT_TRUE(claimMatches(Engine::BsdfPdf(material, PBRModel::ClosureV2), engineClaim, "engine"));
        ASSERT_TRUE(claimMatches(MutantBsdfPdf(material, PBRModel::ClosureV2), mutantClaim, "mutant"));

        const SamplerFn sampler = Engine::BsdfSampler(material, PBRModel::ClosureV2);
        const FamilyVerdict engine = CheckSamplingDistribution(sampler, engineClaim, v, alpha, plan);
        const FamilyVerdict mutant = CheckSamplingDistribution(sampler, mutantClaim, v, alpha, plan);
        Report("engine", engine);
        Report("mutant", mutant);
        EXPECT_TRUE(engine.Pass) << engine.Detail;
        EXPECT_FALSE(mutant.Pass) << "SURVIVED: " << mutant.Detail;
    }

    // =========================================================================
    // M10 — the pre-#1347 ImportanceSampleGGX: sin = sqrt(1 - cos^2) in f32
    // =========================================================================
    TEST(BsdfMutationDetectionTest, M10_Pre1347ImportanceSampleGgxFailsChiSquare)
    {
        // A real bug, found by BsdfSamplingDistributionTest and fixed in
        // ReferenceBRDF.h by #1347. The mutant puts 2^-25 / (alpha^2 + 2^-25)
        // of its half-vectors exactly on the normal — 1.2 % at the Legacy
        // sampling floor 0.04, 0.47 % at 0.05 — all into the first t row of
        // the half-vector grid, where a bin expects 1/576 of the draws. Same
        // plan, same oracle density (an NdfReflection lobe at alpha = r^2),
        // same seeds for both arms.
        const glm::dvec3 v = Direction(0.5, 0.0);
        const SamplingPlan plan{};
        for (f64 roughness : { 0.04, 0.05 })
        {
            const f64 alpha = roughness * roughness;
            const OracleDensity density{ Lobe{ Shape::NdfReflection, 1.0, alpha } };
            const FamilyVerdict engine =
                CheckSamplingDistribution(Engine::GgxReflectionSampler(roughness), density, v, alpha, plan);
            const FamilyVerdict mutant =
                CheckSamplingDistribution(MutantGgxReflectionSampler(roughness), density, v, alpha, plan);
            std::printf("  roughness %.2f\n", roughness);
            Report("engine", engine);
            Report("mutant", mutant);
            EXPECT_TRUE(engine.Pass) << "roughness " << roughness << ": " << engine.Detail;
            EXPECT_FALSE(mutant.Pass) << "SURVIVED at roughness " << roughness << ": " << mutant.Detail;
        }
    }
} // namespace OloEngine::Tests::Oracle
