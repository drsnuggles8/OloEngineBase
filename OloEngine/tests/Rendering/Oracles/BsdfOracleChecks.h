#pragma once

// =============================================================================
// BsdfOracleChecks.h — the oracle checks, parameterised on the function under
// test (issue #1347, acceptance criteria 1, 2 and 4).
// =============================================================================
//
// Each check takes the implementation as a callable and returns a verdict
// instead of asserting. That is what lets one check serve both halves of the
// argument:
//
//   * BsdfIdentityOracleTest / BsdfSamplingDistributionTest pass the ENGINE's
//     function and expect a pass;
//   * BsdfMutationDetectionTest passes a test-local MUTANT of it (a missing
//     Jacobian, a wrong alpha convention, a doubled denominator) and expects a
//     FAIL from the very same check at the very same sample size.
//
// A check that no mutant can fail is not evidence, and a mutant that survives
// every check is a finding (the negative-control rule).
//
// Like the oracle, this header includes nothing from the renderer: callers
// adapt the engine's f32/glm::vec3 signatures to the f64 ones below in a
// lambda at the call site, which is also where a test states which engine
// function it is checking.
// =============================================================================

#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests::Oracle
{
    struct Verdict
    {
        bool Pass = true;
        // The REPORTED case: the worst FAILING entry once anything has failed,
        // otherwise the worst entry overall. A failure report must point at a
        // failure, so once Pass is false a passing entry never replaces it,
        // however large its measure (a passing case can carry a larger
        // relative error under a looser absolute floor).
        f64 Worst = 0.0;         // the reported case's measured error, in the check's own unit
        f64 Bound = 0.0;         // the bound it was held to
        std::string Detail;      // where the reported case was, and why that bound
        f64 WorstMeasured = 0.0; // the largest measure recorded, passing or failing

        void Record(bool pass, f64 measured, f64 bound, const std::string& where)
        {
            WorstMeasured = std::max(WorstMeasured, measured);
            // The first failure always takes the report; after that only a
            // worse failure does. While everything passes, the worst pass does.
            const bool replace = pass ? (Pass && measured > Worst) : (Pass || measured > Worst);
            if (replace)
            {
                Worst = measured;
                Bound = bound;
                Detail = where;
            }
            Pass = Pass && pass;
        }
    };

    // A shading configuration of the grid AC 1 names: roughness x metallic x
    // view angle (grazing included) x radiance scale (HDR).
    struct MaterialCase
    {
        glm::dvec3 Albedo{ 0.8 };
        f64 Metallic = 0.0;
        f64 Roughness = 0.5;
    };

    [[nodiscard]] inline std::string Describe(const MaterialCase& m)
    {
        std::ostringstream s;
        s << "albedo (" << m.Albedo.x << ", " << m.Albedo.y << ", " << m.Albedo.z << "), metallic " << m.Metallic
          << ", roughness " << m.Roughness;
        return s.str();
    }

    [[nodiscard]] inline std::string Describe(const glm::dvec3& d)
    {
        std::ostringstream s;
        s << "(" << d.x << ", " << d.y << ", " << d.z << ")";
        return s.str();
    }

    // The representative grid. Roughness spans near-mirror to fully rough,
    // metallic spans dielectric to metal, and the cosines include grazing
    // (0.02) — where denominators, clamps and Jacobians go wrong first.
    inline const std::vector<f64> kRoughnessGrid{ 0.05, 0.15, 0.3, 0.5, 0.75, 1.0 };
    inline const std::vector<f64> kMetallicGrid{ 0.0, 0.5, 1.0 };
    inline const std::vector<f64> kCosineGrid{ 0.02, 0.1, 0.35, 0.7, 1.0 };
    inline const std::vector<f64> kPhiGrid{ 0.0, 0.9, 2.3, 3.14159265358979 };

    // Pairs of directions over the cosine and azimuth grids, both above the
    // horizon.
    [[nodiscard]] inline std::vector<std::pair<glm::dvec3, glm::dvec3>> DirectionPairs()
    {
        std::vector<std::pair<glm::dvec3, glm::dvec3>> pairs;
        for (f64 cv : kCosineGrid)
            for (f64 cl : kCosineGrid)
                for (f64 phi : kPhiGrid)
                    pairs.emplace_back(Direction(cv, 0.0), Direction(cl, phi));
        return pairs;
    }

    using BrdfFn = std::function<glm::dvec3(const glm::dvec3& v, const glm::dvec3& l)>;
    using PdfFn = std::function<f64(const glm::dvec3& v, const glm::dvec3& l)>;
    // Draws one direction for view v, taking as many IID uniforms from the
    // stream as it needs (a lobe mixture takes three); nullopt means the
    // sampler rejected the draw (terminated the path).
    using SamplerFn = std::function<std::optional<glm::dvec3>(const glm::dvec3& v, IidStream& stream)>;

    // ---- evaluation against the model --------------------------------------

    // |f_impl - f_model| <= relTol * |f_model| + absTol, per channel, at every
    // direction pair. The relative part covers f32 arithmetic; the absolute
    // part covers values that are legitimately ~0.
    [[nodiscard]] inline Verdict CheckEvaluationAgainstModel(const BrdfFn& impl, const BrdfFn& model, f64 relTol,
                                                             f64 absTol, const std::string& label)
    {
        Verdict verdict;
        for (const auto& [v, l] : DirectionPairs())
        {
            const glm::dvec3 a = impl(v, l);
            const glm::dvec3 b = model(v, l);
            for (int c = 0; c < 3; ++c)
            {
                const f64 err = std::abs(a[c] - b[c]);
                const f64 bound = relTol * std::abs(b[c]) + absTol;
                const bool pass = std::isfinite(a[c]) && err <= bound;
                const f64 rel = err / std::max(std::abs(b[c]), 1.0e-12);
                verdict.Record(pass, rel, relTol,
                               label + ": v " + Describe(v) + ", l " + Describe(l) + ", channel " + std::to_string(c) +
                                   ": impl " + std::to_string(a[c]) + ", model " + std::to_string(b[c]));
            }
        }
        return verdict;
    }

    // ---- reciprocity --------------------------------------------------------

    // f(v, l) == f(l, v) (Helmholtz), relative per channel.
    [[nodiscard]] inline Verdict CheckReciprocity(const BrdfFn& impl, f64 relTol, f64 absTol, const std::string& label)
    {
        Verdict verdict;
        for (const auto& [v, l] : DirectionPairs())
        {
            const glm::dvec3 a = impl(v, l);
            const glm::dvec3 b = impl(l, v);
            for (int c = 0; c < 3; ++c)
            {
                const f64 err = std::abs(a[c] - b[c]);
                const f64 scale = std::max(std::abs(a[c]), std::abs(b[c]));
                const bool pass = std::isfinite(a[c]) && std::isfinite(b[c]) && err <= relTol * scale + absTol;
                verdict.Record(pass, err / std::max(scale, 1.0e-12), relTol,
                               label + ": f(v " + Describe(v) + ", l " + Describe(l) + ")[" + std::to_string(c) +
                                   "] = " + std::to_string(a[c]) + " but f(l, v) = " + std::to_string(b[c]));
            }
        }
        return verdict;
    }

    // ---- directional albedo / furnace --------------------------------------

    // int f(v, l) (n.l) dw_l for one channel, with the half-vector quadrature
    // (nodes placed for `nodeAlpha`) plus a uniform-hemisphere pass for any
    // diffuse part; the two are ADDED only when the caller splits the BRDF.
    // For a mixed lobe the half-vector rule alone is exact in the limit; the
    // error estimate says how close it is.
    [[nodiscard]] inline Quadrature DirectionalAlbedo(const BrdfFn& impl, const glm::dvec3& v, f64 nodeAlpha, int channel,
                                                      u32 nT = 512, u32 nPhi = 256)
    {
        return IntegrateOverOutgoingHemisphere(v, nodeAlpha, nT, nPhi,
                                               [&](const glm::dvec3& l)
                                               { return impl(v, l)[channel] * l.z; });
    }

    // ---- pdf normalisation --------------------------------------------------

    // int pdf(v, l) dw_l over the upper hemisphere must equal the fraction of
    // draws the sampler does NOT reject, `expectedMass` (1 for a sampler that
    // never rejects; 1 - the oracle's below-horizon mass for VNDF).
    [[nodiscard]] inline Verdict CheckPdfNormalisation(const PdfFn& pdf, const glm::dvec3& v, f64 nodeAlpha,
                                                       f64 expectedMass, f64 floorTol, const std::string& label)
    {
        Verdict verdict;
        const Quadrature q =
            IntegrateOverOutgoingHemisphere(v, nodeAlpha, 1024, 256, [&](const glm::dvec3& l)
                                            { return pdf(v, l); });
        const f64 err = std::abs(q.Value - expectedMass);
        const f64 bound = std::max(floorTol, 4.0 * q.ErrorEstimate);
        verdict.Record(err <= bound, err, bound,
                       label + ": v " + Describe(v) + ": integral " + std::to_string(q.Value) + " vs expected mass " +
                           std::to_string(expectedMass) + " (quadrature error estimate " +
                           std::to_string(q.ErrorEstimate) + ")");
        return verdict;
    }

    // The fraction of VNDF draws whose reflected direction falls below the
    // horizon — the mass a VNDF sampler legitimately rejects. Oracle-only:
    // integrates D_v over the visible normals whose reflection of v has l.z <= 0.
    //
    // The integrand D_v / (D cos m) carries 1 / cos(m) at the horizon;
    // IntegrateOverNdfOnce's midpoint rule in s = sqrt(1 - t) cancels it (with
    // a midpoint rule in t this returned 0.0192 at cos v 0.1, alpha 0.25,
    // against a converged 0.0308). Measured against the same rule at 16384 x
    // 4096 over cos v {0.02, 0.05, 0.1, 0.2} x alpha {0.0016 .. 1}: absolute
    // error <= 1.5e-4 (0.030809 vs 0.030834 at cos v 0.1, alpha 0.25). The
    // reflection cut is a step inside cells, so ErrorEstimate is an estimate
    // there, not a bound: callers keep a stated floor beside it (2e-3).
    [[nodiscard]] inline Quadrature VndfBelowHorizonMass(const glm::dvec3& v, f64 alpha)
    {
        return IntegrateOverNdf(alpha, 1024, 256,
                                [&](const glm::dvec3& m) -> f64
                                {
                                    const f64 dv = GgxVisibleNormalDensity(v, m, alpha);
                                    if (dv <= 0.0)
                                        return 0.0;
                                    if (Reflect(v, m).z > 0.0)
                                        return 0.0;
                                    return dv / (GgxD(m.z, alpha) * m.z);
                                });
    }

    // ---- sampling distribution (chi-square over half-vector bins) ----------

    // An oracle density over l, as a list of weighted lobes. Every shape is
    // written from the oracle's functions only:
    //   NdfReflection:  m ~ D(m) cos(m) [Walter07], l = reflect(v, m);
    //                   p(l) = D(m) cos(m) / (4 v.m). Draws with v.m <= 0
    //                   or l below the horizon are rejected.
    //   VisibleNormal:  m ~ D_v(m) [Heitz18 eq. 1], l = reflect(v, m);
    //                   p(l) = D_v(m) / (4 v.m). Below-horizon l rejected.
    //   Cosine:         p(l) = cos(theta_l) / pi.
    // The weights of a mixture sum to 1; `Alpha` is ignored by Cosine.
    enum class Shape : u8
    {
        NdfReflection,
        VisibleNormal,
        Cosine,
    };

    struct Lobe
    {
        Shape LobeShape = Shape::Cosine;
        f64 Weight = 1.0;
        f64 Alpha = 1.0;
    };
    using OracleDensity = std::vector<Lobe>;

    // Pointwise solid-angle density of one lobe over l (0 below the horizon).
    [[nodiscard]] inline f64 LobePdf(const Lobe& lobe, const glm::dvec3& v, const glm::dvec3& l)
    {
        if (l.z <= 0.0)
            return 0.0;
        if (lobe.LobeShape == Shape::Cosine)
            return l.z / kPi;
        if (v.z <= 0.0)
            return 0.0;
        const glm::dvec3 m = HalfVector(v, l);
        if (glm::dot(v, m) <= 0.0)
            return 0.0;
        const f64 halfVectorDensity = lobe.LobeShape == Shape::VisibleNormal ? GgxVisibleNormalDensity(v, m, lobe.Alpha)
                                                                             : GgxD(m.z, lobe.Alpha) * m.z;
        return halfVectorDensity * ReflectionJacobian(v, m);
    }

    [[nodiscard]] inline f64 DensityPdf(const OracleDensity& density, const glm::dvec3& v, const glm::dvec3& l)
    {
        f64 sum = 0.0;
        for (const Lobe& lobe : density)
            sum += lobe.Weight * LobePdf(lobe, v, l);
        return sum;
    }

    // The density as a PdfFn, for the pointwise checks (CheckPdfNormalisation,
    // a test comparing an engine Pdf with it direction by direction).
    [[nodiscard]] inline PdfFn AsPdfFn(const OracleDensity& density)
    {
        return [density](const glm::dvec3& v, const glm::dvec3& l)
        { return DensityPdf(density, v, l); };
    }

    struct SamplingPlan
    {
        IndependentRuns Runs{};
        u32 SamplesPerRun = 200000;
        u32 BinsT = 24;   // equal-mass cells of the binning lobe's theta CDF
        u32 BinsPhi = 24; // azimuth
        // The expected-mass deposit (DepositBinMasses): a DepositBaseCells^2
        // grid in each lobe's own inverse-CDF coordinates, cells that straddle
        // a bin edge or the horizon split 2 x 2 down to DepositMaxDepth levels.
        // DepositBaseCells must be a multiple of BinsT and BinsPhi (checked).
        u32 DepositBaseCells = 240;
        u32 DepositMaxDepth = 5;
    };

    // The bin of an accepted direction l: the half-vector of (v, l) on a
    // BinsT x BinsPhi grid in (t, phi), t = GgxThetaCdf(m.z, binAlpha). Index
    // BinsT * BinsPhi is the rejected bin. Observation and expectation both
    // bin through this one function.
    [[nodiscard]] inline u32 SamplingBinOf(const glm::dvec3& v, const glm::dvec3& l, f64 binAlpha,
                                           const SamplingPlan& plan)
    {
        const glm::dvec3 m = HalfVector(v, l);
        const f64 t = GgxThetaCdf(m.z, binAlpha);
        f64 phi = std::atan2(m.y, m.x);
        if (phi < 0.0)
            phi += 2.0 * kPi;
        const u32 it = std::min(static_cast<u32>(t * plan.BinsT), plan.BinsT - 1u);
        const u32 ip = std::min(static_cast<u32>(phi / (2.0 * kPi) * plan.BinsPhi), plan.BinsPhi - 1u);
        return it * plan.BinsPhi + ip;
    }

    namespace DepositImpl
    {
        struct Deposit
        {
            u32 Bin = 0;
            bool HasMass = false;
            f64 Ratio = 0.0; // the lobe's density over its own (a, b) coordinates
        };

        // Where the point (a, b) of a lobe's own inverse-CDF coordinates goes.
        // For a GGX lobe a = GgxThetaCdf of the half-vector (the equal-mass
        // variable of D cos) and b = phi / 2pi; for Lambert a = sin^2 theta_l.
        [[nodiscard]] inline Deposit Locate(const Lobe& lobe, const glm::dvec3& v, f64 a, f64 b, f64 binAlpha,
                                            const SamplingPlan& plan)
        {
            const u32 rejected = plan.BinsT * plan.BinsPhi;
            const f64 phi = 2.0 * kPi * b;
            if (lobe.LobeShape == Shape::Cosine)
            {
                const glm::dvec3 l = Direction(std::sqrt(std::max(0.0, 1.0 - a)), phi);
                return { SamplingBinOf(v, l, binAlpha, plan), true, 1.0 };
            }
            const glm::dvec3 m = Direction(GgxCosThetaFromCdf(a, lobe.Alpha), phi);
            if (!(m.z > 0.0))
                return {};
            f64 ratio = 1.0;
            if (lobe.LobeShape == Shape::VisibleNormal)
            {
                // D_v(m) over the D cos measure: G1(v) (v.m)+ / (v.z m.z).
                ratio = GgxVisibleNormalDensity(v, m, lobe.Alpha) / (GgxD(m.z, lobe.Alpha) * m.z);
                if (!(ratio > 0.0))
                    return {};
            }
            else if (glm::dot(v, m) <= 0.0)
            {
                return { rejected, true, ratio };
            }
            const glm::dvec3 l = Reflect(v, m);
            if (l.z <= 0.0)
                return { rejected, true, ratio };
            return { SamplingBinOf(v, l, binAlpha, plan), true, ratio };
        }

        [[nodiscard]] inline bool SameBin(const Deposit& x, const Deposit& y)
        {
            return x.HasMass == y.HasMass && (!x.HasMass || x.Bin == y.Bin);
        }

        inline void DepositCell(const Lobe& lobe, const glm::dvec3& v, f64 binAlpha, const SamplingPlan& plan, f64 a0,
                                f64 a1, f64 b0, f64 b1, u32 depth, std::vector<f64>& masses)
        {
            const f64 am = 0.5 * (a0 + a1);
            const f64 bm = 0.5 * (b0 + b1);
            const Deposit centre = Locate(lobe, v, am, bm, binAlpha, plan);
            // Corners probed a hair inside the cell, so an edge that coincides
            // with a bin edge is not mistaken for one that crosses the cell.
            const f64 ia = 1.0e-7 * (a1 - a0);
            const f64 ib = 1.0e-7 * (b1 - b0);
            bool uniform = true;
            for (const auto& [a, b] : { std::pair{ a0 + ia, b0 + ib }, std::pair{ a1 - ia, b0 + ib },
                                        std::pair{ a0 + ia, b1 - ib }, std::pair{ a1 - ia, b1 - ib } })
                uniform = uniform && SameBin(Locate(lobe, v, a, b, binAlpha, plan), centre);
            if (uniform || depth >= plan.DepositMaxDepth)
            {
                if (centre.HasMass)
                    masses[centre.Bin] += lobe.Weight * centre.Ratio * (a1 - a0) * (b1 - b0);
                return;
            }
            DepositCell(lobe, v, binAlpha, plan, a0, am, b0, bm, depth + 1u, masses);
            DepositCell(lobe, v, binAlpha, plan, am, a1, b0, bm, depth + 1u, masses);
            DepositCell(lobe, v, binAlpha, plan, a0, am, bm, b1, depth + 1u, masses);
            DepositCell(lobe, v, binAlpha, plan, am, a1, bm, b1, depth + 1u, masses);
        }
    } // namespace DepositImpl

    // The probability of each bin (index SamplingBinOf) and of rejection
    // (the last entry) under the oracle density, NOT renormalised.
    //
    // THE DEPOSIT. Each lobe is integrated in ITS OWN inverse-CDF coordinates
    // (GgxThetaCdf of the half-vector and phi / 2pi for a GGX lobe, sin^2
    // theta_l and phi / 2pi for Lambert), where every base cell of the
    // DepositBaseCells^2 grid carries exactly 1 / DepositBaseCells^2 of the
    // lobe's D cos (or Lambert) mass. A cell's mass goes to the bin its centre
    // maps to, weighted by the smooth density ratio (D_v / (D cos) for the
    // VNDF lobe, 1 otherwise). A cell whose corners map to different bins (a
    // bin edge or the horizon runs through it) is split 2 x 2, down to
    // DepositMaxDepth levels. Because DepositBaseCells is a multiple of the
    // bin counts, a GGX lobe whose alpha IS the bin alpha has cells aligned
    // with its bins, and only the horizon and foreign lobes refine.
    //
    // The rejected probability is the complement of the mass above the
    // horizon, because every lobe has unit mass analytically: D cos by
    // [Heitz14 eq. 2], D_v by the definition of G1 [Heitz14 eq. 22] (both
    // pinned numerically by OracleSelfConsistencyTest). Depositing it directly
    // would integrate the VNDF ratio G1 (v.m) / (v.z m.z), which is singular as
    // m.z -> 0, in a region that only ever reflects below the horizon.
    //
    // Returns an empty vector if the plan's DepositBaseCells is not a
    // multiple of BinsT and BinsPhi.
    [[nodiscard]] inline std::vector<f64> DepositBinMasses(const OracleDensity& density, const glm::dvec3& v,
                                                           f64 binAlpha, const SamplingPlan& plan = {})
    {
        if (plan.BinsT == 0u || plan.BinsPhi == 0u || plan.DepositBaseCells == 0u ||
            plan.DepositBaseCells % plan.BinsT != 0u || plan.DepositBaseCells % plan.BinsPhi != 0u)
            return {};
        const u32 bins = plan.BinsT * plan.BinsPhi;
        std::vector<f64> masses(bins + 1u, 0.0);
        const u32 n = plan.DepositBaseCells;
        const f64 h = 1.0 / static_cast<f64>(n);
        for (const Lobe& lobe : density)
            for (u32 i = 0; i < n; ++i)
                for (u32 j = 0; j < n; ++j)
                    DepositImpl::DepositCell(lobe, v, binAlpha, plan, i * h, (i + 1) * h, j * h, (j + 1) * h, 0u, masses);
        f64 covered = 0.0;
        for (u32 i = 0; i < bins; ++i)
            covered += masses[i];
        masses[bins] = std::max(0.0, 1.0 - covered);
        return masses;
    }

    // Chi-square test of a sampler's DIRECTIONS against an independently
    // derived oracle density. Draws are binned by SamplingBinOf: the
    // half-vector of (v, l) on a (t, phi) grid, t = GgxThetaCdf(m.z,
    // binAlpha), which gives equal-mass cells for a GGX lobe of that alpha and
    // still tiles the whole upper hemisphere of l for any other density. One
    // extra bin holds the rejected draws (nullopt or l.z <= 0). Adjacent bins
    // are pooled, row-major, to expected counts >= 5 (ChiSquareTest).
    //
    // The expected masses come from DepositBinMasses, renormalised to sum to
    // 1 (the sum before is reported in Detail with the mass above the
    // horizon). WHY THE DEPOSIT, NOT A PER-BIN QUADRATURE: this check used to
    // integrate each bin with a 6 x 6 midpoint rule over (t, phi) of the
    // BINNING lobe. That rule cannot resolve the horizon cut of a grazing view
    // (a long discontinuity through many bins), nor a diffuse lobe binned for
    // a sharp specular one (the diffuse mass then sits at 1 - t < 1e-4,
    // between its nodes). Stated as the chi-square non-centrality
    // N sum e_i^2 / p_i it adds for a CORRECT sampler at N = 200k, it measured
    // 18 to 270 on single lobes at grazing views and ~5e8 on sharp mixtures —
    // enough to fail a correct sampler — against <= 1.1 for the deposit
    // (BsdfSamplingDistributionTest.ExpectedBinMassQuadratureErrorIsNegligible
    // keeps the deposit's own figure pinned).
    [[nodiscard]] inline FamilyVerdict CheckSamplingDistribution(const SamplerFn& sampler, const OracleDensity& density,
                                                                 const glm::dvec3& v, f64 binAlpha,
                                                                 const SamplingPlan& plan = {})
    {
        std::vector<f64> expected = DepositBinMasses(density, v, binAlpha, plan);
        if (expected.empty())
        {
            FamilyVerdict refused;
            refused.Pass = false;
            refused.Detail = "INVALID plan: DepositBaseCells " + std::to_string(plan.DepositBaseCells) +
                             " is not a multiple of BinsT " + std::to_string(plan.BinsT) + " and BinsPhi " +
                             std::to_string(plan.BinsPhi) + "\n";
            return refused;
        }
        const u32 rejected = plan.BinsT * plan.BinsPhi;
        f64 total = 0.0;
        for (f64 e : expected)
            total += e;
        // total > 1 only if the quadrature over-fills the upper hemisphere.
        for (f64& e : expected)
            e /= total;

        FamilyVerdict verdict = RunFamily(plan.Runs,
                                          [&](u64 seed) -> GoodnessOfFit
                                          {
                                              IidStream stream(seed);
                                              std::vector<u64> observed(rejected + 1u, 0u);
                                              for (u32 i = 0; i < plan.SamplesPerRun; ++i)
                                              {
                                                  const std::optional<glm::dvec3> l = sampler(v, stream);
                                                  if (!l || l->z <= 0.0)
                                                      ++observed[rejected];
                                                  else
                                                      ++observed[SamplingBinOf(v, *l, binAlpha, plan)];
                                              }
                                              return ChiSquareTest(observed, expected, IidStream::Provenance());
                                          });
        verdict.Detail += "oracle mass above the horizon " + std::to_string(1.0 - expected[rejected]) +
                          ", sum before renormalisation " + std::to_string(total) + "\n";
        return verdict;
    }

    // ---- reconnection Jacobian ---------------------------------------------

    struct ShiftConfiguration
    {
        glm::dvec3 Vertex;
        glm::dvec3 VertexNormal;
        glm::dvec3 Destination;
        glm::dvec3 Source;
    };

    // Representative reconnection geometries: near/far, oblique, and the
    // grazing-at-the-vertex case where a wrong cosine ratio is largest.
    [[nodiscard]] inline std::vector<ShiftConfiguration> ShiftConfigurations()
    {
        return {
            { { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.3, 0.1, 2.0 }, { -0.2, 0.4, 3.0 } },
            { { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 1.5, 0.0, 0.8 }, { -0.5, 0.2, 2.5 } },
            { { 1.0, 2.0, 0.5 }, glm::normalize(glm::dvec3(0.3, -0.2, 1.0)), { 1.2, 2.5, 4.0 }, { 3.0, 1.0, 1.0 } },
            { { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.1, 0.3, 5.0 }, { 0.0, 2.0, 0.5 } },
            { { -2.0, 0.0, 1.0 }, glm::normalize(glm::dvec3(1.0, 1.0, 0.0)), { 0.0, 1.0, 1.0 }, { -1.0, 3.0, -1.0 } },
        };
    }

    using JacobianFn = std::function<f64(const glm::dvec3& vertex, const glm::dvec3& vertexNormal,
                                         const glm::dvec3& destination, const glm::dvec3& source)>;

    // The implementation's Jacobian against the ratio of two exactly measured
    // solid angles. The patch discretisation error is O(h^2) = O(1e-6) here, so
    // the relative bound is f32 precision, not the geometry.
    [[nodiscard]] inline Verdict CheckReconnectionJacobian(const JacobianFn& impl, f64 relTol, const std::string& label)
    {
        Verdict verdict;
        for (const ShiftConfiguration& c : ShiftConfigurations())
        {
            const f64 expected = ReconnectionJacobianBySolidAngles(c.Vertex, c.VertexNormal, c.Destination, c.Source);
            const f64 got = impl(c.Vertex, c.VertexNormal, c.Destination, c.Source);
            const f64 rel = std::abs(got - expected) / std::max(expected, 1.0e-12);
            verdict.Record(std::isfinite(got) && rel <= relTol, rel, relTol,
                           label + ": vertex " + Describe(c.Vertex) + ", dest " + Describe(c.Destination) + ", src " +
                               Describe(c.Source) + ": impl " + std::to_string(got) + ", solid-angle ratio " +
                               std::to_string(expected));
        }
        return verdict;
    }
} // namespace OloEngine::Tests::Oracle
