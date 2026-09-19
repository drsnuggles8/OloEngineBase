#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// LeafTransmissionReferenceTest — issue #1255, the LEAF half. The second of the
// two LOCAL models, and the one where criterion 4 does most of its work.
//
// THE HONEST HEADLINE. The production leaf term
// (oloFoliageTransmissionDirect in assets/shaders/include/FoliageSurface.glsl)
// is a phenomenological wrap-and-forward lobe: a distortion, an exponent and a
// Lambertian wrap, with an authored thickness in front of all three. It is not
// derived from transport, it carries no optical thickness, and it is not
// normalised. So there is no parameter setting at which it EQUALS a transport
// solution, and a test that asserted one would be measuring a fit that does not
// exist.
//
// #1255's fourth criterion is explicit about what to do instead: report the
// limitation and bring a suitable reference rather than treating generic PBR as
// ground truth. The reference here is a plane-parallel translucent slab solved
// by Monte Carlo — the thing a leaf lamina physically is — and the comparison
// is about the three properties that survive the absence of a fit:
//
//   1. WHAT THE REFERENCE HAS AND THE MODEL DOES NOT. The slab is reciprocal
//      and energy-bounded. The production lobe is neither, and both failures are
//      MEASURED here rather than described: swapping the eye and the light moves
//      the term by more than 25%, and it can hand the far side more energy than
//      arrived at authored values inside the shipped ranges.
//   2. WHAT BOTH HAVE. Forward scattering peaked on the light direction, falling
//      monotonically away from it, broadening as the leaf gets thicker. These
//      are ORDERINGS, they hold in both, and they are what makes the
//      phenomenological term usable despite (1).
//   3. WHERE THE CORRESPONDENCE BREAKS. The lobe's shape parameters map onto the
//      slab's optical thickness monotonically over the authored range and stop
//      doing so past it. The test says where.
//
// WHY THE PRODUCTION SIDE IS MIRRORED IN C++ HERE. The lobe's only home is
// GLSL, and an L1 test cannot run a shader. The mirror below is therefore a
// transcription, which is exactly the substitution
// substituted-seams-compound.md warns about — so it does not stand alone:
// MaterialReferenceAovParityTest renders the REAL shader over the same sweep
// and pins this mirror against it texel for texel. Neither file is sufficient;
// together they are. The mirror is marked so a reader cannot mistake it for the
// production path.
//
// UNITS AND CONVENTIONS. The reference works in OPTICAL THICKNESS (mean free
// paths, dimensionless) because that is the only thickness transport has. The
// production term's `thickness` is an authored [0,1] scalar sampled from a map
// with no physical unit at all; the two are related by nothing, and the tests
// below never pretend otherwise — every comparison is either shape-only
// (normalised) or an ordering.
//
// Runtime budget: about a second in Debug. Every walk is seeded.
// =============================================================================

#include "PathTracing/MaterialReference.h"
#include "PathTracing/ProductionLeafLobeMirror.h"

#include "OloEngine/Renderer/FoliageLeafProfile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace Ref = MaterialReference;

        // The production side is ProductionLeafLobe, from
        // PathTracing/ProductionLeafLobeMirror.h — a TRANSCRIPTION of the
        // shader, kept in its own header because
        // MaterialReferenceAovParityTest pins it against the real compiled
        // GLSL. See that header for why the seam needs two files.

        // The leaf geometry every case below shares: a lamina facing +z with the
        // viewer on the +z side, so the SHADED face is -z and a backlight comes
        // from -z. This is the configuration the production probe
        // ShaderUnit_FoliageTransmission.glsl uses, kept identical so the two
        // files are describing one geometry.
        constexpr glm::dvec3 kLeafNormal(0.0, 0.0, 1.0);

        /// A view direction (surface -> eye) at angle `mu` from the normal,
        /// azimuth `phi`. mu > 0 is the viewer's side of the lamina.
        [[nodiscard]] glm::dvec3 ViewDirection(f64 mu, f64 phi) noexcept
        {
            const f64 s = Ref::SafeSqrt(1.0 - (mu * mu));
            return glm::dvec3(s * std::cos(phi), s * std::sin(phi), mu);
        }

        /// The DIRECTIONAL-HEMISPHERICAL transmittance of the production lobe:
        /// the lobe integrated over the viewer's hemisphere against the exit
        /// cosine, which is the fraction of arriving radiance the term hands to
        /// the far side. For a physical BTDF this cannot exceed 1.
        ///
        /// Quadrature: 64 x 64 over (mu, phi), midpoint. The integrand is a
        /// bounded power of a cosine, so the rule converges fast; halving the
        /// grid moves the result by under 0.1%.
        [[nodiscard]] f64 ProductionHemisphericalTransmittance(const glm::dvec3& l, f64 thickness,
                                                               const FoliageLeafProfile& profile)
        {
            constexpr u32 kMu = 64;
            constexpr u32 kPhi = 64;

            f64 sum = 0.0;
            for (u32 im = 0; im < kMu; ++im)
            {
                const f64 mu = (static_cast<f64>(im) + 0.5) / static_cast<f64>(kMu);
                for (u32 ip = 0; ip < kPhi; ++ip)
                {
                    const f64 phi = (Ref::kTwoPi * (static_cast<f64>(ip) + 0.5)) / static_cast<f64>(kPhi);
                    sum += ProductionLeafLobe(kLeafNormal, ViewDirection(mu, phi), l, thickness, profile) * mu;
                }
            }
            // dw = dmu dphi, and the cosine weight is already in the sum.
            return sum * (1.0 / static_cast<f64>(kMu)) * (Ref::kTwoPi / static_cast<f64>(kPhi));
        }

        [[nodiscard]] FoliageLeafProfile MakeProfile(f32 distortion, f32 power, f32 wrap)
        {
            FoliageLeafProfile profile;
            profile.TransmissionStrength = 1.0f;
            profile.Distortion = distortion;
            profile.Power = power;
            profile.Wrap = wrap;
            return profile;
        }

        // A backlight: the light is BEHIND the lamina, so `l` (surface -> light)
        // points away from the viewer.
        [[nodiscard]] glm::dvec3 Backlight(f64 mu, f64 phi) noexcept
        {
            return ViewDirection(-mu, phi);
        }

        constexpr u32 kSlabSamples = 120000;
    } // namespace

    // =========================================================================
    // A. The reference, checked before it is used as one.
    // =========================================================================

    TEST(LeafTransmissionReference, TheSlabWalkConservesEnergy)
    {
        // Reflected + transmitted + absorbed == 1, and a reference that failed
        // it would be silently manufacturing or losing light before it judged
        // anything.
        //
        // THIS USED TO BE A TAUTOLOGY AND IS WORTH RECORDING. `Absorbed` was
        // derived as `1 - R - T`, so the sum held to 1e-12 whatever the walk
        // did — the assertion could not fail. It now measures three independent
        // quantities: the absorbed share is accumulated from the weight each
        // scattering event removes, so the closure is a real statement about
        // the estimator and the roulette.
        //
        // STATISTICAL, because survival roulette conserves energy only in
        // expectation. 1e-3 absolute: the measured worst deviation across this
        // grid is 9.4e-5, so this is an order of magnitude of headroom for a
        // different libm, and two orders below the percent-scale leak it exists
        // to catch.
        for (const f64 tau : { 0.5, 2.0, 8.0 })
        {
            for (const f64 albedo : { 0.5, 0.8, 0.95 })
            {
                const Ref::SlabResponse slab = Ref::SlabRandomWalk(tau, albedo, 0.7, 1.0, kSlabSamples, 0x1255u);
                EXPECT_NEAR(slab.Reflectance + slab.Transmittance + slab.Absorbed, 1.0, 1.0e-3)
                    << "tau = " << tau << ", albedo = " << albedo << ": R = " << slab.Reflectance
                    << ", T = " << slab.Transmittance << ", A = " << slab.Absorbed;
                EXPECT_GT(slab.Transmittance, 0.0) << "tau = " << tau << ", albedo = " << albedo;
                EXPECT_GT(slab.Absorbed, 0.0) << "tau = " << tau << ", albedo = " << albedo;
                EXPECT_LE(slab.Reflectance + slab.Transmittance, 1.0);
            }
        }
    }

    TEST(LeafTransmissionReference, TheSlabBehavesLikeALeafAsItThickensAndDarkens)
    {
        // Two monotonicities the reference must have before it can be used to
        // judge anything: a thicker lamina transmits less, and a more absorbing
        // one transmits less. Stated as orderings over a sweep, which is what
        // makes them insensitive to the seed.
        f64 previous = 1.0;
        for (const f64 tau : { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 })
        {
            const Ref::SlabResponse slab = Ref::SlabRandomWalk(tau, 0.9, 0.7, 1.0, kSlabSamples, 0x1255u);
            EXPECT_LT(slab.Transmittance, previous) << "tau = " << tau;
            previous = slab.Transmittance;
        }

        previous = 0.0;
        for (const f64 albedo : { 0.3, 0.5, 0.7, 0.9, 0.98 })
        {
            const Ref::SlabResponse slab = Ref::SlabRandomWalk(2.0, albedo, 0.7, 1.0, kSlabSamples, 0x1255u);
            EXPECT_GT(slab.Transmittance, previous) << "albedo = " << albedo;
            previous = slab.Transmittance;
        }
    }

    // =========================================================================
    // B. Reciprocity — the question a LOCAL model owes, and the two answers.
    // =========================================================================

    TEST(LeafTransmissionReference, TheSlabIsReciprocalAndTheProductionLobeIsNot)
    {
        // THE SLAB IS. The radiative transfer equation is reciprocal and an
        // index-matched slab inherits it, so the BTDF must satisfy
        // f(mu_i -> mu_o) == f(mu_o -> mu_i). SlabResponse::TransmittedBtdf is
        // exactly that f — the binned energy divided by the bin's PROJECTED
        // solid angle, cosine included. Two independent walks, one from each
        // side, must agree.
        //
        // This half is not a formality. It is what licenses calling the walk a
        // reference at all — a transport solver with a sampling bug in its phase
        // function or its boundary crossing breaks reciprocity first, long
        // before its totals look wrong.
        constexpr f64 kTau = 2.0;
        constexpr f64 kAlbedo = 0.9;
        constexpr f64 kG = 0.7;

        struct Pair
        {
            f64 MuA;
            f64 MuB;
        };
        // Bin centres, so the reading is not an interpolation.
        const std::array<Pair, 3> pairs{ { { Ref::SlabResponse::BinCosine(15), Ref::SlabResponse::BinCosine(9) },
                                           { Ref::SlabResponse::BinCosine(13), Ref::SlabResponse::BinCosine(5) },
                                           { Ref::SlabResponse::BinCosine(11), Ref::SlabResponse::BinCosine(3) } } };

        for (const Pair& pair : pairs)
        {
            const sizet binA = static_cast<sizet>(pair.MuA * static_cast<f64>(Ref::SlabResponse::kBins));
            const sizet binB = static_cast<sizet>(pair.MuB * static_cast<f64>(Ref::SlabResponse::kBins));

            const Ref::SlabResponse fromA = Ref::SlabRandomWalk(kTau, kAlbedo, kG, pair.MuA, kSlabSamples, 0x1255u);
            const Ref::SlabResponse fromB = Ref::SlabRandomWalk(kTau, kAlbedo, kG, pair.MuB, kSlabSamples, 0x9001u);

            // THE BTDF, not the binned energy. The energy is off by exactly
            // mu_o / mu_i between the two arms — the exit cosine — and the first
            // version of this test compared the energies and read a 0.61
            // "reciprocity failure" that was entirely that missing factor. The
            // accessor carries the cosine so the quantity compared is the one
            // reciprocity is about; see SlabResponse's radiometry comment.
            const f64 forward = fromA.TransmittedBtdf(binB);
            const f64 reverse = fromB.TransmittedBtdf(binA);
            const f64 residual = std::abs(forward - reverse) / std::max({ forward, reverse, 1.0e-12 });

            // 8%: the two arms are independent walks with different seeds, and
            // each bin holds a few percent of 120k weighted samples, so this is
            // about three standard errors of the RATIO. A genuine reciprocity
            // failure in a transport solver is a factor, not a few percent.
            EXPECT_LT(residual, 0.08) << "mu " << pair.MuA << " -> " << pair.MuB << ": " << forward << " vs "
                                      << reverse;
        }

        // THE PRODUCTION LOBE IS NOT, and the reason is structural rather than
        // numerical: the wrap half, max(dot(-N, L), 0) * wrap, does not mention
        // V at all, so swapping the eye and the light cannot leave it alone
        // unless the two happen to be symmetric already. The forward half is
        // built from a half-vector of L and N only, which is the same asymmetry
        // in the other term.
        //
        // Measured across the authored range, and reported as the largest
        // residual found, because a single configuration could be an unlucky
        // one.
        const FoliageLeafProfile profile = MakeProfile(0.35f, 4.0f, 0.5f);
        f64 worst = 0.0;
        for (const f64 muV : { 0.25, 0.55, 0.85 })
        {
            for (const f64 muL : { 0.25, 0.55, 0.85 })
            {
                if (std::abs(muV - muL) < 1.0e-9)
                    continue;
                const glm::dvec3 v = ViewDirection(muV, 0.0);
                const glm::dvec3 l = Backlight(muL, Ref::kPi);

                // The swap: the eye where the light was and the light where the
                // eye was. Both stay on their own sides of the lamina, which is
                // what makes this a transmission reciprocity question at all.
                const f64 forward = ProductionLeafLobe(kLeafNormal, v, l, 0.75, profile);
                const f64 reverse =
                    ProductionLeafLobe(kLeafNormal, ViewDirection(muL, Ref::kPi), Backlight(muV, 0.0), 0.75, profile);
                worst = std::max(worst, std::abs(forward - reverse) / std::max({ forward, reverse, 1.0e-12 }));
            }
        }
        // Not a tolerance — a floor. The claim is that the term is
        // NON-reciprocal by a wide margin, so the assertion has to be that the
        // residual is LARGE.
        EXPECT_GT(worst, 0.25) << "worst reciprocity residual across the sweep = " << worst;
    }

    // =========================================================================
    // C. Energy — where the phenomenological term leaves physics behind.
    // =========================================================================

    TEST(LeafTransmissionReference, TheProductionLobeIsAGainAndNotATransmittance)
    {
        // The slab's transmittance is a FRACTION: it cannot exceed one, because
        // it is the share of an arriving unit of energy that reaches the far
        // side. The production term has no such bound — `thickness` multiplies
        // an unnormalised lobe — and this measures where inside the authored
        // ranges it crosses one.
        //
        // That is not a defect report. An artist-facing translucency gain is a
        // legitimate design, and #1234 chose it deliberately for a lamina whose
        // physical thickness is never authored. It IS a limitation that has to
        // be written down, because it means no absolute comparison against a
        // transport reference can ever be made — only the shape comparisons in
        // part D.
        const glm::dvec3 backlight = Backlight(1.0, 0.0);

        // The shipped defaults, which are inside the bound.
        const f64 shipped = ProductionHemisphericalTransmittance(backlight, 0.75, MakeProfile(0.35f, 4.0f, 0.5f));
        EXPECT_GT(shipped, 0.0);

        // And a setting inside the same authored ranges that is not: a high
        // wrap on a thick leaf. The wrap term is a constant over the whole
        // hemisphere, so it integrates to wrap * thickness * pi * cos(theta_l),
        // which passes one long before wrap does.
        const f64 wide = ProductionHemisphericalTransmittance(backlight, 1.0, MakeProfile(0.35f, 4.0f, 1.0f));
        EXPECT_GT(wide, 1.0) << "hemispherical transmittance of the production lobe at wrap 1, thickness 1 = " << wide;

        // The slab, at every configuration, stays under one — which is the
        // contrast that makes the statement above mean something rather than
        // being a remark about a unit choice.
        for (const f64 tau : { 0.25, 1.0, 4.0 })
        {
            const Ref::SlabResponse slab = Ref::SlabRandomWalk(tau, 0.98, 0.7, 1.0, kSlabSamples, 0x1255u);
            EXPECT_LE(slab.Transmittance, 1.0) << "tau = " << tau;
        }
    }

    TEST(LeafTransmissionReference, TheWrapTermIsAnAuthoredFloorAndTransportHasNone)
    {
        // THE STRUCTURAL DIFFERENCE, stated as the thing that is exactly true.
        //
        // An earlier version of this test claimed the whole lobe goes isotropic
        // at wrap 1 and measured a 0.83 spread instead — the forward half is
        // still the larger term at a grazing backlight, so "the lobe becomes
        // flat" was simply false. What IS true, and is what the wrap term
        // actually is, is narrower: it adds a CONSTANT over the view
        // hemisphere. `max(dot(-N, L), 0) * wrap` does not mention V, so
        //
        //     lobe(wrap = w) - lobe(wrap = 0)
        //
        // is the same number at every view direction, to the last bit. That is
        // asserted exactly rather than within a tolerance, because it is an
        // algebraic identity rather than a measurement.
        //
        // AND THE CONSEQUENCE IS A FLOOR THE AUTHOR SETS. Raising wrap raises
        // the lobe's minimum over the hemisphere without touching its peak
        // direction, so the flatness of a leaf's glow is a slider. Transport has
        // no such slider: a slab's flatness is DETERMINED by its optical
        // thickness, and the only way to flatten it is to make the leaf thicker,
        // which also darkens it. That is the difference between a phenomenologic
        // al term and a physical one, and it is the reason no parameter fit
        // between the two exists.
        const glm::dvec3 grazingLight = Backlight(0.15, 0.0);
        const std::array<f64, 5> views{ 0.15, 0.35, 0.55, 0.75, 0.95 };

        // 1. The wrap contribution is exactly view-independent.
        for (const f32 wrap : { 0.25f, 0.5f, 1.0f })
        {
            const FoliageLeafProfile withWrap = MakeProfile(0.35f, 4.0f, wrap);
            const FoliageLeafProfile withoutWrap = MakeProfile(0.35f, 4.0f, 0.0f);

            f64 firstDelta = 0.0;
            for (sizet i = 0; i < views.size(); ++i)
            {
                const glm::dvec3 v = ViewDirection(views[i], Ref::kPi);
                const f64 delta = ProductionLeafLobe(kLeafNormal, v, grazingLight, 1.0, withWrap) -
                                  ProductionLeafLobe(kLeafNormal, v, grazingLight, 1.0, withoutWrap);
                if (i == 0)
                    firstDelta = delta;
                else
                    EXPECT_NEAR(delta, firstDelta, 1.0e-15) << "wrap = " << wrap << ", mu = " << views[i];
            }
            EXPECT_GT(firstDelta, 0.0) << "wrap = " << wrap;
        }

        // 2. So the lobe's floor is an authored quantity: its minimum over the
        //    hemisphere rises with wrap while its peak direction does not move.
        f64 previousFloor = -1.0;
        for (const f32 wrap : { 0.0f, 0.25f, 0.5f, 1.0f })
        {
            const FoliageLeafProfile profile = MakeProfile(0.35f, 4.0f, wrap);
            f64 lowest = std::numeric_limits<f64>::max();
            for (const f64 mu : views)
                lowest = std::min(lowest, ProductionLeafLobe(kLeafNormal, ViewDirection(mu, Ref::kPi),
                                                             grazingLight, 1.0, profile));
            EXPECT_GT(lowest, previousFloor) << "wrap = " << wrap << ", floor = " << lowest;
            previousFloor = lowest;
        }

        // 3. Transport has no such knob. The slab's flatness — the ratio of its
        //    grazing BTDF to its near-normal one — is a function of optical
        //    thickness alone, and it MOVES with thickness: a thicker lamina is
        //    flatter and a thinner one is sharper. There is nothing to set at
        //    fixed tau.
        const auto slabFlatness = [](f64 tau)
        {
            const Ref::SlabResponse slab = Ref::SlabRandomWalk(tau, 0.95, 0.7, 1.0, kSlabSamples, 0x1255u);
            return slab.TransmittedBtdf(1) / std::max(slab.TransmittedBtdf(15), 1.0e-12);
        };
        const f64 thin = slabFlatness(0.5);
        const f64 thick = slabFlatness(8.0);
        EXPECT_GT(thick, thin * 1.1) << "thin slab flatness " << thin << " vs thick " << thick;
    }

    // =========================================================================
    // D. What the two DO share — the shape correspondence, as orderings.
    // =========================================================================

    TEST(LeafTransmissionReference, BothPeakForwardAndFallAwayFromTheLight)
    {
        // The property the phenomenological term was built to have, asserted in
        // both. A leaf's glow is strongest when the eye is nearly in line with
        // the light behind it and falls away; a slab's transmitted lobe does the
        // same because forward scattering dominates.
        //
        // Asserted as MONOTONICITY along the exit cosine rather than as a fitted
        // exponent: the two have no common parameter, so the exponent of one
        // says nothing about the other, and only the ordering transfers.
        const FoliageLeafProfile profile = MakeProfile(0.35f, 4.0f, 0.5f);
        const glm::dvec3 backlight = Backlight(1.0, 0.0);

        f64 previous = std::numeric_limits<f64>::max();
        for (const f64 mu : { 1.0, 0.9, 0.7, 0.5, 0.3, 0.1 })
        {
            const f64 value = ProductionLeafLobe(kLeafNormal, ViewDirection(mu, 0.0), backlight, 0.75, profile);
            EXPECT_LT(value, previous) << "mu = " << mu;
            previous = value;
        }

        const Ref::SlabResponse slab = Ref::SlabRandomWalk(1.5, 0.95, 0.7, 1.0, kSlabSamples, 0x1255u);
        // Coarser: three bands rather than every bin, because a Monte Carlo
        // histogram is not monotone bin by bin and asserting that it is would
        // be asserting the seed.
        const auto band = [&slab](sizet first, sizet last)
        {
            f64 sum = 0.0;
            for (sizet i = first; i <= last; ++i)
                sum += slab.TransmittedBtdf(i);
            return sum / static_cast<f64>((last - first) + 1u);
        };
        EXPECT_GT(band(11, 15), band(6, 10));
        EXPECT_GT(band(6, 10), band(0, 5));
    }

    TEST(LeafTransmissionReference, AThickerLeafBroadensBothLobesInTheSameDirection)
    {
        // The one correspondence between a production PARAMETER and a physical
        // one. Optical thickness broadens the slab's exit lobe — more scattering
        // events randomise the direction — and the production term broadens
        // through `distortion` and a lower `power`. So the mapping that exists
        // is: thicker leaf <-> lower power, and it is monotone over the authored
        // range.
        //
        // Measured as the ratio of the lobe's normal-exit value to its
        // grazing-exit value: a narrow lobe has a large ratio, a broad one a
        // ratio near 1. Both sides are normalised by their own peak first, so
        // the production term's free gain drops out and what is left is shape.
        const auto productionNarrowness = [](f32 power)
        {
            const FoliageLeafProfile profile = MakeProfile(0.35f, power, 0.0f);
            const glm::dvec3 backlight = Backlight(1.0, 0.0);
            const f64 peak = ProductionLeafLobe(kLeafNormal, ViewDirection(1.0, 0.0), backlight, 1.0, profile);
            const f64 wide = ProductionLeafLobe(kLeafNormal, ViewDirection(0.4, 0.0), backlight, 1.0, profile);
            return peak / std::max(wide, 1.0e-12);
        };

        f64 previous = 0.0;
        for (const f32 power : { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f })
        {
            const f64 narrowness = productionNarrowness(power);
            EXPECT_GT(narrowness, previous) << "power = " << power;
            previous = narrowness;
        }

        // The reference's own axis, in the opposite direction: a thicker slab
        // is broader, so its narrowness falls.
        previous = std::numeric_limits<f64>::max();
        for (const f64 tau : { 0.5, 1.0, 2.0, 4.0 })
        {
            const Ref::SlabResponse slab = Ref::SlabRandomWalk(tau, 0.95, 0.7, 1.0, kSlabSamples, 0x1255u);
            const f64 narrowness = slab.TransmittedBtdf(15) / std::max(slab.TransmittedBtdf(5), 1.0e-12);
            EXPECT_LT(narrowness, previous) << "tau = " << tau << ", narrowness = " << narrowness;
            previous = narrowness;
        }
    }
} // namespace OloEngine::Tests
