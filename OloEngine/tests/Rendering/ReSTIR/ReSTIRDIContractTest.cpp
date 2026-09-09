// OLO_TEST_LAYER: plumbing
// =============================================================================
// ReSTIRDIContractTest.cpp — the headless half of ReSTIR DI (issue #1140).
//
// Everything here runs without a device, which is the state every CI runner is
// in, and it pins the parts of the estimator that must hold BEFORE a ray is
// traced. The reason that matters is stated in ReservoirDI.h: a resampled
// estimator that is WRONG still looks plausible. Noise reads as noise, and a
// missing Jacobian or a mis-normalised MIS weight reads as "slightly darker
// contact shadows", which nobody reports as a bug. So:
//
//   * THE MEASURE IDENTITY. Applying the shift Jacobian and evaluating in
//     solid-angle measure gives the SAME estimator as evaluating in area
//     measure with no Jacobian at all. That identity, over randomised
//     geometry, is what catches a cosine taken at the wrong end or distances
//     the wrong way up — the two mistakes the term invites, and the two a
//     hand-picked expected value would not catch.
//   * RIS IS UNBIASED. A Monte Carlo run of the streaming estimator converges
//     to the integral it claims to estimate, for a target function that
//     deliberately disagrees with the source density.
//   * THE TWO NORMALISERS. 1/M and the balance heuristic each produce the
//     weight they claim, and applying both is a detectable error rather than a
//     plausible dimming.
//   * THE GUARDS. A zero weight sum, a NaN, a negative M and an out-of-range
//     sample kind all end as an EMPTY reservoir rather than a clamped one,
//     because a false M suppresses every future candidate at that pixel and so
//     turns a transient corruption into a permanent one.
//   * THE SHADER CONSTANTS. The GLSL loop bounds, layout version and enum
//     values are the C++ ones, scanned out of the shader source — a divergence
//     there is a reservoir reinterpreted at the wrong layout, which is not a
//     crash but a plausible wrong image.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReSTIR/ReSTIRDITechnique.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirDI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::ReSTIR;

    namespace
    {
        [[nodiscard]] std::filesystem::path ResolveShaderPath(const char* relative)
        {
            namespace fs = std::filesystem;
            const fs::path candidates[] = {
                fs::path("OloEditor") / "assets" / "shaders" / relative,
                fs::path("assets") / "shaders" / relative,
                fs::path("..") / "OloEditor" / "assets" / "shaders" / relative,
            };
            for (const auto& candidate : candidates)
            {
                if (fs::exists(candidate))
                    return candidate;
            }
            return {};
        }

        [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        // `#define NAME 12u` or `#define NAME 12`.
        [[nodiscard]] u32 ScanDefine(const std::string& source, const char* name)
        {
            const std::regex pattern(std::string("#define\\s+") + name + "\\s+(\\d+)u?");
            std::smatch match;
            if (!std::regex_search(source, match, pattern))
                return ~0u;
            return static_cast<u32>(std::stoul(match[1].str()));
        }

        // `const uint NAME = 12u;`
        [[nodiscard]] u32 ScanConstUint(const std::string& source, const char* name)
        {
            const std::regex pattern(std::string("const\\s+uint\\s+") + name + "\\s*=\\s*(\\d+)u?\\s*;");
            std::smatch match;
            if (!std::regex_search(source, match, pattern))
                return ~0u;
            return static_cast<u32>(std::stoul(match[1].str()));
        }

        // A deterministic emitter point and two shading points that see it from
        // different angles and distances — the configuration a spatial reuse
        // actually faces.
        struct ShiftFixture
        {
            LightSample Sample{};
            glm::vec3 DestPoint{ 0.0f };
            glm::vec3 SourcePoint{ 0.0f };
        };

        [[nodiscard]] ShiftFixture MakeShiftFixture(std::mt19937& rng)
        {
            std::uniform_real_distribution<f32> coord(-4.0f, 4.0f);
            std::uniform_real_distribution<f32> normalAxis(-1.0f, 1.0f);
            ShiftFixture fixture;
            fixture.Sample.Kind = LightSampleKind::EmissiveTriangle;
            fixture.Sample.LightIndex = 3;
            fixture.Sample.Position = glm::vec3(coord(rng), coord(rng), coord(rng));
            fixture.Sample.Radiance = glm::vec3(1.0f, 2.0f, 3.0f);

            // A normal that is not degenerate and not axis-aligned, so a cosine
            // taken at the wrong end cannot coincidentally match.
            glm::vec3 n{ normalAxis(rng), normalAxis(rng), normalAxis(rng) };
            while (glm::dot(n, n) < 0.05f)
                n = glm::vec3(normalAxis(rng), normalAxis(rng), normalAxis(rng));
            fixture.Sample.Normal = glm::normalize(n);

            // Two shading points well off the emitter's tangent plane, so both
            // cosines are comfortably above the rejection threshold.
            const glm::vec3 offsetA = fixture.Sample.Normal * (1.5f + std::abs(coord(rng)));
            const glm::vec3 offsetB = fixture.Sample.Normal * (2.5f + std::abs(coord(rng)));
            fixture.DestPoint = fixture.Sample.Position + offsetA +
                                glm::vec3(coord(rng) * 0.25f, coord(rng) * 0.25f, coord(rng) * 0.25f);
            fixture.SourcePoint = fixture.Sample.Position + offsetB +
                                  glm::vec3(coord(rng) * 0.25f, coord(rng) * 0.25f, coord(rng) * 0.25f);
            return fixture;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The shift Jacobian
    // -------------------------------------------------------------------------

    // THE ONE TEST THAT MATTERS MOST IN THIS FILE.
    //
    // The estimator can be written two ways, and they must agree:
    //
    //   (a) area measure: the source density is p_area, no Jacobian.
    //   (b) solid-angle measure: the source density is p_area converted at the
    //       SOURCE point, then carried to the destination by the Jacobian.
    //
    // Concretely, converting at the source and then applying J must equal
    // converting at the destination directly. If the Jacobian's cosines are
    // taken at the shading points instead of at the emitter, or the two squared
    // distances are swapped, this identity breaks — and nothing else in a
    // rendered frame does.
    TEST(ReSTIRDIContract, ShiftJacobianIsTheAreaToSolidAngleMeasureChange)
    {
        std::mt19937 rng(0x1140u);
        for (int trial = 0; trial < 512; ++trial)
        {
            const ShiftFixture fixture = MakeShiftFixture(rng);
            constexpr f32 areaPdf = 0.375f;

            const f32 atSource = AreaPdfToSolidAnglePdf(areaPdf, fixture.Sample, fixture.SourcePoint);
            const f32 atDest = AreaPdfToSolidAnglePdf(areaPdf, fixture.Sample, fixture.DestPoint);
            ASSERT_GT(atSource, 0.0f) << "trial " << trial;
            ASSERT_GT(atDest, 0.0f) << "trial " << trial;

            const f32 jacobian = ShiftJacobian(fixture.Sample, fixture.DestPoint, fixture.SourcePoint);
            ASSERT_GT(jacobian, 0.0f) << "trial " << trial;

            // p_solidAngle(dest) == p_solidAngle(source) / J.
            //
            // The direction is the part that is easy to get backwards, and it is
            // asserted here rather than described in a comment: a density that
            // MULTIPLIED by the Jacobian would satisfy no identity at all except
            // when J happens to be 1.
            EXPECT_NEAR(atDest, atSource / jacobian, std::abs(atDest) * 1.0e-4f + 1.0e-6f)
                << "trial " << trial << " J=" << jacobian;
        }
    }

    TEST(ReSTIRDIContract, ShiftJacobianIsReciprocalWhenTheEndpointsSwap)
    {
        std::mt19937 rng(0xB0BAu);
        for (int trial = 0; trial < 256; ++trial)
        {
            const ShiftFixture fixture = MakeShiftFixture(rng);
            const f32 forward = ShiftJacobian(fixture.Sample, fixture.DestPoint, fixture.SourcePoint);
            const f32 backward = ShiftJacobian(fixture.Sample, fixture.SourcePoint, fixture.DestPoint);
            ASSERT_GT(forward, 0.0f);
            ASSERT_GT(backward, 0.0f);
            EXPECT_NEAR(forward * backward, 1.0f, 1.0e-4f) << "trial " << trial;
        }
    }

    // A delta light is the same point in a discrete measure from both pixels.
    // Returning anything but exactly 1 would scale every punctual light's
    // contribution by a geometric factor that has no business being there.
    TEST(ReSTIRDIContract, ShiftJacobianIsExactlyOneForDeltaLights)
    {
        LightSample punctual{};
        punctual.Kind = LightSampleKind::Punctual;
        punctual.Position = glm::vec3(1.0f, 2.0f, 3.0f);
        // Deliberately non-zero, to prove the normal is not consulted.
        punctual.Normal = glm::vec3(0.3f, -0.7f, 0.2f);
        EXPECT_FLOAT_EQ(ShiftJacobian(punctual, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(-2.0f, 4.0f, 1.0f)), 1.0f);

        LightSample directional = punctual;
        directional.Kind = LightSampleKind::Directional;
        EXPECT_FLOAT_EQ(ShiftJacobian(directional, glm::vec3(9.0f, 9.0f, 9.0f), glm::vec3(0.0f)), 1.0f);
    }

    // An edge-on source puts a vanishing cosine in the DENOMINATOR. Rejecting is
    // correct; scaling would inject a firefly that temporal reuse then keeps
    // alive for as long as the M cap allows.
    TEST(ReSTIRDIContract, ShiftJacobianRejectsRatherThanExplodesOnAGrazingSource)
    {
        LightSample sample{};
        sample.Kind = LightSampleKind::EmissiveTriangle;
        sample.Position = glm::vec3(0.0f);
        sample.Normal = glm::vec3(0.0f, 0.0f, 1.0f);

        // The source lies in the emitter's tangent plane: cos(phiSource) == 0.
        const glm::vec3 grazingSource(1.0f, 0.0f, 0.0f);
        const glm::vec3 dest(0.0f, 0.0f, 2.0f);
        EXPECT_FLOAT_EQ(ShiftJacobian(sample, dest, grazingSource), 0.0f);

        // Degenerate geometry: the destination coincides with the emitter point.
        EXPECT_FLOAT_EQ(ShiftJacobian(sample, sample.Position, glm::vec3(0.0f, 0.0f, 3.0f)), 0.0f);

        // A zero normal cannot define a measure at all.
        LightSample noNormal = sample;
        noNormal.Normal = glm::vec3(0.0f);
        EXPECT_FLOAT_EQ(ShiftJacobian(noNormal, dest, glm::vec3(0.0f, 0.0f, 3.0f)), 0.0f);
    }

    // -------------------------------------------------------------------------
    // Streaming RIS
    // -------------------------------------------------------------------------

    // The estimator's whole claim: E[pHat(y) * W] == integral of pHat over the
    // source domain. Checked against a closed form, with a target function that
    // deliberately disagrees with the uniform source density — a target equal to
    // the source would make a broken normaliser invisible.
    TEST(ReSTIRDIContract, StreamingRISIsUnbiased)
    {
        // The domain is [0,1); the source pdf is uniform (density 1); the target
        // function is pHat(x) = 3x^2, whose integral is exactly 1.
        constexpr f32 kExpectedIntegral = 1.0f;
        constexpr u32 kCandidatesPerReservoir = 8;
        constexpr u32 kReservoirs = 40000;

        std::mt19937 rng(0x9E3779B9u);
        std::uniform_real_distribution<f32> uniform(0.0f, 1.0f);

        f64 accumulated = 0.0;
        for (u32 r = 0; r < kReservoirs; ++r)
        {
            Reservoir reservoir{};
            for (u32 c = 0; c < kCandidatesPerReservoir; ++c)
            {
                const f32 x = uniform(rng);
                const f32 targetPdf = 3.0f * x * x;
                LightSample candidate{};
                candidate.Kind = LightSampleKind::EmissiveTriangle;
                // The sampled value rides in the position so the survivor can be
                // read back; nothing here interprets it geometrically.
                candidate.Position = glm::vec3(x, 0.0f, 0.0f);
                // sourcePdf == 1, so the RIS weight is the target function.
                ReservoirUpdate(reservoir, candidate, targetPdf, targetPdf, uniform(rng));
            }
            FinalizeInitialCandidates(reservoir);
            // The estimator: pHat(y) * W. With pHat as the integrand this is the
            // one-sample estimate of the integral.
            accumulated += static_cast<f64>(reservoir.TargetPdf) * static_cast<f64>(reservoir.W);
        }

        const f64 mean = accumulated / static_cast<f64>(kReservoirs);
        // 40k reservoirs of 8 candidates: the standard error is well inside 1%.
        EXPECT_NEAR(mean, static_cast<f64>(kExpectedIntegral), 0.02)
            << "streaming RIS is biased: mean " << mean;
    }

    // M counts every candidate the estimator CONSIDERED, including the ones it
    // rejected. Dropping a rejected candidate would inflate every survivor's
    // weight by the rejection rate.
    TEST(ReSTIRDIContract, ReservoirUpdateCountsEveryAcceptedCandidateInM)
    {
        Reservoir reservoir{};
        LightSample sample{};
        sample.Kind = LightSampleKind::Punctual;

        // Three candidates with a real weight, and one with weight zero: all
        // four are candidates, so M is 4.
        ReservoirUpdate(reservoir, sample, 1.0f, 1.0f, 0.0f);
        ReservoirUpdate(reservoir, sample, 2.0f, 2.0f, 0.5f);
        ReservoirUpdate(reservoir, sample, 0.0f, 0.0f, 0.5f);
        ReservoirUpdate(reservoir, sample, 4.0f, 4.0f, 0.5f);
        EXPECT_FLOAT_EQ(reservoir.M, 4.0f);
        EXPECT_FLOAT_EQ(reservoir.WeightSum, 7.0f);
    }

    // A candidate with a non-finite or negative weight is DROPPED, not clamped:
    // clamping folds a garbage candidate into M and quietly lowers every later
    // candidate's chance of being picked.
    TEST(ReSTIRDIContract, ReservoirUpdateDropsGarbageWithoutTouchingM)
    {
        Reservoir reservoir{};
        LightSample sample{};
        sample.Kind = LightSampleKind::Punctual;
        ReservoirUpdate(reservoir, sample, 1.0f, 1.0f, 0.0f);
        const f32 mAfterGood = reservoir.M;
        const f32 sumAfterGood = reservoir.WeightSum;

        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, std::numeric_limits<f32>::quiet_NaN(), 1.0f, 0.0f));
        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, std::numeric_limits<f32>::infinity(), 1.0f, 0.0f));
        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, -1.0f, 1.0f, 0.0f));
        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, 1.0f, std::numeric_limits<f32>::quiet_NaN(), 0.0f));
        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, 1.0f, -1.0f, 0.0f));

        EXPECT_FLOAT_EQ(reservoir.M, mAfterGood);
        EXPECT_FLOAT_EQ(reservoir.WeightSum, sumAfterGood);
    }

    // The selection test must be FALSE when nothing has been added and the
    // candidate is worthless — the `xi < weight / WeightSum` spelling divides by
    // zero here and selects on a NaN comparison instead.
    TEST(ReSTIRDIContract, ReservoirUpdateSelectsNothingOnAZeroWeightSum)
    {
        Reservoir reservoir{};
        LightSample sample{};
        sample.Kind = LightSampleKind::SphereArea;
        EXPECT_FALSE(ReservoirUpdate(reservoir, sample, 0.0f, 0.0f, 0.0f));
        EXPECT_EQ(reservoir.Sample.Kind, LightSampleKind::None);
        EXPECT_TRUE(reservoir.IsEmpty());
        // The candidate still counted.
        EXPECT_FLOAT_EQ(reservoir.M, 1.0f);
    }

    // -------------------------------------------------------------------------
    // Normalisation
    // -------------------------------------------------------------------------

    TEST(ReSTIRDIContract, ContributionWeightGuardsEveryDegenerateDivisor)
    {
        EXPECT_FLOAT_EQ(ComputeContributionWeight(0.0f, 4.0f, 2.0f), 0.0f);
        EXPECT_FLOAT_EQ(ComputeContributionWeight(4.0f, 0.0f, 2.0f), 0.0f);
        EXPECT_FLOAT_EQ(ComputeContributionWeight(4.0f, 4.0f, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(ComputeContributionWeight(8.0f, 2.0f, 2.0f), 2.0f);
    }

    // The two modes differ by exactly one factor, and getting it wrong is a
    // uniform dimming by the candidate count — which reads as "the tier looks a
    // bit dark" rather than as an error.
    TEST(ReSTIRDIContract, FinalizeCombinedAppliesTheNormaliserTheBiasModeAsksFor)
    {
        const auto build = [](BiasMode mode)
        {
            Reservoir r{};
            r.Sample.Kind = LightSampleKind::Punctual;
            r.TargetPdf = 2.0f;
            r.WeightSum = 12.0f;
            FinalizeCombined(r, mode, 3.0f);
            return r;
        };

        const Reservoir biased = build(BiasMode::Biased);
        EXPECT_FLOAT_EQ(biased.M, 3.0f);
        // 12 / (3 * 2)
        EXPECT_FLOAT_EQ(biased.W, 2.0f);

        const Reservoir unbiased = build(BiasMode::UnbiasedMIS);
        EXPECT_FLOAT_EQ(unbiased.M, 3.0f);
        // 12 / (1 * 2): the MIS weights are already inside WeightSum.
        EXPECT_FLOAT_EQ(unbiased.W, 6.0f);
    }

    // THE IDENTITY THAT CATCHES A DROPPED CONFIDENCE WEIGHT.
    //
    // When the merged reservoirs share a domain — which is exactly the temporal
    // case, since the #976 validity test establishes that last frame's sample sits
    // on the SAME surface — the two bias modes must produce the SAME contribution
    // weight. They reach it by different routes:
    //
    //   1/M          w_i = M_i * pHat(y_i) * W_i,   normaliser = sum M
    //   balance      m_i = M_i / sum M  (pHat_j(y_i) is one function here),
    //                w_i = m_i * pHat(y_i) * W_i,   normaliser = 1
    //
    // so wSum_balance == wSum_biased / sum M and the two W agree exactly. Writing
    // m_i as 1 in the 1/M arm — which is what a first reading of "1/M
    // normalisation" suggests — breaks this identity by a factor of the average M.
    // That is a UNIFORM DARKENING of the resampled image, which reads as "the new
    // tier looks a bit dark" and gets fixed with an exposure tweak; it shipped
    // through a passing measure-identity test and a passing CPU/GPU agreement in
    // the first draft of #1140 and was caught by the oracle comparison, 8x dark
    // against dense quadrature. This test is the cheap headless version of that.
    TEST(ReSTIRDIContract, BothBiasModesAgreeOnReservoirsThatShareADomain)
    {
        // Two reservoirs, same surface, deliberately different M and W so a
        // dropped M cannot cancel out.
        const auto build = [](f32 m, f32 w, f32 targetPdf, u32 lightIndex)
        {
            Reservoir r{};
            r.Sample.Kind = LightSampleKind::EmissiveTriangle;
            r.Sample.LightIndex = lightIndex;
            r.Sample.Position = glm::vec3(static_cast<f32>(lightIndex), 1.0f, 2.0f);
            r.Sample.Normal = glm::vec3(0.0f, 0.0f, -1.0f);
            r.TargetPdf = targetPdf;
            r.M = m;
            r.W = w;
            return r;
        };
        const Reservoir a = build(4.0f, 0.75f, 3.0f, 1u);
        const Reservoir b = build(20.0f, 0.25f, 5.0f, 2u);
        const f32 summedM = a.M + b.M;

        // The same acceptance draw for both modes, so the SELECTION cannot differ
        // and any discrepancy is the arithmetic rather than which sample survived.
        for (const f32 xi : { 0.0f, 0.25f, 0.5f, 0.75f, 0.99f })
        {
            Reservoir biased{};
            ReservoirUpdate(biased, a.Sample, a.M * a.TargetPdf * a.W, a.TargetPdf, xi);
            ReservoirUpdate(biased, b.Sample, b.M * b.TargetPdf * b.W, b.TargetPdf, xi);
            FinalizeCombined(biased, BiasMode::Biased, summedM);

            Reservoir unbiased{};
            ReservoirUpdate(unbiased, a.Sample, (a.M / summedM) * a.TargetPdf * a.W, a.TargetPdf, xi);
            ReservoirUpdate(unbiased, b.Sample, (b.M / summedM) * b.TargetPdf * b.W, b.TargetPdf, xi);
            FinalizeCombined(unbiased, BiasMode::UnbiasedMIS, summedM);

            EXPECT_EQ(biased.Sample.LightIndex, unbiased.Sample.LightIndex) << "xi " << xi;
            ASSERT_GT(biased.W, 0.0f) << "xi " << xi;
            EXPECT_NEAR(unbiased.W, biased.W, biased.W * 1.0e-5f) << "xi " << xi;
        }
    }

    // The balance heuristic's defining property. Anything else and the estimator
    // either double-counts a sample or loses part of it.
    TEST(ReSTIRDIContract, BalanceHeuristicWeightsSumToOneOverTheCandidatesThatCanProduceASample)
    {
        constexpr u32 kCount = 4;
        // targetPdfMatrix[i * kCount + j] = pHat_i(y_j).
        const f32 targetPdfMatrix[kCount * kCount] = {
            // clang-format off
            2.0f, 0.0f, 1.0f, 4.0f,
            1.0f, 3.0f, 0.0f, 2.0f,
            0.0f, 5.0f, 2.0f, 1.0f,
            3.0f, 1.0f, 1.0f, 0.0f,
            // clang-format on
        };
        const f32 confidenceM[kCount] = { 1.0f, 4.0f, 20.0f, 7.0f };

        for (u32 owner = 0; owner < kCount; ++owner)
        {
            f32 total = 0.0f;
            for (u32 selected = 0; selected < kCount; ++selected)
                total += BalanceHeuristicMISWeight(selected, owner, targetPdfMatrix, confidenceM, kCount);
            EXPECT_NEAR(total, 1.0f, 1.0e-5f) << "sample owner " << owner;
        }
    }

    // No reservoir could have produced the sample: the answer is REJECT (0), and
    // emphatically not 1, which would credit the sample with a full weight it
    // has no support for.
    TEST(ReSTIRDIContract, BalanceHeuristicRejectsWhenNoCandidateSupportsTheSample)
    {
        constexpr u32 kCount = 2;
        const f32 zeroColumn[kCount * kCount] = { 0.0f, 1.0f, 0.0f, 1.0f };
        const f32 confidenceM[kCount] = { 3.0f, 5.0f };
        EXPECT_FLOAT_EQ(BalanceHeuristicMISWeight(0, 0, zeroColumn, confidenceM, kCount), 0.0f);
        EXPECT_FLOAT_EQ(BalanceHeuristicMISWeight(1, 0, zeroColumn, confidenceM, kCount), 0.0f);
        // Out-of-range indices and null tables reject rather than read.
        EXPECT_FLOAT_EQ(BalanceHeuristicMISWeight(9, 0, zeroColumn, confidenceM, kCount), 0.0f);
        EXPECT_FLOAT_EQ(BalanceHeuristicMISWeight(0, 0, nullptr, confidenceM, kCount), 0.0f);
        EXPECT_FLOAT_EQ(BalanceHeuristicMISWeight(0, 0, zeroColumn, nullptr, kCount), 0.0f);
    }

    // Capping M without rescaling WeightSum multiplies the pixel's radiance by
    // (cap / M) — it brightens exactly the pixels that have been stable longest,
    // which is the last place anyone would look for a brightness bug.
    TEST(ReSTIRDIContract, TemporalMCapLeavesTheContributionWeightUnchanged)
    {
        Reservoir r{};
        r.Sample.Kind = LightSampleKind::EmissiveTriangle;
        r.TargetPdf = 1.5f;
        r.WeightSum = 90.0f;
        r.M = 60.0f;
        FinalizeCombined(r, BiasMode::Biased, r.M);
        const f32 before = r.W;

        ApplyTemporalMCap(r, 20.0f);
        EXPECT_FLOAT_EQ(r.M, 20.0f);
        EXPECT_FLOAT_EQ(r.WeightSum, 30.0f);

        FinalizeCombined(r, BiasMode::Biased, r.M);
        EXPECT_NEAR(r.W, before, 1.0e-6f);

        // A cap at or above M is a no-op, not a rescale.
        Reservoir untouched = r;
        ApplyTemporalMCap(untouched, 1000.0f);
        EXPECT_EQ(untouched, r);
    }

    // -------------------------------------------------------------------------
    // The guards
    // -------------------------------------------------------------------------

    // A corrupt reservoir becomes EMPTY, never clamped. Clamping keeps M, and a
    // false M suppresses every future candidate at that pixel — so a transient
    // corruption would become permanent, and would spread through spatial reuse.
    TEST(ReSTIRDIContract, SanitizeEmptiesACorruptReservoirRatherThanClampingIt)
    {
        Reservoir good{};
        good.Sample.Kind = LightSampleKind::SphereArea;
        good.Sample.Position = glm::vec3(1.0f, 2.0f, 3.0f);
        good.Sample.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
        good.TargetPdf = 2.0f;
        good.WeightSum = 4.0f;
        good.M = 8.0f;
        good.W = 0.25f;
        EXPECT_EQ(SanitizeReservoir(good), good);

        const auto expectEmptied = [](Reservoir corrupt)
        {
            const Reservoir sanitized = SanitizeReservoir(corrupt);
            EXPECT_TRUE(sanitized.IsEmpty());
            EXPECT_FLOAT_EQ(sanitized.M, 0.0f);
            EXPECT_FLOAT_EQ(sanitized.WeightSum, 0.0f);
        };

        Reservoir nanWeight = good;
        nanWeight.WeightSum = std::numeric_limits<f32>::quiet_NaN();
        expectEmptied(nanWeight);

        Reservoir infPosition = good;
        infPosition.Sample.Position.y = std::numeric_limits<f32>::infinity();
        expectEmptied(infPosition);

        Reservoir negativeM = good;
        negativeM.M = -1.0f;
        expectEmptied(negativeM);

        Reservoir badKind = good;
        badKind.Sample.Kind = static_cast<LightSampleKind>(99u);
        expectEmptied(badKind);
    }

    // -------------------------------------------------------------------------
    // The tier's own vocabulary
    // -------------------------------------------------------------------------

    // A stood-down tier must always be able to say why. An unnamed reason is a
    // log line that reads "unknown", which is worse than no log line because it
    // looks like the code ran.
    TEST(ReSTIRDIContract, EveryFallbackReasonAndDebugViewHasAName)
    {
        for (u32 i = 0; i < std::to_underlying(ReSTIRDIFallbackReason::Count); ++i)
        {
            const auto reason = static_cast<ReSTIRDIFallbackReason>(i);
            EXPECT_NE(ToString(reason), "unknown") << "reason " << i;
            EXPECT_FALSE(ToString(reason).empty()) << "reason " << i;
        }
        for (u32 i = 0; i < std::to_underlying(ReSTIRDIDebugView::Count); ++i)
        {
            const auto view = static_cast<ReSTIRDIDebugView>(i);
            EXPECT_NE(ToString(view), "unknown") << "view " << i;
        }
        for (u32 i = 0; i < std::to_underlying(LightSampleKind::Count); ++i)
        {
            const auto kind = static_cast<LightSampleKind>(i);
            EXPECT_NE(ToString(kind), "unknown") << "kind " << i;
        }
        for (u32 i = 0; i < std::to_underlying(BiasMode::Count); ++i)
        {
            const auto mode = static_cast<BiasMode>(i);
            EXPECT_NE(ToString(mode), "unknown") << "mode " << i;
        }
        EXPECT_NE(ToString(DirectLightingTechnique::Clustered), "unknown");
        EXPECT_NE(ToString(DirectLightingTechnique::ReSTIRDI), "unknown");
    }

    // The selection order is the contract: every guard names something more
    // fundamental than the one below it, so the FIRST match is the root cause
    // rather than the first symptom. A reordering would report "too few lights"
    // on a machine with no ray tracing.
    TEST(ReSTIRDIContract, TechniqueSelectionReportsTheMostFundamentalReasonFirst)
    {
        const auto healthy = []
        {
            ReSTIRDITechniqueInputs inputs{};
            inputs.Requested = true;
            inputs.DeferredPathActive = true;
            inputs.ShadersReady = true;
            inputs.RayTracingAvailable = true;
            inputs.TlasReady = true;
            inputs.GPUSceneAvailable = true;
            inputs.TargetsAvailable = true;
            inputs.HistoryLayoutMatches = true;
            inputs.Engagement = { .CandidateLightCount = 1000, .CandidateBudget = 32 };
            inputs.EngagementMargin = 8;
            return inputs;
        };

        EXPECT_TRUE(SelectReSTIRDITechnique(healthy()).IsReSTIR());
        EXPECT_EQ(SelectReSTIRDITechnique(healthy()).Reason, ReSTIRDIFallbackReason::None);

        const auto reasonWhen = [&healthy](auto mutate)
        {
            ReSTIRDITechniqueInputs inputs = healthy();
            mutate(inputs);
            const auto decision = SelectReSTIRDITechnique(inputs);
            EXPECT_FALSE(decision.IsReSTIR());
            EXPECT_EQ(decision.Effective, DirectLightingTechnique::Clustered);
            return decision.Reason;
        };

        EXPECT_EQ(reasonWhen([](auto& i) { i.Requested = false; }), ReSTIRDIFallbackReason::NotRequested);
        EXPECT_EQ(reasonWhen([](auto& i) { i.DeferredPathActive = false; }),
                  ReSTIRDIFallbackReason::RenderingPathUnsupported);
        EXPECT_EQ(reasonWhen([](auto& i) { i.ShadersReady = false; }),
                  ReSTIRDIFallbackReason::ShaderUnavailable);
        EXPECT_EQ(reasonWhen([](auto& i) { i.RayTracingAvailable = false; }),
                  ReSTIRDIFallbackReason::RayTracingUnavailable);
        EXPECT_EQ(reasonWhen([](auto& i) { i.TlasReady = false; }),
                  ReSTIRDIFallbackReason::AccelerationStructureEmpty);
        EXPECT_EQ(reasonWhen([](auto& i) { i.GPUSceneAvailable = false; }),
                  ReSTIRDIFallbackReason::GPUSceneUnavailable);
        EXPECT_EQ(reasonWhen([](auto& i) { i.TargetsAvailable = false; }),
                  ReSTIRDIFallbackReason::TargetUnavailable);
        EXPECT_EQ(reasonWhen([](auto& i) { i.HistoryLayoutMatches = false; }),
                  ReSTIRDIFallbackReason::LayoutVersionMismatch);
        EXPECT_EQ(reasonWhen([](auto& i) { i.Engagement.CandidateLightCount = 33; }),
                  ReSTIRDIFallbackReason::BelowEngagementThreshold);

        // A missing DEVICE outranks a small light set: reporting "too few
        // lights" on a machine that cannot ray trace at all would send the user
        // to add lights.
        ReSTIRDITechniqueInputs both = healthy();
        both.RayTracingAvailable = false;
        both.Engagement.CandidateLightCount = 0;
        EXPECT_EQ(SelectReSTIRDITechnique(both).Reason, ReSTIRDIFallbackReason::RayTracingUnavailable);
    }

    // The criterion is a COUNT comparison with a hysteresis margin, deliberately
    // not a variance threshold — see ReSTIRDITechnique.h. A margin of zero would
    // make the tier toggle as one light drifts in and out of the cull.
    TEST(ReSTIRDIContract, EngagePredicateNeedsTheMarginAndNotJustAnExcess)
    {
        EXPECT_FALSE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 32, .CandidateBudget = 32 }, 1));
        EXPECT_FALSE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 20, .CandidateBudget = 32 }, 1));
        EXPECT_FALSE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 39, .CandidateBudget = 32 }, 8));
        EXPECT_TRUE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 40, .CandidateBudget = 32 }, 8));
        EXPECT_TRUE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 33, .CandidateBudget = 32 }, 1));
        // Margin 0 degenerates to a strict excess, which is what the field's
        // clamp allows and what the comment warns about.
        EXPECT_TRUE(ReSTIRDIEngagePredicate({ .CandidateLightCount = 33, .CandidateBudget = 32 }, 0));
    }

    // Values also arrive from a live edit, a script or an MCP write, and the
    // shader cannot be the backstop: a NaN in the M cap makes the GLSL clamp
    // undefined, and the poison then spreads through the reservoir history and
    // never washes out.
    TEST(ReSTIRDIContract, SanitizeSettingsHoldsEveryKnobToItsRange)
    {
        ReSTIRDISettings wild{};
        wild.InitialCandidates = 100000;
        wild.SpatialNeighbours = 100000;
        wild.SpatialPasses = 0;
        wild.TemporalMCap = std::numeric_limits<f32>::quiet_NaN();
        wild.SpatialRadiusPixels = -5.0f;
        wild.MaxRadianceClamp = std::numeric_limits<f32>::infinity();
        wild.RayOriginNormalBias = std::numeric_limits<f32>::quiet_NaN();
        wild.BiasMode = static_cast<ReSTIR::BiasMode>(77u);
        wild.DebugView = static_cast<ReSTIRDIDebugView>(77u);
        wild.EngagementMargin = 1u << 30;

        const ReSTIRDISettings clean = SanitizeReSTIRDISettings(wild);
        const ReSTIRDISettings defaults{};

        EXPECT_EQ(clean.InitialCandidates, kReSTIRDIMaxInitialCandidates);
        EXPECT_EQ(clean.SpatialNeighbours, kReSTIRDIMaxSpatialNeighbours);
        EXPECT_GE(clean.SpatialPasses, 1u);
        EXPECT_TRUE(std::isfinite(clean.TemporalMCap));
        EXPECT_FLOAT_EQ(clean.TemporalMCap, defaults.TemporalMCap);
        EXPECT_GE(clean.SpatialRadiusPixels, 1.0f);
        EXPECT_TRUE(std::isfinite(clean.MaxRadianceClamp));
        EXPECT_TRUE(std::isfinite(clean.RayOriginNormalBias));
        EXPECT_FLOAT_EQ(clean.RayOriginNormalBias, defaults.RayOriginNormalBias);
        EXPECT_EQ(clean.BiasMode, defaults.BiasMode);
        EXPECT_EQ(clean.DebugView, ReSTIRDIDebugView::Radiance);
        EXPECT_LE(clean.EngagementMargin, 4096u);

        // Sanitizing twice changes nothing: a sanitizer that was not idempotent
        // would make SettingsClamped fire every frame and the counter useless.
        EXPECT_EQ(SanitizeReSTIRDISettings(clean), clean);

        // The defaults must already be legal, or every scene load reports a clamp.
        EXPECT_EQ(SanitizeReSTIRDISettings(defaults), defaults);
    }

    // -------------------------------------------------------------------------
    // The GLSL twin's constants
    // -------------------------------------------------------------------------

    // A shader loop bound that exceeded the C++ clamp would silently trace less
    // than asked; a LAYOUT VERSION that disagreed would reinterpret last frame's
    // reservoirs at the wrong packing, which is a plausible wrong image rather
    // than a crash. Both are scanned out of the shader source rather than
    // trusted to a comment.
    TEST(ReSTIRDIContract, ShaderConstantsMatchTheCppOnes)
    {
        const auto paramsPath = ResolveShaderPath("include/ReSTIRDIParams.glsl");
        const auto reservoirPath = ResolveShaderPath("include/Reservoir.glsl");
        if (paramsPath.empty() || reservoirPath.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";

        const std::string params = ReadTextFile(paramsPath);
        const std::string reservoir = ReadTextFile(reservoirPath);
        ASSERT_FALSE(params.empty()) << paramsPath.string();
        ASSERT_FALSE(reservoir.empty()) << reservoirPath.string();

        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_MAX_INITIAL_CANDIDATES"), kReSTIRDIMaxInitialCandidates);
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS"), kReSTIRDIMaxSpatialNeighbours);
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_RESERVOIR_LAYOUT_VERSION"), kReservoirLayoutVersion);

        // The packed sample kinds. Their values live in a texture, so a
        // renumber on either side reads every reservoir as the wrong family.
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_NONE"),
                  std::to_underlying(LightSampleKind::None));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_PUNCTUAL"),
                  std::to_underlying(LightSampleKind::Punctual));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_DIRECTIONAL"),
                  std::to_underlying(LightSampleKind::Directional));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_SPHERE_AREA"),
                  std::to_underlying(LightSampleKind::SphereArea));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE"),
                  std::to_underlying(LightSampleKind::EmissiveTriangle));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_LIGHT_SAMPLE_KIND_COUNT"),
                  std::to_underlying(LightSampleKind::Count));

        EXPECT_EQ(ScanConstUint(reservoir, "OLO_RESTIR_BIAS_MODE_BIASED"),
                  std::to_underlying(BiasMode::Biased));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_RESTIR_BIAS_MODE_UNBIASED_MIS"),
                  std::to_underlying(BiasMode::UnbiasedMIS));

        // The debug views are read from the UBO as an integer, so the GLSL macro
        // and the enum must agree or the wrong AOV lands on screen.
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_RADIANCE"),
                  std::to_underlying(ReSTIRDIDebugView::Radiance));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_RAW_CANDIDATE"),
                  std::to_underlying(ReSTIRDIDebugView::RawCandidate));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_HISTORY_VALIDITY"),
                  std::to_underlying(ReSTIRDIDebugView::HistoryValidity));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_VARIANCE"),
                  std::to_underlying(ReSTIRDIDebugView::Variance));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_RESERVOIR_M"),
                  std::to_underlying(ReSTIRDIDebugView::ReservoirM));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_RESERVOIR_W"),
                  std::to_underlying(ReSTIRDIDebugView::ReservoirW));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_SAMPLE_KIND"),
                  std::to_underlying(ReSTIRDIDebugView::SampleKind));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_VIEW_BIAS_CLAMP"),
                  std::to_underlying(ReSTIRDIDebugView::BiasClampState));
    }

    // The shader's light-slot loop bound and the count the C++ side clamps to
    // must be the same number, or the criterion counts emitters the shader
    // cannot reach and the tier engages on a scene it cannot sample.
    TEST(ReSTIRDIContract, SharedLightSamplingSlotBoundMatchesTheMultiLightUBO)
    {
        const auto path = ResolveShaderPath("include/LightSampling.glsl");
        if (path.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";
        const std::string source = ReadTextFile(path);
        ASSERT_FALSE(source.empty()) << path.string();
        EXPECT_EQ(ScanDefine(source, "OLO_LIGHT_MAX_SLOTS"),
                  ShaderBindingLayout::MultiLightUBO::MAX_LIGHTS);
    }

    // The UBO the four draws share is uploaded as one blob, so a size drift is a
    // shader reading the wrong lanes — a wrong image, not a link error. The
    // static_assert in ShaderBindingLayout.h already pins the total; this pins
    // that the block the pass fills is the one the shader declares, by counting
    // the declared members.
    TEST(ReSTIRDIContract, ParamsBlockDeclaresTheLanesTheUBOCarries)
    {
        const auto path = ResolveShaderPath("include/ReSTIRDIParams.glsl");
        if (path.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";
        const std::string source = ReadTextFile(path);
        ASSERT_FALSE(source.empty()) << path.string();

        // 4 mat4 + 5 uvec4 + 3 vec4 = 256 + 80 + 48 = 384 bytes.
        EXPECT_EQ(sizeof(UBOStructures::ReSTIRDIUBO), 384u);

        const auto count = [&source](const char* type)
        {
            const std::regex pattern(std::string("\\b") + type + "\\s+u_[A-Za-z0-9_]+\\s*;");
            return static_cast<u32>(
                std::distance(std::sregex_iterator(source.begin(), source.end(), pattern), std::sregex_iterator()));
        };
        EXPECT_EQ(count("mat4"), 4u);
        EXPECT_EQ(count("uvec4"), 5u);
        EXPECT_EQ(count("vec4"), 3u);

        // The block must sit on UBO_RAY_TRACING: there is no free binding left
        // (UBO_TERRAIN_BRUSH 83 is the last, UBO_BINDING_LIMIT 84 is the GL 4.6
        // floor), so a change here is a silent collision rather than an error.
        EXPECT_NE(source.find("binding = " + std::to_string(ShaderBindingLayout::UBO_RAY_TRACING)),
                  std::string::npos);
    }

    // The history planes are keyed on the effect AND the plane, and the registry
    // hands back a texture only when the descriptor's LayoutVersion matches. A
    // plane that shared a key with another effect's would hand ReSTIR someone
    // else's texture, which is a plausible wrong image.
    TEST(ReSTIRDIContract, ReservoirHistoryPlanesAreDistinctKeys)
    {
        const TemporalHistoryKey sample{ .Effect = TemporalHistoryEffect::ReSTIRDI,
                                         .View = 0,
                                         .Resolution = TemporalHistoryResolution::Scene,
                                         .Plane = TemporalHistoryPlane::ReservoirSample };
        const TemporalHistoryKey radiance{ .Effect = TemporalHistoryEffect::ReSTIRDI,
                                           .View = 0,
                                           .Resolution = TemporalHistoryResolution::Scene,
                                           .Plane = TemporalHistoryPlane::ReservoirRadiance };
        const TemporalHistoryKey state{ .Effect = TemporalHistoryEffect::ReSTIRDI,
                                        .View = 0,
                                        .Resolution = TemporalHistoryResolution::Scene,
                                        .Plane = TemporalHistoryPlane::ReservoirState };
        EXPECT_FALSE(sample == radiance);
        EXPECT_FALSE(sample == state);
        EXPECT_FALSE(radiance == state);

        // And they are not the path tracer's or the shadow tier's planes.
        const TemporalHistoryKey pathTracer{ .Effect = TemporalHistoryEffect::PathTracer,
                                             .View = 0,
                                             .Resolution = TemporalHistoryResolution::Scene,
                                             .Plane = TemporalHistoryPlane::ReservoirSample };
        EXPECT_FALSE(sample == pathTracer);
    }
} // namespace OloEngine::Tests
