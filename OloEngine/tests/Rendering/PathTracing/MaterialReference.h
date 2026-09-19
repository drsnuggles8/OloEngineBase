#pragma once

// =============================================================================
// MaterialReference.h — INDEPENDENT ground truth for the three specialised
// material models, for issue #1255.
//
// WHY THIS FILE EXISTS. The engine already has a reference apparatus
// (PathTracing/ReferenceBRDF.h and the Reference*Test files beside this
// header), and it is a faithful CPU mirror of PBRCommon.glsl's Cook-Torrance
// BRDF. That is exactly the wrong oracle for skin, leaf and fibre: none of the
// three IS a Cook-Torrance surface, so "compared against the reference tracer"
// would mean "compared against a model that cannot represent it". #1255's
// fourth acceptance criterion names that failure outright. So each reference
// here is computed by a route that shares no code and, where possible, no
// derivation with the production model it judges:
//
//   Fibre       a dielectric-cylinder cross-section walk. Explicit 3D geometry,
//               an explicit interface event sequence and an explicit path sum,
//               against Chiang's closed-form attenuation recurrence.
//   Leaf        a plane-parallel translucent slab solved by Monte Carlo random
//               walk, against a phenomenological wrap/forward-scatter lobe.
//   Skin        a semi-infinite searchlight random walk, against the
//               Christensen-Burley normalised-diffusion fit.
//   Coat        a stochastic fibre medium, both by Monte Carlo over realised
//               crossing counts and in closed form for a Poisson medium,
//               against exp(-kappa * E[crossings]).
//
// WHAT IS LOCAL AND WHAT IS NOT — #1255's second criterion, as a property of
// this file rather than a remark in a comment. The first two references are
// LOCAL: they produce a scattering function of two directions at a point, so
// energy, reciprocity and sampling consistency are all questions with answers.
// The last two are NONLOCAL: a diffusion profile is a function of DISTANCE
// ACROSS a surface and a coat transmittance is a function of a PATH THROUGH a
// volume. Asking either for reciprocity is a category error, and a test that
// did would be asserting a coincidence. Each reference below says which class
// it is in, in its own comment, and the test files are split on the same line.
//
// UNITS AND CONVENTIONS ARE DECLARED AT EVERY ENTRY POINT. Every silent failure
// this file is meant to catch is a unit or a handedness, so no function here
// takes a bare float without saying what it is measured in.
//
// DETERMINISM. Every estimator is driven by the integer PCG below from an
// explicit seed, so a number a test asserts on is the same number on every
// machine and in every run — a Monte Carlo reference whose value moves between
// runs cannot carry a tolerance. Nothing here touches the GPU, a file or the
// clock.
//
// THE DECISION RECORD — what each reference is, its units, its coordinate
// convention, its tolerance and its declared limitations — is
// docs/analysis/material-reference-validation-1255.md. Every claim that
// document makes is an assertion in one of the four test files that include
// this header, so it fails loudly when it stops being true rather than ageing
// quietly.
//
// COST. These run inside L1 unit tests, so every default sample count is sized
// for a Debug build: the estimators are cheap random walks with survival
// roulette, and the tolerances in the tests are sized against the standard
// error the defaults actually deliver rather than against an ideal one.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace OloEngine::Tests::MaterialReference
{
    inline constexpr f64 kPi = 3.14159265358979323846;
    inline constexpr f64 kTwoPi = 2.0 * kPi;

    // -------------------------------------------------------------------------
    // Determinism
    // -------------------------------------------------------------------------

    /// PCG32. Integer state, integer advance, and a float conversion that is
    /// exact in binary — so the sequence is identical on every compiler and
    /// every optimisation level. A `std::mt19937` plus a
    /// `uniform_real_distribution` would not be: the distribution's mapping is
    /// implementation-defined, and the reference's numbers would then differ
    /// between the MSVC and the clang-cl build of the same test.
    class Pcg32
    {
      public:
        explicit Pcg32(u64 seed) noexcept : m_State(seed + 0x9E3779B97F4A7C15ull)
        {
            (void)NextU32();
        }

        [[nodiscard]] u32 NextU32() noexcept
        {
            const u64 old = m_State;
            m_State = (old * 6364136223846793005ull) + 1442695040888963407ull;
            const u32 xorshifted = static_cast<u32>(((old >> 18) ^ old) >> 27);
            const u32 rot = static_cast<u32>(old >> 59);
            return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
        }

        /// Uniform in [0, 1). 24 mantissa bits, so the conversion is exact and
        /// 1.0 is unreachable — a walk that could draw exactly 1 would take a
        /// -log(0) step.
        [[nodiscard]] f64 NextDouble() noexcept
        {
            return static_cast<f64>(NextU32() >> 8) * (1.0 / 16777216.0);
        }

      private:
        u64 m_State = 0;
    };

    [[nodiscard]] inline f64 SafeSqrt(f64 x) noexcept
    {
        return std::sqrt(std::max(0.0, x));
    }

    // =========================================================================
    // 1. FIBRE — a dielectric cylinder cross-section walk. LOCAL.
    // =========================================================================
    //
    // CONVENTION, matching the production model so the two are comparable at
    // all (Marschner 2003 / Chiang 2016, which is what
    // OloEngine/Groom/GroomFibreScattering.h implements):
    //
    //   * The fibre is an infinite circular cylinder of UNIT RADIUS with its
    //     axis along local +x.
    //   * A direction's longitudinal angle theta is measured FROM THE PLANE
    //     PERPENDICULAR TO THE AXIS, not from the axis: sin(theta) = w.x. This
    //     is the fibre convention and it is the opposite of the surface one; a
    //     reference that used the surface convention would agree at normal
    //     incidence and diverge everywhere else, which is the quietest possible
    //     way to be wrong.
    //   * h is the SIGNED impact parameter in the normal plane, in RADIUS
    //     units, h in [-1, 1]. gamma_o = asin(h) is the angle of incidence of
    //     the ray's PROJECTION onto the normal plane.
    //   * sigma_a is per unit length with the cylinder at unit radius — so the
    //     head-on chord it multiplies is 2, the DIAMETER in these units. The
    //     production header describes sigma_a as being "in units of the fibre
    //     diameter"; the expression it ships, exp(-sigma_a * 2 cos(gamma_t) /
    //     cos(theta_t)), is per RADIUS. This reference uses the code's
    //     convention, because the code is what renders.
    //
    // WHAT IS INDEPENDENT HERE. The production model reaches its four
    // attenuations through Chiang's recurrence with a closed-form geometric
    // tail. This reference instead builds the 3D incidence geometry explicitly,
    // takes the real dot product with the real cylinder normal, refracts with
    // full 3D Snell, walks the interface events one at a time and sums the
    // series term by term. Agreement is therefore evidence about the model, not
    // about a shared subexpression. Three production formulas fall out as
    // testable consequences rather than as inputs: cos(psi) = cos(theta_o)
    // cos(gamma_o), Bravais' modified index, and the geometric tail.

    struct FibrePathTerm
    {
        /// Fraction of the incident energy leaving on this path, per channel.
        glm::dvec3 Energy{ 0.0 };
        /// Azimuthal deflection of the exit direction, radians, measured in the
        /// normal plane and NOT wrapped into any interval. p = 0 is 2 gamma_o
        /// away from forward; each further internal segment adds 2 gamma_t.
        f64 Phi = 0.0;
    };

    struct FibrePathWalk
    {
        /// Indexed by path order p: 0 = R, 1 = TT, 2 = TRT, 3 = TRRT, ...
        std::vector<FibrePathTerm> Terms;

        /// The 3D cosine of the angle of incidence at the cuticle, from the
        /// real surface normal. Production asserts this equals
        /// cos(theta_o) cos(gamma_o); here it is measured.
        f64 CosIncidence = 1.0;
        /// The cosine of the angle of incidence of an INTERNAL hit, from
        /// inside.
        f64 CosInternalIncidence = 1.0;
        /// External and internal unpolarised Fresnel reflectance. Reversibility
        /// makes these equal; the reference computes them separately so a test
        /// can say so rather than assume it.
        f64 FresnelExternal = 0.0;
        f64 FresnelInternal = 0.0;
        /// The refracted azimuthal angle, radians, signed like h.
        f64 GammaT = 0.0;
        /// Bravais' modified azimuthal index, measured from the 3D refraction
        /// rather than taken from the published formula.
        f64 EtaPrime = 1.0;
        /// Internal chord length in RADIUS units, including the longitudinal
        /// obliquity. 2 at normal incidence head-on.
        f64 ChordLength = 0.0;

        [[nodiscard]] glm::dvec3 TotalEnergy() const noexcept
        {
            glm::dvec3 sum(0.0);
            for (const FibrePathTerm& term : Terms)
                sum += term.Energy;
            return sum;
        }

        /// The energy on paths of order `first` and above — what a model that
        /// folds its tail into one lobe has to reproduce.
        [[nodiscard]] glm::dvec3 TailEnergy(sizet first) const noexcept
        {
            glm::dvec3 sum(0.0);
            for (sizet p = first; p < Terms.size(); ++p)
                sum += Terms[p].Energy;
            return sum;
        }
    };

    /// Unpolarised Fresnel reflectance at a flat dielectric interface.
    /// `cosThetaI` is the cosine of the angle between the incident direction
    /// and the normal, both on the SAME side; `relativeEta` is the index on the
    /// transmitted side over the index on the incident side. Total internal
    /// reflection returns exactly 1.
    ///
    /// Written from the Fresnel equations rather than reusing the production
    /// helper, because the production helper is part of what is under test.
    [[nodiscard]] inline f64 FresnelUnpolarised(f64 cosThetaI, f64 relativeEta) noexcept
    {
        cosThetaI = std::clamp(cosThetaI, 0.0, 1.0);
        const f64 sinThetaI = SafeSqrt(1.0 - (cosThetaI * cosThetaI));
        const f64 sinThetaT = sinThetaI / relativeEta;
        if (sinThetaT >= 1.0)
            return 1.0;
        const f64 cosThetaT = SafeSqrt(1.0 - (sinThetaT * sinThetaT));

        const f64 rs = (cosThetaI - (relativeEta * cosThetaT)) / (cosThetaI + (relativeEta * cosThetaT));
        const f64 rp = ((relativeEta * cosThetaI) - cosThetaT) / ((relativeEta * cosThetaI) + cosThetaT);
        return 0.5 * ((rs * rs) + (rp * rp));
    }

    /// Walk the cross-section.
    ///
    ///   `sinThetaO`  longitudinal sine of the direction light ARRIVES from,
    ///                in [-1, 1].
    ///   `h`          impact parameter in radius units, [-1, 1].
    ///   `eta`        fibre index over vacuum, > 1.
    ///   `sigmaA`     absorption per unit length at unit radius, per channel.
    ///   `maxOrder`   the highest path order p to enumerate. The series is
    ///                geometric with ratio T*f < 1, so the truncation error is
    ///                bounded and a test can state it rather than hope.
    [[nodiscard]] inline FibrePathWalk FibreCylinderWalk(f64 sinThetaO, f64 h, f64 eta, const glm::dvec3& sigmaA,
                                                         u32 maxOrder = 24)
    {
        FibrePathWalk walk;

        sinThetaO = std::clamp(sinThetaO, -1.0, 1.0);
        h = std::clamp(h, -1.0, 1.0);

        const f64 cosThetaO = SafeSqrt(1.0 - (sinThetaO * sinThetaO));
        const f64 gammaO = std::asin(h);

        // ---- the 3D entry geometry, built rather than assumed ---------------
        // The normal plane is (y, z). The incoming direction of TRAVEL is
        //   d = (-sinThetaO, 0, cosThetaO)
        // so its azimuthal part points along +z, and the outward surface normal
        // at the entry point is
        //   n = (0, sin(gamma_o), -cos(gamma_o))
        // whose azimuthal part makes the angle gamma_o with -z. That is the
        // definition of the impact parameter, and everything below follows from
        // these two vectors alone.
        const glm::dvec3 travel(-sinThetaO, 0.0, cosThetaO);
        const glm::dvec3 normal(0.0, std::sin(gammaO), -std::cos(gammaO));

        walk.CosIncidence = std::clamp(-glm::dot(travel, normal), 0.0, 1.0);
        walk.FresnelExternal = FresnelUnpolarised(walk.CosIncidence, eta);

        // ---- full 3D Snell at the cuticle -----------------------------------
        //   t = (d - (d.n) n) / eta  -  n sqrt(1 - (1 - (d.n)^2) / eta^2)
        const f64 dDotN = glm::dot(travel, normal);
        const f64 k = 1.0 - ((1.0 - (dDotN * dDotN)) / (eta * eta));
        const glm::dvec3 refracted = ((travel - (normal * dDotN)) / eta) - (normal * SafeSqrt(k));

        // The longitudinal angle inside. Snell on the axis component, because
        // the axis is tangential to the cylinder everywhere.
        const f64 sinThetaT = refracted.x;
        const f64 cosThetaT = SafeSqrt(1.0 - (sinThetaT * sinThetaT));

        // The azimuthal refraction angle, measured from the same normal.
        const glm::dvec3 refractedAzimuth(0.0, refracted.y, refracted.z);
        const f64 azimuthLength = glm::length(refractedAzimuth);
        const f64 cosGammaT =
            (azimuthLength > 1.0e-12)
                ? std::clamp(-glm::dot(refractedAzimuth / azimuthLength, normal), 0.0, 1.0)
                : 1.0;
        walk.GammaT = std::acos(cosGammaT) * ((h < 0.0) ? -1.0 : 1.0);

        // Bravais' modified index, MEASURED: sin(gamma_o) / sin(gamma_t).
        const f64 sinGammaT = SafeSqrt(1.0 - (cosGammaT * cosGammaT));
        walk.EtaPrime = (sinGammaT > 1.0e-12)
                            ? (std::abs(h) / sinGammaT)
                            : (SafeSqrt((eta * eta) - (sinThetaO * sinThetaO)) / std::max(cosThetaO, 1.0e-12));

        // ---- the internal chord ---------------------------------------------
        // A chord of a unit circle whose ends make the angle gamma_t with the
        // radii has length 2 cos(gamma_t); the 3D path is longer by the
        // longitudinal obliquity 1 / cos(theta_t).
        walk.ChordLength = (2.0 * cosGammaT) / std::max(cosThetaT, 1.0e-12);
        const glm::dvec3 transmittance = glm::exp(-sigmaA * walk.ChordLength);

        // ---- the internal interface, from inside ----------------------------
        // By the circle's symmetry the ray meets every internal wall at the
        // same angle gamma_t azimuthally and theta_t longitudinally, so the 3D
        // incidence cosine inside is cos(theta_t) cos(gamma_t) — a consequence
        // of the geometry here, not an input.
        walk.CosInternalIncidence = cosThetaT * cosGammaT;
        walk.FresnelInternal = FresnelUnpolarised(walk.CosInternalIncidence, 1.0 / eta);

        // ---- the path sum ----------------------------------------------------
        const f64 fExternal = walk.FresnelExternal;
        const f64 fInternal = walk.FresnelInternal;

        walk.Terms.reserve(static_cast<sizet>(maxOrder) + 1u);

        FibrePathTerm surface;
        surface.Energy = glm::dvec3(fExternal);
        // Specular reflection off a circle deflects by pi - 2 gamma_o from the
        // incoming direction; expressed as a deflection from FORWARD, which is
        // the convention the production Phi uses, that is 2 gamma_o.
        surface.Phi = 2.0 * gammaO;
        walk.Terms.push_back(surface);

        glm::dvec3 throughput(1.0 - fExternal);
        for (u32 p = 1; p <= maxOrder; ++p)
        {
            throughput *= transmittance;

            FibrePathTerm term;
            term.Energy = throughput * (1.0 - fInternal);
            // p internal segments, each turning the ray by 2 gamma_t; the entry
            // and exit refractions together undo 2 gamma_o.
            term.Phi = (2.0 * static_cast<f64>(p) * walk.GammaT) - (2.0 * gammaO);
            walk.Terms.push_back(term);

            throughput *= fInternal;
        }

        return walk;
    }

    /// The same energies by an unbiased STOCHASTIC walk: at each interface the
    /// path reflects with probability equal to the Fresnel reflectance and
    /// refracts otherwise, and absorption is applied as a weight along the
    /// chord.
    ///
    /// This exists because the deterministic walk above and Chiang's recurrence
    /// are both energy-SPLITTING schemes, and two splitting schemes can share a
    /// mistake in how the split is written down. A roulette walk cannot: it
    /// never forms the product (1-f)^2 at all. Entry `p` of the result is the
    /// mean energy leaving on path order p, with every higher order folded into
    /// the last entry.
    [[nodiscard]] inline std::vector<glm::dvec3> FibreCylinderStochasticEnergies(f64 sinThetaO, f64 h, f64 eta,
                                                                                 const glm::dvec3& sigmaA,
                                                                                 u32 maxOrder, u32 samples, u64 seed)
    {
        const FibrePathWalk geometry = FibreCylinderWalk(sinThetaO, h, eta, glm::dvec3(0.0), 1);
        const glm::dvec3 transmittance = glm::exp(-sigmaA * geometry.ChordLength);

        std::vector<glm::dvec3> orders(static_cast<sizet>(maxOrder) + 1u, glm::dvec3(0.0));
        Pcg32 rng(seed);

        for (u32 s = 0; s < samples; ++s)
        {
            if (rng.NextDouble() < geometry.FresnelExternal)
            {
                orders[0] += glm::dvec3(1.0);
                continue;
            }

            glm::dvec3 weight(1.0);
            for (u32 p = 1;; ++p)
            {
                weight *= transmittance;
                const sizet slot = std::min(static_cast<sizet>(p), orders.size() - 1u);
                const bool reflected = rng.NextDouble() < geometry.FresnelInternal;
                if (reflected && slot < orders.size() - 1u)
                    continue;

                // Either the path refracted out, or it reached the folded tail
                // bucket — where continuing would only refine a slot that is
                // already terminal.
                orders[slot] += weight;
                break;
            }
        }

        const f64 inv = 1.0 / static_cast<f64>(std::max(1u, samples));
        for (glm::dvec3& value : orders)
            value *= inv;
        return orders;
    }

    /// The reference's per-path energies averaged over the fibre's width and
    /// folded into the FOUR buckets a production far-field model keeps: R, TT,
    /// TRT, and everything from TRRT onwards.
    ///
    /// `nodes` must be the SAME quadrature order the production side runs, and
    /// the nodes are the same uniform midpoints — h_k = -1 + 2(k + 1/2)/n.
    /// Averaging a 4-node production mean against a 4096-node reference mean
    /// would measure the quadrature and report it as a disagreement about the
    /// model, which is the one mistake this helper exists to make impossible to
    /// write by accident.
    [[nodiscard]] inline std::array<glm::dvec3, 4> FibreFoldedAttenuationMean(f64 sinThetaO, f64 eta,
                                                                              const glm::dvec3& sigmaA, u32 nodes)
    {
        std::array<glm::dvec3, 4> mean{};
        for (u32 k = 0; k < nodes; ++k)
        {
            const f64 h = -1.0 + ((2.0 * (static_cast<f64>(k) + 0.5)) / static_cast<f64>(nodes));
            // 48 orders: the series ratio is T*f, well below 0.1 for keratin at
            // every angle short of grazing, so the truncation is decades below
            // any tolerance a caller can state.
            const FibrePathWalk walk = FibreCylinderWalk(sinThetaO, h, eta, sigmaA, 48);
            mean[0] += walk.Terms[0].Energy;
            mean[1] += walk.Terms[1].Energy;
            mean[2] += walk.Terms[2].Energy;
            mean[3] += walk.TailEnergy(3);
        }
        const f64 inv = 1.0 / static_cast<f64>(std::max(1u, nodes));
        for (glm::dvec3& value : mean)
            value *= inv;
        return mean;
    }

    // =========================================================================
    // 2. LEAF — a plane-parallel translucent slab. LOCAL.
    // =========================================================================
    //
    // CONVENTION:
    //
    //   * The slab occupies z in [0, tau] where tau is the OPTICAL thickness,
    //     dimensionless, measured in mean free paths of the extinction
    //     coefficient. A leaf lamina is quoted in millimetres by an artist; the
    //     conversion is the material's business, and this reference works in
    //     optical units so its numbers do not depend on one.
    //   * +z points from the LIT face towards the SHADED face. A direction's mu
    //     is its cosine with +z, so a photon leaving the shaded face has mu > 0
    //     and one leaving the lit face has mu < 0.
    //   * The boundaries are INDEX-MATCHED. A real leaf's cuticle has an index
    //     step and the step matters for the specular sheen, but the production
    //     term this judges has no refractive boundary at all, so adding one to
    //     the reference would measure the difference between two unrelated
    //     decisions. Stated, not hidden: this is the declared limitation of the
    //     leaf reference.
    //   * Scattering is Henyey-Greenstein with asymmetry g. Real leaf mesophyll
    //     is strongly forward scattering; every function takes g as an
    //     argument rather than fixing one.
    //
    // WHAT IS INDEPENDENT. The production term (oloFoliageTransmissionDirect in
    // FoliageSurface.glsl) is a phenomenological wrap-and-forward lobe with no
    // transport in it. This reference is transport and nothing else. They share
    // no parameter, which is the point: the comparison can only be about SHAPE
    // and ENERGY, and saying so is criterion 4's answer for this model.

    /// The exit-angle histogram of a slab, per face, plus the totals.
    struct SlabResponse
    {
        static constexpr sizet kBins = 16;

        /// Fraction of incident energy leaving the LIT face (z < 0).
        f64 Reflectance = 0.0;
        /// Fraction leaving the SHADED face (z > tau).
        f64 Transmittance = 0.0;
        /// Fraction absorbed, ACCUMULATED from the weight each scattering event
        /// removes — deliberately not derived as `1 - R - T`.
        ///
        /// The derived form was the first version and it made the closure
        /// assertion a TAUTOLOGY: `R + T + A == 1` held to 1e-12 whatever the
        /// walk did, because `A` was defined to make it hold. Accumulating the
        /// absorbed weight instead makes the three an independent measurement,
        /// so the sum is 1 only in EXPECTATION and only if nothing leaks.
        ///
        /// Survival roulette contributes nothing to this: a path killed there is
        /// sampling termination, not absorption, and its remaining weight is
        /// already accounted for by the survivors being raised to the floor.
        /// Adding it would double-count.
        f64 Absorbed = 0.0;

        /// Energy leaving each face binned by |mu| in kBins equal-cosine bins,
        /// bin i covering |mu| in [i/kBins, (i+1)/kBins). Equal-cosine bins
        /// carry equal projected solid angle, so a Lambertian exit distribution
        /// is FLAT in these bins — which is what makes a departure from
        /// Lambertian readable in a failure message.
        std::array<f64, kBins> ReflectedByCosine{};
        std::array<f64, kBins> TransmittedByCosine{};

        /// The centre cosine of bin `i`.
        [[nodiscard]] static f64 BinCosine(sizet i) noexcept
        {
            return (static_cast<f64>(i) + 0.5) / static_cast<f64>(kBins);
        }

        // --- the radiometry, spelled out, because getting it wrong is silent --
        //
        // The walk injects ONE PHOTON THROUGH THE SURFACE, which is unit
        // IRRADIANCE on the slab (power per unit slab area), not unit radiance
        // and not unit power through the beam's own cross-section. So the exit
        // radiance is L_o = f E_i = f, and the energy leaving through a bin is
        //
        //     E_bin = integral of f cos(theta_o) dw_o  =  f * mu_c * 2 pi * dmu
        //
        // which makes the BTDF  f = E_bin / (2 pi dmu mu_c).  THE COSINE IS THE
        // PART THAT IS EASY TO DROP, and dropping it leaves a quantity that is
        // not reciprocal — it is off by exactly mu_o/mu_i, which looks like a
        // broken transport solver rather than like a missing factor.

        /// The transmission BTDF at bin `i`, `1/sr`. This is the RECIPROCAL
        /// quantity: f(mu_i -> mu_o) == f(mu_o -> mu_i) for an index-matched
        /// slab, and a test can assert exactly that.
        [[nodiscard]] f64 TransmittedBtdf(sizet i) const noexcept
        {
            constexpr f64 binWidth = 1.0 / static_cast<f64>(kBins);
            return TransmittedByCosine[i] / (kTwoPi * binWidth * BinCosine(i));
        }

        /// The reflection BRDF at bin `i`, `1/sr`. Same derivation.
        [[nodiscard]] f64 ReflectedBrdf(sizet i) const noexcept
        {
            constexpr f64 binWidth = 1.0 / static_cast<f64>(kBins);
            return ReflectedByCosine[i] / (kTwoPi * binWidth * BinCosine(i));
        }
    };

    [[nodiscard]] inline glm::dvec3 HenyeyGreensteinSample(const glm::dvec3& w, f64 g, f64 u1, f64 u2) noexcept
    {
        // The published inversion is stated for the angle measured from the
        // direction the light CAME FROM (pbrt's `wo`, which points back along
        // the incoming ray), where g > 0 concentrates the density at -1. `w`
        // here is the direction of TRAVEL, the opposite axis, so the sampled
        // cosine is negated — and with that, g > 0 means forward scattering,
        // which is what every caller assumes and what a leaf's mesophyll does.
        //
        // Getting this sign wrong is survivable in a slab and therefore worth a
        // comment: a back-scattering slab still transmits, still darkens with
        // thickness and still peaks near the normal exit, because the shortest
        // path out dominates. It is only the lobe's WIDTH that gives it away.
        f64 cosTheta = 0.0;
        if (std::abs(g) < 1.0e-4)
        {
            cosTheta = 1.0 - (2.0 * u1);
        }
        else
        {
            const f64 sqr = (1.0 - (g * g)) / (1.0 + g - (2.0 * g * u1));
            cosTheta = (1.0 + (g * g) - (sqr * sqr)) / (2.0 * g);
        }
        cosTheta = std::clamp(cosTheta, -1.0, 1.0);

        const f64 sinTheta = SafeSqrt(1.0 - (cosTheta * cosTheta));
        const f64 phi = kTwoPi * u2;

        // Duff's branchless orthonormal basis around w, in doubles.
        const f64 sign = (w.z >= 0.0) ? 1.0 : -1.0;
        const f64 a = -1.0 / (sign + w.z);
        const f64 b = w.x * w.y * a;
        const glm::dvec3 t(1.0 + (sign * w.x * w.x * a), sign * b, -sign * w.x);
        const glm::dvec3 s(b, sign + (w.y * w.y * a), -w.y);

        return glm::normalize((t * (sinTheta * std::cos(phi))) + (s * (sinTheta * std::sin(phi))) + (w * cosTheta));
    }

    /// Random-walk a plane-parallel slab.
    ///
    ///   `opticalThickness`  tau, dimensionless, > 0.
    ///   `albedo`            single-scattering albedo in [0, 1].
    ///   `g`                 Henyey-Greenstein asymmetry in (-1, 1).
    ///   `muIncident`        cosine of the incident direction with +z, > 0
    ///                       (the beam travels INTO the slab).
    [[nodiscard]] inline SlabResponse SlabRandomWalk(f64 opticalThickness, f64 albedo, f64 g, f64 muIncident,
                                                     u32 samples, u64 seed)
    {
        SlabResponse response;
        Pcg32 rng(seed);

        f64 absorbed = 0.0;

        const f64 sinIncident = SafeSqrt(1.0 - (muIncident * muIncident));
        const glm::dvec3 entry(sinIncident, 0.0, muIncident);

        constexpr f64 kRouletteFloor = 0.05;

        for (u32 s = 0; s < samples; ++s)
        {
            glm::dvec3 direction = entry;
            f64 z = 0.0;
            f64 weight = 1.0;

            for (;;)
            {
                const f64 step = -std::log(1.0 - rng.NextDouble());
                z += direction.z * step;

                if (z <= 0.0)
                {
                    const f64 mu = std::clamp(-direction.z, 0.0, 1.0);
                    response.Reflectance += weight;
                    const sizet bin = std::min(SlabResponse::kBins - 1u,
                                               static_cast<sizet>(mu * static_cast<f64>(SlabResponse::kBins)));
                    response.ReflectedByCosine[bin] += weight;
                    break;
                }
                if (z >= opticalThickness)
                {
                    const f64 mu = std::clamp(direction.z, 0.0, 1.0);
                    response.Transmittance += weight;
                    const sizet bin = std::min(SlabResponse::kBins - 1u,
                                               static_cast<sizet>(mu * static_cast<f64>(SlabResponse::kBins)));
                    response.TransmittedByCosine[bin] += weight;
                    break;
                }

                // Absorption as a weight, then survival roulette once the
                // weight stops being worth tracking. Killing the photon outright
                // at (1 - albedo) would be correct too, and noisier at the low
                // albedos a dark leaf uses.
                const f64 beforeAbsorption = weight;
                weight *= albedo;
                absorbed += beforeAbsorption - weight;

                if (weight < kRouletteFloor)
                {
                    // A killed path's remaining weight is NOT absorption. The
                    // roulette is unbiased — survivors come back at the floor
                    // with probability weight/floor — so the expected energy is
                    // already conserved without booking anything here.
                    if (rng.NextDouble() > (weight / kRouletteFloor))
                        break;
                    weight = kRouletteFloor;
                }

                direction = HenyeyGreensteinSample(direction, g, rng.NextDouble(), rng.NextDouble());
            }
        }

        const f64 inv = 1.0 / static_cast<f64>(std::max(1u, samples));
        response.Reflectance *= inv;
        response.Transmittance *= inv;
        response.Absorbed = absorbed * inv;

        for (sizet i = 0; i < SlabResponse::kBins; ++i)
        {
            response.ReflectedByCosine[i] *= inv;
            response.TransmittedByCosine[i] *= inv;
        }
        return response;
    }

    // =========================================================================
    // 3. SKIN — a semi-infinite searchlight random walk. NONLOCAL.
    // =========================================================================
    //
    // THIS IS NOT A BSDF AND THE TESTS THAT CONSUME IT ARE NOT BSDF TESTS.
    // Subsurface diffusion moves energy ACROSS the surface: the quantity is
    // R(r), the radiant exitance at distance r from a normally incident pencil
    // beam, and its argument is a LENGTH and nothing else. Reciprocity has no
    // referent here; neither does a sampling-consistency check of the kind the
    // fibre BCSDF gets, because there is no exit direction being sampled. What
    // IS meaningful, and what these functions supply, is the radial SHAPE of
    // the profile and the total energy under it.
    //
    // CONVENTION:
    //
    //   * The medium is semi-infinite, homogeneous, index-MATCHED at the
    //     boundary, with isotropic scattering. Isotropic is not a simplification
    //     of convenience: Christensen and Burley's fit is stated for the
    //     similarity-reduced medium, so the reference has to be in the same
    //     reduced form or the comparison is between two different media.
    //   * Lengths are in MEAN FREE PATHS of the reduced extinction coefficient,
    //     i.e. sigma_t' = 1. The production side works in MILLIMETRES with an
    //     authored ScatterRadiusMM that its own header calls a mean free path,
    //     so one millimetre of authored radius is one unit here. That identity
    //     is the whole unit bridge between the two sides, and the tests assert
    //     it rather than assuming it.
    //   * `albedo` is the single-scattering albedo of the reduced medium, which
    //     is the quantity SkinProfileParameters::ScatterColor carries per
    //     channel.
    //
    // WHAT IS INDEPENDENT. Production evaluates a two-exponential closed form
    // fitted to this random walk. This reference IS the random walk. Nothing is
    // shared but the medium's definition.

    /// THE PARAMETER BRIDGE, and the one place a factor of two hides.
    ///
    /// The single-scattering albedo of the medium whose DIFFUSE SURFACE ALBEDO
    /// — the total fraction of a normally incident beam that comes back out —
    /// is `diffuseAlbedo`. The two are wildly different numbers: a surface that
    /// returns 85% of the light needs a single-scattering albedo of 0.997,
    /// because almost every photon has to survive dozens of scattering events
    /// to get back out.
    ///
    /// WHY THIS FUNCTION EXISTS RATHER THAN A DIRECT COMPARISON.
    /// SkinProfileParameters::ScatterColor is documented as "the fraction of
    /// light entering the surface that leaves it again rather than being
    /// absorbed", which is the DIFFUSE surface albedo, and that is what
    /// Christensen and Burley's shape fit s(A) takes. Driving the reference walk
    /// with ScatterColor as though it were a single-scattering albedo compares
    /// two different media and reports the Burley profile as twice too wide —
    /// which is what the first version of the skin test did, and the error is
    /// entirely in the test rather than in the renderer.
    ///
    /// The inversion is closed form, from the classical semi-infinite
    /// isotropic result A = 1 - sqrt(1-a) H(1) with the standard rational
    /// approximation H(1) = 3 / (1 + 2 sqrt(1-a)). Writing u = sqrt(1-a),
    ///
    ///     A = 1 - 3u / (1 + 2u)   =>   u = (1 - A) / (1 + 2A)
    ///
    /// so a = 1 - u^2. Approximate, so the test that uses it ASSERTS the walk's
    /// measured reflectance against the target rather than trusting it; measured
    /// agreement is under 1% of reflectance across [0.35, 0.95].
    [[nodiscard]] inline f64 SingleScatteringAlbedoForDiffuseAlbedo(f64 diffuseAlbedo) noexcept
    {
        const f64 a = std::clamp(diffuseAlbedo, 0.0, 0.999);
        const f64 u = (1.0 - a) / (1.0 + (2.0 * a));
        return std::clamp(1.0 - (u * u), 0.0, 1.0);
    }

    struct SearchlightProfile
    {
        /// Total diffuse reflectance — the fraction of the beam that comes back
        /// out. This is the number the closed form does NOT carry: Burley's
        /// R(r) is normalised to integrate to one, so the albedo's effect on
        /// TOTAL energy is the renderer's job and only the SHAPE is the
        /// profile's. Keeping it here lets a test say that out loud.
        f64 DiffuseReflectance = 0.0;

        /// Bin edges in mean free paths; `Edges.size() == Energy.size() + 1`.
        std::vector<f64> Edges;
        /// Fraction of the TOTAL EXITING energy landing in each annulus,
        /// normalised to sum to 1 across the bins plus Overflow — so it is
        /// directly a discrete version of the unit-energy profile the
        /// production side ships.
        std::vector<f64> Energy;
        /// Energy that escaped beyond the outermost edge. A reference whose tail
        /// silently fell off the end of its histogram would understate every
        /// quantile, so the overflow is reported rather than dropped.
        f64 Overflow = 0.0;

        /// The radius, in mean free paths, containing `fraction` of the exiting
        /// energy, by linear interpolation inside the containing annulus.
        /// Returns the outermost edge when the fraction lies in the overflow.
        [[nodiscard]] f64 RadiusForFraction(f64 fraction) const
        {
            f64 running = 0.0;
            for (sizet i = 0; i < Energy.size(); ++i)
            {
                const f64 next = running + Energy[i];
                if (next >= fraction && Energy[i] > 0.0)
                {
                    const f64 t = (fraction - running) / Energy[i];
                    return Edges[i] + (t * (Edges[i + 1] - Edges[i]));
                }
                running = next;
            }
            return Edges.back();
        }

        /// The fraction of exiting energy inside radius `r` — the empirical CDF
        /// the production SkinBurleyCdf is compared against.
        [[nodiscard]] f64 CdfAt(f64 r) const
        {
            f64 running = 0.0;
            for (sizet i = 0; i < Energy.size(); ++i)
            {
                if (r >= Edges[i + 1])
                {
                    running += Energy[i];
                    continue;
                }
                if (r <= Edges[i])
                    break;
                const f64 t = (r - Edges[i]) / (Edges[i + 1] - Edges[i]);
                running += t * Energy[i];
                break;
            }
            return running;
        }
    };

    /// Random-walk the searchlight configuration.
    ///
    ///   `albedo`     single-scattering albedo of the reduced medium, [0, 1).
    ///   `maxRadius`  outermost histogram edge, in mean free paths.
    ///   `binCount`   annuli, uniformly spaced in RADIUS (not in area): the
    ///                profile is compared as a CDF, and uniform radius bins
    ///                resolve the near field where the CDF moves fastest.
    [[nodiscard]] inline SearchlightProfile SearchlightRandomWalk(f64 albedo, f64 maxRadius, sizet binCount,
                                                                  u32 samples, u64 seed)
    {
        SearchlightProfile profile;
        profile.Edges.resize(binCount + 1u);
        profile.Energy.assign(binCount, 0.0);
        for (sizet i = 0; i <= binCount; ++i)
            profile.Edges[i] = (maxRadius * static_cast<f64>(i)) / static_cast<f64>(binCount);

        Pcg32 rng(seed);
        constexpr f64 kRouletteFloor = 0.02;

        f64 exited = 0.0;
        for (u32 s = 0; s < samples; ++s)
        {
            // The beam enters normally, so the FIRST interaction is at depth
            // z ~ Exp(1) directly below the entry point; nothing can exit before
            // it, and the energy that survives to it is the full beam.
            glm::dvec3 position(0.0, 0.0, -std::log(1.0 - rng.NextDouble()));
            f64 weight = albedo;

            for (;;)
            {
                if (weight < kRouletteFloor)
                {
                    if (rng.NextDouble() > (weight / kRouletteFloor))
                        break;
                    weight = kRouletteFloor;
                }

                // Isotropic scatter, then a free flight.
                const f64 cosTheta = 1.0 - (2.0 * rng.NextDouble());
                const f64 sinTheta = SafeSqrt(1.0 - (cosTheta * cosTheta));
                const f64 phi = kTwoPi * rng.NextDouble();
                const glm::dvec3 direction(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);

                const f64 step = -std::log(1.0 - rng.NextDouble());
                const glm::dvec3 next = position + (direction * step);

                if (next.z <= 0.0)
                {
                    // Crossed the surface: the exit point is where z == 0.
                    const f64 t = position.z / std::max(position.z - next.z, 1.0e-15);
                    const glm::dvec3 exitPoint = position + ((next - position) * t);
                    const f64 r = std::sqrt((exitPoint.x * exitPoint.x) + (exitPoint.y * exitPoint.y));

                    exited += weight;
                    if (r >= maxRadius)
                    {
                        profile.Overflow += weight;
                    }
                    else
                    {
                        const sizet bin =
                            std::min(binCount - 1u, static_cast<sizet>((r / maxRadius) * static_cast<f64>(binCount)));
                        profile.Energy[bin] += weight;
                    }
                    break;
                }

                position = next;
                weight *= albedo;
            }
        }

        profile.DiffuseReflectance = exited / static_cast<f64>(std::max(1u, samples));

        if (exited > 0.0)
        {
            for (f64& value : profile.Energy)
                value /= exited;
            profile.Overflow /= exited;
        }
        return profile;
    }

    // =========================================================================
    // 4. COAT TRANSPORT — a stochastic fibre medium. NONLOCAL.
    // =========================================================================
    //
    // THE QUESTION IS AN EXPECTATION OF AN EXPONENTIAL, AND THE PRODUCTION
    // ANSWER IS AN EXPONENTIAL OF AN EXPECTATION. GroomCoatShadow computes
    // tau = E[crossings] over a jittered ray bundle and returns exp(-kappa tau).
    // The transmittance a fragment footprint actually receives is the MEAN of
    // the per-ray transmittances, E[exp(-kappa N)]. Jensen's inequality makes
    // the second at least the first, with equality only when N has no variance,
    // so the shipped form is a LOWER BOUND on transmittance — it over-darkens,
    // by an amount that grows with the disorder of the coat. That is a
    // statement about a model rather than a bug in an implementation, and it is
    // exactly what an independent reference is for.
    //
    // CONVENTION: lengths are in the segment set's own units (metres, for a
    // coat built by BuildCoatSegments from a scene-space groom). `kappa` is the
    // dimensionless per-crossing extinction the production model authors: 1.0
    // means one expected crossing attenuates to 1/e.
    //
    // WHAT IS INDEPENDENT. Two things, checking different halves. The Monte
    // Carlo walk below counts crossings through a medium this file GENERATES,
    // with an intersection test written here, so it shares no code with the
    // production ray-cylinder routine. The closed form beside it is analytic:
    // for a Poisson medium the crossing count is Poisson distributed and
    //     E[exp(-kappa N)] = exp(-mu (1 - e^-kappa))
    // exactly, which is a ground truth with no sampling error at all.

    /// One realised straight fibre, for the synthetic medium.
    struct ReferenceFibre
    {
        glm::dvec3 Origin{ 0.0 };
        glm::dvec3 Axis{ 0.0, 0.0, 1.0 };
        f64 Radius = 0.0;
        f64 HalfLength = 0.0;
    };

    /// Does the ray from `origin` along unit `direction` cross `fibre` within
    /// its half-length and within `maxDistance`?
    ///
    /// The classical quadratic in the component perpendicular to the fibre
    /// axis. Independent of GroomCoatShadow's own intersection routine by
    /// construction — this is test-local and nothing in the engine calls it.
    [[nodiscard]] inline bool RayCrossesFibre(const glm::dvec3& origin, const glm::dvec3& direction,
                                              const ReferenceFibre& fibre, f64 maxDistance) noexcept
    {
        const glm::dvec3 delta = origin - fibre.Origin;
        const glm::dvec3 dPerp = direction - (fibre.Axis * glm::dot(direction, fibre.Axis));
        const glm::dvec3 oPerp = delta - (fibre.Axis * glm::dot(delta, fibre.Axis));

        const f64 a = glm::dot(dPerp, dPerp);
        if (a < 1.0e-18)
            return false; // parallel to the fibre: a grazing case of measure zero
        const f64 b = 2.0 * glm::dot(dPerp, oPerp);
        const f64 c = glm::dot(oPerp, oPerp) - (fibre.Radius * fibre.Radius);
        const f64 disc = (b * b) - (4.0 * a * c);
        if (disc < 0.0)
            return false;

        const f64 sqrtDisc = std::sqrt(disc);
        for (const f64 t : { (-b - sqrtDisc) / (2.0 * a), (-b + sqrtDisc) / (2.0 * a) })
        {
            if (t < 0.0 || t > maxDistance)
                continue;
            const f64 along = glm::dot((origin + (direction * t)) - fibre.Origin, fibre.Axis);
            if (std::abs(along) <= fibre.HalfLength)
                return true;
        }
        return false;
    }

    struct CoatTransmittanceEstimate
    {
        /// E[N] — the expected crossing count. This is what the production
        /// optical depth means.
        f64 MeanCrossings = 0.0;
        /// Var[N]. Near zero for a regular coat; equal to the mean for a
        /// Poisson one.
        f64 CrossingVariance = 0.0;
        /// E[exp(-kappa N)] — the transmittance the footprint actually gets.
        f64 MeanTransmittance = 0.0;
        /// exp(-kappa E[N]) — what the production model returns.
        f64 ExponentialOfMean = 0.0;
    };

    /// Trace a jittered bundle through a realised fibre set and report both
    /// estimators.
    ///
    ///   `footprintRadius`  radius of the disc the bundle origins are jittered
    ///                      over, perpendicular to `direction`. It must span
    ///                      several fibre spacings or the estimate aliases
    ///                      against the medium — the same trap
    ///                      GroomCoatShadow::ReferenceSettings documents, and
    ///                      it applies to this reference for the same reason.
    [[nodiscard]] inline CoatTransmittanceEstimate CoatBundleTransmittance(const std::vector<ReferenceFibre>& fibres,
                                                                           const glm::dvec3& origin,
                                                                           const glm::dvec3& direction, f64 kappa,
                                                                           f64 footprintRadius, f64 maxDistance,
                                                                           u32 rays, u64 seed)
    {
        CoatTransmittanceEstimate estimate;
        if (rays == 0)
            return estimate;

        const glm::dvec3 d = glm::normalize(direction);
        const f64 sign = (d.z >= 0.0) ? 1.0 : -1.0;
        const f64 a = -1.0 / (sign + d.z);
        const f64 b = d.x * d.y * a;
        const glm::dvec3 tangent(1.0 + (sign * d.x * d.x * a), sign * b, -sign * d.x);
        const glm::dvec3 bitangent(b, sign + (d.y * d.y * a), -d.y);

        Pcg32 rng(seed);
        f64 sumN = 0.0;
        f64 sumN2 = 0.0;
        f64 sumT = 0.0;

        for (u32 i = 0; i < rays; ++i)
        {
            // sqrt of the first uniform, so the origins are uniform over the
            // disc's AREA rather than crowded at its centre.
            const f64 r = footprintRadius * std::sqrt(rng.NextDouble());
            const f64 phi = kTwoPi * rng.NextDouble();
            const glm::dvec3 o = origin + (tangent * (r * std::cos(phi))) + (bitangent * (r * std::sin(phi)));

            u32 crossings = 0;
            for (const ReferenceFibre& fibre : fibres)
            {
                if (RayCrossesFibre(o, d, fibre, maxDistance))
                    ++crossings;
            }

            const f64 n = static_cast<f64>(crossings);
            sumN += n;
            sumN2 += n * n;
            sumT += std::exp(-kappa * n);
        }

        const f64 inv = 1.0 / static_cast<f64>(rays);
        estimate.MeanCrossings = sumN * inv;
        estimate.CrossingVariance = std::max(0.0, (sumN2 * inv) - (estimate.MeanCrossings * estimate.MeanCrossings));
        estimate.MeanTransmittance = sumT * inv;
        estimate.ExponentialOfMean = std::exp(-kappa * estimate.MeanCrossings);
        return estimate;
    }

    /// The exact transmittance of a POISSON medium whose mean crossing count is
    /// `mu`: E[exp(-kappa N)] with N ~ Poisson(mu) is that distribution's
    /// probability generating function evaluated at exp(-kappa).
    ///
    /// Analytic, so it carries no sampling error and is the reference the Monte
    /// Carlo estimate above is itself checked against.
    [[nodiscard]] inline f64 PoissonMediumTransmittance(f64 mu, f64 kappa) noexcept
    {
        return std::exp(-mu * (1.0 - std::exp(-kappa)));
    }

    /// A cube of randomly placed, randomly oriented fibres — a Poisson medium
    /// by construction, so PoissonMediumTransmittance applies to it exactly.
    ///
    /// `extent` is the half-size of the cube the fibre centres are drawn in,
    /// `radius` their common radius, `halfLength` their common half-length.
    [[nodiscard]] inline std::vector<ReferenceFibre> MakePoissonFibreSlab(f64 extent, f64 radius, f64 halfLength,
                                                                          u32 count, u64 seed)
    {
        std::vector<ReferenceFibre> fibres;
        fibres.reserve(count);
        Pcg32 rng(seed);

        for (u32 i = 0; i < count; ++i)
        {
            ReferenceFibre fibre;
            fibre.Origin = glm::dvec3(extent * ((2.0 * rng.NextDouble()) - 1.0),
                                      extent * ((2.0 * rng.NextDouble()) - 1.0),
                                      extent * ((2.0 * rng.NextDouble()) - 1.0));
            const f64 cosTheta = 1.0 - (2.0 * rng.NextDouble());
            const f64 sinTheta = SafeSqrt(1.0 - (cosTheta * cosTheta));
            const f64 phi = kTwoPi * rng.NextDouble();
            fibre.Axis = glm::dvec3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
            fibre.Radius = radius;
            fibre.HalfLength = halfLength;
            fibres.push_back(fibre);
        }
        return fibres;
    }

    /// A regular lattice of parallel fibres — the ORDERED control.
    ///
    /// GEOMETRY, and it is chosen so that one ray direction has a DETERMINISTIC
    /// crossing count. The fibres run along +y, at lattice positions (x_i, z_j)
    /// with spacing `pitch`. A ray along +x therefore keeps its z, and crosses
    /// every one of the `perAxis` columns of whichever z-row it falls in — so
    /// with `radius == pitch / 2` the rows tile the z axis, every ray falls in
    /// exactly one, and the count is `perAxis` for all of them. The crossing
    /// variance collapses to zero and Jensen's gap closes with it, which is the
    /// control the disordered measurement needs.
    ///
    /// THE FIRST VERSION OF THIS FUNCTION LAID THE FIBRES ALONG +z AND WAS THE
    /// OPPOSITE OF A CONTROL. A ray along +x through z-aligned fibres crosses
    /// either the whole of a row or none of it, so the count was bimodal 0-or-40
    /// and its variance was THIRTY TIMES the mean — more disordered than the
    /// Poisson medium it was meant to be a control for. "Regular" is a property
    /// of a medium AND a ray direction, never of the medium alone, and a lattice
    /// viewed down its own axis is the worst case rather than the best.
    ///
    /// With `radius < pitch / 2` the rows leave gaps, which is what the
    /// footprint-aliasing case wants instead.
    [[nodiscard]] inline std::vector<ReferenceFibre> MakeLatticeFibreSlab(f64 pitch, f64 radius, f64 halfLength,
                                                                          u32 perAxis)
    {
        std::vector<ReferenceFibre> fibres;
        fibres.reserve(static_cast<sizet>(perAxis) * perAxis);
        const f64 origin = -0.5 * pitch * static_cast<f64>(perAxis - 1u);

        for (u32 i = 0; i < perAxis; ++i)
        {
            for (u32 j = 0; j < perAxis; ++j)
            {
                ReferenceFibre fibre;
                fibre.Origin =
                    glm::dvec3(origin + (pitch * static_cast<f64>(i)), 0.0, origin + (pitch * static_cast<f64>(j)));
                fibre.Axis = glm::dvec3(0.0, 1.0, 0.0);
                fibre.Radius = radius;
                fibre.HalfLength = halfLength;
                fibres.push_back(fibre);
            }
        }
        return fibres;
    }
} // namespace OloEngine::Tests::MaterialReference
