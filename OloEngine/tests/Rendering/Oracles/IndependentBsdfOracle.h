#pragma once

// =============================================================================
// IndependentBsdfOracle.h — the local-BSDF model, derived again from the
// literature, in f64, sharing no code with the renderer (issue #1347).
// =============================================================================
//
// THE INDEPENDENCE RULE. An oracle that calls the implementation it checks
// proves agreement, not correctness: a shared plausible mistake passes both.
// This header is therefore written against the PAPERS, never against
// PBRCommon.glsl or ReferenceBRDF.h, and it includes nothing from the renderer.
// The rule is enforced by the includes, not by review:
// OracleIndependenceTest.IncludesOnlyTheStandardLibraryGlmAndBaseTypes reads
// this file and fails on any include outside that list.
//
// Where the engine's formula and the paper's are algebraically the same
// quantity, this header deliberately uses a DIFFERENT algebraic form (the
// tan-theta forms of Walter 2007 and Heitz 2014, not the cos-theta forms the
// shaders use), so a transcription error in one is not copied into the other.
//
// WHAT IS MODEL AND WHAT IS CONVENTION. Two versioned closures ship:
//
//   * ClosureV2 (issue #975, ADR 0016) claims to BE the physical model:
//     GGX + height-correlated Smith + Schlick + Kulla-Conty compensation, and
//     (issue #1479) a Lambert diffuse coupled to what the specular lobe
//     leaves, so a white dielectric reflects exactly what it receives. Its
//     oracle below is the physics, with its two documented guards (ADR 0016
//     §5) as its only conventions: the alpha clamp at roughness 0.04, and the
//     multi-scatter lobe switched off when 1 - E_avg < 1e-4 (a near-mirror
//     lobe sheds nothing worth compensating).
//   * Legacy is FROZEN, intentionally versioned behaviour (AC 6 of #1347):
//     UE4's separable Schlick-GGX G with k = (r+1)^2/8, a 1e-4 floor on the
//     NDF denominator and a 1e-4 addend on the 4 NdotV NdotL denominator. It is
//     not physics and is not meant to be. `LegacyConventions` names each of
//     those choices with its source line so the oracle reproduces the version,
//     and so a test can switch a convention off to measure what it costs —
//     never so the engine can be "corrected" toward the physics.
//
// Nonlocal transport (skin diffusion, leaf and fibre transport) is NOT here.
// Those have their own independent references in
// Rendering/PathTracing/MaterialReference.h, and reciprocity is a category
// error for them (that header says why). Everything in this file is a LOCAL
// BSDF: f(n, v, l) at one point.
//
// Frame: every direction is in the local shading frame, n = +z. Callers that
// want world space rotate into it themselves.
//
// References
//   [Walter07] Walter, Marschner, Li, Torrance, "Microfacet Models for
//              Refraction through Rough Surfaces", EGSR 2007. Eq. 33 (GGX D),
//              eq. 34 (G1 via Lambda).
//   [Heitz14]  Heitz, "Understanding the Masking-Shadowing Function in
//              Microfacet-Based BRDFs", JCGT 3(2), 2014. Eq. 72 (GGX Lambda),
//              eq. 99 (height-correlated G2), eq. 2 (D cos integrates to 1).
//   [Heitz18]  Heitz, "Sampling the GGX Distribution of Visible Normals",
//              JCGT 7(4), 2018. Eq. 1-2 (the visible-normal density D_v).
//   [Schlick94] Schlick, "An Inexpensive BRDF Model for Physically-based
//              Rendering", CGF 13(3), 1994.
//   [Karis13]  Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course
//              notes, p. 3 (k = (r+1)^2/8 for analytic lights).
//   [KullaConty17] Kulla, Conty, "Revisiting Physically Based Shading at
//              Imageworks", SIGGRAPH 2017 course. Eq. for f_ms and F_avg.
//   [Veach97]  Veach, PhD thesis, 1997, §8.2.2 (solid angle to area measure).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OloEngine::Tests::Oracle
{
    inline constexpr f64 kPi = std::numbers::pi;

    // =========================================================================
    // Microfacet primitives (model)
    // =========================================================================

    // GGX / Trowbridge-Reitz NDF in the tan form of [Walter07] eq. 33:
    //   D(m) = chi+(m.n) / (pi alpha^2 cos^4(theta_m) (1 + tan^2(theta_m)/alpha^2)^2)
    // No clamp of any kind: this is the distribution.
    [[nodiscard]] inline f64 GgxD(f64 cosThetaM, f64 alpha)
    {
        if (cosThetaM <= 0.0)
            return 0.0;
        const f64 c2 = cosThetaM * cosThetaM;
        const f64 tan2 = (1.0 - c2) / c2;
        const f64 a2 = alpha * alpha;
        const f64 k = 1.0 + tan2 / a2;
        return 1.0 / (kPi * a2 * c2 * c2 * k * k);
    }

    // The CDF of theta_m under the projected-area density D(m) cos(theta_m),
    // integrated in closed form from [Walter07] eq. 33:
    //   P(theta_m <= theta) = tan^2(theta) / (alpha^2 + tan^2(theta)).
    // It is the natural quadrature variable for a GGX lobe (equal-mass cells at
    // every roughness) and the equal-probability binning for the sampling
    // tests. Returned for a cosine, so it is monotone decreasing in it.
    [[nodiscard]] inline f64 GgxThetaCdf(f64 cosThetaM, f64 alpha)
    {
        const f64 c = std::clamp(cosThetaM, 0.0, 1.0);
        const f64 c2 = c * c;
        if (c2 <= 0.0)
            return 1.0;
        const f64 tan2 = (1.0 - c2) / c2;
        return tan2 / (alpha * alpha + tan2);
    }

    // The inverse: the cosine whose CDF is t. Used to place quadrature nodes.
    [[nodiscard]] inline f64 GgxCosThetaFromCdf(f64 t, f64 alpha)
    {
        const f64 tc = std::clamp(t, 0.0, 1.0);
        if (tc >= 1.0)
            return 0.0;
        const f64 tan2 = alpha * alpha * tc / (1.0 - tc);
        return 1.0 / std::sqrt(1.0 + tan2);
    }

    // Smith Lambda for GGX, [Heitz14] eq. 72, in its a = 1/(alpha tan theta)
    // form: Lambda = (-1 + sqrt(1 + 1/a^2)) / 2.
    [[nodiscard]] inline f64 GgxLambda(f64 cosTheta, f64 alpha)
    {
        const f64 c = std::min(std::abs(cosTheta), 1.0);
        if (c >= 1.0)
            return 0.0;
        if (c <= 0.0)
            return 1.0e300; // tangent to the surface: fully masked
        const f64 tanTheta = std::sqrt(1.0 - c * c) / c;
        const f64 a = 1.0 / (alpha * tanTheta);
        return 0.5 * (-1.0 + std::sqrt(1.0 + 1.0 / (a * a)));
    }

    // Smith masking for one direction, [Walter07] eq. 34 with [Heitz14] eq. 43.
    [[nodiscard]] inline f64 GgxG1(f64 cosTheta, f64 alpha)
    {
        return 1.0 / (1.0 + GgxLambda(cosTheta, alpha));
    }

    // Height-correlated masking-shadowing, [Heitz14] eq. 99.
    [[nodiscard]] inline f64 GgxG2HeightCorrelated(f64 cosV, f64 cosL, f64 alpha)
    {
        if (cosV <= 0.0 || cosL <= 0.0)
            return 0.0;
        return 1.0 / (1.0 + GgxLambda(cosV, alpha) + GgxLambda(cosL, alpha));
    }

    // Visible-normal density of [Heitz18] eq. 1: the density, over microfacet
    // normals m, of the normals a view direction v actually sees.
    //   D_v(m) = G1(v) max(0, v.m) D(m) / (v.n)
    [[nodiscard]] inline f64 GgxVisibleNormalDensity(const glm::dvec3& v, const glm::dvec3& m, f64 alpha)
    {
        if (v.z <= 0.0 || m.z <= 0.0)
            return 0.0;
        const f64 vDotM = glm::dot(v, m);
        if (vDotM <= 0.0)
            return 0.0;
        return GgxG1(v.z, alpha) * vDotM * GgxD(m.z, alpha) / v.z;
    }

    // Solid-angle density of l = reflect(-v, m) when m has density p_m(m):
    //   p_l(l) = p_m(m) / (4 |v.m|)                            [Walter07] eq. 14
    [[nodiscard]] inline f64 ReflectionJacobian(const glm::dvec3& v, const glm::dvec3& m)
    {
        const f64 vDotM = std::abs(glm::dot(v, m));
        return vDotM > 0.0 ? 1.0 / (4.0 * vDotM) : 0.0;
    }

    // Schlick's approximation [Schlick94]. There is no second derivation of an
    // approximation: this is its definition, spelled with std::pow.
    [[nodiscard]] inline glm::dvec3 SchlickFresnel(f64 cosTheta, const glm::dvec3& f0)
    {
        const f64 m = std::pow(std::clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
        return f0 + (glm::dvec3(1.0) - f0) * m;
    }

    [[nodiscard]] inline glm::dvec3 Reflect(const glm::dvec3& v, const glm::dvec3& m)
    {
        return 2.0 * glm::dot(v, m) * m - v;
    }

    [[nodiscard]] inline glm::dvec3 HalfVector(const glm::dvec3& v, const glm::dvec3& l)
    {
        return glm::normalize(v + l);
    }

    // Unit direction from spherical angles, n = +z.
    [[nodiscard]] inline glm::dvec3 Direction(f64 cosTheta, f64 phi)
    {
        const f64 s = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
        return { s * std::cos(phi), s * std::sin(phi), cosTheta };
    }

    // The metal/dielectric F0 blend both shipped closures use, with the
    // dielectric reflectance of IOR 1.5: ((1.5-1)/(1.5+1))^2 = 0.04.
    [[nodiscard]] inline glm::dvec3 BaseF0(const glm::dvec3& albedo, f64 metallic)
    {
        constexpr f64 dielectric = ((1.5 - 1.0) / (1.5 + 1.0)) * ((1.5 - 1.0) / (1.5 + 1.0));
        return glm::mix(glm::dvec3(dielectric), albedo, metallic);
    }

    // =========================================================================
    // Quadrature over directions (numerical integration with a stated error)
    // =========================================================================

    // A quadrature value with its error estimate. The estimate is the change
    // between the rule at N and at N/2 cells per axis; for the midpoint rule
    // on a smooth integrand the error of the finer rule is about a third of
    // that, so the full difference is a conservative bound. It is NOT a bound
    // where the integrand has a kink inside a cell (the horizon cutoff), which
    // is why tests state a floor beside it.
    struct Quadrature
    {
        f64 Value = 0.0;
        f64 ErrorEstimate = 0.0;
    };

    // Integrate g(m) over microfacet normals against the measure D(m) cos(m) dm.
    // Because D cos has unit mass and t = GgxThetaCdf is its CDF, the measure
    // is uniform in (t, phi): a mirror-like lobe is no harder than a diffuse
    // one.
    //
    // The rule is the midpoint rule in s = sqrt(1 - t), not in t. Every
    // integrand that divides by the node density (a pdf or a BRDF integrated
    // over outgoing directions, a visible-normal mass) carries 1 / cos(m), and
    // near the horizon 1 - t ~ alpha^2 cos^2(m), so that factor is
    // (1 - t)^(-1/2): integrable, but a midpoint rule in t converges on it only
    // as N^(-1/2), and its N-vs-N/2 error estimate does not see it (measured:
    // a visible-normal mass of 0.0192 against a converged 0.0309). With
    // t = 1 - s^2, dt = 2 s ds cancels the singularity and the rule is
    // second-order again.
    template<typename G>
    [[nodiscard]] f64 IntegrateOverNdfOnce(f64 alpha, u32 nT, u32 nPhi, G&& g)
    {
        f64 sum = 0.0;
        for (u32 i = 0; i < nT; ++i)
        {
            const f64 s = (static_cast<f64>(i) + 0.5) / static_cast<f64>(nT);
            const f64 t = 1.0 - s * s;
            const f64 cosM = GgxCosThetaFromCdf(t, alpha);
            f64 row = 0.0;
            for (u32 j = 0; j < nPhi; ++j)
            {
                const f64 phi = 2.0 * kPi * (static_cast<f64>(j) + 0.5) / static_cast<f64>(nPhi);
                row += g(Direction(cosM, phi));
            }
            sum += 2.0 * s * row;
        }
        return sum / (static_cast<f64>(nT) * static_cast<f64>(nPhi));
    }

    template<typename G>
    [[nodiscard]] Quadrature IntegrateOverNdf(f64 alpha, u32 nT, u32 nPhi, G&& g)
    {
        const f64 fine = IntegrateOverNdfOnce(alpha, nT, nPhi, g);
        const f64 coarse = IntegrateOverNdfOnce(alpha, nT / 2u, nPhi / 2u, g);
        return { fine, std::abs(fine - coarse) };
    }

    // Integrate h(l) over the upper hemisphere of OUTGOING directions l with
    // respect to solid angle, by changing variables to the half-vector m of
    // (v, l) and integrating against D cos:
    //   dw_l = 4 (v.m) dw_m                                   [Walter07] eq. 14
    //   int h(l) dw_l = int h(l(m)) 4 (v.m) / (D(m) cos m) * [D(m) cos m dm].
    // `alpha` only shapes the node placement; any alpha integrates any h, and
    // matching it to the lobe is what makes a sharp lobe cheap. Directions
    // with v.m <= 0 or l below the horizon contribute nothing.
    template<typename H>
    [[nodiscard]] Quadrature IntegrateOverOutgoingHemisphere(const glm::dvec3& v, f64 alpha, u32 nT, u32 nPhi, H&& h)
    {
        auto integrand = [&](const glm::dvec3& m) -> f64
        {
            const f64 vDotM = glm::dot(v, m);
            if (vDotM <= 0.0)
                return 0.0;
            const glm::dvec3 l = Reflect(v, m);
            if (l.z <= 0.0)
                return 0.0;
            const f64 density = GgxD(m.z, alpha) * m.z;
            if (!(density > 0.0))
                return 0.0;
            return h(l) * 4.0 * vDotM / density;
        };
        return IntegrateOverNdf(alpha, nT, nPhi, integrand);
    }

    // Plain midpoint rule over the upper hemisphere in (cos theta, phi) with
    // respect to solid angle. For smooth integrands (Lambert, a pdf check at
    // high roughness); prefer IntegrateOverOutgoingHemisphere for lobes.
    template<typename H>
    [[nodiscard]] f64 IntegrateHemisphereUniformOnce(u32 nCos, u32 nPhi, H&& h)
    {
        f64 sum = 0.0;
        for (u32 i = 0; i < nCos; ++i)
        {
            const f64 c = (static_cast<f64>(i) + 0.5) / static_cast<f64>(nCos);
            for (u32 j = 0; j < nPhi; ++j)
            {
                const f64 phi = 2.0 * kPi * (static_cast<f64>(j) + 0.5) / static_cast<f64>(nPhi);
                sum += h(Direction(c, phi));
            }
        }
        return sum * 2.0 * kPi / (static_cast<f64>(nCos) * static_cast<f64>(nPhi));
    }

    template<typename H>
    [[nodiscard]] Quadrature IntegrateHemisphereUniform(u32 nCos, u32 nPhi, H&& h)
    {
        const f64 fine = IntegrateHemisphereUniformOnce(nCos, nPhi, h);
        const f64 coarse = IntegrateHemisphereUniformOnce(nCos / 2u, nPhi / 2u, h);
        return { fine, std::abs(fine - coarse) };
    }

    // =========================================================================
    // Single-scattering GGX energy (model)
    // =========================================================================

    // Directional albedo of the single-scattering GGX lobe with F = 1:
    //   E(mu_v) = int D G2 / (4 mu_v mu_l) mu_l dw_l
    // In half-vector form (dw_l = 4 (v.m) dw_m) this is
    //   E = int [G2(v, l(m)) (v.m) / (mu_v cos m)] D(m) cos(m) dm,
    // integrated with IntegrateOverNdf. This is the quantity the engine's
    // generated energy table (GgxEnergyTables.h) stores as 1 - Ess; that table
    // is produced with the engine's own VNDF sampler, so this quadrature is
    // the first estimate of it that shares nothing with the code it checks.
    // The directional integral of the single-scattering GGX lobe with F = 1,
    // times a weight w(v.m) of the microfacet's incidence cosine:
    //   int D G2 w(v.m) / (4 mu_v mu_l) mu_l dw_l.
    template<typename W>
    [[nodiscard]] Quadrature GgxDirectionalMoment(f64 muV, f64 alpha, u32 nT, u32 nPhi, W&& weight)
    {
        const glm::dvec3 v = Direction(muV, 0.0);
        auto g = [&](const glm::dvec3& m) -> f64
        {
            const f64 vDotM = glm::dot(v, m);
            if (vDotM <= 0.0)
                return 0.0;
            const glm::dvec3 l = Reflect(v, m);
            if (l.z <= 0.0)
                return 0.0;
            return weight(vDotM) * GgxG2HeightCorrelated(v.z, l.z, alpha) * vDotM / (v.z * m.z);
        };
        return IntegrateOverNdf(alpha, nT, nPhi, g);
    }

    [[nodiscard]] inline Quadrature GgxDirectionalAlbedo(f64 muV, f64 alpha, u32 nT = 512, u32 nPhi = 128)
    {
        return GgxDirectionalMoment(muV, alpha, nT, nPhi, [](f64)
                                    { return 1.0; });
    }

    // The Schlick moment of the same lobe: its albedo with Schlick's grazing
    // factor (1 - v.m)^5 as F. Schlick's F = F0 + (1 - F0)(1 - v.m)^5 is
    // affine in F0 [Schlick94], so the lobe's albedo for any F0 is
    //   E_ss(mu, F0) = F0 E(mu) + (1 - F0) S(mu),
    // the quantity ClosureV2's energy-conserving diffuse weight subtracts
    // (issue #1479).
    [[nodiscard]] inline Quadrature GgxDirectionalSchlickMoment(f64 muV, f64 alpha, u32 nT = 512, u32 nPhi = 128)
    {
        return GgxDirectionalMoment(muV, alpha, nT, nPhi, [](f64 vDotM)
                                    { return std::pow(1.0 - vDotM, 5.0); });
    }

    // Cosine-weighted average of a directional moment, [KullaConty17]:
    //   M_avg = 2 int_0^1 M(mu) mu dmu,  midpoint rule over mu.
    template<typename M>
    [[nodiscard]] Quadrature CosineAverage(M&& moment, u32 nMu)
    {
        auto run = [&](u32 n) -> f64
        {
            f64 sum = 0.0;
            for (u32 i = 0; i < n; ++i)
            {
                const f64 mu = (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
                sum += moment(mu) * mu;
            }
            return 2.0 * sum / static_cast<f64>(n);
        };
        const f64 fine = run(nMu);
        const f64 coarse = run(nMu / 2u);
        return { fine, std::abs(fine - coarse) };
    }

    // E_avg = 2 int_0^1 E(mu) mu dmu.
    [[nodiscard]] inline Quadrature GgxAverageAlbedo(f64 alpha, u32 nMu = 64, u32 nT = 256, u32 nPhi = 64)
    {
        return CosineAverage([&](f64 mu)
                             { return GgxDirectionalAlbedo(mu, alpha, nT, nPhi).Value; }, nMu);
    }

    // S_avg = 2 int_0^1 S(mu) mu dmu.
    [[nodiscard]] inline Quadrature GgxAverageSchlickMoment(f64 alpha, u32 nMu = 64, u32 nT = 256, u32 nPhi = 64)
    {
        return CosineAverage([&](f64 mu)
                             { return GgxDirectionalSchlickMoment(mu, alpha, nT, nPhi).Value; }, nMu);
    }

    // =========================================================================
    // The two shipped closures, as models
    // =========================================================================

    // ---- ClosureV2 (#975): the physical model plus one documented guard ----

    // The v2 closure's only guard: roughness is clamped to [0.04, 1] before it
    // is squared, identically for evaluation, sampling and density (ADR 0016).
    [[nodiscard]] inline f64 ClosureV2Alpha(f64 roughness)
    {
        const f64 r = std::clamp(roughness, 0.04, 1.0);
        return r * r;
    }

    // Single-scattering microfacet specular, cosine NOT included:
    //   f_s = D G2 F / (4 mu_v mu_l)                     [Walter07] eq. 20
    [[nodiscard]] inline glm::dvec3 MicrofacetSpecular(const glm::dvec3& v, const glm::dvec3& l, f64 alpha,
                                                       const glm::dvec3& f0)
    {
        if (v.z <= 0.0 || l.z <= 0.0)
            return glm::dvec3(0.0);
        const glm::dvec3 m = HalfVector(v, l);
        const f64 d = GgxD(m.z, alpha);
        const f64 g = GgxG2HeightCorrelated(v.z, l.z, alpha);
        return d * g * SchlickFresnel(glm::dot(v, m), f0) / (4.0 * v.z * l.z);
    }

    // Kulla-Conty multiple-scattering lobe from the model's own energies:
    //   f_ms = F_ms (1 - E(mu_v)) (1 - E(mu_l)) / (pi (1 - E_avg))
    //   F_ms = F_avg^2 E_avg / (1 - F_avg (1 - E_avg)),  F_avg = F0 + (1 - F0)/21
    // `eV`, `eL` and `eAvg` are the single-scattering energies — from
    // GgxDirectionalAlbedo / GgxAverageAlbedo, not from the engine's table.
    // F_ms of [KullaConty17]: the average Fresnel of a multiply scattered path.
    [[nodiscard]] inline glm::dvec3 KullaContyFresnel(f64 eAvg, const glm::dvec3& f0)
    {
        const f64 lossAvg = 1.0 - eAvg;
        if (lossAvg <= 0.0)
            return glm::dvec3(0.0);
        const glm::dvec3 fAvg = f0 + (glm::dvec3(1.0) - f0) / 21.0;
        return fAvg * fAvg * eAvg / (glm::dvec3(1.0) - fAvg * lossAvg);
    }

    [[nodiscard]] inline glm::dvec3 KullaContyMultiScatter(f64 eV, f64 eL, f64 eAvg, const glm::dvec3& f0)
    {
        const f64 lossAvg = 1.0 - eAvg;
        if (lossAvg <= 0.0)
            return glm::dvec3(0.0);
        return KullaContyFresnel(eAvg, f0) * (1.0 - eV) * (1.0 - eL) / (kPi * lossAvg);
    }

    // The directional albedo of the whole specular lobe — single scattering
    // with Schlick's F, plus the Kulla-Conty lobe — from the single-scattering
    // moments E(mu) and S(mu):
    //   E_spec = F0 E + (1 - F0) S        [Schlick94], affine in F0
    //          + F_ms (1 - E)             int f_ms cos dw_l, [KullaConty17]:
    //                                     the lobe's l-factor (1 - E(mu_l)) /
    //                                     (1 - E_avg) integrates to exactly 1
    // `fresnelMs` is 0 where the multi-scatter lobe is switched off. The same
    // expression on the averages (E_avg, S_avg) is E_spec_avg.
    [[nodiscard]] inline glm::dvec3 SpecularAlbedo(f64 e, f64 s, const glm::dvec3& f0, const glm::dvec3& fresnelMs)
    {
        return f0 * e + (glm::dvec3(1.0) - f0) * s + fresnelMs * (1.0 - e);
    }

    // The energy-conserving diffuse weight (issue #1479): the Lambert lobe
    // scaled by what the specular lobe leaves, in both directions,
    //   w_d = (1 - E_spec(mu_v)) (1 - E_spec(mu_l)) / (1 - E_spec_avg).
    // Its l-factor integrates against mu_l dw_l / pi to exactly 1, so the
    // diffuse albedo is (1 - E_spec(mu_v)) x albedo: specular plus diffuse is
    // the albedo, and 1 for a white dielectric. Symmetric in v and l. The
    // Kelemen/Kulla-Conty coupling, per channel. A channel whose specular
    // takes everything on average (F0 = 1) has no diffuse left: 0.
    [[nodiscard]] inline glm::dvec3 CoupledDiffuseWeight(const glm::dvec3& eSpecV, const glm::dvec3& eSpecL,
                                                         const glm::dvec3& eSpecAvg)
    {
        glm::dvec3 w(0.0);
        for (int c = 0; c < 3; ++c)
        {
            const f64 remainingAvg = 1.0 - eSpecAvg[c];
            if (remainingAvg > 0.0)
                w[c] = (1.0 - eSpecV[c]) * (1.0 - eSpecL[c]) / remainingAvg;
        }
        return w;
    }

    // The single-scattering moments the v2 closure reads, at mu_v, mu_l and
    // averaged — from GgxDirectionalAlbedo / GgxDirectionalSchlickMoment and
    // their averages, or from a model of the engine's table. `MultiScatter`
    // false is the documented guard: no Kulla-Conty lobe, and no F_ms in
    // E_spec either.
    struct ClosureV2Energies
    {
        f64 EV = 1.0;
        f64 EL = 1.0;
        f64 EAvg = 1.0;
        f64 SV = 0.0;
        f64 SL = 0.0;
        f64 SAvg = 0.0;
        bool MultiScatter = true;
    };

    // The two table-driven terms of the v2 closure, cosine NOT included.
    struct ClosureV2TableTerms
    {
        glm::dvec3 MultiScatter{ 0.0 };
        glm::dvec3 Diffuse{ 0.0 };
    };

    [[nodiscard]] inline ClosureV2TableTerms EvaluateClosureV2TableTerms(const glm::dvec3& albedo, f64 metallic,
                                                                         const ClosureV2Energies& e)
    {
        const glm::dvec3 f0 = BaseF0(albedo, metallic);
        ClosureV2TableTerms terms;
        glm::dvec3 fresnelMs(0.0);
        if (e.MultiScatter)
        {
            fresnelMs = KullaContyFresnel(e.EAvg, f0);
            terms.MultiScatter = KullaContyMultiScatter(e.EV, e.EL, e.EAvg, f0);
        }
        const glm::dvec3 weight = CoupledDiffuseWeight(SpecularAlbedo(e.EV, e.SV, f0, fresnelMs),
                                                       SpecularAlbedo(e.EL, e.SL, f0, fresnelMs),
                                                       SpecularAlbedo(e.EAvg, e.SAvg, f0, fresnelMs));
        terms.Diffuse = weight * (1.0 - metallic) * albedo / kPi;
        return terms;
    }

    // The full v2 closure: single-scattering microfacet specular, the
    // Kulla-Conty lobe and the coupled Lambert diffuse.
    [[nodiscard]] inline glm::dvec3 ClosureV2Brdf(const glm::dvec3& v, const glm::dvec3& l, const glm::dvec3& albedo,
                                                  f64 metallic, f64 roughness, const ClosureV2Energies& energies)
    {
        if (v.z <= 0.0 || l.z <= 0.0)
            return glm::dvec3(0.0);
        const ClosureV2TableTerms terms = EvaluateClosureV2TableTerms(albedo, metallic, energies);
        return terms.Diffuse + MicrofacetSpecular(v, l, ClosureV2Alpha(roughness), BaseF0(albedo, metallic)) +
               terms.MultiScatter;
    }

    // ---- Legacy: frozen, versioned conventions (AC 6) ----------------------

    // Each field is one deliberate difference between the Legacy closure and
    // the model, with the line that ships it. Default-constructed, the struct
    // IS the shipped version; a test may zero a field to measure what that one
    // convention costs.
    struct LegacyConventions
    {
        // PBRCommon.glsl distributionGGX: `a2 / max(denom, EPSILON)`, EPSILON
        // 1e-4 — caps the NDF peak below roughness ~0.27.
        f64 NdfDenominatorFloor = 1.0e-4;
        // PBRCommon.glsl cookTorranceBRDF: `4 NdotV NdotL + EPSILON` — a 1e-4
        // addend that dims the lobe at grazing angles (6 % at NdotV = NdotL = 0.02).
        f64 SpecularDenominatorAddend = 1.0e-4;
        // PBRCommon.glsl geometrySchlickGGX: `NdotV / max(NdotV (1-k) + k, EPSILON)`.
        f64 GeometryDenominatorFloor = 1.0e-4;
    };

    // UE4's separable Schlick-GGX with the analytic-light remap [Karis13]:
    // G1(mu) = mu / (mu (1 - k) + k), k = (r + 1)^2 / 8. Takes ROUGHNESS.
    [[nodiscard]] inline f64 LegacySchlickG1(f64 mu, f64 roughness, const LegacyConventions& c = {})
    {
        const f64 k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        return mu / std::max(mu * (1.0 - k) + k, c.GeometryDenominatorFloor);
    }

    // The Legacy NDF: the model D, with the frozen denominator floor. Written
    // in the cos form here because the floor is defined on THAT denominator.
    [[nodiscard]] inline f64 LegacyD(f64 cosM, f64 roughness, const LegacyConventions& c = {})
    {
        const f64 a2 = std::pow(roughness, 4.0);
        const f64 cm = std::max(cosM, 0.0);
        const f64 q = cm * cm * (a2 - 1.0) + 1.0;
        return a2 / std::max(kPi * q * q, c.NdfDenominatorFloor);
    }

    // The Legacy closure, cosine NOT included. F at v.m, kD = (1 - F)(1 - metallic).
    [[nodiscard]] inline glm::dvec3 LegacyBrdf(const glm::dvec3& v, const glm::dvec3& l, const glm::dvec3& albedo,
                                               f64 metallic, f64 roughness, const LegacyConventions& c = {})
    {
        const f64 muV = std::max(v.z, 0.0);
        const f64 muL = std::max(l.z, 0.0);
        const glm::dvec3 m = HalfVector(v, l);
        const glm::dvec3 f0 = BaseF0(albedo, metallic);
        const glm::dvec3 f = SchlickFresnel(std::max(glm::dot(v, m), 0.0), f0);
        const f64 d = LegacyD(m.z, roughness, c);
        const f64 g = LegacySchlickG1(muV, roughness, c) * LegacySchlickG1(muL, roughness, c);
        const glm::dvec3 specular = d * g * f / (4.0 * muV * muL + c.SpecularDenominatorAddend);
        const glm::dvec3 diffuse = (glm::dvec3(1.0) - f) * (1.0 - metallic) * albedo / kPi;
        return diffuse + specular;
    }

    // =========================================================================
    // Path-reuse geometry (model): the reconnection Jacobian as a ratio of
    // solid angles, measured, not transcribed.
    // =========================================================================

    // Exact solid angle of the triangle (a, b, c) seen from p, by the formula
    // of Van Oosterom and Strackee (IEEE TBME 30(2), 1983):
    //   tan(Omega/2) = |a.(b x c)| / (|a||b||c| + (a.b)|c| + (a.c)|b| + (b.c)|a|)
    // with a, b, c relative to p.
    [[nodiscard]] inline f64 TriangleSolidAngle(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b,
                                                const glm::dvec3& c)
    {
        const glm::dvec3 ra = a - p;
        const glm::dvec3 rb = b - p;
        const glm::dvec3 rc = c - p;
        const f64 la = glm::length(ra);
        const f64 lb = glm::length(rb);
        const f64 lc = glm::length(rc);
        const f64 numerator = std::abs(glm::dot(ra, glm::cross(rb, rc)));
        const f64 denominator = la * lb * lc + glm::dot(ra, rb) * lc + glm::dot(ra, rc) * lb + glm::dot(rb, rc) * la;
        return 2.0 * std::atan2(numerator, denominator);
    }

    // Solid angle of a small square patch of half-size `h` centred at `x` with
    // normal `n`, seen from `p` — two exact triangles.
    [[nodiscard]] inline f64 PatchSolidAngle(const glm::dvec3& p, const glm::dvec3& x, const glm::dvec3& n, f64 h)
    {
        const glm::dvec3 helper = std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
        const glm::dvec3 t = glm::normalize(glm::cross(helper, n));
        const glm::dvec3 b = glm::cross(n, t);
        const glm::dvec3 c00 = x - h * t - h * b;
        const glm::dvec3 c10 = x + h * t - h * b;
        const glm::dvec3 c11 = x + h * t + h * b;
        const glm::dvec3 c01 = x - h * t + h * b;
        return TriangleSolidAngle(p, c00, c10, c11) + TriangleSolidAngle(p, c00, c11, c01);
    }

    // The reconnection shift's Jacobian from its DEFINITION: reusing a sample
    // vertex x1 found from `source` at `destination` changes the solid-angle
    // density by the ratio of the solid angles one small area element at x1
    // subtends from the two shading points ([Veach97] §8.2.2: dw = cos/d^2 dA).
    // Measured with exact solid angles of a finite patch, so it shares nothing
    // with a cos/d^2 transcription. Converges as h -> 0 (error O(h^2)).
    [[nodiscard]] inline f64 ReconnectionJacobianBySolidAngles(const glm::dvec3& vertex, const glm::dvec3& vertexNormal,
                                                               const glm::dvec3& destination, const glm::dvec3& source,
                                                               f64 patchHalfSize = 1.0e-3)
    {
        const f64 omegaDestination = PatchSolidAngle(destination, vertex, vertexNormal, patchHalfSize);
        const f64 omegaSource = PatchSolidAngle(source, vertex, vertexNormal, patchHalfSize);
        return omegaSource > 0.0 ? omegaDestination / omegaSource : 0.0;
    }
} // namespace OloEngine::Tests::Oracle
