#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// NonlocalTransportReferenceTest — issue #1255, the two NONLOCAL models: skin
// screen-space diffusion and groom coat transport.
//
// THIS FILE IS SEPARATE FROM THE TWO LOCAL ONES ON PURPOSE, and the separation
// IS acceptance criterion 2. A local scattering function is a function of two
// directions at one point, and it owes three answers: energy, reciprocity and
// sampling consistency. Neither model here is one:
//
//   * SKIN DIFFUSION moves energy ACROSS a surface. Its quantity is R(r), a
//     function of one LENGTH, and it has no exit direction. Asking it for
//     reciprocity has no referent, and a BSDF-shaped test of it would be
//     asserting a coincidence — it would pass or fail on whatever the screen-
//     space pass happened to do with directions it never consults.
//   * COAT TRANSPORT is a function of a PATH THROUGH a volume. Its quantity is
//     a transmittance along a ray, and the interesting question is not whether
//     it is reciprocal (it trivially is, a ray reversed crosses the same
//     fibres) but whether the ESTIMATOR is the right one — an expectation of an
//     exponential against an exponential of an expectation.
//
// So the questions here are different questions, and each is stated at its
// test. What the two share with the local pair is the discipline: an
// independent oracle, declared units, and a tolerance that comes from the
// estimator rather than from taste.
//
// THE ORACLES:
//
//   Skin  MaterialReference::SearchlightRandomWalk — a Monte Carlo random walk
//         in the semi-infinite, index-matched, similarity-reduced medium that
//         Christensen and Burley's normalised-diffusion fit was FITTED TO. The
//         production side evaluates the fit; this side evaluates the thing
//         fitted. Nothing is shared but the medium's definition.
//   Coat  MaterialReference::CoatBundleTransmittance over a medium this test
//         generates, plus PoissonMediumTransmittance, which is exact and
//         carries no sampling error at all.
//
// WHAT THE CPU/GPU REFERENCE TRACER CANNOT DO, said plainly for criterion 4.
// PathTracing/ReferenceBRDF.h and the GPU path tracer beside it represent a
// surface BSDF. Neither can represent either model here: the path tracer has no
// participating medium and no subsurface transport, so a "reference render" of
// a skin head through it would be a Cook-Torrance head — generic PBR presented
// as ground truth, which is the one thing criterion 4 forbids by name. Both
// references below are therefore new apparatus, and both are deliberately
// SMALLER than a path tracer: the quantity each model actually computes is a
// one-dimensional function, and a reference for a one-dimensional function does
// not need a renderer.
//
// Runtime budget: about 16 seconds in Debug, dominated by the searchlight
// walks at high albedo — a diffuse albedo of 0.95 needs a single-scattering
// albedo of 1.0, where a photon's path before it escapes is long. Every
// estimator is seeded, so a failure reproduces exactly.
// =============================================================================

#include "PathTracing/MaterialReference.h"

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace Ref = MaterialReference;

        // -------------------------------------------------------------------
        // THE UNIT BRIDGE, in one place, because it is where a silent failure
        // would live.
        //
        // The reference works in MEAN FREE PATHS of the reduced medium. The
        // production profile works in MILLIMETRES, with ScatterRadiusMM
        // documented as a mean free path. So a profile authored at
        // ScatterRadiusMM = 1 puts one millimetre on one reference unit, and
        // every radius below can be read in either. Authoring the radius as
        // exactly 1 is not a convenience — it is what makes the comparison have
        // no conversion factor to get wrong.
        // -------------------------------------------------------------------
        constexpr f32 kUnitRadiusMM = 1.0f;

        [[nodiscard]] SkinProfileParameters MakeProfile(f32 albedo)
        {
            SkinProfileParameters parameters;
            parameters.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;
            parameters.ScatterColor = glm::vec3(albedo);
            parameters.ScatterRadiusMM = glm::vec3(kUnitRadiusMM);
            (void)parameters.Sanitize();
            return parameters;
        }

        /// The production profile's own scaling for a scalar albedo, in the
        /// same millimetres the reference calls mean free paths.
        [[nodiscard]] f64 BurleyScaling(f32 albedo)
        {
            return static_cast<f64>(SkinBurleyScalingMM(MakeProfile(albedo)).x);
        }

        constexpr u32 kWalkSamples = 70000;
        constexpr f64 kMaxRadius = 32.0;
        constexpr sizet kBins = 800;

        constexpr u64 kWalkSeed = 0x1255u;

        /// The walk for an authored ScatterColor. `albedo` is the DIFFUSE
        /// SURFACE albedo the profile authors; the walk needs the medium's
        /// single-scattering albedo, and the two differ by a lot. See
        /// MaterialReference::SingleScatteringAlbedoForDiffuseAlbedo.
        ///
        /// MEMOISED, because the six albedos in this file are reached 24 times
        /// between them and the walk is the Debug runtime's bottleneck. The
        /// cache is safe rather than merely convenient:
        ///
        ///   * the walk is a pure function of (albedo, seed) and the seed is
        ///     fixed here, so a hit and a miss return the same bytes;
        ///   * ctest runs concurrent suites in separate processes and GoogleTest
        ///     runs the cases inside one process sequentially, so there is no
        ///     concurrent mutation;
        ///   * nothing outside this file can observe it, and a shuffled or
        ///     filtered run only changes which call pays for the walk.
        ///
        /// The key is a bare f32 and that is deliberate: this is an IDENTITY
        /// lookup ("is this the same authored value?"), which is the one case
        /// cpp-coding-quality.md §2a says exact float comparison is right for.
        [[nodiscard]] const Ref::SearchlightProfile& WalkForAuthoredAlbedo(f32 albedo)
        {
            static std::map<f32, Ref::SearchlightProfile> cache;

            const auto found = cache.find(albedo);
            if (found != cache.end())
                return found->second;

            return cache
                .emplace(albedo,
                         Ref::SearchlightRandomWalk(
                             Ref::SingleScatteringAlbedoForDiffuseAlbedo(static_cast<f64>(albedo)), kMaxRadius,
                             kBins, kWalkSamples, kWalkSeed))
                .first->second;
        }
    } // namespace

    // =========================================================================
    // A. SKIN DIFFUSION — nonlocal. The profile's SHAPE against the walk.
    // =========================================================================

    TEST(SkinDiffusionReference, TheAuthoredScatterColorIsADiffuseSurfaceAlbedo)
    {
        // THE PARAMETER BRIDGE, and it comes first because every comparison
        // after it is meaningless if the two sides describe different media.
        //
        // ScatterColor's own header calls it "the fraction of light entering the
        // surface that leaves it again rather than being absorbed" — a DIFFUSE
        // SURFACE albedo, the total reflectance of the half-space. It is NOT the
        // medium's single-scattering albedo, and the gap between the two is
        // enormous: a surface returning 85% of the light needs a
        // single-scattering albedo of 0.997, because a photon has to survive
        // dozens of events to get back out.
        //
        // This is not a hypothetical confusion. The first version of the test
        // below drove the walk with ScatterColor directly and measured the
        // Burley profile as TWICE too wide at the median — a finding that was
        // entirely an error in the test. So the bridge is asserted rather than
        // assumed: the walk driven by the inverted albedo must come back with
        // the reflectance the profile authored.
        for (const f32 authored : { 0.35f, 0.45f, 0.55f, 0.70f, 0.85f, 0.95f })
        {
            const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(authored);
            // 0.02 absolute. The inversion is a rational approximation to
            // Chandrasekhar's H-function, and this is its measured accuracy
            // across the band; a wrong inversion is a factor, not two points.
            EXPECT_NEAR(walk.DiffuseReflectance, static_cast<f64>(authored), 0.02)
                << "authored " << authored << ", walk returned " << walk.DiffuseReflectance;
        }
    }

    TEST(SkinDiffusionReference, TheSearchlightWalkReproducesTheBurleyProfileShape)
    {
        // THE HEADLINE. Production ships Christensen and Burley's normalised
        // diffusion, a two-exponential closed form whose shape parameter
        // s(A) = 1.85 - A + 7|A - 0.8|^3 was fitted to Monte Carlo transport in
        // the searchlight configuration. This runs that transport and compares.
        //
        // WHY QUANTILES AND NOT R(r) POINTWISE. The profile has a 1/r pole at the
        // origin; a pointwise comparison there measures the histogram's bin
        // width, and everywhere else it measures a Monte Carlo bin's standard
        // error. The CDF is bounded, monotone and analytic on the production
        // side, so a quantile comparison is about the profile at every radius at
        // once and is insensitive to the binning.
        //
        // 10% relative. The fit is itself approximate — this is what "a few
        // percent" costs once it is measured rather than quoted — and the
        // failures it exists to catch are factors: a missing s(A) division is
        // 1.85 at A = 1 and over 2 at low albedo, and a radius/diameter slip is
        // exactly 2.
        //
        // THE BAND IS DELIBERATE. Up to a diffuse albedo of 0.70 the fit holds
        // here; above it, it does not, and that is the next test rather than a
        // loosened tolerance in this one.
        for (const f32 authored : { 0.35f, 0.45f, 0.55f, 0.70f })
        {
            const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(authored);
            ASSERT_LT(walk.Overflow, 0.01) << authored << ": " << (100.0 * walk.Overflow)
                                           << "% of the exiting energy fell outside " << kMaxRadius
                                           << " mean free paths";

            const f64 d = BurleyScaling(authored);
            for (const f64 fraction : { 0.25, 0.5, 0.75, 0.9 })
            {
                const f64 reference = walk.RadiusForFraction(fraction);
                const f64 production =
                    static_cast<f64>(SkinBurleyRadiusForFraction(static_cast<f32>(fraction), static_cast<f32>(d)));

                EXPECT_LT(std::abs(production - reference) / std::max(reference, 1.0e-12), 0.10)
                    << "authored albedo " << authored << ", the " << fraction << " quantile: walk " << reference
                    << " mfp, Burley " << production << " mm, d = " << d;
            }
        }
    }

    TEST(SkinDiffusionReference, TheFitRunsNarrowAtHighAlbedoAndTheErrorGrowsIntoTheTail)
    {
        // THE MEASURED LIMIT OF THE FIT, and it lands squarely on the channel
        // that matters. The default skin profile's ScatterColor is
        // (0.85, 0.55, 0.45): green and blue sit inside the band the test above
        // covers, and RED — the channel that carries the visible bleed past a
        // terminator, the thing subsurface scattering is FOR — sits above it.
        //
        // Above a diffuse albedo of about 0.7 the transport profile is WIDER
        // than the fit, and the gap grows with the radius: the fit tracks the
        // near field and loses the tail.
        //
        // ASSERTED AS A DIRECTION AND A GROWTH, not as a value. A threshold on
        // the ratio would pin this machine's walk; the statements that are about
        // the model are "the fit is the narrower of the two" and "the error is a
        // tail effect rather than a scale error", and the second is what the
        // growth across quantiles says. A scale error would give the SAME ratio
        // at every quantile.
        for (const f32 authored : { 0.85f, 0.95f })
        {
            const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(authored);
            // Looser than the moderate band: at a single-scattering albedo of
            // 0.997 the tail is genuinely long, and this is the reference
            // reporting its own resolution rather than hiding it.
            ASSERT_LT(walk.Overflow, 0.05) << authored << ": overflow " << walk.Overflow;

            const f64 d = BurleyScaling(authored);
            const auto ratioAt = [&](f64 fraction)
            {
                const f64 reference = walk.RadiusForFraction(fraction);
                const f64 production =
                    static_cast<f64>(SkinBurleyRadiusForFraction(static_cast<f32>(fraction), static_cast<f32>(d)));
                return reference / std::max(production, 1.0e-12);
            };

            const f64 median = ratioAt(0.5);
            const f64 upper = ratioAt(0.75);
            const f64 tail = ratioAt(0.9);

            EXPECT_GT(median, 1.05) << "authored " << authored << ": median ratio " << median;
            EXPECT_GT(upper, median) << "authored " << authored << ": " << upper << " vs " << median;
            EXPECT_GT(tail, upper) << "authored " << authored << ": " << tail << " vs " << upper;
            // Bounded, so a genuinely broken profile still fails: this is a
            // known accuracy limit, not an open licence.
            EXPECT_LT(tail, 2.0) << "authored " << authored << ": tail ratio " << tail;
        }

        // AND IT IS A LIMIT THAT IS NOT WORTH REPAIRING, which #1361 settled and
        // which is the claim this test now carries. The narrowness above is real
        // and this is still where it is measured — but the pass is separable,
        // and at the image a tail error shows up in (a bright small feature, not
        // a terminator) the separable projection costs MORE than the fit's
        // narrowness does. Repairing the fit moves the image away from transport
        // at the default's red channel rather than towards it.
        //
        // See TheSeparableProjectionOutweighsBothErrorsAtASmallBrightFeature,
        // which is the control that measured it, and the comment on
        // kSkinDiffusionSupportFraction, which records the decision.
    }

    TEST(SkinDiffusionReference, TheProfileCarriesShapeAndTheAlbedoCarriesEnergy)
    {
        // A DIFFERENT KIND OF STATEMENT, and it is what stops the tests above
        // from being read as more than they are. SkinBurleyProfile is
        // NORMALISED: its integral over the plane is exactly 1 at every albedo.
        // The transport it approximates is not — a surface authored at 0.35
        // returns about a third of the beam. So the profile describes WHERE the
        // energy goes and says nothing about HOW MUCH; the renderer supplies the
        // rest by multiplying the diffuse irradiance it blurs.
        //
        // Writing that down as a test matters because the obvious mistake is to
        // read the fit as a reflectance and apply the albedo twice, which darkens
        // every head by a factor that reads as a lighting change.
        f64 previous = 0.0;
        for (const f32 authored : { 0.35f, 0.45f, 0.55f, 0.70f, 0.85f })
        {
            const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(authored);
            EXPECT_GT(walk.DiffuseReflectance, previous) << "authored = " << authored;
            EXPECT_LT(walk.DiffuseReflectance, 1.0) << "authored = " << authored;
            previous = walk.DiffuseReflectance;

            const f64 d = BurleyScaling(authored);
            EXPECT_NEAR(static_cast<f64>(SkinBurleyCdf(static_cast<f32>(400.0 * d), static_cast<f32>(d))), 1.0, 1.0e-5)
                << "authored = " << authored;
        }
    }

    TEST(SkinDiffusionReference, TheSupportRadiusTruncatesTransportAtHighAlbedo)
    {
        // The kernel's outermost tap is placed at the radius holding
        // kSkinDiffusionSupportFraction (99.5%) of the PROFILE's energy. Where
        // the fit is accurate that is also 99.5% of the transport — and where it
        // is not, the pass truncates real energy.
        //
        // Measured: the support holds over 99% of the walk up to an authored
        // 0.55, and about 97% at 0.85. So the red channel of the default skin
        // profile loses roughly three percent of its scattered energy to the
        // support radius, on top of running narrow. The consequence is a head
        // that reads subtly crisper at the terminator than transport says —
        // a bounded, deliberate artefact rather than a defect, and this is its
        // size.
        //
        // #1361 DECIDED WHAT TO DO ABOUT IT: NOTHING, and this test now says so
        // rather than leaving the number hanging as an implied defect. The three
        // percent cannot be recovered by widening the support, because a support
        // radius carries no energy — the tap WEIGHTS do, and they come from the
        // same narrow fit. Widening it to cover the transport needs 1.8x the
        // radius at the same 17 taps, which coarsens the kernel everywhere and
        // moves the image by less than a fiftieth of the error it was meant to
        // fix. WideningTheSupportSpendsTapBudgetAndDoesNotMoveTheImage is that
        // measurement.
        const auto coverage = [](f32 authored)
        {
            const SkinProfileParameters parameters = MakeProfile(authored);
            const f64 support = static_cast<f64>(SkinDiffusionSupportRadiusMM(parameters));
            return std::pair<f64, f64>{ support, WalkForAuthoredAlbedo(authored).CdfAt(support) };
        };

        for (const f32 authored : { 0.35f, 0.45f, 0.55f })
        {
            const auto [support, covered] = coverage(authored);
            ASSERT_LT(support, kMaxRadius) << "the histogram cannot answer for a support of " << support;
            EXPECT_GT(covered, 0.99) << "authored " << authored << ": the " << support << " mm support holds "
                                     << (100.0 * covered) << "% of the walk's exiting energy";
        }

        const auto [support, covered] = coverage(0.85f);
        ASSERT_LT(support, kMaxRadius);
        // A BAND, not a bound: the claim is that the truncation is real and
        // small, and a test that only said "greater than 0.9" would also pass if
        // it were zero.
        EXPECT_GT(covered, 0.94) << "authored 0.85: support " << support << " mm holds " << (100.0 * covered) << "%";
        EXPECT_LT(covered, 0.99) << "authored 0.85: support " << support << " mm holds " << (100.0 * covered)
                                 << "% — if this is now above 99% the support sizing changed";
    }

    TEST(SkinDiffusionReference, TheSeparableKernelIsNotTheTwoDimensionalProfile)
    {
        // A SECOND, INDEPENDENT APPROXIMATION sits between the profile and the
        // pixels, and the test above cannot see it. The screen-space pass is a
        // two-pass SEPARABLE blur, and a separable kernel whose 1D weights are
        // the profile's line-spread function reproduces a straight EDGE exactly
        // and a POINT not at all: the outer product of two line-spread functions
        // is not the radial profile, and cannot be, because the profile is not
        // separable.
        //
        // That is a known and deliberate trade — the alternative is a 2D
        // gather — but its SIZE is not written down anywhere, and a reviewer
        // asked to accept it deserves a number. This measures it: the separable
        // point-spread function against the true 2D profile, both unit-mass,
        // both sampled on the same grid.
        const SkinProfileParameters parameters = MakeProfile(0.85f);
        const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::High);
        ASSERT_FALSE(kernel.IsIdentity());
        ASSERT_GT(kernel.TapCount, 1u);

        const f64 d = BurleyScaling(0.85f);
        const f64 support = static_cast<f64>(kernel.SupportRadiusMM);

        // The separable PSF on a grid of tap offsets: weight(i) * weight(j) at
        // (x_i, y_j). Red's weight is lane y of the tap.
        f64 separablePeak = 0.0;
        f64 truePeak = 0.0;
        f64 worstRelative = 0.0;
        f64 separableMass = 0.0;

        for (u32 i = 0; i < kernel.TapCount; ++i)
        {
            for (u32 j = 0; j < kernel.TapCount; ++j)
            {
                const f64 weight = static_cast<f64>(kernel.Taps[i].y) * static_cast<f64>(kernel.Taps[j].y);
                separableMass += weight;

                const f64 x = static_cast<f64>(kernel.Taps[i].x) * support;
                const f64 y = static_cast<f64>(kernel.Taps[j].x) * support;
                const f64 r = std::sqrt((x * x) + (y * y));
                if (r < 1.0e-6)
                {
                    separablePeak = weight;
                    continue;
                }

                // The true 2D profile's share of the same cell. Cell area is the
                // product of the two tap spacings, taken locally so a
                // non-uniform tap layout is handled.
                const f64 dx = (i + 1u < kernel.TapCount)
                                   ? (static_cast<f64>(kernel.Taps[i + 1u].x - kernel.Taps[i].x) * support)
                                   : (static_cast<f64>(kernel.Taps[i].x - kernel.Taps[i - 1u].x) * support);
                const f64 dy = (j + 1u < kernel.TapCount)
                                   ? (static_cast<f64>(kernel.Taps[j + 1u].x - kernel.Taps[j].x) * support)
                                   : (static_cast<f64>(kernel.Taps[j].x - kernel.Taps[j - 1u].x) * support);
                const f64 truth =
                    static_cast<f64>(SkinBurleyProfile(static_cast<f32>(r), static_cast<f32>(d))) * dx * dy;
                truePeak = std::max(truePeak, truth);

                if (truth > 1.0e-4)
                    worstRelative = std::max(worstRelative, std::abs(weight - truth) / truth);
            }
        }

        // The separable kernel is unit mass — that part IS exact, because each
        // 1D pass sums to one by construction, and it is why the pass does not
        // change a flat region's brightness.
        EXPECT_NEAR(separableMass, 1.0, 1.0e-4) << "separable mass = " << separableMass;

        // And it is NOT the 2D profile. The assertion is a floor, not a
        // tolerance: the claim is that the two differ materially, so a test that
        // demanded they agree would be asserting the opposite of the truth.
        EXPECT_GT(worstRelative, 0.2) << "the separable outer product agreed with the radial profile to "
                                      << worstRelative << " relative, which would mean the profile IS separable";
        EXPECT_GT(separablePeak, 0.0);
    }

    TEST(SkinDiffusionReference, TheBlurFootprintIsInvariantUnderResolutionAndFieldOfView)
    {
        // THE UPSCALE / NON-NATIVE-RESOLUTION CELL, settled here rather than in
        // the editor, because it is arithmetic. A screen-space diffusion kernel
        // whose footprint is measured in PIXELS is the classic thing that is
        // correct at native resolution and wrong at any other: render at 70% and
        // the blur covers 70% of the skin it should.
        //
        // The pass defends against that by making the pixel radius a function of
        // the target's own height. So the WORLD footprint — the pixel radius
        // divided by the pixels per world unit at that depth — must not move
        // when the resolution does. Asserted as an exact ratio, because the
        // relation is linear and a tolerance would hide a partial fix.
        constexpr f32 kRadiusMM = 3.0f;
        constexpr f32 kDepth = 0.6f;
        constexpr f32 kProjectionScaleY = 2.144507f; // cot(25 deg), a 50 deg vertical FOV

        const f32 atNative = SkinDiffusionRadiusPixels(kRadiusMM, kDepth, kProjectionScaleY, 1080.0f);
        const f32 atDouble = SkinDiffusionRadiusPixels(kRadiusMM, kDepth, kProjectionScaleY, 2160.0f);
        const f32 atSeventy = SkinDiffusionRadiusPixels(kRadiusMM, kDepth, kProjectionScaleY, 756.0f);

        ASSERT_GT(atNative, 0.0f);
        EXPECT_NEAR(static_cast<f64>(atDouble / atNative), 2.0, 1.0e-5);
        EXPECT_NEAR(static_cast<f64>(atSeventy / atNative), 0.7, 1.0e-5);

        // The kernel itself carries no resolution at all: its offsets are
        // normalised and its support is millimetres, so the profile a pixel
        // receives is the same object at every resolution. That is the other
        // half of the invariance, and it is a property of the data rather than
        // of the expression above.
        const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(MakeProfile(0.85f), SkinDiffusionQuality::High);
        EXPECT_GT(kernel.SupportRadiusMM, 0.0f);
        for (u32 i = 0; i < kernel.TapCount; ++i)
        {
            EXPECT_GE(kernel.Taps[i].x, -1.0f);
            EXPECT_LE(kernel.Taps[i].x, 1.0f);
        }

        // Halving the field of view doubles the footprint at a fixed depth,
        // which is the other conditional axis the pass has to survive.
        const f32 narrow = SkinDiffusionRadiusPixels(kRadiusMM, kDepth, kProjectionScaleY * 2.0f, 1080.0f);
        EXPECT_NEAR(static_cast<f64>(narrow / atNative), 2.0, 1.0e-5);
    }

    TEST(SkinDiffusionReference, TheBlurredEdgeAovMatchesTheWalksEdgeResponse)
    {
        // THE ALIGNED AOV COMPARISON FOR SKIN — criterion 3 — and it is CPU-side
        // for a reason worth stating rather than hiding. SkinDiffusion.glsl
        // evaluates no profile: it consumes the tap table this test builds, so
        // there is nothing on the device to probe and no rendered AOV that would
        // add information. What CAN be compared is the pass's OUTPUT IMAGE, and
        // the comparison is exact in alignment because there is no camera, no
        // exposure and no tonemap between the two sides at all.
        //
        // THE SIGNAL IS A STEP EDGE, which is the one input the whole design is
        // about: a shadow terminator crossing skin, where red bleeds out and blue
        // stays put. For a signal that varies only along x the vertical pass of
        // the separable blur is the identity, so the pass's output is exactly the
        // 1D convolution of the step with the tap weights — no simulation of the
        // shader is needed, and none is written, which keeps this from being a
        // test of a second transcription.
        //
        // THE REFERENCE SIDE comes from the random walk's radial histogram
        // without any fitting. For an isotropic profile, an annulus of radius r
        // distributes its energy across x as the arcsine law, so the exiting
        // energy left of x is
        //     sum over annuli of  E_r * (1/2 + asin(clamp(x / r)) / pi)
        // which is the edge response the physics gives. That step — radial
        // histogram to line response — is exactly the polar rearrangement
        // SkinBurleyStripFraction performs on the FIT, done here on the TRUTH by
        // an independent route.
        //
        // RUN AT AN AUTHORED 0.55, NOT AT THE RED CHANNEL'S 0.85, and the choice
        // is the point rather than a convenience. This test is about the PASS —
        // the tap placement and the separable projection — and at 0.85 the fit
        // itself runs narrow (see TheFitRunsNarrowAtHighAlbedo), so a comparison
        // there would just re-measure that and attribute it to the pass. The
        // high-albedo arm at the end asserts the two effects ADD, which is what
        // says they are two effects.
        constexpr f32 kAlbedo = 0.55f;
        const SkinProfileParameters parameters = MakeProfile(kAlbedo);
        const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::High);
        ASSERT_FALSE(kernel.IsIdentity());

        const f64 support = static_cast<f64>(kernel.SupportRadiusMM);

        const auto referenceEdge = [](const Ref::SearchlightProfile& walk, f64 x)
        {
            f64 sum = 0.0;
            for (sizet i = 0; i < walk.Energy.size(); ++i)
            {
                // The annulus' representative radius is its centre; the bins are
                // narrow enough (kMaxRadius / kBins = 0.04 mfp) that a finer rule
                // moves the result below the walk's own noise.
                const f64 r = 0.5 * (walk.Edges[i] + walk.Edges[i + 1]);
                sum += walk.Energy[i] * (0.5 + (std::asin(std::clamp(x / r, -1.0, 1.0)) / Ref::kPi));
            }
            // The overflow sits at radii past the histogram, so at any x inside
            // it contributes almost exactly half. Folding it in rather than
            // dropping it keeps the response asymptotically 1.
            return sum + (0.5 * walk.Overflow);
        };

        const auto productionEdge = [](const SkinDiffusionKernel& taps, f64 supportMM, f64 x)
        {
            // Red's weight is lane y. The convolution of a unit step with the
            // tap set is the sum of the weights whose offset lands left of x.
            f64 sum = 0.0;
            for (u32 i = 0; i < taps.TapCount; ++i)
            {
                if ((static_cast<f64>(taps.Taps[i].x) * supportMM) <= x)
                    sum += static_cast<f64>(taps.Taps[i].y);
            }
            return sum;
        };

        const auto worstEdgeError = [&](f32 authored)
        {
            const SkinDiffusionKernel k = BuildSkinDiffusionKernel(MakeProfile(authored), SkinDiffusionQuality::High);
            const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(authored);
            const f64 s = static_cast<f64>(k.SupportRadiusMM);
            f64 worst = 0.0;
            for (i32 i = -40; i <= 40; ++i)
            {
                const f64 x = (s * static_cast<f64>(i)) / 40.0;
                worst = std::max(worst, std::abs(productionEdge(k, s, x) - referenceEdge(walk, x)));
            }
            return worst;
        };

        // Both responses run 0 -> 1 across the terminator. The comparison is
        // ABSOLUTE rather than relative because the quantity IS a fraction of
        // unit irradiance: a 5% absolute error is 5% of the frame's brightness at
        // that pixel, which is the unit a reviewer cares about.
        //
        // 0.06 absolute, and the floor under it is already documented in
        // production: SkinDiffusion.cpp records that a 17-tap line-spread kernel
        // sits 0.052 of a unit step away from a true 2D convolution of the SAME
        // profile. So almost all of this budget is the discretisation that #1241
        // measured and chose. A unit slip or a missing s(A) would move it by
        // tens of percent.
        const f64 moderate = worstEdgeError(kAlbedo);
        EXPECT_LT(moderate, 0.06) << "largest edge-response disagreement at an authored " << kAlbedo << " = "
                                  << moderate << " (support " << support << " mm)";

        // AND THE FIT'S NARROWNESS BARELY REACHES THE TERMINATOR, which is the
        // more interesting half and is not what was expected. At the red
        // channel's authored 0.85 the profile runs some 13% narrow at the median
        // and 35% narrow at the 90th percentile — and the edge response moves by
        // only about a tenth of its error. The reason is that an edge response is
        // a CUMULATIVE quantity: it is dominated by where the bulk of the energy
        // sits, and the fit's error is in the tail, which carries little of it.
        //
        // So the two effects do NOT add, and the pass's terminator is dominated
        // by the tap discretisation rather than by the profile's accuracy.
        // Asserted as a direction plus a ceiling: the tail error is visible, and
        // it is not what sizes the budget above.
        const f64 high = worstEdgeError(0.85f);
        EXPECT_GT(high, moderate) << "authored 0.55 -> " << moderate << ", authored 0.85 -> " << high;
        EXPECT_LT(high, moderate * 1.5) << "authored 0.55 -> " << moderate << ", authored 0.85 -> " << high
                                        << ": the tail error now dominates the edge response, which it did not";

        // Monotone and reaching their endpoints, which rules out the failure
        // where two wrong curves happen to cross near the middle.
        const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(kAlbedo);
        EXPECT_LT(productionEdge(kernel, support, -support * 1.5), 0.02);
        EXPECT_GT(productionEdge(kernel, support, support * 1.5), 0.98);
        EXPECT_LT(referenceEdge(walk, -support * 1.5), 0.06);
        EXPECT_GT(referenceEdge(walk, support * 1.5), 0.94);
    }

    // -------------------------------------------------------------------------
    // THE SMALL-BRIGHT-FEATURE APPARATUS (#1361).
    //
    // #1255 measured two errors in the pass — the fit running narrow above a
    // diffuse albedo of 0.7, and the support radius truncating real transport at
    // the same albedos — and bounded them AWAY from the terminator, which is
    // dominated by tap discretisation. That leaves the image where a tail error
    // does show: a BRIGHT SMALL FEATURE against dark skin, where the question is
    // how much of the feature's energy ends up how far out, i.e. the halo.
    //
    // THE METRIC IS THE BLURRED FEATURE'S RADIAL ENERGY, and it is computed in
    // CLOSED FORM on both sides rather than by convolving images. A disc of
    // radius `rho` convolved with a set of weighted point responses, asked for
    // its energy inside radius `r`, is a sum of CIRCLE-CIRCLE OVERLAPS — and the
    // separable kernel IS a set of weighted points (the outer product of the tap
    // table), while the walk's radial histogram is a set of weighted annuli that
    // take the same formula. So the two sides differ only in where their weights
    // come from, there is no grid, no FFT and no resolution to tune, and a Debug
    // run costs milliseconds.
    // -------------------------------------------------------------------------
    namespace
    {
        /// The fraction of a disc of radius `rho`, whose centre lies `dist` from
        /// the origin, that falls inside radius `r` of the origin.
        [[nodiscard]] f64 DiscOverlapFraction(f64 dist, f64 rho, f64 r)
        {
            if (!(rho > 0.0) || !(r > 0.0))
                return 0.0;
            if (dist >= (rho + r))
                return 0.0;

            const f64 discArea = Ref::kPi * rho * rho;
            if (dist <= std::abs(rho - r))
            {
                const f64 smaller = std::min(rho, r);
                return (Ref::kPi * smaller * smaller) / discArea;
            }

            const f64 a1 =
                rho * rho *
                std::acos(std::clamp(((dist * dist) + (rho * rho) - (r * r)) / (2.0 * dist * rho), -1.0, 1.0));
            const f64 a2 =
                r * r * std::acos(std::clamp(((dist * dist) + (r * r) - (rho * rho)) / (2.0 * dist * r), -1.0, 1.0));
            const f64 a3 =
                0.5 * std::sqrt(std::max((-dist + rho + r) * (dist + rho - r) * (dist - rho + r) * (dist + rho + r),
                                         0.0));
            return std::clamp((a1 + a2 - a3) / discArea, 0.0, 1.0);
        }

        /// A 1D separable kernel: positions in millimetres and weights summing
        /// to 1. Both the 17-tap production table and the "unlimited taps"
        /// control below are handed over in this shape, so the comparison sees
        /// one code path and differs only in the numbers.
        struct SeparableKernel1D
        {
            std::vector<f64> PositionsMM;
            std::vector<f64> Weights;
        };

        /// Energy of a bright disc of radius `rho`, blurred by the separable
        /// kernel, lying inside radius `r`. The 2D response is the outer product
        /// of the 1D weights, so this is a double sum over tap PAIRS.
        [[nodiscard]] f64 SeparableFeatureEnergy(const SeparableKernel1D& kernel, f64 rho, f64 r)
        {
            f64 total = 0.0;
            for (sizet i = 0; i < kernel.PositionsMM.size(); ++i)
            {
                f64 inner = 0.0;
                for (sizet j = 0; j < kernel.PositionsMM.size(); ++j)
                {
                    const f64 dist = std::hypot(kernel.PositionsMM[i], kernel.PositionsMM[j]);
                    inner += kernel.Weights[j] * DiscOverlapFraction(dist, rho, r);
                }
                total += kernel.Weights[i] * inner;
            }
            return total;
        }

        /// The same quantity for the walk. Its histogram is already a radial
        /// energy distribution, so each annulus contributes its energy times the
        /// same overlap fraction at the annulus' representative radius.
        ///
        /// The overflow is NOT folded in and NOT renormalised away: it sits
        /// beyond every radius asked for below, so leaving it out is the
        /// conservative reading — it can only make the reference look tighter
        /// than it is, which works against the claims made here rather than for
        /// them.
        [[nodiscard]] f64 TransportFeatureEnergy(const Ref::SearchlightProfile& walk, f64 rho, f64 r)
        {
            f64 total = 0.0;
            for (sizet i = 0; i < walk.Energy.size(); ++i)
            {
                const f64 mid = 0.5 * (walk.Edges[i] + walk.Edges[i + 1]);
                total += walk.Energy[i] * DiscOverlapFraction(mid, rho, r);
            }
            return total;
        }

        /// The production tap table as a SeparableKernel1D, red's lane.
        [[nodiscard]] SeparableKernel1D ProductionKernel(const SkinDiffusionKernel& kernel)
        {
            SeparableKernel1D out;
            for (u32 i = 0; i < kernel.TapCount; ++i)
            {
                out.PositionsMM.push_back(static_cast<f64>(kernel.Taps[i].x) *
                                          static_cast<f64>(kernel.SupportRadiusMM));
                out.Weights.push_back(static_cast<f64>(kernel.Taps[i].y));
            }
            return out;
        }

        /// The walk's own line-spread function: the same polar rearrangement
        /// SkinBurleyStripFraction performs on the FIT, done here on the walk.
        [[nodiscard]] f64 WalkStripFraction(const Ref::SearchlightProfile& walk, f64 a)
        {
            f64 total = 0.0;
            for (sizet i = 0; i < walk.Energy.size(); ++i)
            {
                const f64 mid = 0.5 * (walk.Edges[i] + walk.Edges[i + 1]);
                total += walk.Energy[i] * ((2.0 / Ref::kPi) * std::asin(std::clamp(a / mid, 0.0, 1.0)));
            }
            return total;
        }

        /// THE CONTROL: a separable kernel carrying the WALK's own profile on a
        /// fine uniform grid, in place of the Burley fit and the 17-tap budget.
        ///
        /// IT IS FINITE AND IT IS TRUNCATED, and both words matter enough to be
        /// in the name rather than in a footnote. `bins` per side gives 2*bins
        /// 1D entries — 600 at the 300 the test uses, against production's 17 —
        /// and the grid stops at `supportMM`, with whatever energy lies beyond
        /// folded back in by the normalisation. So this is not "the exact
        /// profile with unlimited taps"; it is the fit error and the tap budget
        /// removed, which is what the comparison needs. The test drives it at
        /// the walk's FULL histogram support for exactly this reason.
        [[nodiscard]] SeparableKernel1D FinelySampledTransportKernel(const Ref::SearchlightProfile& walk,
                                                                     f64 supportMM, i32 bins)
        {
            std::vector<f64> mid;
            std::vector<f64> weight;
            const f64 step = supportMM / static_cast<f64>(bins);
            f64 sum = 0.0;
            for (i32 i = 0; i < bins; ++i)
            {
                const f64 lo = step * static_cast<f64>(i);
                const f64 hi = lo + step;
                // Half of the strip's energy, because S() covers both sides.
                const f64 w = 0.5 * std::max(WalkStripFraction(walk, hi) - WalkStripFraction(walk, lo), 0.0);
                mid.push_back(0.5 * (lo + hi));
                weight.push_back(w);
                sum += w;
            }

            SeparableKernel1D out;
            for (i32 i = bins - 1; i >= 0; --i)
            {
                out.PositionsMM.push_back(-mid[static_cast<sizet>(i)]);
                out.Weights.push_back(weight[static_cast<sizet>(i)]);
            }
            for (i32 i = 0; i < bins; ++i)
            {
                out.PositionsMM.push_back(mid[static_cast<sizet>(i)]);
                out.Weights.push_back(weight[static_cast<sizet>(i)]);
            }

            const f64 total = 2.0 * sum;
            for (f64& w : out.Weights)
                w = (total > 0.0) ? (w / total) : 0.0;
            return out;
        }

        /// A MIRROR of BuildSkinDiffusionKernel's tap binning with the support
        /// radius as a FREE PARAMETER — the one thing the production function
        /// cannot be asked for, because kSkinDiffusionSupportFraction is a
        /// compile-time constant and the support it yields is therefore always
        /// the profile's own.
        ///
        /// ONLY THE BINNING IS MIRRORED. The profile still comes from
        /// production's SkinBurleyStripFraction, so the physics is not
        /// transcribed; what is repeated here is the quadratic tap layout and
        /// the midpoint edges, twenty lines of arithmetic whose only purpose is
        /// to answer "what if the constant were larger".
        ///
        /// AND THE MIRROR IS CHECKED, NOT TRUSTED: at the natural support it
        /// must reproduce the real tap table, which the test asserts before it
        /// uses the widened one. A mirror nobody checks is how a test ends up
        /// measuring its own transcription.
        [[nodiscard]] SeparableKernel1D MirroredKernel(f64 dChannelMM, f64 supportMM, i32 tapCount)
        {
            const i32 half = tapCount / 2;
            const auto offsetAt = [half](i32 i) -> f64
            {
                const f64 t = static_cast<f64>(i) / static_cast<f64>(half);
                return (t < 0.0 ? -1.0 : 1.0) * t * t;
            };
            const auto stripEnergy = [&](f64 loNorm, f64 hiNorm) -> f64
            {
                const f64 lo = loNorm * supportMM;
                const f64 hi = hiNorm * supportMM;
                if (!(hi > lo))
                    return 0.0;
                if (lo <= 0.0)
                    return static_cast<f64>(SkinBurleyStripFraction(static_cast<f32>(hi),
                                                                    static_cast<f32>(dChannelMM)));
                return 0.5 * std::max(static_cast<f64>(SkinBurleyStripFraction(static_cast<f32>(hi),
                                                                               static_cast<f32>(dChannelMM))) -
                                          static_cast<f64>(SkinBurleyStripFraction(static_cast<f32>(lo),
                                                                                   static_cast<f32>(dChannelMM))),
                                      0.0);
            };

            SeparableKernel1D out;
            f64 total = 0.0;
            for (i32 i = -half; i <= half; ++i)
            {
                const f64 offset = offsetAt(i);
                const f64 prev = (i > -half) ? offsetAt(i - 1) : offset;
                const f64 next = (i < half) ? offsetAt(i + 1) : offset;
                const f64 innerEdge = std::abs(0.5 * (offset + prev));
                const f64 outerEdge = std::abs(0.5 * (offset + next));
                const f64 lo = (i == 0) ? 0.0 : std::min(innerEdge, outerEdge);
                const f64 hi = (i == 0) ? std::max(innerEdge, outerEdge)
                                        : ((i == -half || i == half) ? 1.0 : std::max(innerEdge, outerEdge));

                const f64 weight = stripEnergy(lo, hi);
                out.PositionsMM.push_back(offset * supportMM);
                out.Weights.push_back(weight);
                total += weight;
            }
            for (f64& weight : out.Weights)
                weight = (total > 0.0) ? (weight / total) : 0.0;
            return out;
        }

        /// A bright feature 1.5 mm across — a specular-adjacent highlight on a
        /// human-scale face, and comfortably smaller than red's support so the
        /// halo measured is the kernel's and not the feature's.
        constexpr f64 kFeatureRadiusMM = 1.5;

        /// The radii the halo is read at, millimetres. They span the near field
        /// the feature occupies out to past red's support, so a kernel that is
        /// right in one region and wrong in another cannot average out.
        constexpr std::array<f64, 7> kHaloRadiiMM = { 2.0, 3.0, 5.0, 8.0, 12.0, 18.0, 25.0 };

        /// The largest disagreement between a separable kernel's halo and the
        /// walk's, over kHaloRadiiMM. A fraction of the feature's own energy, so
        /// 0.05 means "5% of the highlight's light is in the wrong place".
        [[nodiscard]] f64 WorstHaloError(const SeparableKernel1D& kernel, const Ref::SearchlightProfile& walk)
        {
            f64 worst = 0.0;
            for (const f64 r : kHaloRadiiMM)
            {
                const f64 production = SeparableFeatureEnergy(kernel, kFeatureRadiusMM, r);
                const f64 reference = TransportFeatureEnergy(walk, kFeatureRadiusMM, r);
                worst = std::max(worst, std::abs(production - reference));
            }
            return worst;
        }
    } // namespace

    TEST(SkinDiffusionReference, WideningTheSupportSpendsTapBudgetAndDoesNotMoveTheImage)
    {
        // ACCEPTANCE CRITERION 1 OF #1361, DECIDED: kSkinDiffusionSupportFraction
        // STAYS AT 0.995. This is the measurement that decided it.
        //
        // The obvious reading of "the support holds only 97% of the transport at
        // 0.85" is that the support is too small. It is not, and the reason is
        // that A SUPPORT RADIUS CARRIES NO ENERGY — the WEIGHTS do, and the
        // weights come from the fit. Move the outer taps further out and they
        // take the fit's near-zero outer weight with them; what changes is not
        // the halo but the SPACING, because the tap count does not grow and
        // every offset scales together.
        //
        // So the cost is paid in the near field and the benefit never arrives.
        // Both halves are asserted here.
        constexpr f32 kAuthored = 0.85f; // the default ScatterColor's red channel
        const SkinProfileParameters parameters = MakeProfile(kAuthored);
        const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(kAuthored);

        const f64 fitSupport = static_cast<f64>(SkinDiffusionSupportRadiusMM(parameters));
        ASSERT_GT(fitSupport, 0.0);

        // WHAT COVERING THE TRANSPORT WOULD ACTUALLY COST, stated as the number
        // the decision turns on: the radius holding 99.5% of the WALK against
        // the radius holding 99.5% of the FIT.
        const f64 transportSupport = walk.RadiusForFraction(0.995);
        ASSERT_LT(transportSupport, kMaxRadius)
            << "the histogram cannot answer for a transport support of " << transportSupport;
        const f64 widening = transportSupport / fitSupport;
        EXPECT_GT(widening, 1.5) << "fit support " << fitSupport << " mm, transport support " << transportSupport
                                 << " mm, widening " << widening;
        EXPECT_LT(widening, 2.5) << "widening " << widening;

        // THE COST, IN THE ONLY CURRENCY THE KERNEL HAS. The taps are shared
        // across channels and sized to the widest, so a 1.8x support at a fixed
        // 17 taps is a 1.8x coarser kernel everywhere. The centre tap is the
        // cleanest single reading of that: its WEIGHT does not change when every
        // offset scales together, but the millimetres it stands for — the part
        // of the profile the pass does not resolve at all — scale with it.
        const SkinDiffusionKernel shipped = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::Medium);
        ASSERT_FALSE(shipped.IsIdentity());
        ASSERT_GT(shipped.TapCount, 2u);
        const u32 centre = shipped.TapCount / 2u;

        const f64 shippedCentreSpan =
            static_cast<f64>(std::abs(shipped.Taps[centre + 1u].x - shipped.Taps[centre].x)) * fitSupport;
        const f64 widenedCentreSpan = shippedCentreSpan * widening;
        EXPECT_GT(shippedCentreSpan, 0.0);
        EXPECT_GT(widenedCentreSpan, shippedCentreSpan * 1.5)
            << "the centre tap spans " << shippedCentreSpan << " mm shipped and " << widenedCentreSpan
            << " mm widened";

        // THE MIRROR, CHECKED AGAINST THE REAL TABLE FIRST. Raising the support
        // fraction does NOT simply move the taps outwards — each tap then stands
        // for a WIDER strip of the same profile, so the weights change too, and
        // a test that only rescaled the offsets would be answering an easier
        // question than the one asked.
        const f64 dRedMM = static_cast<f64>(SkinBurleyScalingMM(parameters).x);
        const SeparableKernel1D mirroredAtNaturalSupport =
            MirroredKernel(dRedMM, fitSupport, static_cast<i32>(shipped.TapCount));
        ASSERT_EQ(mirroredAtNaturalSupport.Weights.size(), static_cast<sizet>(shipped.TapCount));
        for (u32 i = 0; i < shipped.TapCount; ++i)
        {
            EXPECT_NEAR(mirroredAtNaturalSupport.Weights[i], static_cast<f64>(shipped.Taps[i].y), 1.0e-4)
                << "the mirror disagrees with the production tap table at tap " << i
                << " — every number below it is meaningless until it does not";
            EXPECT_NEAR(mirroredAtNaturalSupport.PositionsMM[i],
                        static_cast<f64>(shipped.Taps[i].x) * fitSupport, 1.0e-4)
                << "tap " << i;
        }

        // And now the same construction at the support the transport actually
        // needs: same profile, same tap count, wider support, weights REBUILT.
        const SeparableKernel1D widened =
            MirroredKernel(dRedMM, transportSupport, static_cast<i32>(shipped.TapCount));

        // THE COST, READ OFF THE WEIGHTS. The centre tap is the energy the pass
        // does NOT move — it is the pixel's own radiance, kept. Widening the
        // support hands it a wider strip of the same profile, so a larger share
        // of the blur turns into no blur at all.
        const f64 shippedCentreWeight = static_cast<f64>(shipped.Taps[centre].y);
        const f64 widenedCentreWeight = widened.Weights[centre];
        EXPECT_GT(widenedCentreWeight, shippedCentreWeight * 1.3)
            << "the centre tap holds " << shippedCentreWeight << " of the profile shipped and "
            << widenedCentreWeight << " widened";

        // THE BENEFIT, AND IT IS NOT THERE. The halo of a bright small feature —
        // the image #1255's terminator measurement bounded the tail error ONTO —
        // barely moves. 0.02 of the feature's energy is a generous ceiling on
        // "barely" when the errors themselves are around 0.056.
        const f64 shippedHalo = WorstHaloError(ProductionKernel(shipped), walk);
        const f64 widenedHalo = WorstHaloError(widened, walk);
        EXPECT_LT(std::abs(widenedHalo - shippedHalo), 0.02)
            << "shipped halo error " << shippedHalo << ", widened " << widenedHalo
            << " — widening the support was supposed to be the cheap fix and it moves nothing";

        // REPORTED, NOT ONLY ASSERTED. This suite's job is to measure, and a
        // decision to change nothing is the one outcome that leaves no trace in
        // the diff — so the numbers it rests on are printed rather than left to
        // be re-derived by whoever doubts it next.
        GTEST_LOG_(INFO) << "#1361 support decision @ authored 0.85: fit support " << fitSupport
                         << " mm holds " << (100.0 * walk.CdfAt(fitSupport)) << "% of transport; covering 99.5% "
                         << "needs " << transportSupport << " mm (x" << widening << "); centre tap "
                         << shippedCentreWeight << " -> " << widenedCentreWeight << " of the profile; halo error "
                         << shippedHalo << " -> " << widenedHalo;

        // And it is not an IMPROVEMENT either, which is the statement that
        // closes the criterion. Asserted with a small slack rather than as a
        // strict inequality: the claim is "no better", not "worse by a specific
        // amount", and a walk reseeded on another machine should not fail it.
        EXPECT_GT(widenedHalo, shippedHalo - 0.005) << "shipped " << shippedHalo << ", widened " << widenedHalo;
    }

    TEST(SkinDiffusionReference, TheSeparableProjectionOutweighsBothErrorsAtASmallBrightFeature)
    {
        // ACCEPTANCE CRITERION 2 OF #1361, DECIDED: THE FIT IS NOT REVISITED,
        // and this is why. It is the one measurement in this file that compares
        // the pass against a version of ITSELF with the errors #1255 found
        // removed, and the result is the opposite of the expected one.
        //
        // The control carries the WALK's own profile, sampled 600 entries to a
        // side against production's 17, across the walk's FULL histogram
        // support: the Burley fit gone, the 0.995 support truncation gone, the
        // tap budget gone. What is left is the SEPARABLE PROJECTION and the
        // control's own quadrature. At the default ScatterColor's red channel it
        // is FURTHER from transport at a bright small feature than the shipped
        // kernel is.
        //
        // FULL SUPPORT RATHER THAN THE WALK's 99.5% RADIUS, so that "the control
        // is itself truncated" cannot explain the result away. It does not: the
        // two differ by 0.0004 of the feature's energy.
        //
        // The reason is the third approximation, the one neither lever touches:
        // the pass is SEPARABLE. TheSeparableKernelIsNotTheTwoDimensionalProfile
        // above says the outer product of two line-spread functions is not the
        // radial profile; this says what that costs in the image, and it costs
        // more than the other two together. A separable kernel over-spreads the
        // core of a point response along the axes; the narrow fit and the
        // truncated support pull energy back in; at 0.85 the pair lands closer
        // to transport than the repaired profile does alone.
        //
        // THAT CANCELLATION IS A COINCIDENCE AND IS NOT ASSERTED AS A VIRTUE.
        // What is asserted is the ordering that makes both levers pointless:
        // repairing the profile does not improve this image, so there is nothing
        // to buy by repairing it.
        constexpr f32 kAuthored = 0.85f;
        const SkinProfileParameters parameters = MakeProfile(kAuthored);
        const Ref::SearchlightProfile& walk = WalkForAuthoredAlbedo(kAuthored);
        ASSERT_LT(walk.Overflow, 0.05) << "overflow " << walk.Overflow;

        const SkinDiffusionKernel shipped = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::Medium);
        ASSERT_FALSE(shipped.IsIdentity());

        // The control spans the walk's whole histogram, not its 99.5% radius.
        const f64 controlSupport = walk.Edges.back();
        ASSERT_GT(controlSupport, 0.0);

        const SeparableKernel1D shippedKernel = ProductionKernel(shipped);
        const SeparableKernel1D control = FinelySampledTransportKernel(walk, controlSupport, 60);
        const SeparableKernel1D controlFine = FinelySampledTransportKernel(walk, controlSupport, 300);

        const f64 shippedError = WorstHaloError(shippedKernel, walk);
        const f64 exactError = WorstHaloError(control, walk);
        const f64 fineError = WorstHaloError(controlFine, walk);

        // THE CONTROL IS CONVERGED, AND THAT IS CHECKED AT EVERY RADIUS RATHER
        // THAN ON THE WORST ONE. Two kernels can reach the same worst error at
        // DIFFERENT radii while disagreeing everywhere else, so comparing the
        // two maxima would be comparing two summaries rather than two curves.
        // The energies themselves are what has to agree.
        for (const f64 r : kHaloRadiiMM)
        {
            EXPECT_NEAR(SeparableFeatureEnergy(control, kFeatureRadiusMM, r),
                        SeparableFeatureEnergy(controlFine, kFeatureRadiusMM, r), 0.01)
                << "the 60-bin and 300-bin controls disagree at r = " << r << " mm";
        }
        EXPECT_LT(std::abs(exactError - fineError), 0.01)
            << "60 bins -> " << exactError << ", 300 bins -> " << fineError;

        // AND THE CONTROL IS NOT A SMALL PERTURBATION OF WHAT SHIPS. Both
        // numbers above are a DISTANCE TO TRANSPORT; this is the distance
        // between the two KERNELS, and without it "the exact one is worse"
        // could be two nearly identical curves straddling the truth.
        f64 kernelGap = 0.0;
        for (const f64 r : kHaloRadiiMM)
        {
            kernelGap = std::max(kernelGap, std::abs(SeparableFeatureEnergy(shippedKernel, kFeatureRadiusMM, r) -
                                                     SeparableFeatureEnergy(control, kFeatureRadiusMM, r)));
        }
        EXPECT_GT(kernelGap, 0.02) << "the shipped kernel and the control differ by only " << kernelGap
                                   << " of the feature's energy, so neither is telling us much about the other";

        // Both are real errors — neither side is exact, and a test that implied
        // the shipped kernel is RIGHT would overclaim badly.
        EXPECT_GT(shippedError, 0.02) << "shipped halo error " << shippedError;
        EXPECT_GT(exactError, 0.02) << "exact-transport halo error " << exactError;

        // THE ORDERING, which is the finding, and what it does and does not say.
        //
        // The control has ONE error term left in it — the separable projection —
        // and it lands FURTHER from transport than the shipped kernel does with
        // ALL of them. A single term measured alone exceeding the total of every
        // term together is what "dominant" means here; it is not inferred from
        // the cancellation, it is read off the control.
        //
        // A margin is required so a reseeded walk cannot flip it by noise.
        EXPECT_GT(exactError, shippedError + 0.01)
            << "shipped kernel " << shippedError << " from transport; the walk's own profile, finely sampled at "
            << "full support, " << exactError
            << " — if the control is now the closer of the two, the separable projection is no longer the "
            << "dominant error and #1361's decision to leave the fit alone should be revisited";

        GTEST_LOG_(INFO) << "#1361 fit decision @ authored 0.85: halo error, shipped kernel " << shippedError
                         << " vs a separable pass carrying the walk's own profile at full support, 600 entries "
                         << "a side: " << fineError << " (60-bin control " << exactError << "); the two kernels "
                         << "themselves differ by " << kernelGap;

        // AND IT IS A LARGE ERROR IN ABSOLUTE TERMS, not merely the larger of two
        // small ones: nearly a twelfth of the feature's light lands at the wrong
        // radius even with a perfect profile and unlimited taps. Pinned as a
        // floor so that a future change which genuinely fixes the projection —
        // a 2D gather, or a second separable pass on the diagonal — fails this
        // test and gets to delete it.
        //
        // (It is NOT compared against the 0.052 that SkinDiffusion.cpp records
        // for tap discretisation. That number is an edge response, a fraction of
        // a unit STEP; this is a fraction of a FEATURE's energy. Same spirit,
        // different quantity, and putting them in one inequality would be a
        // units error dressed up as a finding.)
        EXPECT_GT(exactError, 0.05) << "exact-transport halo error " << exactError;
    }

    // =========================================================================
    // B. COAT TRANSPORT — nonlocal. The estimator, not the geometry.
    // =========================================================================

    TEST(CoatTransportReference, ThePoissonMediumWalkMatchesItsClosedForm)
    {
        // The reference checked before it is used as one. A cube of randomly
        // placed, randomly oriented fibres is a Poisson medium, so the number of
        // crossings along a ray is Poisson distributed and the exact mean
        // transmittance is the generating function exp(-mu (1 - e^-kappa)).
        //
        // The Monte Carlo bundle and the closed form must agree. They are
        // computed by completely different means — one traces geometry, the
        // other evaluates an exponential — so agreement validates the tracer,
        // the medium's Poisson-ness and the estimator together.
        const std::vector<Ref::ReferenceFibre> medium = Ref::MakePoissonFibreSlab(1.0, 0.012, 1.0, 900, 0x1248u);

        // The footprint has to span several mean spacings. At 900 fibres in a
        // 2 x 2 x 2 cube the mean spacing is around 0.2, so 0.8 is four across —
        // the same rule GroomCoatShadow::ReferenceSettings states, and it
        // applies to this reference for the same reason.
        constexpr f64 kFootprint = 0.8;
        constexpr u32 kRays = 4096;

        for (const f64 kappa : { 0.25, 0.75, 2.0 })
        {
            const Ref::CoatTransmittanceEstimate estimate = Ref::CoatBundleTransmittance(
                medium, glm::dvec3(0.0, 0.0, -3.0), glm::dvec3(0.0, 0.0, 1.0), kappa, kFootprint, 6.0, kRays, 0x1255u);

            ASSERT_GT(estimate.MeanCrossings, 0.5) << "the medium is too thin to say anything";
            const f64 closedForm = Ref::PoissonMediumTransmittance(estimate.MeanCrossings, kappa);

            // 4%: the bundle is 4096 rays and the estimator is a mean of values
            // in [0, 1], so one standard error is at most 0.008; the medium is
            // also only approximately Poisson over a finite footprint. A wrong
            // generating function is a factor.
            EXPECT_NEAR(estimate.MeanTransmittance, closedForm, 0.04)
                << "kappa = " << kappa << ", mu = " << estimate.MeanCrossings << ": walk "
                << estimate.MeanTransmittance << " vs Poisson " << closedForm;

            // And the crossing count really is Poisson: variance equals the
            // mean, to within the bundle's own sampling error.
            EXPECT_NEAR(estimate.CrossingVariance / estimate.MeanCrossings, 1.0, 0.15)
                << "mean " << estimate.MeanCrossings << ", variance " << estimate.CrossingVariance;
        }
    }

    TEST(CoatTransportReference, TheProductionFormIsALowerBoundAndTheGapIsMeasured)
    {
        // THE FINDING. GroomCoatShadow::CoatTransmittance returns
        // exp(-kappa * tau) with tau = E[crossings]. The transmittance a
        // fragment footprint actually receives is E[exp(-kappa * N)]. Jensen's
        // inequality puts the second at or above the first, with equality only
        // when N has no variance — so the shipped form OVER-DARKENS a
        // disordered coat, and the amount is a property of the coat's disorder
        // rather than of any parameter.
        //
        // This is a statement about a model, not a bug in an implementation, and
        // #1255 does not ask for it to be fixed. It asks for it to be measured,
        // so that the next person changing coat shadowing knows which direction
        // the error points and roughly how big it is.
        const std::vector<Ref::ReferenceFibre> medium = Ref::MakePoissonFibreSlab(1.0, 0.012, 1.0, 900, 0x1248u);
        constexpr f64 kFootprint = 0.8;
        constexpr u32 kRays = 4096;

        f64 largestGap = 0.0;
        for (const f64 kappa : { 0.25, 0.5, 1.0, 2.0 })
        {
            const Ref::CoatTransmittanceEstimate estimate = Ref::CoatBundleTransmittance(
                medium, glm::dvec3(0.0, 0.0, -3.0), glm::dvec3(0.0, 0.0, 1.0), kappa, kFootprint, 6.0, kRays, 0x1255u);

            // The inequality itself, which is the direction claim.
            EXPECT_GE(estimate.MeanTransmittance, estimate.ExponentialOfMean - 1.0e-9)
                << "kappa = " << kappa << ": Jensen was violated, which means one of the two estimators is wrong";

            largestGap = std::max(largestGap, estimate.MeanTransmittance - estimate.ExponentialOfMean);
        }

        // And it is not a rounding difference. A floor, not a tolerance: the
        // claim is that the gap is real and worth knowing about.
        EXPECT_GT(largestGap, 0.02) << "largest transmittance gap over the kappa sweep = " << largestGap;

        // The production helper is exactly the arm this is compared against, so
        // the comparison is against what ships rather than against a
        // description of it.
        const Ref::CoatTransmittanceEstimate estimate = Ref::CoatBundleTransmittance(
            medium, glm::dvec3(0.0, 0.0, -3.0), glm::dvec3(0.0, 0.0, 1.0), 1.0, kFootprint, 6.0, kRays, 0x1255u);
        EXPECT_NEAR(static_cast<f64>(GroomCoatShadow::CoatTransmittance(estimate.MeanCrossings, 1.0f)),
                    estimate.ExponentialOfMean, 1.0e-5);
    }

    TEST(CoatTransportReference, TheGapClosesOnAnOrderedCoat)
    {
        // THE CONTROL, and without it the test above is asserting a coincidence.
        // If the gap were an artefact of the estimator rather than of the
        // medium's disorder, it would appear on an ordered coat too.
        //
        // THE ORDERED MEDIUM IS A DENSE, TOUCHING LATTICE: fibres along +y at
        // lattice positions (x_i, z_j) with the radius set to half the pitch, so
        // the rows tile the z axis. A ray along +x keeps its z, falls inside
        // exactly one row, and crosses every one of that row's columns — the
        // same count for every ray in the bundle, so the crossing variance
        // collapses and Jensen's gap closes with it.
        //
        // AN EARLIER VERSION OF THIS TEST WAS THE OPPOSITE OF A CONTROL and is
        // worth recording. It laid the fibres along +z and fired along +x, so a
        // ray crossed either a whole row of forty or none of it: the count was
        // bimodal, its variance was THIRTY TIMES its mean, and the "ordered"
        // medium was more disordered than the Poisson one. Regularity is a
        // property of a medium AND a ray direction, never of the medium alone.
        //
        // THE TWO ARMS ARE COMPARED AT MATCHED OPTICAL DEPTH. Jensen's gap
        // depends on both the crossing variance and the mean, so a comparison at
        // a fixed kappa across media with different densities would confound the
        // two. kappa is chosen per medium to put both at tau = 1.2, which is the
        // transmittance a renderer would actually be choosing between.
        constexpr f64 kTargetOpticalDepth = 1.2;
        constexpr u32 kRays = 1024;

        // -- ordered ---------------------------------------------------------
        constexpr f64 kPitch = 0.05;
        const std::vector<Ref::ReferenceFibre> lattice = Ref::MakeLatticeFibreSlab(kPitch, 0.5 * kPitch, 1.0, 40);
        const glm::dvec3 latticeOrigin(-3.0, 0.0, 0.0);
        const glm::dvec3 latticeDirection(1.0, 0.0, 0.0);
        // Eight pitches across, so the bundle samples many rows rather than one.
        constexpr f64 kLatticeFootprint = 0.4;

        const f64 orderedMean =
            Ref::CoatBundleTransmittance(lattice, latticeOrigin, latticeDirection, 1.0, kLatticeFootprint, 8.0, kRays,
                                         0x1255u)
                .MeanCrossings;
        ASSERT_GT(orderedMean, 5.0) << "the lattice is not dense enough to say anything";

        const Ref::CoatTransmittanceEstimate ordered =
            Ref::CoatBundleTransmittance(lattice, latticeOrigin, latticeDirection, kTargetOpticalDepth / orderedMean,
                                         kLatticeFootprint, 8.0, kRays, 0x1255u);

        // -- disordered ------------------------------------------------------
        const std::vector<Ref::ReferenceFibre> poisson = Ref::MakePoissonFibreSlab(1.0, 0.012, 1.0, 900, 0x1248u);
        const glm::dvec3 poissonOrigin(0.0, 0.0, -3.0);
        const glm::dvec3 poissonDirection(0.0, 0.0, 1.0);
        constexpr f64 kPoissonFootprint = 0.8;

        const f64 disorderedMean =
            Ref::CoatBundleTransmittance(poisson, poissonOrigin, poissonDirection, 1.0, kPoissonFootprint, 6.0, kRays,
                                         0x1255u)
                .MeanCrossings;
        ASSERT_GT(disorderedMean, 1.0);

        const Ref::CoatTransmittanceEstimate disordered = Ref::CoatBundleTransmittance(
            poisson, poissonOrigin, poissonDirection, kTargetOpticalDepth / disorderedMean, kPoissonFootprint, 6.0,
            kRays, 0x1255u);

        // -- the mechanism ---------------------------------------------------
        // The ordered coat's crossing count is nearly deterministic; the Poisson
        // one's variance equals its mean. That is the cause.
        const f64 orderedDispersion = ordered.CrossingVariance / ordered.MeanCrossings;
        const f64 disorderedDispersion = disordered.CrossingVariance / disordered.MeanCrossings;
        EXPECT_LT(orderedDispersion, 0.1) << "ordered variance/mean = " << orderedDispersion;
        EXPECT_GT(disorderedDispersion, 0.5) << "disordered variance/mean = " << disorderedDispersion;

        // -- the consequence -------------------------------------------------
        // At the same optical depth, the production form is essentially exact on
        // the ordered coat and measurably dark on the disordered one.
        const auto relativeGap = [](const Ref::CoatTransmittanceEstimate& e)
        { return (e.MeanTransmittance / std::max(e.ExponentialOfMean, 1.0e-12)) - 1.0; };

        const f64 orderedGap = relativeGap(ordered);
        const f64 disorderedGap = relativeGap(disordered);
        EXPECT_LT(orderedGap, 0.005) << "ordered relative gap = " << orderedGap;
        EXPECT_GT(disorderedGap, 4.0 * std::max(orderedGap, 1.0e-6))
            << "ordered " << orderedGap << " vs disordered " << disorderedGap;
    }

    TEST(CoatTransportReference, AFootprintNarrowerThanTheSpacingReportsAConfidentWrongAnswer)
    {
        // The trap GroomCoatShadow::ReferenceSettings documents, reproduced
        // against an independent medium and an independent tracer — so the
        // warning is a property of the CONFIGURATION rather than of that one
        // implementation, which is what a reader needs to know before trusting
        // any coat measurement.
        //
        // On a lattice at pitch 0.05, a footprint of 0.01 fits between strands
        // and every ray in it misses the same way. The answer is not noisy, it
        // is confidently wrong — and it is wrong in the direction that makes a
        // coat look fine: fully lit.
        // A GAPPED lattice this time: the radius is well under half the pitch,
        // so the rows leave clear lanes between them. The fibres run along +y at
        // (x_i, z_j) as always, and the rows sit at +/- pitch/2, +/- 3 pitch/2,
        // ... so z = 0 is exactly a lane.
        constexpr f64 kPitch = 0.05;
        const std::vector<Ref::ReferenceFibre> lattice = Ref::MakeLatticeFibreSlab(kPitch, 0.006, 1.0, 40);
        const glm::dvec3 direction(1.0, 0.0, 0.0);
        const glm::dvec3 gapOrigin(-3.0, 0.0, 0.0);

        const Ref::CoatTransmittanceEstimate narrow =
            Ref::CoatBundleTransmittance(lattice, gapOrigin, direction, 1.0, 0.005, 8.0, 1024, 0x1255u);
        const Ref::CoatTransmittanceEstimate wide =
            Ref::CoatBundleTransmittance(lattice, gapOrigin, direction, 1.0, 0.4, 8.0, 1024, 0x1255u);

        EXPECT_LT(narrow.MeanCrossings, 0.5 * wide.MeanCrossings)
            << "narrow " << narrow.MeanCrossings << " vs wide " << wide.MeanCrossings;
        // Confidently, not noisily: the narrow bundle's own variance is tiny, so
        // nothing in its output says it is wrong.
        EXPECT_LT(narrow.CrossingVariance, wide.CrossingVariance);
    }
} // namespace OloEngine::Tests
