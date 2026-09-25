// OLO_TEST_LAYER: L1
// =============================================================================
// BsdfIdentityOracleTest.cpp — the shipped local BSDFs against an oracle that
// shares no code with them (issue #1347, acceptance criterion 1).
//
// ClosureV2Test and ReferenceBRDFTest check the closures against THEMSELVES:
// the energy table is recomputed with the engine's own VNDF sampler, the
// furnace integrates the engine's own f, reciprocity swaps the engine's own
// arguments. That catches rot, not a plausible mistake shared by both sides.
// Every expectation here comes from IndependentBsdfOracle.h (f64, the
// tan-theta forms of the papers, no renderer include) or from a closed form
// written here with its citation. The engine is reached only through the
// Oracle::Engine adapters and three local adapters in the anonymous namespace
// below, each naming the engine line it wraps.
//
// What each test pins, and why:
//
//   1. ClosureV2SingleScatterAndDiffuseMatchTheModel — ClosureV2Evaluate
//      without its multiple-scattering lobe IS GGX + height-correlated Smith +
//      Schlick + the (1 - F)(1 - metallic) Lambert of ADR 0016. f32-tight.
//   2. ClosureV2MultiScatterIsKullaContyOnItsTable — the lobe it adds is
//      [KullaConty17] evaluated on the energy table, and how far the table's
//      resolution puts it from Kulla-Conty on the true energies.
//   3. LegacyMatchesItsFrozenConventions — CookTorranceBRDF is the oracle with
//      the default LegacyConventions: the frozen version (AC 6).
//   4. LegacyDepartsFromThePhysicalModelByDesign — how far Legacy is from the
//      physics at near-mirror roughness and at grazing, measured, direction
//      and size asserted, so a silent "correction" fails.
//   5. EnergyTableEntriesMatchAnIndependentQuadrature — every stored entry of
//      the generated table against quadrature that does not use the VNDF
//      sampler the table was generated with.
//   6. EnergyLookupErrorIsInterpolationAndEdgeClamp — between and beyond the
//      cell centres: the lookup is the bilinear, edge-clamped interpolation of
//      correct entries, and what that costs against the true energies.
//   7. WhiteFurnaceIsTheModelPlusTheTablesPredictedError — the v2 white metal:
//      its single-scatter part integrates to the oracle's E(mu), and the full
//      closure departs from the model's 1 by what test 6 predicts.
//   8. EnergyConservation — metals never reflect more than they receive
//      beyond that predicted table error; dielectrics reflect exactly what
//      their model defines, which is MORE than 1 at grazing (reported).
//   9. ClosuresAreReciprocal — Helmholtz reciprocity for BOTH closures.
//  10. PdfNormalisesToTheSamplersAcceptedMass — BSDF::Pdf integrates to the
//      mass the sampler does not reject, that mass from the oracle.
//  11. SpecularPdfsMatchTheirOracleDensities — pointwise D_v / (4 v.m) and
//      D cos / (4 v.m).
//  12. HdrRadianceStaysFiniteAndAccurate — f L cos in f32 at radiance up to
//      the fp16 maximum, including the near-mirror peak (D ~ 1e5).
//
// Design. Every integral is a deterministic midpoint rule with an error
// estimate (the difference to the half-resolution rule), and every tolerance
// is max(a stated floor, 4 x that estimate) plus any stated model term.
// Integrals of ENGINE functions over outgoing directions use a two-rule MIS
// quadrature (IntegrateMis): the oracle's half-vector rule at the lobe's alpha
// plus its uniform-hemisphere rule, balanced by the balance heuristic, so a
// sharp lobe, a diffuse term and the horizon are each resolved by the rule
// whose nodes are there. The oracle is evaluated at the f32-rounded directions
// the engine received, so input rounding is not charged to the engine; what is
// left is the engine's f32 arithmetic plus one conditioning effect of its
// cos-form NDF (NdfConditioning), which the furnace and pdf tests measure.
//
// Classification: L1 (pure CPU math, no GL).
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/Oracles/BsdfOracleChecks.h"
#include "Rendering/Oracles/EngineBsdfAdapters.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/GgxEnergyTables.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests::Oracle
{
    namespace
    {
        // ---- the direction the engine actually received ----------------------

        // The adapters round every direction to f32 at the boundary. Evaluating
        // the oracle at the same rounded direction means input rounding is not
        // charged to the engine.
        [[nodiscard]] glm::dvec3 AsReceived(const glm::dvec3& d)
        {
            return glm::dvec3(glm::vec3(d));
        }

        // ---- tolerance components --------------------------------------------

        // f32 evaluation of either closure is ~30 rounded operations of at most
        // 2^-24 (6e-8) relative each, pow5 and one normalize being the longest
        // chains: ~2e-6. 2e-5 is that with a factor of ten of headroom.
        constexpr f64 kF32Relative = 2.0e-5;

        // Conditioning of the engine's cos-form NDF near its peak. Both
        // closures compute D from B = nDotH^2 (a2 - 1) + 1, which at the peak
        // is a2 = alpha^2 left over from cancelling 1 - nDotH^2. nDotH comes
        // from an f32 normalize and dot: a few ulps of 1, ~2.4e-7 absolute, so
        // B carries ~6e-7 absolute error with its own rounding, and D ~ 1/B^2
        // carries 2 x 6e-7 / B relative — at the peak 1.2e-6 / alpha^2: 0.47
        // at roughness 0.04, 0.19 at 0.05, 2.4e-3 at 0.15, negligible above.
        // Averaged over the lobe's D cos measure, 1/B has mean 1/(2 a2) (with
        // u = tan^2 / alpha^2, B ~ a2 (1 + u) and D cos du ~ (1 + u)^-2 du), so
        // an INTEGRAL of the lobe carries at most half of it. The oracle's tan
        // form, and a B written as sin^2 + a2 cos^2, do not cancel.
        [[nodiscard]] f64 NdfConditioning(f64 alpha)
        {
            return 1.2e-6 / (alpha * alpha);
        }

        // Legacy never sees that peak: its D is floored at pi B^2 >= 1e-4, so
        // wherever the floor is inactive B >= sqrt(1e-4 / pi) = 5.6e-3 and the
        // same 1.2e-6 / B is at most 2.2e-4.
        constexpr f64 kLegacyNdfConditioning = 1.2e-6 / 5.6e-3;

        // Error of one stored table entry against the true energy (test 5
        // asserts it): half-float rounding, at most half an ulp of the [0.5, 1)
        // binade = 2.4e-4, plus the generator's quasi-Monte-Carlo error, taken
        // as 2 / N — twice the left-endpoint bias 1 / (2N) of the Hammersley
        // first coordinate i / N — with N = 4096 samples per Ess entry and 2048
        // per E_avg point (tools/OloGgxEnergyTableGen/main.cpp).
        constexpr f64 kHalfRoundingMax = 2.4e-4;
        constexpr f64 kQmcEss = 2.0 / 4096.0;
        constexpr f64 kQmcAvg = 2.0 / 2048.0;
        constexpr f64 kEntryError = kHalfRoundingMax + kQmcEss;
        constexpr f64 kAvgEntryError = kHalfRoundingMax + kQmcAvg;

        // ---- MIS quadrature over outgoing directions -------------------------

        // int h(l) dw_l over the upper hemisphere as the SUM of two rules,
        // weighted by the balance heuristic ([Veach97] §9.2.2):
        //   A: the half-vector rule at the lobe's alpha, integrand h w_A,
        //   B: the uniform-hemisphere rule,          integrand h w_B,
        //   w_A = p_A / (p_A + p_B), w_B = p_B / (p_A + p_B), w_A + w_B = 1,
        // with p_A(l) = D(m) cos(m) / (4 v.m) the density the half-vector nodes
        // represent ([Walter07] eq. 14) and p_B = 1 / (2 pi). Both rules then
        // integrate h / (p_A + p_B) <= 2 pi h: bounded, so a sharp lobe, a
        // diffuse term and the grazing horizon (aligned with rule B's cells)
        // are each resolved by the rule whose nodes are there. The error
        // estimate is the sum of the two rules' estimates.
        template<typename H>
        [[nodiscard]] Quadrature IntegrateMis(const glm::dvec3& v, f64 alpha, H&& h, u32 n)
        {
            constexpr f64 pB = 1.0 / (2.0 * kPi);
            auto pA = [&](const glm::dvec3& l) -> f64
            {
                const glm::dvec3 m = HalfVector(v, l);
                const f64 vDotM = glm::dot(v, m);
                if (vDotM <= 0.0 || m.z <= 0.0)
                    return 0.0;
                return GgxD(m.z, alpha) * m.z / (4.0 * vDotM);
            };
            const Quadrature a = IntegrateOverOutgoingHemisphere(v, alpha, n, n / 2u,
                                                                 [&](const glm::dvec3& l)
                                                                 {
                                                                     const f64 p = pA(l);
                                                                     return h(l) * p / (p + pB);
                                                                 });
            const Quadrature b = IntegrateHemisphereUniform(n / 2u, n / 2u,
                                                            [&](const glm::dvec3& l)
                                                            {
                                                                const f64 p = pA(l);
                                                                return h(l) * pB / (p + pB);
                                                            });
            return { a.Value + b.Value, a.ErrorEstimate + b.ErrorEstimate };
        }

        // Refine until the error estimate is under `target` or n reaches
        // `maxN`; the caller reports the returned estimate either way.
        template<typename H>
        [[nodiscard]] Quadrature IntegrateMisAdaptive(const glm::dvec3& v, f64 alpha, H&& h, f64 target,
                                                      u32 maxN = 1024u)
        {
            Quadrature q;
            for (u32 n = 256u; n <= maxN; n *= 2u)
            {
                q = IntegrateMis(v, alpha, h, n);
                if (q.ErrorEstimate < target)
                    break;
            }
            return q;
        }

        // Directional albedo int f(v, l) (n.l) dw_l of one channel.
        [[nodiscard]] Quadrature Albedo(const BrdfFn& f, const glm::dvec3& v, f64 alpha, int channel, f64 target,
                                        u32 maxN = 1024u)
        {
            return IntegrateMisAdaptive(
                v, alpha, [&](const glm::dvec3& l)
                { return f(v, l)[channel] * l.z; }, target, maxN);
        }

        // ---- oracle energies ---------------------------------------------------

        // E(mu) of the single-scattering GGX lobe with F == 1:
        // Oracle::GgxDirectionalAlbedo, refined until its error estimate is
        // under 5e-5 (grazing columns need 1024-2048 nodes, the rest 256).
        // Memoised: a pure function of (mu, alpha).
        [[nodiscard]] const Quadrature& OracleE(f64 mu, f64 alpha)
        {
            static std::map<std::pair<f64, f64>, Quadrature> cache;
            const auto key = std::make_pair(mu, alpha);
            auto it = cache.find(key);
            if (it == cache.end())
            {
                Quadrature q;
                for (u32 n = 256u; n <= 2048u; n *= 2u)
                {
                    q = GgxDirectionalAlbedo(mu, alpha, n, n / 2u);
                    if (q.ErrorEstimate < 5.0e-5)
                        break;
                }
                it = cache.emplace(key, q).first;
            }
            return it->second;
        }

        // E_avg = 2 int E(mu) mu dmu ([KullaConty17]), Oracle::GgxAverageAlbedo.
        [[nodiscard]] const Quadrature& OracleEAvg(f64 alpha)
        {
            static std::map<f64, Quadrature> cache;
            auto it = cache.find(alpha);
            if (it == cache.end())
                it = cache.emplace(alpha, GgxAverageAlbedo(alpha)).first;
            return it->second;
        }

        // The documented f_ms guard (ADR 0016 §5, ReferenceBRDF.h
        // ClosureV2MultiScatter): the lobe is 0 when 1 - E_avg < 1e-4. The
        // oracle reproduces the convention from its OWN E_avg. (At roughness
        // 0.05 the true 1 - E_avg is 4.3e-5 and the engine's lookup 8.9e-5,
        // both gated; the table's row-1 E_avg entry is 2.7e-4 against a true
        // 4.0e-4, so the gate's edge moves with that entry's sampling error.)
        constexpr f64 kMultiScatterGate = 1.0e-4;

        [[nodiscard]] bool MultiScatterGated(f64 roughness)
        {
            return 1.0 - OracleEAvg(ClosureV2Alpha(roughness)).Value < kMultiScatterGate;
        }

        // ---- the table's lookup convention, applied to ORACLE values ---------

        // GgxEnergyTables.h "Conventions" and ReferenceBRDF.h GgxEnergyLoss:
        // a cell-centred grid, mu_j = (j + 0.5)/16, r_k = (k + 0.5)/16, alpha =
        // ClosureV2Alpha(r_k), read bilinearly and clamped to the edge cells.
        // The same interpolation of the TRUE energies at the cell centres is
        // what a table with perfect entries would return: the lookup model.
        constexpr u32 kTableSize = PathTracing::kGgxEnergyTableSize;

        [[nodiscard]] f64 CellCentre(u32 i)
        {
            return (static_cast<f64>(i) + 0.5) / static_cast<f64>(kTableSize);
        }

        // Flat, lazily filled copies of the oracle at the cell centres: the
        // lookup model is evaluated at every quadrature node of the tests
        // below, far too often for a map lookup per cell. NaN marks "not yet".
        [[nodiscard]] f64 OracleCellLoss(u32 row, u32 column)
        {
            static std::vector<f64> cells(static_cast<sizet>(kTableSize) * kTableSize,
                                          std::numeric_limits<f64>::quiet_NaN());
            f64& cell = cells[static_cast<sizet>(row) * kTableSize + column];
            if (std::isnan(cell))
                cell = 1.0 - OracleE(CellCentre(column), ClosureV2Alpha(CellCentre(row))).Value;
            return cell;
        }

        [[nodiscard]] f64 OracleCellLossAvg(u32 row)
        {
            static std::vector<f64> rows(kTableSize, std::numeric_limits<f64>::quiet_NaN());
            f64& cell = rows[row];
            if (std::isnan(cell))
                cell = 1.0 - OracleEAvg(ClosureV2Alpha(CellCentre(row))).Value;
            return cell;
        }

        struct CellCoordinate
        {
            u32 I0 = 0;
            u32 I1 = 0;
            f64 Fraction = 0.0;
        };

        [[nodiscard]] CellCoordinate LocateCell(f64 x)
        {
            const auto size = static_cast<f64>(kTableSize);
            const f64 c = std::clamp(std::clamp(x, 0.0, 1.0) * size - 0.5, 0.0, size - 1.0);
            CellCoordinate cell;
            cell.I0 = static_cast<u32>(c);
            cell.I1 = std::min(cell.I0 + 1u, kTableSize - 1u);
            cell.Fraction = c - static_cast<f64>(cell.I0);
            return cell;
        }

        [[nodiscard]] f64 OracleLookupLoss(f64 mu, f64 roughness)
        {
            const CellCoordinate x = LocateCell(mu);
            const CellCoordinate y = LocateCell(roughness);
            const f64 row0 = glm::mix(OracleCellLoss(y.I0, x.I0), OracleCellLoss(y.I0, x.I1), x.Fraction);
            const f64 row1 = glm::mix(OracleCellLoss(y.I1, x.I0), OracleCellLoss(y.I1, x.I1), x.Fraction);
            return glm::mix(row0, row1, y.Fraction);
        }

        [[nodiscard]] f64 OracleLookupLossAvg(f64 roughness)
        {
            const CellCoordinate y = LocateCell(roughness);
            return glm::mix(OracleCellLossAvg(y.I0), OracleCellLossAvg(y.I1), y.Fraction);
        }

        // 2 int L(mu) mu dmu of the lookup model: the numerator of the
        // Kulla-Conty lobe's hemispherical integral. Piecewise linear in mu, so
        // a 1024-point midpoint rule is exact to ~1e-7.
        [[nodiscard]] f64 OracleLookupMeanLoss(f64 roughness)
        {
            constexpr u32 n = 1024u;
            f64 sum = 0.0;
            for (u32 i = 0; i < n; ++i)
            {
                const f64 mu = (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
                sum += OracleLookupLoss(mu, roughness) * mu;
            }
            return 2.0 * sum / static_cast<f64>(n);
        }

        // ---- the v2 closure, as models --------------------------------------

        // The v2 model with the multiple-scattering lobe removed: energies of 1
        // make KullaContyMultiScatter return exactly 0 (its lossAvg <= 0 guard).
        [[nodiscard]] BrdfFn OracleClosureV2SingleScatter(const MaterialCase& m)
        {
            return [m](const glm::dvec3& vIn, const glm::dvec3& lIn)
            {
                return ClosureV2Brdf(AsReceived(vIn), AsReceived(lIn), m.Albedo, m.Metallic, m.Roughness,
                                     ClosureV2Energies{ 1.0, 1.0, 1.0 });
            };
        }

        // [KullaConty17] on the TRUE energies at alpha = ClosureV2Alpha(authored
        // r). The table rows bake that clamp in and are indexed by authored r,
        // so the quantity the table approximates at authored r IS E at the
        // clamped alpha.
        [[nodiscard]] BrdfFn OracleMultiScatterTrue(const MaterialCase& m)
        {
            return [m](const glm::dvec3& vIn, const glm::dvec3& lIn)
            {
                const glm::dvec3 v = AsReceived(vIn);
                const glm::dvec3 l = AsReceived(lIn);
                if (v.z <= 0.0 || l.z <= 0.0 || MultiScatterGated(m.Roughness))
                    return glm::dvec3(0.0);
                const f64 alpha = ClosureV2Alpha(m.Roughness);
                return KullaContyMultiScatter(OracleE(v.z, alpha).Value, OracleE(l.z, alpha).Value,
                                              OracleEAvg(alpha).Value, BaseF0(m.Albedo, m.Metallic));
            };
        }

        // [KullaConty17] on the lookup model's energies: what the engine's lobe
        // is if every table entry is right.
        [[nodiscard]] BrdfFn OracleMultiScatterOnTable(const MaterialCase& m)
        {
            return [m](const glm::dvec3& vIn, const glm::dvec3& lIn)
            {
                const glm::dvec3 v = AsReceived(vIn);
                const glm::dvec3 l = AsReceived(lIn);
                if (v.z <= 0.0 || l.z <= 0.0 || MultiScatterGated(m.Roughness))
                    return glm::dvec3(0.0);
                return KullaContyMultiScatter(1.0 - OracleLookupLoss(v.z, m.Roughness),
                                              1.0 - OracleLookupLoss(l.z, m.Roughness),
                                              1.0 - OracleLookupLossAvg(m.Roughness), BaseF0(m.Albedo, m.Metallic));
            };
        }

        // First-order relative error of f_ms from the entry error: f_ms ~
        // L_v L_l / L_avg, each read with kEntryError / kAvgEntryError.
        [[nodiscard]] f64 MultiScatterEntryRelative(f64 muV, f64 muL, f64 roughness)
        {
            const f64 lv = std::max(OracleLookupLoss(muV, roughness), 1.0e-12);
            const f64 ll = std::max(OracleLookupLoss(muL, roughness), 1.0e-12);
            const f64 la = std::max(OracleLookupLossAvg(roughness), 1.0e-12);
            return kEntryError / lv + kEntryError / ll + kAvgEntryError / la;
        }

        [[nodiscard]] BrdfFn OracleLegacy(const MaterialCase& m, const LegacyConventions& c = {})
        {
            return [m, c](const glm::dvec3& vIn, const glm::dvec3& lIn)
            { return LegacyBrdf(AsReceived(vIn), AsReceived(lIn), m.Albedo, m.Metallic, m.Roughness, c); };
        }

        // ---- local engine adapters (not in EngineBsdfAdapters.h) -------------

        // PathTracing::ClosureV2MultiScatter exactly as ClosureV2Evaluate calls
        // it: clamped cosines, AUTHORED roughness into the lookup.
        [[nodiscard]] BrdfFn EngineClosureV2MultiScatter(const MaterialCase& m)
        {
            return [m](const glm::dvec3& v, const glm::dvec3& l)
            {
                const glm::vec3 vf = Engine::ToF32(v);
                const glm::vec3 lf = Engine::ToF32(l);
                const glm::vec3 f0 = glm::mix(glm::vec3(PathTracing::kDefaultDielectricF0), Engine::ToF32(m.Albedo),
                                              static_cast<f32>(m.Metallic));
                return Engine::ToF64(PathTracing::ClosureV2MultiScatter(
                    std::max(vf.z, 0.0f), std::max(lf.z, 0.0f), static_cast<f32>(m.Roughness), f0));
            };
        }

        // ClosureV2Evaluate minus the multiple-scattering term it adds: the
        // single-scatter specular plus the Lambert, as the engine composes them.
        [[nodiscard]] BrdfFn EngineClosureV2SingleScatter(const MaterialCase& m)
        {
            const BrdfFn full = Engine::ClosureV2Brdf(m);
            const BrdfFn ms = EngineClosureV2MultiScatter(m);
            return [full, ms](const glm::dvec3& v, const glm::dvec3& l)
            { return full(v, l) - ms(v, l); };
        }

        // PathTracing::PdfGGX at BSDF::SamplingRoughness, over l — the Legacy
        // specular density exactly as BSDF::Pdf forms it.
        [[nodiscard]] PdfFn EngineLegacySpecularPdf(f64 roughness)
        {
            return [roughness](const glm::dvec3& v, const glm::dvec3& l)
            {
                const glm::vec3 vf = Engine::ToF32(v);
                const glm::vec3 lf = Engine::ToF32(l);
                if (lf.z <= 0.0f)
                    return 0.0;
                const glm::vec3 h = glm::normalize(vf + lf);
                const f32 rs = PathTracing::BSDF::SamplingRoughness(static_cast<f32>(roughness));
                return static_cast<f64>(PathTracing::PdfGGX(h.z, glm::dot(vf, h), rs));
            };
        }

        // ---- white-metal furnace --------------------------------------------

        // int f_ms cos dw of the ENGINE's lobe for view v: f_ms depends on l only
        // through n.l, so the hemisphere integral is 2 pi int_0^1 f_ms(mu_l)
        // mu_l dmu_l, a 2048-point midpoint rule.
        [[nodiscard]] f64 EngineMultiScatterAlbedo(const MaterialCase& m, const glm::dvec3& v)
        {
            constexpr u32 n = 2048u;
            const BrdfFn ms = EngineClosureV2MultiScatter(m);
            f64 sum = 0.0;
            for (u32 i = 0; i < n; ++i)
            {
                const f64 mu = (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
                sum += ms(v, Direction(mu, 0.0)).x * mu;
            }
            return 2.0 * kPi * sum / static_cast<f64>(n);
        }

        struct FurnacePrediction
        {
            f64 Model = 1.0;         // the model: 1, or E(mu) where the gate drops f_ms
            f64 OnTable = 1.0;       // the model with the lookup model's energies in f_ms
            f64 EntryBound = 0.0;    // how far the table's entry error can move OnTable
            f64 SingleScatter = 1.0; // the oracle's E(mu)
            f64 SingleScatterError = 0.0;
        };

        // For a white metal F == 1 and F_ms == 1, so [KullaConty17] gives
        // int f_ms cos = L(mu_v) * 2 int L(mu) mu dmu / L_avg: exactly
        // 1 - E(mu_v) on the true energies (the model closes to 1), the lookup
        // model's value on the table.
        [[nodiscard]] FurnacePrediction PredictWhiteMetalFurnace(f64 muV, f64 roughness)
        {
            const Quadrature& e = OracleE(muV, ClosureV2Alpha(roughness));
            FurnacePrediction p;
            p.SingleScatter = e.Value;
            p.SingleScatterError = e.ErrorEstimate;
            if (MultiScatterGated(roughness))
            {
                p.Model = e.Value;
                p.OnTable = e.Value;
                return p;
            }
            const f64 lv = OracleLookupLoss(muV, roughness);
            const f64 mean = OracleLookupMeanLoss(roughness);
            const f64 avg = OracleLookupLossAvg(roughness);
            p.OnTable = e.Value + lv * mean / avg;
            p.EntryBound = (kEntryError + lv * (kEntryError / mean + kAvgEntryError / avg)) * mean / avg;
            return p;
        }

        // ---- reporting ----------------------------------------------------------

        [[nodiscard]] std::string Fmt(f64 x)
        {
            std::ostringstream s;
            s.precision(6);
            s << x;
            return s.str();
        }

        [[nodiscard]] std::string Report(const Verdict& v)
        {
            return "worst " + Fmt(v.Worst) + " (bound " + Fmt(v.Bound) + ") at " + v.Detail;
        }

        const glm::dvec3 kGrey{ 0.8 };
        const glm::dvec3 kColoured{ 0.9, 0.5, 0.1 };
        const glm::dvec3 kWhite{ 1.0 };

        // Grazing twice over: 0.02 and 0.05 are where denominators, the table's
        // edge clamp and the horizon cutoff go wrong first.
        const std::vector<f64> kFurnaceCosines{ 0.02, 0.05, 0.1, 0.35, 0.7, 1.0 };
    } // namespace

    // =========================================================================
    // 1. ClosureV2Evaluate, minus its multiple-scattering lobe, IS the model:
    //    D and G2 of [Walter07]/[Heitz14], Schlick F at v.h, and the Lambert
    //    weight (1 - F)(1 - metallic) of ADR 0016. Only f32 arithmetic and the
    //    NDF conditioning separate the two.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, ClosureV2SingleScatterAndDiffuseMatchTheModel)
    {
        f64 worst = 0.0;
        for (const glm::dvec3& albedo : { kGrey, kColoured })
        {
            for (f64 roughness : kRoughnessGrid)
            {
                for (f64 metallic : kMetallicGrid)
                {
                    const MaterialCase m{ albedo, metallic, roughness };
                    const f64 relTol = kF32Relative + NdfConditioning(ClosureV2Alpha(roughness));
                    const Verdict verdict =
                        CheckEvaluationAgainstModel(EngineClosureV2SingleScatter(m), OracleClosureV2SingleScatter(m),
                                                    relTol, 1.0e-7, Describe(m));
                    worst = std::max(worst, verdict.Worst);
                    EXPECT_TRUE(verdict.Pass) << "ClosureV2Evaluate's single-scatter + Lambert part departs from "
                                                 "the independent model: "
                                              << Report(verdict);
                }
            }
        }
        std::cout << "[ oracle ] ClosureV2 single-scatter + diffuse vs model: worst relative " << worst << "\n";
    }

    // =========================================================================
    // 2. The Kulla-Conty lobe ClosureV2Evaluate adds.
    //
    //   (a) IS [KullaConty17] on its table: against the formula fed with the
    //       lookup model's energies, to the entry error (kEntryError,
    //       kAvgEntryError) propagated to first order.
    //   (b) What the table's resolution costs, lookup model against the TRUE
    //       energies — measured over this grid, both albedos:
    //         interior (both cosines >= 1/32): worst 0.175 relative, at
    //         roughness 0.3 and v = l = n, where the lookup reads the 31/32
    //         column for mu = 1 and interpolates rows 1/16 apart across a
    //         1 - E that grows much faster than linearly. Bound 0.2.
    //         edge (a cosine < 1/32, here 0.02): worst 1.05 — the lookup
    //         returns 1 - E(1/32) for 1 - E(0.02). Bound 1.1.
    //       Both over an absolute 1e-4, below which the lobe carries nothing.
    //       What these add up to in energy is test 7's furnace.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, ClosureV2MultiScatterIsKullaContyOnItsTable)
    {
        constexpr f64 kInteriorRelative = 0.2;
        constexpr f64 kEdgeRelative = 1.1;
        constexpr f64 kResolutionAbsolute = 1.0e-4;
        f64 worstOnTable = 0.0;
        f64 worstInterior = 0.0;
        f64 worstEdge = 0.0;
        std::string worstInteriorWhere;
        for (const glm::dvec3& albedo : { kGrey, kColoured })
        {
            for (f64 roughness : kRoughnessGrid)
            {
                for (f64 metallic : kMetallicGrid)
                {
                    const MaterialCase m{ albedo, metallic, roughness };
                    const BrdfFn impl = EngineClosureV2MultiScatter(m);
                    const BrdfFn onTable = OracleMultiScatterOnTable(m);
                    const BrdfFn truth = OracleMultiScatterTrue(m);
                    Verdict a;
                    Verdict interior;
                    Verdict edge;
                    for (const auto& [v, l] : DirectionPairs())
                    {
                        const glm::dvec3 got = impl(v, l);
                        const glm::dvec3 table = onTable(v, l);
                        const glm::dvec3 exact = truth(v, l);
                        const f64 entryRel = kF32Relative + MultiScatterEntryRelative(v.z, l.z, roughness);
                        const bool isEdge = std::min(v.z, l.z) < 1.0 / 32.0;
                        const std::string where = Describe(m) + ": v " + Describe(v) + ", l " + Describe(l);
                        for (int c = 0; c < 3; ++c)
                        {
                            const f64 errA = std::abs(got[c] - table[c]);
                            a.Record(std::isfinite(got[c]) && errA <= entryRel * table[c] + 1.0e-6,
                                     errA / std::max(table[c], 1.0e-12), entryRel,
                                     where + ", channel " + std::to_string(c) + ": impl " + Fmt(got[c]) +
                                         ", Kulla-Conty on the table " + Fmt(table[c]));

                            const f64 relTol = isEdge ? kEdgeRelative : kInteriorRelative;
                            const f64 errB = std::abs(table[c] - exact[c]);
                            (isEdge ? edge : interior)
                                .Record(errB <= relTol * exact[c] + kResolutionAbsolute,
                                        errB / (exact[c] + kResolutionAbsolute), relTol,
                                        where + ", channel " + std::to_string(c) + ": on the table " + Fmt(table[c]) +
                                            ", on the true energies " + Fmt(exact[c]));
                        }
                    }
                    worstOnTable = std::max(worstOnTable, a.Worst);
                    if (interior.Worst > worstInterior)
                    {
                        worstInterior = interior.Worst;
                        worstInteriorWhere = interior.Detail;
                    }
                    worstEdge = std::max(worstEdge, edge.Worst);
                    EXPECT_TRUE(a.Pass) << "ClosureV2MultiScatter is not Kulla-Conty on its own table: " << Report(a);
                    EXPECT_TRUE(interior.Pass) << "the table's bilinear resolution costs the lobe more than stated: "
                                               << Report(interior);
                    EXPECT_TRUE(edge.Pass) << "the table's edge clamp below mu = 1/32 costs more than stated: "
                                           << Report(edge);
                }
            }
        }
        std::cout << "[ oracle ] ClosureV2 multi-scatter: vs Kulla-Conty on its table worst relative " << worstOnTable
                  << "; table vs true energies: interior " << worstInterior << " (bound " << kInteriorRelative
                  << ", at " << worstInteriorWhere << "), edge mu < 1/32 " << worstEdge << " (bound " << kEdgeRelative << ")\n";
    }

    // =========================================================================
    // 3. CookTorranceBRDF IS the frozen Legacy version (AC 6): the oracle with
    //    default LegacyConventions — the 1e-4 NDF floor, the 1e-4 addend, the
    //    separable Schlick-GGX k = (r+1)^2/8 — reproduces it to f32 precision.
    //    Nothing here moves Legacy toward the physics; test 4 measures how far
    //    from it the version is.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, LegacyMatchesItsFrozenConventions)
    {
        const f64 relTol = kF32Relative + kLegacyNdfConditioning;
        f64 worst = 0.0;
        for (const glm::dvec3& albedo : { kGrey, kColoured })
        {
            for (f64 roughness : kRoughnessGrid)
            {
                for (f64 metallic : kMetallicGrid)
                {
                    const MaterialCase m{ albedo, metallic, roughness };
                    const Verdict verdict =
                        CheckEvaluationAgainstModel(Engine::LegacyBrdf(m), OracleLegacy(m), relTol, 1.0e-7, Describe(m));
                    worst = std::max(worst, verdict.Worst);
                    EXPECT_TRUE(verdict.Pass) << "CookTorranceBRDF is no longer the frozen Legacy version the oracle "
                                                 "reproduces (Legacy is versioned behaviour, AC 6 of #1347 — a "
                                                 "change here is a new version, not a fix): "
                                              << Report(verdict);
                }
            }
        }
        std::cout << "[ oracle ] Legacy vs its frozen conventions: worst relative " << worst << " (bound " << relTol
                  << ")\n";
    }

    // =========================================================================
    // 4. How far Legacy is from the physical model, measured — asserted in
    //    direction and rough size, so a silent "correction" of the frozen
    //    version fails here as loudly as a regression would.
    //
    //   (a) near-mirror collapse: the 1e-4 floor on pi B^2 caps D at
    //       a2 / 1e-4, so at roughness 0.05 the mirror-peak value is ~1e-6 of
    //       the model's, and the directional albedo a few tenths of a percent.
    //   (b) grazing darkening at roughness 1: separable Schlick-GGX with
    //       k = 0.5 gives G1(0.02)^2 = 0.0016 against a height-correlated G2 of
    //       0.02 ([Heitz14] eq. 99), so the mirror value at mu = 0.02 is ~7 %
    //       of the model's; the closed form of that ratio is asserted.
    //   (c) the 1e-4 addend alone: 4 mu^2 / (4 mu^2 + 1e-4) = 0.941 at
    //       mu = 0.02, the 6 % the LegacyConventions comment names.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, LegacyDepartsFromThePhysicalModelByDesign)
    {
        const glm::dvec3 f0(1.0);

        // (a) near-mirror collapse, at the mirror pair where D peaks.
        for (f64 roughness : { 0.05, 0.15 })
        {
            const MaterialCase m{ kWhite, 1.0, roughness };
            const glm::dvec3 v = AsReceived(Direction(0.7, 0.0));
            const glm::dvec3 l = AsReceived(Direction(0.7, kPi));
            const f64 legacy = Engine::LegacyBrdf(m)(v, l).x;
            const f64 physical = MicrofacetSpecular(v, l, roughness * roughness, f0).x;
            const f64 ratio = legacy / physical;
            std::cout << "[ oracle ] Legacy / model at the mirror peak, roughness " << roughness << ", mu 0.7: "
                      << legacy << " / " << physical << " = " << ratio << "\n";
            // 1e-4 and 2e-2 sit between the measured 1.1e-6 / 7.0e-3 and any
            // lobe that stopped collapsing (ratio ~1).
            EXPECT_LT(ratio, roughness < 0.1 ? 1.0e-4 : 2.0e-2)
                << "roughness " << roughness << ": Legacy's mirror peak is " << legacy << " against the model's "
                << physical << ". Legacy's NDF floor collapsing the near-mirror lobe is intentional versioned "
                << "behaviour (AC 6 of #1347); if it no longer collapses, the frozen Legacy closure was changed.";
        }
        {
            const f64 alpha = 0.05 * 0.05;
            const Quadrature legacy =
                Albedo(Engine::LegacyBrdf(MaterialCase{ kWhite, 1.0, 0.05 }), Direction(0.7, 0.0), alpha, 0, 1.0e-4);
            const Quadrature& model = OracleE(0.7, alpha);
            std::cout << "[ oracle ] Legacy white-metal albedo at roughness 0.05, mu 0.7: " << legacy.Value
                      << " (err " << legacy.ErrorEstimate << ") vs model E " << model.Value << "\n";
            EXPECT_LT(legacy.Value, 0.1 * model.Value)
                << "Legacy directional albedo " << legacy.Value << " vs model " << model.Value
                << " at roughness 0.05: the near-mirror collapse is intentional versioned behaviour (AC 6).";
        }

        // (b) grazing, roughness 1: D agrees (a2 = 1 gives 1/pi in both, the
        // floor inactive), so the ratio is the closed form of the G's and the
        // denominators.
        {
            const MaterialCase m{ kWhite, 1.0, 1.0 };
            const glm::dvec3 v = AsReceived(Direction(0.02, 0.0));
            const glm::dvec3 l = AsReceived(Direction(0.02, kPi));
            const f64 ratio = Engine::LegacyBrdf(m)(v, l).x / MicrofacetSpecular(v, l, 1.0, f0).x;
            const f64 g1 = LegacySchlickG1(v.z, 1.0);
            const f64 expected =
                (g1 * g1 / (4.0 * v.z * l.z + 1.0e-4)) / (GgxG2HeightCorrelated(v.z, l.z, 1.0) / (4.0 * v.z * l.z));
            std::cout << "[ oracle ] Legacy / model at the grazing mirror pair, roughness 1, mu 0.02: " << ratio
                      << " (closed form " << expected << ")\n";
            EXPECT_NEAR(ratio, expected, 1.0e-4 * expected)
                << "the grazing Legacy / model ratio is no longer the closed form of its G and denominator";
            EXPECT_LT(ratio, 1.0) << "Legacy is darker than the model at grazing by design (AC 6)";
        }

        // (c) the addend's share alone.
        {
            const MaterialCase m{ kWhite, 1.0, 0.5 };
            LegacyConventions noAddend;
            noAddend.SpecularDenominatorAddend = 0.0;
            const glm::dvec3 v = Direction(0.02, 0.0);
            const glm::dvec3 l = Direction(0.02, kPi);
            const f64 ratio = Engine::LegacyBrdf(m)(v, l).x / OracleLegacy(m, noAddend)(v, l).x;
            const f64 mu = AsReceived(v).z;
            const f64 expected = 4.0 * mu * mu / (4.0 * mu * mu + 1.0e-4);
            std::cout << "[ oracle ] Legacy addend share at mu 0.02: " << ratio << " (closed form " << expected
                      << ")\n";
            EXPECT_NEAR(ratio, expected, 1.0e-4)
                << "the 1e-4 addend no longer dims the grazing lobe by 4 mu^2 / (4 mu^2 + 1e-4)";
        }
    }

    // =========================================================================
    // 5. Every stored entry of the generated table against an independent
    //    quadrature.
    //
    // GgxEnergyTables.h "Conventions": entry (row k, column j) is
    // 1 - E(mu_j) at alpha = clamp(r_k, 0.04, 1)^2, mu_j = (j + 0.5)/16,
    // r_k = (k + 0.5)/16; the E_avg row holds 1 - E_avg(alpha_k). The bound per
    // entry is half an ulp of ITS half-float binade + the generator's QMC error
    // (kQmcEss / kQmcAvg) + 4 x the oracle's error estimate. Nothing here uses
    // the VNDF sampler the table was generated with; the oracle is the D cos
    // half-vector quadrature of [Heitz14] eq. 99, cross-checked on the hardest
    // column by the MIS rule, whose uniform half is aligned with the horizon.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, EnergyTableEntriesMatchAnIndependentQuadrature)
    {
        auto halfRounding = [](f64 x)
        {
            const f64 a = std::abs(x);
            if (a < std::ldexp(1.0, -14))
                return std::ldexp(1.0, -25); // subnormal half: spacing 2^-24
            return std::ldexp(1.0, static_cast<int>(std::floor(std::log2(a))) - 11);
        };

        Verdict entries;
        Verdict averages;
        std::ostringstream largest;
        for (u32 k = 0; k < kTableSize; ++k)
        {
            const f64 alpha = ClosureV2Alpha(CellCentre(k));
            for (u32 j = 0; j < kTableSize; ++j)
            {
                const Quadrature& e = OracleE(CellCentre(j), alpha);
                const auto stored = static_cast<f64>(PathTracing::GgxEnergyLossEntry(k * kTableSize + j));
                const f64 deviation = std::abs(stored - (1.0 - e.Value));
                const f64 bound = halfRounding(stored) + kQmcEss + 4.0 * e.ErrorEstimate;
                if (deviation > 2.5e-4)
                    largest << " [" << k << "," << j << "] " << Fmt(stored - (1.0 - e.Value));
                entries.Record(deviation <= bound, deviation, bound,
                               "row " + std::to_string(k) + " (r " + Fmt(CellCentre(k)) + "), column " +
                                   std::to_string(j) + " (mu " + Fmt(CellCentre(j)) + "): stored 1 - Ess " +
                                   Fmt(stored) + ", oracle " + Fmt(1.0 - e.Value) + " (error estimate " +
                                   Fmt(e.ErrorEstimate) + ")");
            }
            const Quadrature& avg = OracleEAvg(alpha);
            const auto stored = static_cast<f64>(PathTracing::GgxEnergyLossAvgEntry(k));
            const f64 deviation = std::abs(stored - (1.0 - avg.Value));
            const f64 bound = halfRounding(stored) + kQmcAvg + 4.0 * avg.ErrorEstimate;
            averages.Record(deviation <= bound, deviation, bound,
                            "row " + std::to_string(k) + " (r " + Fmt(CellCentre(k)) + "): stored 1 - E_avg " +
                                Fmt(stored) + ", oracle " + Fmt(1.0 - avg.Value) + " (error estimate " +
                                Fmt(avg.ErrorEstimate) + ")");
        }
        EXPECT_TRUE(entries.Pass) << "a GgxEnergyTables.h entry disagrees with the independent quadrature beyond "
                                     "half rounding + the generator's sampling error: "
                                  << Report(entries);
        EXPECT_TRUE(averages.Pass) << "a 1 - E_avg entry disagrees with the independent quadrature: "
                                   << Report(averages);
        std::cout << "[ oracle ] table entries: worst |stored - oracle| " << entries.Worst << " at " << entries.Detail
                  << "\n[ oracle ]   entries off by > 2.5e-4 [row,col] stored - oracle:" << largest.str()
                  << "\n[ oracle ] E_avg row: worst " << averages.Worst << " at " << averages.Detail << "\n";

        for (u32 k = 0; k < kTableSize; ++k)
        {
            const f64 alpha = ClosureV2Alpha(CellCentre(k));
            const glm::dvec3 v = Direction(CellCentre(0), 0.0);
            const Quadrature mis = IntegrateMisAdaptive(
                v, alpha,
                [&](const glm::dvec3& l) -> f64
                {
                    if (l.z <= 0.0)
                        return 0.0;
                    return GgxD(HalfVector(v, l).z, alpha) * GgxG2HeightCorrelated(v.z, l.z, alpha) / (4.0 * v.z);
                },
                5.0e-5, 2048u);
            const Quadrature& half = OracleE(CellCentre(0), alpha);
            // 1e-4 floor: the two rules' estimates under-state their residual
            // here by ~2x (measured 4.2e-5 at row 1); 1e-4 is a fifth of the
            // entry bound above, so agreement at it is what the check needs.
            EXPECT_NEAR(mis.Value, half.Value, std::max(1.0e-4, 4.0 * (mis.ErrorEstimate + half.ErrorEstimate)))
                << "row " << k << ", mu 1/32: the two oracle rules for E disagree (MIS " << mis.Value << ", error "
                << mis.ErrorEstimate << "; half-vector " << half.Value << ", error " << half.ErrorEstimate << ")";
        }
    }

    // =========================================================================
    // 6. Between and beyond the cell centres. GgxEnergyLoss(mu, r) is:
    //   (a) the bilinear, edge-clamped interpolation of correct entries — the
    //       lookup model to within kEntryError — so everything else it costs
    //       is resolution, not a wrong number;
    //   (b) that resolution against the true 1 - E(mu, alpha(r)), per regime,
    //       measured worst over this grid -> bound:
    //         interior bilinear       0.018 (mu 0.0625, r 0.25: 0.093 vs 0.111)
    //                                 -> 0.02
    //         mu < 1/32 edge column   0.072 (mu 0.005, r 0.05: 0.014 vs 0.086)
    //                                 -> 0.08
    //         mu > 31/32              0.005 (mu 1, r 0.9) -> 0.006
    //         r > 31/32 edge row      0.078 (mu 0.005, r 0.98: 0.103 vs 0.026)
    //                                 -> 0.085
    //       (r < 1/32 is exact: row 0 bakes the 0.04 clamp that also governs
    //       the truth there). GgxEnergyLossAverage: interior 7.9e-4 (r 0.5)
    //       -> 1.5e-3, the entry error; edge row 0.028 (r 1: 0.5625 vs 0.591)
    //       -> 0.03. The edge rows and columns are the table's real cost: a
    //       cell-centred grid clamps a quarter-cell short of mu = 0, mu = 1
    //       and r = 1.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, EnergyLookupErrorIsInterpolationAndEdgeClamp)
    {
        const std::vector<f64> mus{ 0.005, 0.02, 0.0625, 0.15, 0.3125, 0.5, 0.71875, 0.9, 0.98, 1.0 };
        const std::vector<f64> roughnesses{ 0.02, 0.05, 0.1, 0.15, 0.25, 0.4, 0.5, 0.65, 0.8, 0.9, 0.98, 1.0 };
        constexpr f64 kLow = 1.0 / 32.0;
        constexpr f64 kHigh = 31.0 / 32.0;
        struct Regime
        {
            const char* Name;
            f64 Bound;
            Verdict V;
        };
        Regime interior{ "interior bilinear", 0.02, {} };
        Regime muLow{ "mu < 1/32 edge column", 0.08, {} };
        Regime muHigh{ "mu > 31/32", 0.006, {} };
        Regime rHigh{ "r > 31/32 edge row", 0.085, {} };
        Verdict entries;

        for (f64 r : roughnesses)
        {
            const f64 alpha = ClosureV2Alpha(r);
            for (f64 mu : mus)
            {
                const auto lookup =
                    static_cast<f64>(PathTracing::GgxEnergyLoss(static_cast<f32>(mu), static_cast<f32>(r)));
                const f64 onTable = OracleLookupLoss(mu, r);
                const Quadrature& e = OracleE(mu, alpha);
                const f64 truth = 1.0 - e.Value;
                const std::string where = "mu " + Fmt(mu) + ", r " + Fmt(r) + ": lookup " + Fmt(lookup) +
                                          ", lookup model " + Fmt(onTable) + ", true " + Fmt(truth);

                const f64 errA = std::abs(lookup - onTable);
                entries.Record(errA <= kEntryError + 1.0e-4, errA, kEntryError + 1.0e-4, where);

                Regime& regime = r > kHigh ? rHigh : (mu < kLow ? muLow : (mu > kHigh ? muHigh : interior));
                const f64 errB = std::abs(onTable - truth);
                const f64 bound = regime.Bound + 4.0 * e.ErrorEstimate;
                regime.V.Record(errB <= bound, errB, bound, where);
            }
        }
        EXPECT_TRUE(entries.Pass) << "GgxEnergyLoss is not the bilinear edge-clamped interpolation of its entries: "
                                  << Report(entries);
        for (const Regime* regime : { &interior, &muLow, &muHigh, &rHigh })
        {
            EXPECT_TRUE(regime->V.Pass) << "the table's " << regime->Name
                                        << " error exceeds its stated bound: " << Report(regime->V);
            std::cout << "[ oracle ] lookup vs true 1 - E, " << regime->Name << ": worst " << regime->V.Worst << " at "
                      << regime->V.Detail << "\n";
        }
        std::cout << "[ oracle ] lookup vs lookup model (entries): worst " << entries.Worst << "\n";

        // The averaged row, the Kulla-Conty denominator.
        Verdict avgInterior;
        Verdict avgEdge;
        for (f64 r : roughnesses)
        {
            const auto lookup = static_cast<f64>(PathTracing::GgxEnergyLossAverage(static_cast<f32>(r)));
            const f64 truth = 1.0 - OracleEAvg(ClosureV2Alpha(r)).Value;
            const f64 err = std::abs(lookup - truth);
            const bool isEdge = r > kHigh;
            const f64 bound = isEdge ? 0.03 : 1.5e-3;
            (isEdge ? avgEdge : avgInterior)
                .Record(err <= bound, err, bound, "r " + Fmt(r) + ": lookup " + Fmt(lookup) + ", true 1 - E_avg " + Fmt(truth));
        }
        EXPECT_TRUE(avgInterior.Pass) << "GgxEnergyLossAverage interior error: " << Report(avgInterior);
        EXPECT_TRUE(avgEdge.Pass) << "GgxEnergyLossAverage edge-row error: " << Report(avgEdge);
        std::cout << "[ oracle ] lookup vs true 1 - E_avg: interior worst " << avgInterior.Worst << " at "
                  << avgInterior.Detail << "; edge worst " << avgEdge.Worst << " at " << avgEdge.Detail << "\n";
    }

    // =========================================================================
    // 7. The white furnace of the v2 closure: albedo 1, metallic 1, so F0 = 1,
    //    F == 1, F_ms = 1 and the Lambert term is exactly zero.
    //
    //   * single scatter: int f_ss cos = E(mu), the oracle's own quadrature.
    //     Bound: 4 x the two quadrature estimates, floor 5e-4, plus the NDF
    //     conditioning of an integral, NdfConditioning(alpha) / 2 — the effect
    //     is measured here as +1.3 % at roughness 0.05, normal incidence.
    //   * full closure: [KullaConty17] makes int f_ms cos = 1 - E(mu) exactly
    //     at F_avg = 1, so the MODEL closes to 1 (to E(mu) where the gate drops
    //     the lobe). The engine departs from that by the table's resolution
    //     (test 6), which the lookup model PREDICTS: the assertion is on the
    //     prediction, to the entry error; the departure from 1 is then bounded
    //     by the prediction's own, and reported.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, WhiteFurnaceIsTheModelPlusTheTablesPredictedError)
    {
        constexpr f64 kSingleScatterFloor = 5.0e-4;
        constexpr f64 kPredictionFloor = 1.0e-3;
        f64 lowest = 2.0;
        f64 highest = 0.0;
        f64 worstPrediction = 0.0;
        for (f64 roughness : kRoughnessGrid)
        {
            const MaterialCase m{ kWhite, 1.0, roughness };
            const f64 alpha = ClosureV2Alpha(roughness);
            for (f64 mu : kFurnaceCosines)
            {
                const glm::dvec3 v = Direction(mu, 0.0);
                const FurnacePrediction p = PredictWhiteMetalFurnace(AsReceived(v).z, roughness);

                const Quadrature full = Albedo(Engine::ClosureV2Brdf(m), v, alpha, 0, 1.25e-4, 512u);
                const f64 ss = full.Value - EngineMultiScatterAlbedo(m, v);
                const f64 quadrature = 4.0 * (full.ErrorEstimate + p.SingleScatterError);
                const f64 conditioning = 0.5 * NdfConditioning(alpha) * p.SingleScatter;
                EXPECT_NEAR(ss, p.SingleScatter, std::max(kSingleScatterFloor, quadrature) + conditioning)
                    << "roughness " << roughness << ", mu " << mu << ": the engine's single-scatter albedo " << ss
                    << " (quadrature error " << full.ErrorEstimate << ") is not the model's E " << p.SingleScatter;

                const f64 predictionBound = std::max(kPredictionFloor, quadrature) + conditioning + p.EntryBound;
                worstPrediction = std::max(worstPrediction, std::abs(full.Value - p.OnTable));
                EXPECT_NEAR(full.Value, p.OnTable, predictionBound)
                    << "roughness " << roughness << ", mu " << mu << ": the white furnace gives " << full.Value
                    << " (quadrature error " << full.ErrorEstimate << "); the model predicts " << p.Model
                    << " and, through the table's resolution, " << p.OnTable;
                EXPECT_LE(std::abs(full.Value - p.Model), std::abs(p.OnTable - p.Model) + predictionBound)
                    << "roughness " << roughness << ", mu " << mu << ": the furnace departs from the model's "
                    << p.Model << " by more than the table's resolution predicts";

                lowest = std::min(lowest, full.Value);
                highest = std::max(highest, full.Value);
                std::cout << "[ oracle ] v2 white furnace r " << roughness << " mu " << mu
                          << (MultiScatterGated(roughness) ? " (f_ms gated)" : "") << ": " << full.Value << " (err "
                          << full.ErrorEstimate << "), predicted on table " << p.OnTable << ", model " << p.Model
                          << "; single scatter " << ss << " vs E " << p.SingleScatter << "\n";
            }
        }
        std::cout << "[ oracle ] v2 white furnace: range [" << lowest << ", " << highest
                  << "], worst |furnace - table prediction| " << worstPrediction << "\n";
    }

    // =========================================================================
    // 8. Energy conservation, white albedo, every roughness and view cosine.
    //
    //   * Metals (albedo 1, metallic 1): <= 1 + max(1e-3, 4 x the quadrature
    //     estimate). For v2 the bound is the table's predicted furnace (test
    //     7) where that exceeds 1: at mu = 0.02 the edge clamp over-reports the
    //     loss and the closure returns up to 2.5 % more than it receives.
    //   * Dielectrics (albedo 1, metallic 0): the albedo is asserted equal to
    //     the oracle MODEL's — and the model itself exceeds 1. The Lambert
    //     weight (1 - F(v.h))(1 - metallic) of ADR 0016 §5 ("an energy
    //     heuristic") does not subtract the specular lobe's directional albedo:
    //     at grazing view F(v.h) at the mirror half vector is ~0.9, the lobe
    //     reflects that, and the diffuse term still reflects ~0.9 on top.
    //     Reported, not asserted <= 1: this is the model's definition, and a
    //     change to it is a closure version decision (a FINDING of #1347).
    // =========================================================================
    TEST(BsdfIdentityOracleTest, EnergyConservation)
    {
        constexpr f64 kFloor = 1.0e-3;
        const std::vector<f64> cosines{ 0.02, 0.35, 1.0 };
        f64 legacyMetalMax = 0.0;
        f64 v2MetalMax = 0.0;
        f64 legacyDielectricMax = 0.0;
        f64 v2DielectricMax = 0.0;
        std::string legacyDielectricWhere;
        std::string v2DielectricWhere;
        for (f64 roughness : kRoughnessGrid)
        {
            const f64 alpha = ClosureV2Alpha(roughness);
            for (f64 mu : cosines)
            {
                const glm::dvec3 v = Direction(mu, 0.0);
                const std::string where = "roughness " + Fmt(roughness) + ", mu " + Fmt(mu);

                // Metals.
                const MaterialCase metal{ kWhite, 1.0, roughness };
                const Quadrature legacyMetal = Albedo(Engine::LegacyBrdf(metal), v, alpha, 0, 2.5e-4, 512u);
                legacyMetalMax = std::max(legacyMetalMax, legacyMetal.Value);
                EXPECT_LE(legacyMetal.Value, 1.0 + std::max(kFloor, 4.0 * legacyMetal.ErrorEstimate))
                    << "Legacy white metal, " << where << ": albedo " << legacyMetal.Value << " (error "
                    << legacyMetal.ErrorEstimate << ") — the closure creates energy";

                const Quadrature v2Metal = Albedo(Engine::ClosureV2Brdf(metal), v, alpha, 0, 2.5e-4, 512u);
                const FurnacePrediction p = PredictWhiteMetalFurnace(AsReceived(v).z, roughness);
                const f64 v2Allowance =
                    std::max(0.0, p.OnTable - 1.0) + p.EntryBound + 0.5 * NdfConditioning(alpha) * p.SingleScatter;
                v2MetalMax = std::max(v2MetalMax, v2Metal.Value);
                EXPECT_LE(v2Metal.Value, 1.0 + std::max(kFloor, 4.0 * v2Metal.ErrorEstimate) + v2Allowance)
                    << "ClosureV2 white metal, " << where << ": albedo " << v2Metal.Value << " (error "
                    << v2Metal.ErrorEstimate << ") exceeds 1 by more than the table predicts (" << p.OnTable << ")";

                // Dielectrics: engine against model, both through the same rule.
                const MaterialCase dielectric{ kWhite, 0.0, roughness };
                const Quadrature legacyD = Albedo(Engine::LegacyBrdf(dielectric), v, alpha, 0, 2.5e-4, 512u);
                const Quadrature legacyModel = Albedo(OracleLegacy(dielectric), v, alpha, 0, 2.5e-4, 512u);
                EXPECT_NEAR(legacyD.Value, legacyModel.Value,
                            std::max(kFloor, 4.0 * (legacyD.ErrorEstimate + legacyModel.ErrorEstimate)))
                    << "Legacy white dielectric, " << where << ": engine albedo " << legacyD.Value
                    << " is not the frozen model's " << legacyModel.Value;
                if (legacyModel.Value > legacyDielectricMax)
                {
                    legacyDielectricMax = legacyModel.Value;
                    legacyDielectricWhere = where;
                }

                const BrdfFn ss = OracleClosureV2SingleScatter(dielectric);
                const BrdfFn ms = OracleMultiScatterOnTable(dielectric);
                const BrdfFn v2Model = [&](const glm::dvec3& a, const glm::dvec3& b)
                { return ss(a, b) + ms(a, b); };
                const Quadrature v2D = Albedo(Engine::ClosureV2Brdf(dielectric), v, alpha, 0, 2.5e-4, 512u);
                const Quadrature v2M = Albedo(v2Model, v, alpha, 0, 2.5e-4, 512u);
                EXPECT_NEAR(v2D.Value, v2M.Value,
                            std::max(kFloor, 4.0 * (v2D.ErrorEstimate + v2M.ErrorEstimate)) +
                                0.5 * NdfConditioning(alpha) * v2M.Value)
                    << "ClosureV2 white dielectric, " << where << ": engine albedo " << v2D.Value
                    << " is not the model's " << v2M.Value;
                if (v2M.Value > v2DielectricMax)
                {
                    v2DielectricMax = v2M.Value;
                    v2DielectricWhere = where;
                }
                std::cout << "[ oracle ] white albedo " << where << ": Legacy metal " << legacyMetal.Value
                          << ", v2 metal " << v2Metal.Value << ", Legacy dielectric " << legacyD.Value << " (model "
                          << legacyModel.Value << "), v2 dielectric " << v2D.Value << " (model " << v2M.Value << ")\n";
            }
        }
        std::cout << "[ oracle ] energy conservation: max white-metal albedo Legacy " << legacyMetalMax
                  << ", ClosureV2 " << v2MetalMax << "; white-dielectric MODEL albedo max Legacy "
                  << legacyDielectricMax << " (" << legacyDielectricWhere << "), ClosureV2 " << v2DielectricMax << " ("
                  << v2DielectricWhere << ") — above 1 by the (1 - F(v.h)) Lambert heuristic\n";
    }

    // =========================================================================
    // 9. Helmholtz reciprocity f(v, l) == f(l, v) for BOTH closures.
    //
    // Legacy is reciprocal too, which ReferenceBRDF.h's "mildly non-reciprocal"
    // note on CookTorranceBRDF does not reflect: F is evaluated at the HALF
    // vector and h.v == h.l for unit v, l, so kD = 1 - F(h.v) is symmetric, and
    // D(h), the separable G1(v) G1(l) and 4 NdotV NdotL + 1e-4 are symmetric by
    // inspection. (kD at F(n.v) would not be.) Both are held to the f32 bound;
    // the only asymmetry left is the rounding of dot(h, v) against dot(h, l).
    // =========================================================================
    TEST(BsdfIdentityOracleTest, ClosuresAreReciprocal)
    {
        f64 worstV2 = 0.0;
        f64 worstLegacy = 0.0;
        for (const glm::dvec3& albedo : { kGrey, kColoured })
        {
            for (f64 roughness : kRoughnessGrid)
            {
                for (f64 metallic : kMetallicGrid)
                {
                    const MaterialCase m{ albedo, metallic, roughness };
                    const Verdict v2 =
                        CheckReciprocity(Engine::ClosureV2Brdf(m), kF32Relative, 1.0e-7, "ClosureV2 " + Describe(m));
                    const Verdict legacy =
                        CheckReciprocity(Engine::LegacyBrdf(m), kF32Relative, 1.0e-7, "Legacy " + Describe(m));
                    worstV2 = std::max(worstV2, v2.Worst);
                    worstLegacy = std::max(worstLegacy, legacy.Worst);
                    EXPECT_TRUE(v2.Pass) << "ClosureV2 is not reciprocal: " << Report(v2);
                    EXPECT_TRUE(legacy.Pass) << "Legacy is not reciprocal: " << Report(legacy);
                }
            }
        }
        std::cout << "[ oracle ] reciprocity worst relative asymmetry: ClosureV2 " << worstV2 << ", Legacy "
                  << worstLegacy << " (bound " << kF32Relative << ")\n";
    }

    // =========================================================================
    // 10. BSDF::Pdf integrates to the mass its sampler does NOT reject.
    //
    // BSDF::Sample draws the specular lobe with probability pS and the cosine
    // lobe otherwise, and terminates a draw below the horizon. So
    //   int Pdf dw (upper hemisphere) = pS (1 - m_rejected) + (1 - pS),
    // where pS = Engine::SpecularProbability is a POLICY input (any value in
    // (0, 1) is unbiased, so it is not re-derived) and m_rejected is the
    // ORACLE's mass of the specular draws that fail:
    //   * ClosureV2: VNDF draws at alpha = ClosureV2Alpha(r) reflecting below
    //     the horizon — Oracle::VndfBelowHorizonMass;
    //   * Legacy: D cos draws at SamplingRoughness = clamp(r, 0.04, 1) with
    //     v.m <= 0 or a reflection below the horizon — the oracle's D cos
    //     quadrature of that indicator.
    // Bound: max(1e-3, 4 x the estimates) + pS (1 - m_rejected)
    // NdfConditioning(alpha) / 2 — the cos-form D's integral bias, measured
    // here at normal incidence as +3.5-3.8 % of the specular mass at roughness
    // 0 / 0.04 and +1.3 % at 0.05, the same effect as test 7's furnace.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, PdfNormalisesToTheSamplersAcceptedMass)
    {
        constexpr f64 kFloor = 1.0e-3;
        const std::vector<f64> roughnesses{ 0.0, 0.05, 0.15, 0.5, 1.0 };
        const std::vector<f64> cosines{ 0.02, 0.05, 0.5, 1.0 };
        f64 worst = 0.0;
        f64 worstSpecularExcess = 0.0;
        for (const PBRModel model : { PBRModel::ClosureV2, PBRModel::Legacy })
        {
            const char* name = model == PBRModel::ClosureV2 ? "ClosureV2" : "Legacy";
            for (f64 roughness : roughnesses)
            {
                const f64 sampling = std::clamp(roughness, 0.04, 1.0);
                const f64 alpha = sampling * sampling; // ClosureV2Alpha and Legacy's SamplingRoughness agree
                for (f64 cosV : cosines)
                {
                    const glm::dvec3 v = Direction(cosV, 0.0);
                    const glm::dvec3 vr = AsReceived(v);
                    const Quadrature rejected = model == PBRModel::ClosureV2
                                                    ? VndfBelowHorizonMass(vr, alpha)
                                                    : IntegrateOverNdf(alpha, 1024, 256,
                                                                       [&](const glm::dvec3& mm) -> f64
                                                                       {
                                                                           if (glm::dot(vr, mm) <= 0.0)
                                                                               return 1.0;
                                                                           return Reflect(vr, mm).z <= 0.0 ? 1.0 : 0.0;
                                                                       });
                    for (f64 metallic : { 0.0, 1.0 })
                    {
                        const MaterialCase m{ kGrey, metallic, roughness };
                        const f64 pS = Engine::SpecularProbability(m, model);
                        const f64 accepted = 1.0 - rejected.Value;
                        const f64 expected = pS * accepted + (1.0 - pS);
                        const PdfFn pdf = Engine::BsdfPdf(m, model);
                        const Quadrature q =
                            IntegrateMisAdaptive(v, alpha, [&](const glm::dvec3& l)
                                                 { return pdf(v, l); }, 2.5e-4, 512u);
                        const f64 err = std::abs(q.Value - expected);
                        const f64 bound = std::max(kFloor, 4.0 * (q.ErrorEstimate + pS * rejected.ErrorEstimate)) +
                                          pS * accepted * 0.5 * NdfConditioning(alpha);
                        worst = std::max(worst, err);
                        worstSpecularExcess = std::max(worstSpecularExcess, (q.Value - expected) / (pS * accepted));
                        EXPECT_LE(err, bound) << name << ", " << Describe(m) << ", cos v " << cosV
                                              << ": int Pdf = " << q.Value << " (quadrature error " << q.ErrorEstimate
                                              << ") but the sampler accepts " << expected << " = pS " << pS
                                              << " x (1 - rejected " << rejected.Value << ", error "
                                              << rejected.ErrorEstimate << ") + (1 - pS)";
                    }
                }
            }
        }
        std::cout << "[ oracle ] pdf normalisation: worst |int Pdf - accepted mass| " << worst
                  << ", largest excess of the specular density's integral over its accepted mass "
                  << worstSpecularExcess << " (relative)\n";
    }

    // =========================================================================
    // 11. The specular densities, pointwise, against their definitions:
    //   * PdfGGXVNDF = D_v(m) / (4 v.m), D_v of [Heitz18] eq. 1 and the
    //     reflection Jacobian of [Walter07] eq. 14, at alpha = ClosureV2Alpha;
    //   * PdfGGX at SamplingRoughness = D(m) cos(m) / (4 v.m), the density of a
    //     D cos half-vector draw reflected about v.
    // Both use the cos-form D, so they carry NdfConditioning on top of f32.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, SpecularPdfsMatchTheirOracleDensities)
    {
        f64 worstVndf = 0.0;
        f64 worstGgx = 0.0;
        for (f64 roughness : { 0.0, 0.05, 0.15, 0.3, 0.5, 0.75, 1.0 })
        {
            const f64 sampling = std::clamp(roughness, 0.04, 1.0);
            const f64 alpha = sampling * sampling; // v2 and Legacy sampling share the 0.04 floor
            const PdfFn vndf = Engine::VndfPdf(sampling);
            const PdfFn ggx = EngineLegacySpecularPdf(roughness);
            const f64 tol = kF32Relative + NdfConditioning(alpha);
            for (const auto& [vIn, lIn] : DirectionPairs())
            {
                const glm::dvec3 v = AsReceived(vIn);
                const glm::dvec3 l = AsReceived(lIn);
                const glm::dvec3 m = HalfVector(v, l);
                const f64 expectedVndf = GgxVisibleNormalDensity(v, m, alpha) * ReflectionJacobian(v, m);
                const f64 expectedGgx = GgxD(m.z, alpha) * m.z * ReflectionJacobian(v, m);
                const f64 gotVndf = vndf(vIn, lIn);
                const f64 gotGgx = ggx(vIn, lIn);
                const f64 relV = std::abs(gotVndf - expectedVndf) / std::max(expectedVndf, 1.0e-30);
                const f64 relG = std::abs(gotGgx - expectedGgx) / std::max(expectedGgx, 1.0e-30);
                worstVndf = std::max(worstVndf, relV);
                worstGgx = std::max(worstGgx, relG);
                EXPECT_LE(relV, tol) << "PdfGGXVNDF, roughness " << roughness << ", v " << Describe(v) << ", l "
                                     << Describe(l) << ": " << gotVndf << " vs D_v / (4 v.m) = " << expectedVndf;
                EXPECT_LE(relG, tol) << "PdfGGX, roughness " << roughness << ", v " << Describe(v) << ", l "
                                     << Describe(l) << ": " << gotGgx << " vs D cos / (4 v.m) = " << expectedGgx;
            }
        }
        std::cout << "[ oracle ] specular pdfs pointwise: worst relative VNDF " << worstVndf << ", D cos " << worstGgx
                  << "\n";
    }

    // =========================================================================
    // 12. HDR: the f32 product f L cos the integrator forms stays finite and
    //     within tolerance of the f64 oracle at radiance 1, 1e3 and 65504 (the
    //     fp16 maximum, the largest value an RGBA16F buffer hands the shading),
    //     across the grid and at the near-mirror peak of roughness 0, 0.04 and
    //     0.05 (h = n exactly), where D = 1 / (pi alpha^2) reaches 1.2e5.
    //     Expected: the single-scatter model plus Kulla-Conty on the table
    //     (test 2a); tolerance: test 1's for the first, test 2a's for the
    //     second, both scaled by L cos. The largest product is reported against
    //     the fp16 maximum: an unclamped f L cos at the peak does not fit an
    //     RGBA16F accumulation target.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, HdrRadianceStaysFiniteAndAccurate)
    {
        std::vector<std::pair<glm::dvec3, glm::dvec3>> pairs = DirectionPairs();
        for (f64 mu : { 0.02, 0.1, 0.5, 0.9 })
            pairs.emplace_back(Direction(mu, 0.3), Direction(mu, 0.3 + kPi)); // mirror pair: h = n
        pairs.emplace_back(glm::dvec3(0.0, 0.0, 1.0), glm::dvec3(0.0, 0.0, 1.0));
        f64 worst = 0.0;
        f64 largestProduct = 0.0;
        for (f64 radiance : { 1.0, 1.0e3, 65504.0 })
        {
            for (f64 roughness : { 0.0, 0.04, 0.05, 0.3, 1.0 })
            {
                for (f64 metallic : { 0.0, 1.0 })
                {
                    const MaterialCase m{ kColoured, metallic, roughness };
                    const f64 ssRel = kF32Relative + NdfConditioning(ClosureV2Alpha(roughness));
                    const BrdfFn ss = OracleClosureV2SingleScatter(m);
                    const BrdfFn ms = OracleMultiScatterOnTable(m);
                    for (const auto& [vIn, lIn] : pairs)
                    {
                        const glm::vec3 vf = Engine::ToF32(vIn);
                        const glm::vec3 lf = Engine::ToF32(lIn);
                        const glm::vec3 product =
                            PathTracing::ClosureV2Evaluate(Engine::kNormal, vf, lf, Engine::ToF32(m.Albedo),
                                                           static_cast<f32>(m.Metallic), static_cast<f32>(roughness)) *
                            static_cast<f32>(radiance) * lf.z;
                        const glm::dvec3 fs = ss(vIn, lIn);
                        const glm::dvec3 fm = ms(vIn, lIn);
                        const f64 scale = radiance * static_cast<f64>(lf.z);
                        const f64 msRel = kF32Relative + MultiScatterEntryRelative(vIn.z, lIn.z, roughness);
                        for (int c = 0; c < 3; ++c)
                        {
                            const f64 expected = (fs[c] + fm[c]) * scale;
                            const auto got = static_cast<f64>(product[c]);
                            const f64 bound = (ssRel * fs[c] + msRel * fm[c] + 1.0e-6) * scale;
                            largestProduct = std::max(largestProduct, got);
                            worst = std::max(worst, std::abs(got - expected) / std::max(expected, 1.0e-30));
                            EXPECT_TRUE(std::isfinite(got))
                                << "radiance " << radiance << ", " << Describe(m) << ", v " << Describe(vIn) << ", l "
                                << Describe(lIn) << ": f L cos is not finite in f32";
                            EXPECT_LE(std::abs(got - expected), bound)
                                << "radiance " << radiance << ", " << Describe(m) << ", v " << Describe(vIn) << ", l "
                                << Describe(lIn) << ", channel " << c << ": f32 f L cos " << got << " vs f64 oracle "
                                << expected;
                        }
                    }
                }
            }
        }
        std::cout << "[ oracle ] HDR: worst relative |f32 - f64| " << worst << ", largest f L cos " << largestProduct
                  << " (fp16 max 65504)\n";
    }
} // namespace OloEngine::Tests::Oracle
