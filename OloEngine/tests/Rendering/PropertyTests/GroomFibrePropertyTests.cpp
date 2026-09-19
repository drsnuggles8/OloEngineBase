#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomFibrePropertyTests — issue #1247, every acceptance criterion that can be
// settled in arithmetic.
//
// THIS FILE IS THE COMPARISON, not a description of one. The issue asks for
// measured/reference fibre lobes to be compared BEFORE an approximation is
// chosen; docs/analysis/groom-fibre-scattering-1247.md records the numbers, and
// every claim that document makes is an assertion here — so the evidence fails
// loudly when it stops being true rather than ageing quietly in a document.
//
// The four groups below map onto the criteria:
//
//   1. PARAMETERS — the pigment, roughness and tilt controls mean what the
//      header says, and a corrupt authored value cannot reach the model.
//   2. THE COMPARISON — the shipped h-quadrature rule against the converged far
//      field, and against the rule that was rejected. This is the one that
//      would have let a wrong-but-plausible approximation ship.
//   3. APPEARANCE — dark, pale and coloured fibres differ in the way the
//      physics says they should, under frontal, grazing and backlight, per
//      lobe.
//   4. ENERGY — the BCSDF conserves energy, and the sampling triple agrees with
//      the evaluation.
//
// Tolerances here are WIDE ON PURPOSE where the quantity is a quadrature
// result: the point of each assertion is the claim in the analysis, not the
// last digit of this machine's floating point. Where a number is exact — the
// lobe tilt shifts, the attenuation identity — the tolerance is tight, and the
// difference between the two kinds is stated at each site.
// =============================================================================

#include "OloEngine/Groom/GroomFibreScattering.h"

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
        constexpr f32 kPi = 3.14159265358979323846f;

        struct FibreCase
        {
            const char* Name;
            f32 Eumelanin;
            f32 Pheomelanin;
        };

        // The dark / pale / coloured axis acceptance criterion 3 names.
        constexpr std::array<FibreCase, 4> kFibres{ {
            { "dark", 4.0f, 0.0f },
            { "brown", 1.3f, 0.0f },
            { "pale", 0.1f, 0.05f },
            { "red", 0.35f, 1.4f },
        } };

        [[nodiscard]] GroomFibreParams MakeFibre(const FibreCase& fibre, f32 longitudinal = 0.3f,
                                                 f32 azimuthal = 0.3f, u32 hSamples = 4)
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = fibre.Eumelanin;
            authored.Pheomelanin = fibre.Pheomelanin;
            authored.LongitudinalRoughness = longitudinal;
            authored.AzimuthalRoughness = azimuthal;
            authored.HSamples = hSamples;
            return MakeGroomFibreParams(authored);
        }

        // The angle grid every comparison runs over: longitudinal angles from
        // -80 to +80 degrees for both directions, and the full azimuth range.
        // Coarser than the analysis's grid so the suite stays quick; the
        // analysis records the fine-grid numbers and this asserts the same
        // conclusions hold on a grid a test can afford.
        struct AnglePoint
        {
            f32 SinThetaI;
            f32 SinThetaO;
            f32 Phi;
        };

        [[nodiscard]] std::vector<AnglePoint> MakeAngleGrid()
        {
            std::vector<AnglePoint> grid;
            // 7 x 7 x 19 = 931 triples. The analysis runs 17 629 on a finer
            // grid; this is sized for a Debug build, where the N = 256
            // reference costs 256 quadrature nodes at every point. The
            // conclusions are the same, and cutting the grid is what keeps the
            // reference CACHED PER FIBRE below from being the slow part of the
            // suite.
            constexpr int kLongitudinal = 7;
            constexpr int kAzimuthal = 19;
            grid.reserve(kLongitudinal * kLongitudinal * kAzimuthal);
            for (int i = 0; i < kLongitudinal; ++i)
            {
                const f32 thetaI = -1.4f + (2.8f * static_cast<f32>(i) / static_cast<f32>(kLongitudinal - 1));
                for (int o = 0; o < kLongitudinal; ++o)
                {
                    const f32 thetaO = -1.4f + (2.8f * static_cast<f32>(o) / static_cast<f32>(kLongitudinal - 1));
                    for (int p = 0; p < kAzimuthal; ++p)
                    {
                        grid.push_back({ std::sin(thetaI), std::sin(thetaO),
                                         kPi * static_cast<f32>(p) / static_cast<f32>(kAzimuthal - 1) });
                    }
                }
            }
            return grid;
        }

        struct ErrorSummary
        {
            f64 RelativeRms = 0.0;
            f64 RelativeMax = 0.0;
        };

        // The converged far field over the whole grid, plus its own RMS.
        // COMPUTED ONCE PER FIBRE and reused across every order and both rules:
        // the reference costs 256 quadrature nodes per point, so recomputing it
        // inside each comparison would make this file the slowest thing in the
        // suite for no extra coverage.
        struct Reference
        {
            std::vector<glm::vec3> Values;
            f64 Rms = 0.0;
        };

        [[nodiscard]] Reference MakeReference(const GroomFibreParams& params, const std::vector<AnglePoint>& grid)
        {
            Reference reference;
            reference.Values.reserve(grid.size());
            f64 squares = 0.0;
            for (const AnglePoint& point : grid)
            {
                const glm::vec3 value =
                    GroomFibreEvaluateReference(params, point.SinThetaO, point.SinThetaI, point.Phi).Sum();
                reference.Values.push_back(value);
                for (int channel = 0; channel < 3; ++channel)
                {
                    squares += static_cast<f64>(value[channel]) * value[channel];
                }
            }
            reference.Rms = std::sqrt(squares / (3.0 * grid.size()));
            return reference;
        }

        // Error of `rule` at order `n` against the converged far field, relative
        // to the reference's own RMS — so the number is a fraction of the signal
        // rather than an absolute radiance nobody can size.
        [[nodiscard]] ErrorSummary MeasureQuadrature(const GroomFibreParams& params,
                                                     const std::vector<AnglePoint>& grid, const Reference& reference,
                                                     u32 n, GroomFibreQuadrature rule)
        {
            f64 errorSquares = 0.0;
            f64 worst = 0.0;

            for (sizet i = 0; i < grid.size(); ++i)
            {
                const glm::vec3 candidate =
                    GroomFibreEvaluateFar(params, grid[i].SinThetaO, grid[i].SinThetaI, grid[i].Phi, n, rule).Sum();
                for (int channel = 0; channel < 3; ++channel)
                {
                    const f64 delta = static_cast<f64>(candidate[channel]) - reference.Values[i][channel];
                    errorSquares += delta * delta;
                    worst = std::max(worst, std::abs(delta));
                }
            }

            if (!(reference.Rms > 0.0))
            {
                return {};
            }
            return { std::sqrt(errorSquares / (3.0 * grid.size())) / reference.Rms, worst / reference.Rms };
        }

        // Total variation of the response along the azimuth, relative to the
        // reference's. The RMS error alone cannot see SPIKINESS, and spikiness
        // is what a coat shows as banding when the strand tangent turns — so
        // this is the metric that describes the actual artefact.
        [[nodiscard]] f64 RelativeAzimuthalVariation(const GroomFibreParams& params, f32 sinThetaO, u32 n,
                                                     GroomFibreQuadrature rule)
        {
            constexpr int kSteps = 360;
            f64 referenceVariation = 0.0;
            f64 candidateVariation = 0.0;
            glm::vec3 previousReference(0.0f);
            glm::vec3 previousCandidate(0.0f);

            for (int i = 0; i <= kSteps; ++i)
            {
                const f32 phi = kPi * static_cast<f32>(i) / static_cast<f32>(kSteps);
                const glm::vec3 reference = GroomFibreEvaluateReference(params, sinThetaO, 0.1f, phi).Sum();
                const glm::vec3 candidate = GroomFibreEvaluateFar(params, sinThetaO, 0.1f, phi, n, rule).Sum();
                if (i > 0)
                {
                    referenceVariation += std::abs(static_cast<f64>(reference.g) - previousReference.g);
                    candidateVariation += std::abs(static_cast<f64>(candidate.g) - previousCandidate.g);
                }
                previousReference = reference;
                previousCandidate = candidate;
            }

            return (referenceVariation > 0.0) ? candidateVariation / referenceVariation : 0.0;
        }

        // The longitudinal angle at which a lobe peaks, in degrees, swept at
        // 0.1-degree resolution.
        [[nodiscard]] f32 LongitudinalPeakDegrees(const GroomFibreParams& params, GroomFibreLobe lobe, f32 phi)
        {
            f32 best = -1.0f;
            f32 bestDegrees = 0.0f;
            for (int i = -700; i <= 700; ++i)
            {
                const f32 degrees = static_cast<f32>(i) / 10.0f;
                const f32 value =
                    GroomFibreEvaluateReference(params, 0.0f, std::sin(glm::radians(degrees)), phi)[lobe].g;
                if (value > best)
                {
                    best = value;
                    bestDegrees = degrees;
                }
            }
            return bestDegrees;
        }
    } // namespace

    // ── 1. Parameters ───────────────────────────────────────────────────────

    TEST(GroomFibreParametersTest, MeasuredPigmentSpectraProduceTheExpectedHairColours)
    {
        // The claim is not that these numbers are pretty: it is that the
        // MEASURED absorption spectra, fed concentrations from the hair
        // literature, land on the colours those concentrations are named for.
        // A transcription error in either spectrum fails here.
        const glm::vec3 black = GroomFibreAmbientResponse(MakeFibre(kFibres[0]), 0.0f).Sum();
        const glm::vec3 brown = GroomFibreAmbientResponse(MakeFibre(kFibres[1]), 0.0f).Sum();
        const glm::vec3 blonde = GroomFibreAmbientResponse(MakeFibre(kFibres[2]), 0.0f).Sum();
        const glm::vec3 red = GroomFibreAmbientResponse(MakeFibre(kFibres[3]), 0.0f).Sum();

        // Dark is dark in every channel.
        EXPECT_LT(black.r, 0.20f);
        EXPECT_LT(black.b, 0.20f);
        // Pale is pale in every channel.
        EXPECT_GT(blonde.r, 0.70f);
        EXPECT_GT(blonde.b, 0.55f);
        // Eumelanin absorbs blue about three times as hard as red, so every
        // eumelanin fibre is warmer than it is cool — the reason dark hair
        // reads dark brown rather than grey.
        EXPECT_GT(brown.r, brown.g);
        EXPECT_GT(brown.g, brown.b);
        // Pheomelanin is what makes red hair red: at a concentration that would
        // make a eumelanin fibre nearly black, it stays saturated.
        EXPECT_GT(red.r / std::max(red.b, 1.0e-4f), 3.0f);
    }

    TEST(GroomFibreParametersTest, ColourInversionReproducesTheAuthoredReflectance)
    {
        // THE BaseColor MODE'S WHOLE PROMISE: what you author is what renders.
        // It is solved by bisection against the model's own albedo, so this is
        // an identity to within the solver's resolution rather than a fit's
        // neighbourhood — and the tight tolerance is the point, because the
        // published fit this replaced missed by up to 0.66 (see below).
        for (const f32 target : { 0.1f, 0.3f, 0.6f, 0.9f })
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::BaseColor;
            authored.BaseColor = glm::vec3(target);
            authored.HSamples = 16;
            const glm::vec3 albedo = GroomFibreAmbientResponse(MakeGroomFibreParams(authored), 0.0f).Sum();
            EXPECT_NEAR(albedo.g, target, 0.01f) << "authored reflectance " << target;
        }
    }

    TEST(GroomFibreParametersTest, ChiangsAssemblyFitIsRejectedForASingleFibre)
    {
        // WHY THE BaseColor MODE DOES NOT USE THE PUBLISHED FIT, measured
        // rather than asserted. Chiang et al.'s inversion answers "what
        // absorption makes a fibre ASSEMBLY look like this colour" — it has
        // inter-fibre multiple scattering baked in. Nothing in this slice
        // transports light between fibres (#1248 owns that), so the absorption
        // it asks for is far too low: it expects neighbours to do the darkening
        // and there are no neighbours.
        //
        // Kept as a test because the fit is still the right answer once #1248
        // lands, so the gap is a number someone will want, not a footnote.
        for (const f32 target : { 0.1f, 0.3f, 0.6f })
        {
            GroomFibreAuthoring viaFit;
            viaFit.PigmentMode = GroomFibrePigmentMode::Absorption;
            viaFit.Absorption = GroomFibreSigmaAFromColor(glm::vec3(target), 0.3f);
            viaFit.HSamples = 16;
            const f32 fitAlbedo = GroomFibreAmbientResponse(MakeGroomFibreParams(viaFit), 0.0f).Sum().g;

            // The fit's fibre is much too BRIGHT, and by a margin no tolerance
            // could absorb.
            EXPECT_GT(fitAlbedo, target + 0.25f)
                << "the assembly fit produced a plausible single-fibre albedo for " << target
                << ", which would mean this test no longer describes why the bisection exists";
        }
    }

    TEST(GroomFibreParametersTest, AFibreCannotBeDarkerThanItsOwnSurfaceReflection)
    {
        // R never enters the fibre, so no amount of pigment removes it — which
        // is why black hair still has a white sheen. An unreachably dark
        // authored colour must therefore SATURATE at the maximum absorption
        // rather than send the solver chasing something that does not exist.
        const glm::vec3 sigma = GroomFibreSigmaAForAlbedo(glm::vec3(0.0f), kGroomFibreDefaultIOR, 4);
        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_FLOAT_EQ(sigma[channel], GroomFibreLimits::MaxAbsorption);
        }

        GroomFibreAuthoring authored;
        authored.PigmentMode = GroomFibrePigmentMode::BaseColor;
        authored.BaseColor = glm::vec3(0.0f);
        const f32 floorAlbedo = GroomFibreAmbientResponse(MakeGroomFibreParams(authored), 0.0f).Sum().g;
        EXPECT_GT(floorAlbedo, 0.0f) << "the darkest fibre reflects nothing at all, so R has been lost";
        EXPECT_LT(floorAlbedo, 0.1f) << "the darkest fibre is not dark";
    }

    TEST(GroomFibreParametersTest, TheCuticleTiltShiftsTheLobesByTwoAlphaAndMinusFourAlpha)
    {
        // Marschner's measured geometry, as an exact prediction: the cuticle
        // scales tilt by alpha, R meets them once and TRT three times, so the R
        // highlight moves to +2 alpha and the TRT highlight to -4 alpha. This
        // is what separates the two highlights on a real head of hair, and it
        // is the one thing in the model with an analytic answer to check
        // against — hence the tight tolerance.
        for (const f32 alphaDegrees : { 1.0f, 2.0f, 5.0f })
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = 0.3f;
            authored.LongitudinalRoughness = 0.15f;
            authored.AzimuthalRoughness = 0.15f;
            authored.TiltDegrees = alphaDegrees;
            const GroomFibreParams params = MakeGroomFibreParams(authored);

            EXPECT_NEAR(LongitudinalPeakDegrees(params, GroomFibreLobe::R, 0.0f), 2.0f * alphaDegrees, 0.6f)
                << "R lobe, alpha = " << alphaDegrees;
            EXPECT_NEAR(LongitudinalPeakDegrees(params, GroomFibreLobe::TRT, glm::radians(150.0f)),
                        -4.0f * alphaDegrees, 0.8f)
                << "TRT lobe, alpha = " << alphaDegrees;
        }
    }

    TEST(GroomFibreParametersTest, CorruptAuthoredValuesAreRefusedRatherThanPropagated)
    {
        // Scene YAML, a save game and the MCP write path all reach the
        // component directly, and these values land in a pow(), a log() and a
        // division inside a fragment shader. A NaN there is not a wrong colour;
        // it is a NaN written into scene colour and then spread over the frame
        // by the post chain.
        GroomFibreAuthoring authored;
        authored.PigmentMode = static_cast<GroomFibrePigmentMode>(77);
        authored.Eumelanin = std::numeric_limits<f32>::quiet_NaN();
        authored.Pheomelanin = -5.0f;
        authored.LongitudinalRoughness = 0.0f;
        authored.AzimuthalRoughness = std::numeric_limits<f32>::infinity();
        authored.TiltDegrees = 1000.0f;
        authored.IndexOfRefraction = 1.0f;
        authored.Intensity = -std::numeric_limits<f32>::infinity();
        authored.HSamples = 100000;

        const GroomFibreAuthoring clean = SanitizeGroomFibreAuthoring(authored);
        // REJECTED to the default, not saturated: a mode index is discriminated,
        // so the nearest valid value is a different material and not a closer one.
        EXPECT_EQ(clean.PigmentMode, GroomFibrePigmentMode::Melanin);
        EXPECT_TRUE(std::isfinite(clean.Eumelanin));
        EXPECT_GE(clean.Pheomelanin, 0.0f);
        EXPECT_GE(clean.LongitudinalRoughness, GroomFibreLimits::MinRoughness);
        EXPECT_TRUE(std::isfinite(clean.AzimuthalRoughness));
        EXPECT_LE(clean.TiltDegrees, GroomFibreLimits::MaxTiltDegrees);
        EXPECT_GE(clean.IndexOfRefraction, GroomFibreLimits::MinIOR);
        EXPECT_GE(clean.Intensity, 0.0f);
        EXPECT_LE(clean.HSamples, GroomFibreLimits::MaxHSamples);

        // And the whole thing evaluates finite afterwards, which is the
        // property the clamping exists for rather than the clamping itself.
        const GroomFibreLobeSet lobes = GroomFibreEvaluateFar(MakeGroomFibreParams(authored), 0.3f, -0.2f, 1.0f);
        for (u32 lobe = 0; lobe < kGroomFibreLobeCount; ++lobe)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_TRUE(std::isfinite(lobes.Lobe[lobe][channel]));
            }
        }
    }

    TEST(GroomFibreParametersTest, APureBlackAuthoredColourDoesNotReachAnInfiniteAbsorption)
    {
        // log(0) is -inf and the inversion squares it. Pure black is a legal
        // thing to type into a colour picker, so it must land on the model's
        // darkest representable fibre rather than on a NaN.
        const glm::vec3 sigma = GroomFibreSigmaAFromColor(glm::vec3(0.0f), 0.3f);
        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_TRUE(std::isfinite(sigma[channel]));
            EXPECT_LE(sigma[channel], GroomFibreLimits::MaxAbsorption);
        }
    }

    // ── 2. The comparison that chose the approximation ──────────────────────

    TEST(GroomFibreQuadratureTest, ThePointSampledRuleIsRejectedAtEveryAffordableOrder)
    {
        // THE MEASUREMENT THAT REJECTED THE OBVIOUS IMPLEMENTATION. Averaging N
        // point samples of the near field is what a first version of this
        // writes, and at every order a fragment shader can afford it reproduces
        // the far-field lobe as N separate spikes: over 50 % RMS away from the
        // truth, with individual angles off by more than ten times the signal's
        // own RMS.
        //
        // Kept as a test rather than as a paragraph because the rejected rule
        // is still callable, so this re-runs the rejection on every build. The
        // bounds are loose — the claim is "unusable", not a particular number.
        const std::vector<AnglePoint> grid = MakeAngleGrid();
        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            const Reference reference = MakeReference(params, grid);
            const ErrorSummary pointSampled =
                MeasureQuadrature(params, grid, reference, 4, GroomFibreQuadrature::PointSampled);
            EXPECT_GT(pointSampled.RelativeRms, 0.4) << fibre.Name;
            EXPECT_GT(pointSampled.RelativeMax, 5.0) << fibre.Name;
        }
    }

    TEST(GroomFibreQuadratureTest, TheShippedRuleIsWithinTheMeasuredBandAtTheDefaultOrder)
    {
        // The shipped rule at the shipped order, against the converged far
        // field. The analysis's table 1 records 0.10-0.18 relative RMS at N=4
        // across these fibres on a finer grid; the bound here is the claim
        // "under a quarter of the signal", which is what makes the difference
        // from the rejected rule categorical rather than marginal.
        const std::vector<AnglePoint> grid = MakeAngleGrid();
        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            const Reference reference = MakeReference(params, grid);
            const ErrorSummary widened =
                MeasureQuadrature(params, grid, reference, 4, GroomFibreQuadrature::NodeWidened);
            EXPECT_LT(widened.RelativeRms, 0.25) << fibre.Name;
            EXPECT_LT(widened.RelativeMax, 4.0) << fibre.Name;

            // And it must be strictly better than the rule it replaced, on the
            // same fibre at the same order — the comparison the choice rests on.
            const ErrorSummary pointSampled =
                MeasureQuadrature(params, grid, reference, 4, GroomFibreQuadrature::PointSampled);
            EXPECT_LT(widened.RelativeRms, pointSampled.RelativeRms * 0.5) << fibre.Name;
        }
    }

    TEST(GroomFibreQuadratureTest, TheShippedRuleIsAsSmoothInAzimuthAsTheTruth)
    {
        // The RMS error cannot see SPIKINESS, and spikiness is the artefact:
        // a response with N narrow peaks bands visibly as the strand tangent
        // turns, whatever its RMS. Total variation along the azimuth is the
        // metric that describes it, and the shipped rule has to be close to the
        // reference's while the rejected rule is a multiple of it.
        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            const f64 widened = RelativeAzimuthalVariation(params, 0.2f, 4, GroomFibreQuadrature::NodeWidened);
            const f64 pointSampled = RelativeAzimuthalVariation(params, 0.2f, 4, GroomFibreQuadrature::PointSampled);

            EXPECT_GT(widened, 0.5) << fibre.Name;
            EXPECT_LT(widened, 1.6) << fibre.Name;
            EXPECT_GT(pointSampled, widened) << fibre.Name;
        }
    }

    TEST(GroomFibreQuadratureTest, RaisingTheOrderImprovesTheShippedRule)
    {
        // Monotone convergence, which is what makes the order an honest quality
        // knob rather than a number that happened to look good at 4. Checked at
        // the ends rather than step by step: the rule is a quadrature, not a
        // sequence with a guaranteed per-step improvement.
        const std::vector<AnglePoint> grid = MakeAngleGrid();
        const GroomFibreParams params = MakeFibre(kFibres[1]);
        const Reference reference = MakeReference(params, grid);
        const ErrorSummary coarse = MeasureQuadrature(params, grid, reference, 2, GroomFibreQuadrature::NodeWidened);
        const ErrorSummary fine = MeasureQuadrature(params, grid, reference, 16, GroomFibreQuadrature::NodeWidened);
        EXPECT_LT(fine.RelativeRms, coarse.RelativeRms);
    }

    // ── 3. Appearance: dark, pale and coloured, front to back ───────────────

    TEST(GroomFibreAppearanceTest, TransmissionDominatesInPaleFibresUnderBacklight)
    {
        // THE CRITERION-3 CLAIM, AS A NUMBER. What separates pale hair from
        // dark hair is not brightness, it is which path carries the light: a
        // pale fibre backlit is almost entirely TT, and a dark fibre backlit
        // has had its TT absorbed away and is left with the same surface
        // reflection it had from the front.
        const GroomFibreParams pale = MakeFibre(kFibres[2], 0.15f, 0.15f, 16);
        const GroomFibreParams dark = MakeFibre(kFibres[0], 0.15f, 0.15f, 16);

        const GroomFibreLobeSet paleBacklit = GroomFibreEvaluateReference(pale, 0.0f, 0.0f, glm::radians(175.0f));
        const GroomFibreLobeSet darkBacklit = GroomFibreEvaluateReference(dark, 0.0f, 0.0f, glm::radians(175.0f));

        const f32 paleRatio =
            paleBacklit[GroomFibreLobe::TT].g / std::max(paleBacklit[GroomFibreLobe::R].g, 1.0e-9f);
        const f32 darkRatio =
            darkBacklit[GroomFibreLobe::TT].g / std::max(darkBacklit[GroomFibreLobe::R].g, 1.0e-9f);

        EXPECT_GT(paleRatio, 20.0f);
        EXPECT_GT(paleRatio, darkRatio * 20.0f);
    }

    TEST(GroomFibreAppearanceTest, TheSurfaceReflectionIsUncolouredWhateverThePigment)
    {
        // R never enters the fibre, so it cannot pick up the pigment: it
        // carries the LIGHT's colour. This is why even black hair has a white
        // sheen, and an R lobe that tinted with the pigment would be the most
        // common way to get dark hair wrong.
        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            const GroomFibreLobeSet lobes = GroomFibreEvaluateReference(params, 0.2f, -0.2f, glm::radians(20.0f));
            const glm::vec3& r = lobes[GroomFibreLobe::R];
            EXPECT_NEAR(r.r, r.g, 1.0e-6f) << fibre.Name;
            EXPECT_NEAR(r.g, r.b, 1.0e-6f) << fibre.Name;
        }
    }

    TEST(GroomFibreAppearanceTest, EveryFibreRespondsAtEveryLightingAngle)
    {
        // The plainest failure a shading model can have, and the one a
        // screenshot hides: a whole lighting configuration that returns zero.
        // Frontal, grazing and backlit, for each fibre, with the sum required
        // to be positive and finite.
        struct Configuration
        {
            const char* Name;
            f32 SinThetaO;
            f32 SinThetaI;
            f32 Phi;
        };
        const std::array<Configuration, 3> configurations{ {
            { "frontal", 0.0f, 0.0f, glm::radians(10.0f) },
            { "grazing", 0.75f, 0.70f, glm::radians(90.0f) },
            { "backlit", 0.0f, 0.0f, glm::radians(170.0f) },
        } };

        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            for (const Configuration& configuration : configurations)
            {
                const glm::vec3 value =
                    GroomFibreEvaluateFar(params, configuration.SinThetaO, configuration.SinThetaI, configuration.Phi)
                        .Sum();
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_TRUE(std::isfinite(value[channel])) << fibre.Name << " / " << configuration.Name;
                }
                EXPECT_GT(value.g, 0.0f) << fibre.Name << " / " << configuration.Name;
            }
        }
    }

    TEST(GroomFibreAppearanceTest, TheSeparatedLobesSumToTheFullResponse)
    {
        // The diagnostic contributions are a DECOMPOSITION, not four
        // independently computed pictures. If they did not sum to the material,
        // a debug capture would be describing something other than what ships —
        // which is the failure mode a diagnostic view is supposed to prevent.
        const GroomFibreParams params = MakeFibre(kFibres[1]);
        for (const f32 phi : { 0.2f, 1.0f, 2.0f, 3.0f })
        {
            const GroomFibreLobeSet lobes = GroomFibreEvaluateFar(params, 0.3f, -0.1f, phi);
            const glm::vec3 sum = lobes[GroomFibreLobe::R] + lobes[GroomFibreLobe::TT] + lobes[GroomFibreLobe::TRT] +
                                  lobes[GroomFibreLobe::Residual];
            EXPECT_NEAR(sum.g, lobes.Sum().g, 1.0e-6f);
        }
    }

    // ── 4. Energy, and the sampling triple ──────────────────────────────────

    TEST(GroomFibreEnergyTest, AnAbsorptionFreeFibreReturnsAllOfTheEnergyItReceives)
    {
        // ACCEPTANCE CRITERION 4 AS A MEASUREMENT. With no absorption the four
        // attenuations sum to exactly one by construction, so the whole BCSDF
        // integrates to one over the sphere — for every roughness, at every
        // view angle, and at every quadrature order, because the quadrature is
        // a convex combination and cannot change a total.
        //
        // The 2 % band is the angular quadrature's own error, not the model's:
        // the integral is evaluated on a 512x512 grid over a function with a
        // narrow lobe.
        for (const f32 longitudinal : { 0.1f, 0.3f, 0.6f, 0.9f })
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Absorption;
            authored.Absorption = glm::vec3(0.0f);
            authored.LongitudinalRoughness = longitudinal;
            for (const u32 order : { 1u, 4u, 16u })
            {
                authored.HSamples = order;
                const GroomFibreParams params = MakeGroomFibreParams(authored);
                for (const f32 sinThetaO : { 0.0f, 0.4f, 0.8f })
                {
                    const glm::vec3 albedo = GroomFibreWhiteFurnace(params, sinThetaO, 512, 512);
                    EXPECT_NEAR(albedo.g, 1.0f, 0.02f)
                        << "beta_M " << longitudinal << ", N " << order << ", sinThetaO " << sinThetaO;
                }
            }
        }
    }

    TEST(GroomFibreEnergyTest, AnAbsorbingFibreReturnsLessThanItReceives)
    {
        // The other half of the statement: energy is conserved, not created.
        // A model that passed the furnace test by normalising its output would
        // pass it here too and be wrong — so the pigment has to darken it.
        for (const FibreCase& fibre : kFibres)
        {
            const glm::vec3 albedo = GroomFibreWhiteFurnace(MakeFibre(fibre, 0.3f, 0.3f, 16), 0.1f, 360, 360);
            EXPECT_LT(albedo.g, 1.0f) << fibre.Name;
            EXPECT_GT(albedo.g, 0.0f) << fibre.Name;
        }
        // And more pigment is darker, monotonically.
        const f32 dark = GroomFibreWhiteFurnace(MakeFibre(kFibres[0], 0.3f, 0.3f, 16), 0.1f, 360, 360).g;
        const f32 brown = GroomFibreWhiteFurnace(MakeFibre(kFibres[1], 0.3f, 0.3f, 16), 0.1f, 360, 360).g;
        const f32 pale = GroomFibreWhiteFurnace(MakeFibre(kFibres[2], 0.3f, 0.3f, 16), 0.1f, 360, 360).g;
        EXPECT_LT(dark, brown);
        EXPECT_LT(brown, pale);
    }

    TEST(GroomFibreEnergyTest, SamplingAndEvaluationAgreeOnTheSameIntegral)
    {
        // THE TRIPLE'S CONSISTENCY CHECK, and the reason the sampling routine
        // exists at all in a slice with no stochastic consumer. The same number
        // is reached two independent ways — a deterministic quadrature of the
        // evaluation, and a Monte Carlo average of value/pdf through the
        // sampler. They agree only if evaluate, sample AND pdf all agree, so a
        // density that did not match its own sampling fails here rather than
        // converging beautifully to the wrong image in #1248.
        for (const f32 longitudinal : { 0.15f, 0.4f, 0.8f })
        {
            for (const f32 azimuthal : { 0.2f, 0.5f, 0.9f })
            {
                GroomFibreAuthoring authored;
                authored.PigmentMode = GroomFibrePigmentMode::Absorption;
                authored.Absorption = glm::vec3(0.0f);
                authored.LongitudinalRoughness = longitudinal;
                authored.AzimuthalRoughness = azimuthal;
                authored.HSamples = 1;
                const GroomFibreParams params = MakeGroomFibreParams(authored);

                const glm::vec3 sampled = GroomFibreSampledFurnace(params, 0.2f, 0.3f, 60000, 1247u);
                EXPECT_NEAR(sampled.g, 1.0f, 0.02f) << "beta_M " << longitudinal << ", beta_N " << azimuthal;
            }
        }
    }

    TEST(GroomFibreEnergyTest, TheSampledDirectionsCarryTheDensityTheSamplerReports)
    {
        // The pdf returned by the sampler must equal the pdf the standalone
        // density function reports for the same direction. They are separate
        // code paths — the sampler accumulates its density while inverting, the
        // pdf function evaluates it from scratch — so agreement is a real check
        // and not a tautology.
        const GroomFibreParams params = MakeFibre(kFibres[1], 0.3f, 0.3f, 1);
        const glm::vec3 wo(0.25f, 0.968f, 0.0f);

        u32 checked = 0;
        for (u32 i = 0; i < 64; ++i)
        {
            const f32 u0 = (static_cast<f32>(i) + 0.5f) / 64.0f;
            const glm::vec4 u(u0, std::fmod(u0 * 7.0f, 1.0f), std::fmod(u0 * 13.0f, 1.0f),
                              std::fmod(u0 * 29.0f, 1.0f));
            const GroomFibreSampleResult sample = GroomFibreSample(params, wo, 0.2f, u);
            if (!sample.Valid)
            {
                continue;
            }
            const f32 direct = GroomFibrePdf(params, wo, sample.Wi, 0.2f);
            EXPECT_NEAR(sample.Pdf, direct, std::max(1.0e-4f, direct * 1.0e-3f));
            ++checked;
        }
        EXPECT_GT(checked, 50u) << "the sampler refused nearly every draw, so nothing was compared";
    }

    TEST(GroomFibreEnergyTest, TheEnvironmentTermUsesTheSameAttenuationsAsTheDirectLighting)
    {
        // Criterion 4's consistency requirement, checked where it can actually
        // be wrong: the ambient response has to move with the pigment exactly
        // as the direct response does. A separately authored ambient — the
        // usual way this criterion gets violated — would not track.
        for (const FibreCase& fibre : kFibres)
        {
            const GroomFibreParams params = MakeFibre(fibre);
            const glm::vec3 ambient = GroomFibreAmbientResponse(params, 0.0f).Sum();
            // With no absorption the ambient response is the full cos(theta_o),
            // which at theta_o = 0 is one: the fibre returns everything.
            GroomFibreAuthoring clear;
            clear.PigmentMode = GroomFibrePigmentMode::Absorption;
            clear.Absorption = glm::vec3(0.0f);
            const glm::vec3 clearAmbient = GroomFibreAmbientResponse(MakeGroomFibreParams(clear), 0.0f).Sum();
            EXPECT_NEAR(clearAmbient.g, 1.0f, 0.01f);
            EXPECT_LT(ambient.g, clearAmbient.g) << fibre.Name;
        }
    }
} // namespace OloEngine::Tests
