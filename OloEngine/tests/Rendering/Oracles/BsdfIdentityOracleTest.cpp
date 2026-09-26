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
//   1. ClosureV2SingleScatterSpecularMatchesTheModel — ClosureV2Evaluate
//      without its two table-driven terms IS GGX + height-correlated Smith +
//      Schlick. f32-tight.
//   2. ClosureV2TableTermsAreTheModelOnItsTable — the Kulla-Conty lobe and
//      the energy-conserving diffuse (#1479) it adds are [KullaConty17] and
//      the coupled Lambert evaluated on the energy table, and how far the
//      table's resolution puts them from the model on the true energies.
//   3. LegacyMatchesItsFrozenConventions — CookTorranceBRDF is the oracle with
//      the default LegacyConventions: the frozen version (AC 6).
//   4. LegacyDepartsFromThePhysicalModelByDesign — how far Legacy is from the
//      physics at near-mirror roughness and at grazing, measured, direction
//      and size asserted, so a silent "correction" fails.
//   5. EnergyTableEntriesMatchAnIndependentQuadrature — every stored node of
//      the generated table (both moments) against quadrature that does not
//      use the VNDF sampler the table was generated with.
//   6. EnergyLookupErrorIsInterpolation — between the nodes: the lookup is the
//      bilinear interpolation of correct entries on the node-centred grid
//      (#1478: no edge clamp left), and what that costs against the true
//      energies, over the whole domain.
//   7. WhiteFurnaceIsTheModelPlusTheTablesPredictedError — the v2 white metal:
//      its single-scatter part integrates to the oracle's E(mu), the full
//      closure departs from the model's 1 by what test 6 predicts, and that
//      departure is bounded everywhere, r = 1 and grazing included (#1478).
//   8. EnergyConservation — metals and dielectrics never reflect more than
//      they receive beyond the table's predicted error (#1479: a white
//      dielectric's model albedo is exactly 1).
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
// left is the engine's f32 arithmetic plus the conditioning of its NDF in the
// half vector it forms (kVectorNdfConditioning for the vector-form D that
// ClosureV2Evaluate and both BSDF::Pdf lobes use, kLegacyNdfConditioning for
// the cos-form D of CookTorranceBRDF).
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
#include <tuple>
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

        // Conditioning of the VECTOR-form NDF (ReferenceBRDF.h
        // DistributionGGXSamplingDensity(n, h, r): B = |n x h|^2 + a2 (n.h)^2),
        // which ClosureV2Evaluate and both lobes of BSDF::Pdf use. With
        // t = tan(theta_h) and D = a2 / (pi cos^4 (t^2 + a2)^2),
        //   d ln D / d ln t = 4 sin^2 - 4 t^2 / (t^2 + a2),  |.| <= 4,
        // so a relative error e in tan(theta_h) moves D by at most 4 e, at
        // every alpha. The engine forms h = normalize(v + l) in f32 from the
        // received v, l (the oracle sees the same ones), and with n = +z the
        // cross product (-h.y, h.x, 0) is exact, so t = |s_t| / s_z of s = v + l.
        // Each component of the sum is correctly rounded, 2^-24 relative however
        // much v and l cancel, so |s_t| and s_z carry 2^-24 each; normalize's
        // common scale cancels in the ratio and its per-component products add
        // 2^-24 each. So e <= 4 x 2^-24, and D's relative error is at most
        // 16 x 2^-24 = 9.5e-7 at every alpha, 20x under kF32Relative.
        // Measured: ClosureV2 evaluation 1.7e-6 worst relative (with its f32
        // arithmetic), PdfGGXVNDF 9.1e-7, PdfGGX 8.4e-7. The bound holds for an
        // integral of the lobe too (a weighted mean of values each within it).
        //
        // It is NOT the allowance the SCALAR form would need. That form takes
        // B = nDotH^2 (a2 - 1) + 1, which at the peak is a2 left over from
        // cancelling 1 - nDotH^2 around an nDotH already rounded to a few ulps
        // of 1: 1.2e-6 / alpha^2 relative, 19 % at roughness 0.05. Holding the
        // v2 closure and the pdfs to 9.5e-7 is what makes a revert to the
        // scalar form fail here (it measured +17 % pointwise, +1.3 % on the
        // furnace and +3.5 % on the pdf integral).
        constexpr f64 kVectorNdfConditioning = 16.0 / 16777216.0;

        // Legacy's CookTorranceBRDF still evaluates the SCALAR cos form
        // (DistributionGGX: B = nDotH^2 (a2 - 1) + 1 on nDotH = dot(n, h)).
        // nDotH carries ~2.4e-7 absolute from the f32 normalize and dot, B then
        // ~6e-7 absolute with its own rounding, and D ~ 1/B^2 carries 2 x 6e-7
        // / B relative. Legacy never sees the a2-sized peak, though: its D is
        // floored at pi B^2 >= 1e-4, so wherever the floor is inactive
        // B >= sqrt(1e-4 / pi) = 5.6e-3 and 1.2e-6 / B is at most 2.2e-4.
        constexpr f64 kLegacyNdfConditioning = 1.2e-6 / 5.6e-3;

        // Error of one stored table entry against the true energy (test 5
        // asserts it): half-float rounding, at most half an ulp of the [0.5, 1)
        // binade = 2^-12, plus the generator's quasi-Monte-Carlo error, taken
        // as 2 / N — twice the left-endpoint bias 1 / (2N) of the Hammersley
        // first coordinate i / N — with N = 4096 samples per node and 2048
        // per averages point (tools/OloGgxEnergyTableGen/main.cpp). The same
        // bound holds for both moments a node stores.
        constexpr f64 kHalfRoundingMax = 1.0 / 4096.0;
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
        // Memoised: a pure function of (mu, alpha). `moment` picks the Schlick
        // moment S(mu) instead (Oracle::GgxDirectionalSchlickMoment).
        [[nodiscard]] const Quadrature& OracleMoment(f64 mu, f64 alpha, bool schlick)
        {
            static std::map<std::tuple<f64, f64, bool>, Quadrature> cache;
            const auto key = std::make_tuple(mu, alpha, schlick);
            auto it = cache.find(key);
            if (it == cache.end())
            {
                Quadrature q;
                for (u32 n = 256u; n <= 2048u; n *= 2u)
                {
                    q = schlick ? GgxDirectionalSchlickMoment(mu, alpha, n, n / 2u)
                                : GgxDirectionalAlbedo(mu, alpha, n, n / 2u);
                    if (q.ErrorEstimate < 5.0e-5)
                        break;
                }
                it = cache.emplace(key, q).first;
            }
            return it->second;
        }

        [[nodiscard]] const Quadrature& OracleE(f64 mu, f64 alpha)
        {
            return OracleMoment(mu, alpha, false);
        }

        [[nodiscard]] const Quadrature& OracleS(f64 mu, f64 alpha)
        {
            return OracleMoment(mu, alpha, true);
        }

        // E_avg = 2 int E(mu) mu dmu ([KullaConty17]), Oracle::GgxAverageAlbedo,
        // and S_avg the same of the Schlick moment.
        [[nodiscard]] const Quadrature& OracleEAvg(f64 alpha)
        {
            static std::map<f64, Quadrature> cache;
            auto it = cache.find(alpha);
            if (it == cache.end())
                it = cache.emplace(alpha, GgxAverageAlbedo(alpha)).first;
            return it->second;
        }

        [[nodiscard]] const Quadrature& OracleSAvg(f64 alpha)
        {
            static std::map<f64, Quadrature> cache;
            auto it = cache.find(alpha);
            if (it == cache.end())
                it = cache.emplace(alpha, GgxAverageSchlickMoment(alpha)).first;
            return it->second;
        }

        // The documented f_ms guard (ADR 0016 §5, ReferenceBRDF.h
        // ClosureV2Energy): the lobe is 0 when 1 - E_avg < 1e-4. The oracle
        // reproduces the convention from its OWN E_avg. (At roughness 0.05 the
        // true 1 - E_avg is 4.3e-5, gated on both sides; the edge of the gate
        // moves with the averages row's sampling error.)
        constexpr f64 kMultiScatterGate = 1.0e-4;

        [[nodiscard]] bool MultiScatterGated(f64 roughness)
        {
            return 1.0 - OracleEAvg(ClosureV2Alpha(roughness)).Value < kMultiScatterGate;
        }

        // ---- the table's lookup convention, applied to ORACLE values ---------

        // GgxEnergyTables.h "Conventions" and ReferenceBRDF.h GgxEnergy: a
        // node-centred grid on a square-root axis, node i at (i / 15)^2 in both
        // mu and r (mu = 0 baked at the 1e-4 cosine floor), alpha =
        // ClosureV2Alpha(r_i), read bilinearly — both endpoints are nodes, so
        // nothing clamps (#1478). The same interpolation of the TRUE moments at
        // the nodes is what a table with perfect entries would return: the
        // lookup model.
        constexpr u32 kTableSize = PathTracing::kGgxEnergyTableSize;
        constexpr f64 kMuFloor = 1.0e-4;

        [[nodiscard]] f64 NodeValue(u32 i)
        {
            const f64 x = static_cast<f64>(i) / static_cast<f64>(kTableSize - 1u);
            return x * x;
        }

        [[nodiscard]] f64 NodeMu(u32 i)
        {
            return std::max(NodeValue(i), kMuFloor);
        }

        // One node of the lookup model: (1 - E, S), or the averages' (1 -
        // E_avg, S_avg). Flat, lazily filled copies of the oracle at the
        // nodes: the lookup model is evaluated at every quadrature node of the
        // tests below, far too often for a map lookup per node. NaN marks "not
        // yet".
        [[nodiscard]] glm::dvec2 OracleNode(u32 row, u32 column)
        {
            static std::vector<glm::dvec2> nodes(static_cast<sizet>(kTableSize) * kTableSize,
                                                 glm::dvec2(std::numeric_limits<f64>::quiet_NaN()));
            glm::dvec2& node = nodes[static_cast<sizet>(row) * kTableSize + column];
            if (std::isnan(node.x))
            {
                const f64 alpha = ClosureV2Alpha(NodeValue(row));
                node = { 1.0 - OracleE(NodeMu(column), alpha).Value, OracleS(NodeMu(column), alpha).Value };
            }
            return node;
        }

        [[nodiscard]] glm::dvec2 OracleAvgNode(u32 row)
        {
            static std::vector<glm::dvec2> rows(kTableSize, glm::dvec2(std::numeric_limits<f64>::quiet_NaN()));
            glm::dvec2& node = rows[row];
            if (std::isnan(node.x))
            {
                const f64 alpha = ClosureV2Alpha(NodeValue(row));
                node = { 1.0 - OracleEAvg(alpha).Value, OracleSAvg(alpha).Value };
            }
            return node;
        }

        struct NodeCoordinate
        {
            u32 I0 = 0;
            u32 I1 = 1;
            f64 Fraction = 0.0;
        };

        [[nodiscard]] NodeCoordinate LocateNode(f64 x)
        {
            const f64 c = std::sqrt(std::clamp(x, 0.0, 1.0)) * static_cast<f64>(kTableSize - 1u);
            NodeCoordinate node;
            node.I0 = std::min(static_cast<u32>(c), kTableSize - 2u);
            node.I1 = node.I0 + 1u;
            node.Fraction = c - static_cast<f64>(node.I0);
            return node;
        }

        // (1 - E, S) of the lookup model at (mu, r).
        [[nodiscard]] glm::dvec2 OracleLookup(f64 mu, f64 roughness)
        {
            const NodeCoordinate x = LocateNode(mu);
            const NodeCoordinate y = LocateNode(roughness);
            const glm::dvec2 row0 = glm::mix(OracleNode(y.I0, x.I0), OracleNode(y.I0, x.I1), x.Fraction);
            const glm::dvec2 row1 = glm::mix(OracleNode(y.I1, x.I0), OracleNode(y.I1, x.I1), x.Fraction);
            return glm::mix(row0, row1, y.Fraction);
        }

        [[nodiscard]] glm::dvec2 OracleLookupAvg(f64 roughness)
        {
            const NodeCoordinate y = LocateNode(roughness);
            return glm::mix(OracleAvgNode(y.I0), OracleAvgNode(y.I1), y.Fraction);
        }

        [[nodiscard]] f64 OracleLookupLoss(f64 mu, f64 roughness)
        {
            return OracleLookup(mu, roughness).x;
        }

        [[nodiscard]] f64 OracleLookupLossAvg(f64 roughness)
        {
            return OracleLookupAvg(roughness).x;
        }

        // 2 int L(mu) mu dmu of the lookup model: the numerator of the
        // Kulla-Conty lobe's hemispherical integral. Piecewise smooth in mu, so
        // a 1024-point midpoint rule is exact to ~1e-6.
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

        // The single-scattering microfacet specular alone: the part of
        // ClosureV2Evaluate that reads no table.
        [[nodiscard]] BrdfFn OracleClosureV2Specular(const MaterialCase& m)
        {
            return [m](const glm::dvec3& vIn, const glm::dvec3& lIn)
            {
                const glm::dvec3 v = AsReceived(vIn);
                const glm::dvec3 l = AsReceived(lIn);
                if (v.z <= 0.0 || l.z <= 0.0)
                    return glm::dvec3(0.0);
                return MicrofacetSpecular(v, l, ClosureV2Alpha(m.Roughness), BaseF0(m.Albedo, m.Metallic));
            };
        }

        // The moments the closure reads for (mu_v, mu_l), from the TRUE
        // energies at alpha = ClosureV2Alpha(authored r). The table rows bake
        // that clamp in and are indexed by authored r, so the quantity the
        // table approximates at authored r IS the moment at the clamped alpha.
        [[nodiscard]] ClosureV2Energies TrueEnergies(f64 roughness, f64 muV, f64 muL)
        {
            const f64 alpha = ClosureV2Alpha(roughness);
            ClosureV2Energies e;
            e.EV = OracleE(muV, alpha).Value;
            e.EL = OracleE(muL, alpha).Value;
            e.EAvg = OracleEAvg(alpha).Value;
            e.SV = OracleS(muV, alpha).Value;
            e.SL = OracleS(muL, alpha).Value;
            e.SAvg = OracleSAvg(alpha).Value;
            e.MultiScatter = !MultiScatterGated(roughness);
            return e;
        }

        // The same from the lookup model: what the engine reads if every
        // table entry is right.
        [[nodiscard]] ClosureV2Energies TableEnergies(f64 roughness, f64 muV, f64 muL)
        {
            const glm::dvec2 v = OracleLookup(muV, roughness);
            const glm::dvec2 l = OracleLookup(muL, roughness);
            const glm::dvec2 avg = OracleLookupAvg(roughness);
            ClosureV2Energies e;
            e.EV = 1.0 - v.x;
            e.EL = 1.0 - l.x;
            e.EAvg = 1.0 - avg.x;
            e.SV = v.y;
            e.SL = l.y;
            e.SAvg = avg.y;
            e.MultiScatter = !MultiScatterGated(roughness);
            return e;
        }

        // The table-driven terms — [KullaConty17] and the coupled Lambert — on
        // the true energies or on the lookup model's. `diffuse` picks which.
        [[nodiscard]] BrdfFn OracleTableTerm(const MaterialCase& m, bool onTable, bool diffuse)
        {
            return [m, onTable, diffuse](const glm::dvec3& vIn, const glm::dvec3& lIn)
            {
                const glm::dvec3 v = AsReceived(vIn);
                const glm::dvec3 l = AsReceived(lIn);
                if (v.z <= 0.0 || l.z <= 0.0)
                    return glm::dvec3(0.0);
                const ClosureV2Energies e =
                    onTable ? TableEnergies(m.Roughness, v.z, l.z) : TrueEnergies(m.Roughness, v.z, l.z);
                const ClosureV2TableTerms terms = EvaluateClosureV2TableTerms(m.Albedo, m.Metallic, e);
                return diffuse ? terms.Diffuse : terms.MultiScatter;
            };
        }

        [[nodiscard]] BrdfFn OracleMultiScatterTrue(const MaterialCase& m)
        {
            return OracleTableTerm(m, false, false);
        }

        [[nodiscard]] BrdfFn OracleMultiScatterOnTable(const MaterialCase& m)
        {
            return OracleTableTerm(m, true, false);
        }

        [[nodiscard]] BrdfFn OracleDiffuseTrue(const MaterialCase& m)
        {
            return OracleTableTerm(m, false, true);
        }

        [[nodiscard]] BrdfFn OracleDiffuseOnTable(const MaterialCase& m)
        {
            return OracleTableTerm(m, true, true);
        }

        // The whole v2 model on the table: exact single scatter plus both
        // table-driven terms on the lookup model — what the engine is, if every
        // table entry is right.
        [[nodiscard]] BrdfFn OracleClosureV2OnTable(const MaterialCase& m)
        {
            const BrdfFn specular = OracleClosureV2Specular(m);
            const BrdfFn ms = OracleMultiScatterOnTable(m);
            const BrdfFn diffuse = OracleDiffuseOnTable(m);
            return [specular, ms, diffuse](const glm::dvec3& v, const glm::dvec3& l)
            { return specular(v, l) + ms(v, l) + diffuse(v, l); };
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

        // First-order relative error of the coupled diffuse from the entry
        // error. E_spec = F0 E + (1 - F0) S + F_ms (1 - E) moves by at most
        // |F0 - F_ms| dE + (1 - F0) dS <= 2 x the entry error (F0, F_ms in
        // [0, 1]); the averages add F_ms's own dependence on E_avg, taken as a
        // third entry error. w_d = R_v R_l / R_avg with R = 1 - E_spec, so the
        // relative errors add. Per channel, worst channel.
        [[nodiscard]] f64 DiffuseEntryRelative(const MaterialCase& m, f64 muV, f64 muL)
        {
            const ClosureV2Energies e = TableEnergies(m.Roughness, muV, muL);
            const glm::dvec3 f0 = BaseF0(m.Albedo, m.Metallic);
            const glm::dvec3 fMs = e.MultiScatter ? KullaContyFresnel(e.EAvg, f0) : glm::dvec3(0.0);
            const glm::dvec3 rv = glm::dvec3(1.0) - SpecularAlbedo(e.EV, e.SV, f0, fMs);
            const glm::dvec3 rl = glm::dvec3(1.0) - SpecularAlbedo(e.EL, e.SL, f0, fMs);
            const glm::dvec3 ra = glm::dvec3(1.0) - SpecularAlbedo(e.EAvg, e.SAvg, f0, fMs);
            f64 worst = 0.0;
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, 2.0 * kEntryError / std::max(rv[c], 1.0e-12) +
                                            2.0 * kEntryError / std::max(rl[c], 1.0e-12) +
                                            3.0 * kAvgEntryError / std::max(ra[c], 1.0e-12));
            return worst;
        }

        [[nodiscard]] BrdfFn OracleLegacy(const MaterialCase& m, const LegacyConventions& c = {})
        {
            return [m, c](const glm::dvec3& vIn, const glm::dvec3& lIn)
            { return LegacyBrdf(AsReceived(vIn), AsReceived(lIn), m.Albedo, m.Metallic, m.Roughness, c); };
        }

        // ---- local engine adapters (not in EngineBsdfAdapters.h) -------------

        // PathTracing::ClosureV2Energy exactly as ClosureV2Evaluate calls it:
        // clamped cosines, AUTHORED roughness into the lookup. `diffuse` picks
        // the Lambert term Evaluate composes from its coupling —
        // DiffuseCoupling (1 - metallic) albedo / pi — over the Kulla-Conty lobe.
        [[nodiscard]] BrdfFn EngineClosureV2TableTerm(const MaterialCase& m, bool diffuse)
        {
            return [m, diffuse](const glm::dvec3& v, const glm::dvec3& l)
            {
                const glm::vec3 vf = Engine::ToF32(v);
                const glm::vec3 lf = Engine::ToF32(l);
                const glm::vec3 albedo = Engine::ToF32(m.Albedo);
                const auto metallic = static_cast<f32>(m.Metallic);
                const glm::vec3 f0 = glm::mix(glm::vec3(PathTracing::kDefaultDielectricF0), albedo, metallic);
                const PathTracing::ClosureV2EnergyTerms terms = PathTracing::ClosureV2Energy(
                    std::max(vf.z, 0.0f), std::max(lf.z, 0.0f), static_cast<f32>(m.Roughness), f0);
                if (!diffuse)
                    return Engine::ToF64(terms.MultiScatter);
                const glm::vec3 kD = terms.DiffuseCoupling * (1.0f - metallic);
                return Engine::ToF64(kD * albedo * PathTracing::kInvPi);
            };
        }

        [[nodiscard]] BrdfFn EngineClosureV2MultiScatter(const MaterialCase& m)
        {
            return EngineClosureV2TableTerm(m, false);
        }

        [[nodiscard]] BrdfFn EngineClosureV2Diffuse(const MaterialCase& m)
        {
            return EngineClosureV2TableTerm(m, true);
        }

        // ClosureV2Evaluate minus both table-driven terms it adds: the
        // single-scatter specular, as the engine composes it.
        [[nodiscard]] BrdfFn EngineClosureV2Specular(const MaterialCase& m)
        {
            const BrdfFn full = Engine::ClosureV2Brdf(m);
            const BrdfFn ms = EngineClosureV2MultiScatter(m);
            const BrdfFn diffuse = EngineClosureV2Diffuse(m);
            return [full, ms, diffuse](const glm::dvec3& v, const glm::dvec3& l)
            { return full(v, l) - ms(v, l) - diffuse(v, l); };
        }

        // PathTracing::PdfGGX at BSDF::SamplingRoughness, over l — the Legacy
        // specular density exactly as BSDF::Pdf forms it: the vector overload,
        // n = +z, h = normalize(v + l) in f32.
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
                return static_cast<f64>(PathTracing::PdfGGX(Engine::kNormal, vf, h, rs));
            };
        }

        // ---- directional albedo of a term that depends on l only via n.l ----

        // 2 pi int_0^1 g(mu_l) mu_l dmu_l, a 2048-point midpoint rule: the
        // hemispherical integral of f cos for a term with no azimuthal
        // dependence (both table-driven terms).
        template<typename G>
        [[nodiscard]] f64 IntegrateOverCosine(G&& g)
        {
            constexpr u32 n = 2048u;
            f64 sum = 0.0;
            for (u32 i = 0; i < n; ++i)
            {
                const f64 mu = (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
                sum += g(mu) * mu;
            }
            return 2.0 * kPi * sum / static_cast<f64>(n);
        }

        // ---- white-metal furnace --------------------------------------------

        // int f_ms cos dw of the ENGINE's lobe for view v.
        [[nodiscard]] f64 EngineMultiScatterAlbedo(const MaterialCase& m, const glm::dvec3& v)
        {
            const BrdfFn ms = EngineClosureV2MultiScatter(m);
            return IntegrateOverCosine([&](f64 mu) { return ms(v, Direction(mu, 0.0)).x; });
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

        // The white dielectric (albedo 1, metallic 0), whose MODEL albedo is
        // exactly 1 (#1479): specular E_spec(mu_v) plus diffuse
        // 1 - E_spec(mu_v). On the table it is
        //   E_ss(mu_v) + F_ms L_v mean(L) / L_avg + R_v mean(R) / R_avg,
        // R = 1 - E_spec of the lookup model, mean(x) = 2 int x mu dmu: the
        // exact single scatter plus both table terms' closed-form integrals.
        struct DielectricPrediction
        {
            f64 OnTable = 1.0;
            f64 EntryBound = 0.0;
        };

        [[nodiscard]] DielectricPrediction PredictWhiteDielectric(f64 muV, f64 roughness)
        {
            const f64 alpha = ClosureV2Alpha(roughness);
            const glm::dvec3 f0 = BaseF0(glm::dvec3(1.0), 0.0);
            const bool ms = !MultiScatterGated(roughness);
            const glm::dvec2 avg = OracleLookupAvg(roughness);
            const f64 fMs = ms ? KullaContyFresnel(1.0 - avg.x, f0).x : 0.0;
            auto remaining = [&](const glm::dvec2& node)
            { return 1.0 - SpecularAlbedo(1.0 - node.x, node.y, f0, glm::dvec3(fMs)).x; };

            constexpr u32 n = 1024u;
            f64 meanRemaining = 0.0;
            for (u32 i = 0; i < n; ++i)
            {
                const f64 mu = (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
                meanRemaining += remaining(OracleLookup(mu, roughness)) * mu;
            }
            meanRemaining = 2.0 * meanRemaining / static_cast<f64>(n);

            const f64 singleScatter =
                f0.x * OracleE(muV, alpha).Value + (1.0 - f0.x) * OracleS(muV, alpha).Value;
            const glm::dvec2 view = OracleLookup(muV, roughness);
            const f64 multiScatter =
                ms ? fMs * view.x * OracleLookupMeanLoss(roughness) / std::max(avg.x, 1.0e-12) : 0.0;
            const f64 diffuseAlbedo = remaining(view) * meanRemaining / std::max(remaining(avg), 1.0e-12);

            DielectricPrediction p;
            p.OnTable = singleScatter + multiScatter + diffuseAlbedo;
            p.EntryBound = diffuseAlbedo * DiffuseEntryRelative({ glm::dvec3(1.0), 0.0, roughness }, muV, muV) +
                           multiScatter * MultiScatterEntryRelative(muV, muV, roughness);
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
    // 1. ClosureV2Evaluate, minus its two table-driven terms, IS the model:
    //    D and G2 of [Walter07]/[Heitz14] and Schlick F at v.h. Only f32
    //    arithmetic and the vector-form NDF's conditioning
    //    (kVectorNdfConditioning) separate the two. The subtraction also pins
    //    the COMPOSITION: a Lambert weight that is not the coupling test 2
    //    checks, or a table term added twice, leaves a residue here.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, ClosureV2SingleScatterSpecularMatchesTheModel)
    {
        f64 worst = 0.0;
        for (const glm::dvec3& albedo : { kGrey, kColoured })
        {
            for (f64 roughness : kRoughnessGrid)
            {
                for (f64 metallic : kMetallicGrid)
                {
                    const MaterialCase m{ albedo, metallic, roughness };
                    const f64 relTol = kF32Relative + kVectorNdfConditioning;
                    const Verdict verdict = CheckEvaluationAgainstModel(
                        EngineClosureV2Specular(m), OracleClosureV2Specular(m), relTol, 1.0e-7, Describe(m));
                    worst = std::max(worst, verdict.WorstMeasured);
                    EXPECT_TRUE(verdict.Pass) << "ClosureV2Evaluate's single-scatter specular departs from the "
                                                 "independent model: "
                                              << Report(verdict);
                }
            }
        }
        std::cout << "[ oracle ] ClosureV2 single-scatter specular vs model: worst relative " << worst << "\n";
    }

    // =========================================================================
    // 2. The two terms ClosureV2Evaluate reads from the energy table: the
    //    Kulla-Conty lobe and the energy-conserving Lambert (#1479).
    //
    //   (a) Each IS the model on its table: [KullaConty17] and the coupled
    //       Lambert w_d = (1 - E_spec(v)) (1 - E_spec(l)) / (1 - E_spec_avg)
    //       fed with the lookup model's moments, to the entry error
    //       (kEntryError, kAvgEntryError) propagated to first order.
    //   (b) What the table's resolution costs, lookup model against the TRUE
    //       moments. The grid's grazing cosine 0.02 is its own regime: 1 - E
    //       peaks near mu ~ alpha, narrower than the first node spacings at
    //       low roughness, so a cosine there is where any 16-node axis loses
    //       most. Measured worst over this grid and both albedos -> bound:
    //         Kulla-Conty, both cosines >= 0.05  0.183 (r 0.15, cos 0.1 / 0.35,
    //                                            where 1 - E is ~0.01 and
    //                                            steep in r) -> 0.2
    //         Kulla-Conty, a cosine at 0.02      0.212 -> 0.25 (the
    //                                            cell-centred table's edge
    //                                            clamp read 1.05 here)
    //         coupled Lambert, cosines >= 0.05   0.034 (r 0.15, cos 0.1 / 0.1)
    //                                            -> 0.04
    //         coupled Lambert, a cosine at 0.02  0.30 (r 0.05, both cosines
    //                                            0.02, where the term is
    //                                            ~1e-3: the specular lobe
    //                                            takes almost everything)
    //                                            -> 0.35
    //       All over an absolute 1e-4, below which a term carries nothing.
    //       What these add up to in energy is test 7's and test 8's furnaces,
    //       both within 1.1 % of 1.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, ClosureV2TableTermsAreTheModelOnItsTable)
    {
        constexpr f64 kResolutionAbsolute = 1.0e-4;
        struct Term
        {
            const char* Name;
            bool Diffuse;
            f64 InteriorBound;
            f64 GrazingBound;
            f64 WorstOnTable = 0.0;
            f64 WorstInterior = 0.0;
            f64 WorstGrazing = 0.0;
            std::string InteriorWhere;
            std::string GrazingWhere;
        };
        Term terms[] = { { "Kulla-Conty lobe", false, 0.2, 0.25 }, { "coupled Lambert", true, 0.04, 0.35 } };

        for (Term& term : terms)
        {
            for (const glm::dvec3& albedo : { kGrey, kColoured })
            {
                for (f64 roughness : kRoughnessGrid)
                {
                    for (f64 metallic : kMetallicGrid)
                    {
                        const MaterialCase m{ albedo, metallic, roughness };
                        const BrdfFn impl = EngineClosureV2TableTerm(m, term.Diffuse);
                        const BrdfFn onTable = OracleTableTerm(m, true, term.Diffuse);
                        const BrdfFn truth = OracleTableTerm(m, false, term.Diffuse);
                        Verdict a;
                        Verdict interior;
                        Verdict grazing;
                        for (const auto& [v, l] : DirectionPairs())
                        {
                            const glm::dvec3 got = impl(v, l);
                            const glm::dvec3 table = onTable(v, l);
                            const glm::dvec3 exact = truth(v, l);
                            const f64 entryRel = kF32Relative + (term.Diffuse ? DiffuseEntryRelative(m, v.z, l.z)
                                                                              : MultiScatterEntryRelative(v.z, l.z, roughness));
                            const bool isGrazing = std::min(v.z, l.z) < 0.05;
                            const std::string where = Describe(m) + ": v " + Describe(v) + ", l " + Describe(l);
                            for (int c = 0; c < 3; ++c)
                            {
                                const f64 errA = std::abs(got[c] - table[c]);
                                a.Record(std::isfinite(got[c]) && errA <= entryRel * table[c] + 1.0e-6,
                                         errA / std::max(table[c], 1.0e-12), entryRel,
                                         where + ", channel " + std::to_string(c) + ": impl " + Fmt(got[c]) +
                                             ", the model on the table " + Fmt(table[c]));

                                const f64 relTol = isGrazing ? term.GrazingBound : term.InteriorBound;
                                const f64 errB = std::abs(table[c] - exact[c]);
                                (isGrazing ? grazing : interior)
                                    .Record(errB <= relTol * exact[c] + kResolutionAbsolute,
                                            errB / (exact[c] + kResolutionAbsolute), relTol,
                                            where + ", channel " + std::to_string(c) + ": on the table " +
                                                Fmt(table[c]) + ", on the true moments " + Fmt(exact[c]));
                            }
                        }
                        term.WorstOnTable = std::max(term.WorstOnTable, a.WorstMeasured);
                        if (interior.Worst > term.WorstInterior)
                        {
                            term.WorstInterior = interior.Worst;
                            term.InteriorWhere = interior.Detail;
                        }
                        if (grazing.Worst > term.WorstGrazing)
                        {
                            term.WorstGrazing = grazing.Worst;
                            term.GrazingWhere = grazing.Detail;
                        }
                        EXPECT_TRUE(a.Pass) << "the engine's " << term.Name
                                            << " is not the model on its own table: " << Report(a);
                        EXPECT_TRUE(interior.Pass) << "the table's resolution costs the " << term.Name
                                                   << " more than stated (both cosines >= 0.05): " << Report(interior);
                        EXPECT_TRUE(grazing.Pass) << "the table's resolution costs the " << term.Name
                                                  << " more than stated at a grazing cosine: " << Report(grazing);
                    }
                }
            }
            std::cout << "[ oracle ] ClosureV2 " << term.Name << ": vs the model on its table worst relative "
                      << term.WorstOnTable << "; table vs true moments: cosines >= 0.05 " << term.WorstInterior
                      << " (bound " << term.InteriorBound << ", at " << term.InteriorWhere << "), grazing "
                      << term.WorstGrazing << " (bound " << term.GrazingBound << ", at " << term.GrazingWhere << ")\n";
        }
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
                    worst = std::max(worst, verdict.WorstMeasured);
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
    // 5. Every stored node of the generated table against an independent
    //    quadrature.
    //
    // GgxEnergyTables.h "Conventions": node (row k, column j) holds
    // (1 - E(mu_j), S(mu_j)) at alpha = clamp(r_k, 0.04, 1)^2, mu_j = (j/15)^2
    // (mu_0 baked at 1e-4), r_k = (k/15)^2; the averages row holds
    // (1 - E_avg(alpha_k), S_avg(alpha_k)). The bound per entry is half an ulp
    // of ITS half-float binade + the generator's QMC error (kQmcEss / kQmcAvg)
    // + 4 x the oracle's error estimate. Nothing here uses the VNDF sampler the
    // table was generated with; the oracle is the D cos half-vector quadrature
    // of [Heitz14] eq. 99, cross-checked on a grazing column by the MIS rule,
    // whose uniform half is aligned with the horizon.
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

        const char* kMomentName[] = { "1 - E", "S" };
        Verdict entries[2];
        Verdict averages[2];
        std::ostringstream largest;
        for (u32 k = 0; k < kTableSize; ++k)
        {
            const f64 alpha = ClosureV2Alpha(NodeValue(k));
            for (u32 j = 0; j < kTableSize; ++j)
            {
                const glm::vec2 stored = PathTracing::GgxEnergyEntry(k * kTableSize + j);
                const Quadrature* oracle[2] = { &OracleE(NodeMu(j), alpha), &OracleS(NodeMu(j), alpha) };
                for (int moment = 0; moment < 2; ++moment)
                {
                    const auto value = static_cast<f64>(stored[moment]);
                    const f64 expected = moment == 0 ? 1.0 - oracle[0]->Value : oracle[1]->Value;
                    const f64 deviation = std::abs(value - expected);
                    const f64 bound = halfRounding(value) + kQmcEss + 4.0 * oracle[moment]->ErrorEstimate;
                    if (deviation > 2.5e-4)
                        largest << " " << kMomentName[moment] << "[" << k << "," << j << "] " << Fmt(value - expected);
                    entries[moment].Record(deviation <= bound, deviation, bound,
                                           "row " + std::to_string(k) + " (r " + Fmt(NodeValue(k)) + "), column " +
                                               std::to_string(j) + " (mu " + Fmt(NodeMu(j)) + "): stored " +
                                               kMomentName[moment] + " " + Fmt(value) + ", oracle " + Fmt(expected) +
                                               " (error estimate " + Fmt(oracle[moment]->ErrorEstimate) + ")");
                }
            }
            const glm::vec2 stored = PathTracing::GgxEnergyAvgEntry(k);
            const Quadrature* oracle[2] = { &OracleEAvg(alpha), &OracleSAvg(alpha) };
            for (int moment = 0; moment < 2; ++moment)
            {
                const auto value = static_cast<f64>(stored[moment]);
                const f64 expected = moment == 0 ? 1.0 - oracle[0]->Value : oracle[1]->Value;
                const f64 deviation = std::abs(value - expected);
                const f64 bound = halfRounding(value) + kQmcAvg + 4.0 * oracle[moment]->ErrorEstimate;
                averages[moment].Record(deviation <= bound, deviation, bound,
                                        "row " + std::to_string(k) + " (r " + Fmt(NodeValue(k)) + "): stored " +
                                            kMomentName[moment] + "_avg " + Fmt(value) + ", oracle " + Fmt(expected) +
                                            " (error estimate " + Fmt(oracle[moment]->ErrorEstimate) + ")");
            }
        }
        for (int moment = 0; moment < 2; ++moment)
        {
            EXPECT_TRUE(entries[moment].Pass)
                << "a GgxEnergyTables.h " << kMomentName[moment]
                << " entry disagrees with the independent quadrature beyond half rounding + the generator's "
                   "sampling error: "
                << Report(entries[moment]);
            EXPECT_TRUE(averages[moment].Pass) << "an averages-row " << kMomentName[moment]
                                               << " entry disagrees with the independent quadrature: "
                                               << Report(averages[moment]);
            std::cout << "[ oracle ] table " << kMomentName[moment] << ": worst |stored - oracle| "
                      << entries[moment].Worst << " at " << entries[moment].Detail << "; averages row worst "
                      << averages[moment].Worst << " at " << averages[moment].Detail << "\n";
        }
        std::cout << "[ oracle ]   entries off by > 2.5e-4 [row,col] stored - oracle:" << largest.str() << "\n";

        // Column 2 (mu = 4/225 = 0.018), the first column where the loss peak
        // of the low-roughness rows sits: the two oracle rules must agree.
        for (u32 k = 0; k < kTableSize; ++k)
        {
            const f64 alpha = ClosureV2Alpha(NodeValue(k));
            const glm::dvec3 v = Direction(NodeMu(2), 0.0);
            const Quadrature mis = IntegrateMisAdaptive(
                v, alpha,
                [&](const glm::dvec3& l) -> f64
                {
                    if (l.z <= 0.0)
                        return 0.0;
                    return GgxD(HalfVector(v, l).z, alpha) * GgxG2HeightCorrelated(v.z, l.z, alpha) / (4.0 * v.z);
                },
                5.0e-5, 2048u);
            const Quadrature& half = OracleE(NodeMu(2), alpha);
            // 4e-4 floor: the two rules' estimates under-state their residual
            // at grazing (measured 3.1e-4 apart at row 6 against estimates of
            // 2-3e-5); 4e-4 is still half the entry bound above (kEntryError,
            // 7.3e-4), so agreement at it is what the check needs.
            EXPECT_NEAR(mis.Value, half.Value, std::max(4.0e-4, 4.0 * (mis.ErrorEstimate + half.ErrorEstimate)))
                << "row " << k << ", mu " << NodeMu(2) << ": the two oracle rules for E disagree (MIS " << mis.Value
                << ", error " << mis.ErrorEstimate << "; half-vector " << half.Value << ", error "
                << half.ErrorEstimate << ")";
        }
    }

    // =========================================================================
    // 6. Between the nodes. GgxEnergy(mu, r) is:
    //   (a) the bilinear interpolation of correct entries on the node-centred
    //       square-root grid — the lookup model to within kEntryError — so
    //       everything else it costs is resolution, not a wrong number. Both
    //       endpoints are nodes (#1478): there is no edge clamp any more, and
    //       no edge regime;
    //   (b) that resolution against the true moments, measured worst over this
    //       grid -> bound (the measurement prints below):
    //         1 - E, mu >= 0.05          0.0066 (mu 0.0625, r 0.25) -> 0.009
    //         1 - E, mu < 0.05           0.062 (mu 0.002, r 0) -> 0.07: the
    //                                    loss peak at mu ~ alpha is narrower
    //                                    than the first node spacings at low
    //                                    r (the cell-centred table read 0.072
    //                                    off below mu = 1/32)
    //         S, mu >= 0.05              0.0091 (mu 0.0625, r 0.1) -> 0.012
    //         S, mu < 0.05               0.055 (mu 0.002, r 0) -> 0.065
    //       The averages row: 1 - E_avg and S_avg within 2.8e-3 (r 0.5, where
    //       the square-root axis spaces nodes 0.093 apart) -> 3.5e-3, r = 1
    //       included (1 - E_avg was 0.028 off there on the cell-centred grid).
    // =========================================================================
    TEST(BsdfIdentityOracleTest, EnergyLookupErrorIsInterpolation)
    {
        const std::vector<f64> mus{ 0.0, 0.002, 0.005, 0.02, 0.0625, 0.15, 0.3125, 0.5, 0.71875, 0.9, 0.98, 1.0 };
        const std::vector<f64> roughnesses{ 0.0, 0.02, 0.05, 0.1, 0.15, 0.25, 0.4, 0.5, 0.65, 0.8, 0.9, 0.98, 1.0 };
        constexpr f64 kGrazing = 0.05;
        struct Regime
        {
            const char* Name;
            f64 Bound;
            Verdict V;
        };
        Regime regimes[2][2] = { { { "1 - E, mu >= 0.05", 0.009, {} }, { "1 - E, mu < 0.05", 0.07, {} } },
                                 { { "S, mu >= 0.05", 0.012, {} }, { "S, mu < 0.05", 0.065, {} } } };
        Verdict entries;

        for (f64 r : roughnesses)
        {
            const f64 alpha = ClosureV2Alpha(r);
            for (f64 mu : mus)
            {
                const glm::vec2 lookup = PathTracing::GgxEnergy(static_cast<f32>(mu), static_cast<f32>(r));
                const glm::dvec2 onTable = OracleLookup(mu, r);
                const f64 muTrue = std::max(mu, kMuFloor);
                const Quadrature* oracle[2] = { &OracleE(muTrue, alpha), &OracleS(muTrue, alpha) };
                const glm::dvec2 truth(1.0 - oracle[0]->Value, oracle[1]->Value);
                for (int moment = 0; moment < 2; ++moment)
                {
                    const std::string where = "mu " + Fmt(mu) + ", r " + Fmt(r) + ": lookup " +
                                              Fmt(lookup[moment]) + ", lookup model " + Fmt(onTable[moment]) +
                                              ", true " + Fmt(truth[moment]);
                    const f64 errA = std::abs(static_cast<f64>(lookup[moment]) - onTable[moment]);
                    entries.Record(errA <= kEntryError + 1.0e-4, errA, kEntryError + 1.0e-4, where);

                    Regime& regime = regimes[moment][mu < kGrazing ? 1 : 0];
                    const f64 errB = std::abs(onTable[moment] - truth[moment]);
                    const f64 bound = regime.Bound + 4.0 * oracle[moment]->ErrorEstimate;
                    regime.V.Record(errB <= bound, errB, bound, where);
                }
            }
        }
        EXPECT_TRUE(entries.Pass) << "GgxEnergy is not the bilinear node-centred interpolation of its entries: "
                                  << Report(entries);
        for (auto& moment : regimes)
        {
            for (Regime& regime : moment)
            {
                EXPECT_TRUE(regime.V.Pass) << "the table's " << regime.Name
                                           << " interpolation error exceeds its stated bound: " << Report(regime.V);
                std::cout << "[ oracle ] lookup vs true, " << regime.Name << ": worst " << regime.V.Worst << " at "
                          << regime.V.Detail << "\n";
            }
        }
        std::cout << "[ oracle ] lookup vs lookup model (entries): worst " << entries.Worst << "\n";

        // The averages row: the Kulla-Conty and coupling denominators.
        Verdict avg;
        for (f64 r : roughnesses)
        {
            const glm::vec2 lookup = PathTracing::GgxEnergyAverage(static_cast<f32>(r));
            const f64 alpha = ClosureV2Alpha(r);
            const glm::dvec2 truth(1.0 - OracleEAvg(alpha).Value, OracleSAvg(alpha).Value);
            for (int moment = 0; moment < 2; ++moment)
            {
                const f64 err = std::abs(static_cast<f64>(lookup[moment]) - truth[moment]);
                avg.Record(err <= 3.5e-3, err, 3.5e-3,
                           "r " + Fmt(r) + (moment == 0 ? ": 1 - E_avg " : ": S_avg ") + Fmt(lookup[moment]) +
                               ", true " + Fmt(truth[moment]));
            }
        }
        EXPECT_TRUE(avg.Pass) << "GgxEnergyAverage error: " << Report(avg);
        std::cout << "[ oracle ] lookup vs true averages row: worst " << avg.Worst << " at " << avg.Detail << "\n";
    }

    // =========================================================================
    // 7. The white furnace of the v2 closure: albedo 1, metallic 1, so F0 = 1,
    //    F == 1, F_ms = 1 and the Lambert term is exactly zero.
    //
    //   * single scatter: int f_ss cos = E(mu), the oracle's own quadrature.
    //     Bound: 4 x the two quadrature estimates, floor 5e-4, plus the
    //     vector-form NDF's conditioning, kVectorNdfConditioning x E. (The
    //     scalar cos form read +1.3 % here at roughness 0.05, normal
    //     incidence; this bound rejects it.)
    //   * full closure: [KullaConty17] makes int f_ms cos = 1 - E(mu) exactly
    //     at F_avg = 1, so the MODEL closes to 1 (to E(mu) where the gate drops
    //     the lobe). The engine departs from that by the table's resolution
    //     (test 6), which the lookup model PREDICTS: the assertion is on the
    //     prediction, to the entry error.
    //   * #1478's acceptance: that departure is bounded everywhere on the grid,
    //     r = 1 and cos v 0.02 included. Measured range [0.9902, 1.0064],
    //     worst 0.0064 (r 0.15, cos v 0.1); r = 1 reads 0.9993 at cos v 1 and
    //     1.0002 at cos v 0.02, where the cell-centred table it replaced read
    //     0.9615 and 1.0247. kFurnaceResolution = 0.008.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, WhiteFurnaceIsTheModelPlusTheTablesPredictedError)
    {
        constexpr f64 kSingleScatterFloor = 5.0e-4;
        constexpr f64 kPredictionFloor = 1.0e-3;
        constexpr f64 kFurnaceResolution = 0.008;
        f64 lowest = 2.0;
        f64 highest = 0.0;
        f64 worstPrediction = 0.0;
        f64 worstDeparture = 0.0;
        std::string worstDepartureWhere;
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
                const f64 conditioning = kVectorNdfConditioning * p.SingleScatter;
                EXPECT_NEAR(ss, p.SingleScatter, std::max(kSingleScatterFloor, quadrature) + conditioning)
                    << "roughness " << roughness << ", mu " << mu << ": the engine's single-scatter albedo " << ss
                    << " (quadrature error " << full.ErrorEstimate << ") is not the model's E " << p.SingleScatter;

                const f64 predictionBound = std::max(kPredictionFloor, quadrature) + conditioning + p.EntryBound;
                worstPrediction = std::max(worstPrediction, std::abs(full.Value - p.OnTable));
                EXPECT_NEAR(full.Value, p.OnTable, predictionBound)
                    << "roughness " << roughness << ", mu " << mu << ": the white furnace gives " << full.Value
                    << " (quadrature error " << full.ErrorEstimate << "); the model predicts " << p.Model
                    << " and, through the table's resolution, " << p.OnTable;

                const f64 departure = std::abs(full.Value - p.Model);
                if (departure > worstDeparture)
                {
                    worstDeparture = departure;
                    worstDepartureWhere = "roughness " + Fmt(roughness) + ", mu " + Fmt(mu);
                }
                EXPECT_LE(departure, kFurnaceResolution + predictionBound)
                    << "roughness " << roughness << ", mu " << mu << ": the furnace gives " << full.Value
                    << " against the model's " << p.Model << " — more than the node-centred table's resolution "
                    << "allows (#1478)";

                lowest = std::min(lowest, full.Value);
                highest = std::max(highest, full.Value);
                std::cout << "[ oracle ] v2 white furnace r " << roughness << " mu " << mu
                          << (MultiScatterGated(roughness) ? " (f_ms gated)" : "") << ": " << full.Value << " (err "
                          << full.ErrorEstimate << "), predicted on table " << p.OnTable << ", model " << p.Model
                          << "; single scatter " << ss << " vs E " << p.SingleScatter << "\n";
            }
        }
        std::cout << "[ oracle ] v2 white furnace: range [" << lowest << ", " << highest
                  << "], worst |furnace - table prediction| " << worstPrediction << ", worst |furnace - model| "
                  << worstDeparture << " at " << worstDepartureWhere << " (bound " << kFurnaceResolution << ")\n";
    }

    // =========================================================================
    // 8. Energy conservation, white albedo, every roughness and view cosine.
    //
    //   * Metals (albedo 1, metallic 1): <= 1 + max(1e-3, 4 x the quadrature
    //     estimate), plus the table's predicted furnace excess (test 7) where
    //     that exceeds 1.
    //   * Dielectrics (albedo 1, metallic 0): the MODEL albedo is exactly 1 —
    //     specular E_spec(mu_v) plus the coupled Lambert's 1 - E_spec(mu_v)
    //     (#1479). The engine is asserted (a) equal to the model on its table,
    //     integrated through the same rule, (b) equal to that model's closed
    //     form PredictWhiteDielectric, and (c) <= 1 plus the prediction's
    //     excess, with |prediction - 1| <= kDielectricResolution. Measured:
    //     the engine's white dielectric reads [0.9976, 1.0106], the maximum at
    //     roughness 0.05, cos v 0.02 (closed form 1.0112), where the
    //     (1 - F(v.h)) weight it replaced gave 1.83.
    // =========================================================================
    TEST(BsdfIdentityOracleTest, EnergyConservation)
    {
        constexpr f64 kFloor = 1.0e-3;
        constexpr f64 kDielectricResolution = 0.012;
        const std::vector<f64> cosines{ 0.02, 0.35, 1.0 };
        f64 legacyMetalMax = 0.0;
        f64 v2MetalMax = 0.0;
        f64 legacyDielectricMax = 0.0;
        f64 v2DielectricMax = 0.0;
        f64 v2DielectricMin = 2.0;
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
                    std::max(0.0, p.OnTable - 1.0) + p.EntryBound + kVectorNdfConditioning * p.SingleScatter;
                v2MetalMax = std::max(v2MetalMax, v2Metal.Value);
                EXPECT_LE(v2Metal.Value, 1.0 + std::max(kFloor, 4.0 * v2Metal.ErrorEstimate) + v2Allowance)
                    << "ClosureV2 white metal, " << where << ": albedo " << v2Metal.Value << " (error "
                    << v2Metal.ErrorEstimate << ") exceeds 1 by more than the table predicts (" << p.OnTable << ")";

                // Dielectrics. Legacy: engine against its frozen model.
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

                // ClosureV2: (a) the engine against the model on its table.
                const Quadrature v2D = Albedo(Engine::ClosureV2Brdf(dielectric), v, alpha, 0, 2.5e-4, 512u);
                const Quadrature v2M = Albedo(OracleClosureV2OnTable(dielectric), v, alpha, 0, 2.5e-4, 512u);
                const DielectricPrediction pd = PredictWhiteDielectric(AsReceived(v).z, roughness);
                const f64 quadrature = std::max(kFloor, 4.0 * (v2D.ErrorEstimate + v2M.ErrorEstimate));
                EXPECT_NEAR(v2D.Value, v2M.Value, quadrature + kVectorNdfConditioning * v2M.Value + pd.EntryBound)
                    << "ClosureV2 white dielectric, " << where << ": engine albedo " << v2D.Value
                    << " is not the model's on its table " << v2M.Value;
                // (b) the model on its table against its closed form.
                EXPECT_NEAR(v2M.Value, pd.OnTable, std::max(kFloor, 4.0 * v2M.ErrorEstimate))
                    << "ClosureV2 white dielectric, " << where << ": the model on the table integrates to "
                    << v2M.Value << " but its closed form is " << pd.OnTable;
                // (c) energy: no more than it receives, beyond the table's
                // predicted excess, which is itself bounded.
                const f64 allowance = std::max(0.0, pd.OnTable - 1.0) + pd.EntryBound;
                EXPECT_LE(v2D.Value, 1.0 + quadrature + allowance)
                    << "ClosureV2 white dielectric, " << where << ": albedo " << v2D.Value
                    << " — the closure creates energy beyond the table's predicted " << pd.OnTable;
                EXPECT_LE(std::abs(pd.OnTable - 1.0), kDielectricResolution)
                    << "ClosureV2 white dielectric, " << where << ": the table predicts " << pd.OnTable
                    << " against the model's exact 1 — more than the coupling's stated resolution";
                if (v2D.Value > v2DielectricMax)
                {
                    v2DielectricMax = v2D.Value;
                    v2DielectricWhere = where;
                }
                v2DielectricMin = std::min(v2DielectricMin, v2D.Value);
                std::cout << "[ oracle ] white albedo " << where << ": Legacy metal " << legacyMetal.Value
                          << ", v2 metal " << v2Metal.Value << ", Legacy dielectric " << legacyD.Value << " (model "
                          << legacyModel.Value << "), v2 dielectric " << v2D.Value << " (model on table " << v2M.Value
                          << ", closed form " << pd.OnTable << ")\n";
            }
        }
        std::cout << "[ oracle ] energy conservation: max white-metal albedo Legacy " << legacyMetalMax
                  << ", ClosureV2 " << v2MetalMax << "; white-dielectric albedo max Legacy (model) "
                  << legacyDielectricMax << " (" << legacyDielectricWhere << "), ClosureV2 range [" << v2DielectricMin
                  << ", " << v2DielectricMax << "] (max at " << v2DielectricWhere << "; the model is exactly 1)\n";
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
                    worstV2 = std::max(worstV2, v2.WorstMeasured);
                    worstLegacy = std::max(worstLegacy, legacy.WorstMeasured);
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
    // kVectorNdfConditioning: both models' BSDF::Pdf take the vector-form D.
    // (The scalar cos form's integral bias read +3.5-3.8 % of the specular
    // mass here at roughness 0 / 0.04 and +1.3 % at 0.05, normal incidence.)
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
                                          pS * accepted * kVectorNdfConditioning;
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
    // Both are the vector overloads BSDF::Pdf calls, so they carry
    // kVectorNdfConditioning on top of f32.
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
            const f64 tol = kF32Relative + kVectorNdfConditioning;
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
    //     Expected: the single-scatter model plus both table terms on the
    //     table (test 2a); tolerance: test 1's for the first, test 2a's for
    //     the others, all scaled by L cos. The largest product is reported against
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
                    const f64 ssRel = kF32Relative + kVectorNdfConditioning;
                    const BrdfFn ss = OracleClosureV2Specular(m);
                    const BrdfFn ms = OracleMultiScatterOnTable(m);
                    const BrdfFn diffuse = OracleDiffuseOnTable(m);
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
                        const glm::dvec3 fd = diffuse(vIn, lIn);
                        const f64 scale = radiance * static_cast<f64>(lf.z);
                        const f64 msRel = kF32Relative + MultiScatterEntryRelative(vIn.z, lIn.z, roughness);
                        const f64 dRel = kF32Relative + DiffuseEntryRelative(m, vIn.z, lIn.z);
                        for (int c = 0; c < 3; ++c)
                        {
                            const f64 expected = (fs[c] + fm[c] + fd[c]) * scale;
                            const auto got = static_cast<f64>(product[c]);
                            const f64 bound = (ssRel * fs[c] + msRel * fm[c] + dRel * fd[c] + 1.0e-6) * scale;
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
