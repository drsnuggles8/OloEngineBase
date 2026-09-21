#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCoveragePropertyTests — issue #1246, acceptance criteria 1 and 3.
//
// THIS FILE IS THE COMPARISON. Criterion 1 asks for a bounded comparison of
// strand composition approaches on measured evidence — near silhouette, dense
// overlap, sub-pixel coverage, composition cost and memory — before an
// approach is selected. That comparison is done here, in arithmetic, against a
// 16x16-supersampled analytic reference, and the numbers it prints are the
// numbers in docs/analysis/groom-strand-visibility-1246.md.
//
// WHY IT IS A TEST AND NOT A SCRIPT. A measurement that lives in a PR body is
// defended by nothing: the first change to the width projection or the alpha
// model moves it, and nobody finds out. Here every claim the analysis makes is
// an assertion, so the decision's evidence fails loudly if it stops being
// true. The claims asserted are the DISCRIMINATING ones — each is a property
// that separates one approach from another, and each would be false if the
// approaches were interchangeable.
//
// WHAT IT CANNOT SEE. Whether the GPU agrees. The model here is faithful (see
// GroomCoverage.h) but it is still a model; GroomStrandVisualEvidenceTest and
// the live editor captures are what tie it to real pixels, and
// GroomStrandGpuParityTest is what pins the one piece of arithmetic — the
// stochastic hash — that both sides must compute identically.
// =============================================================================

#include <gtest/gtest.h>

#include "../../Groom/GroomStrandFixture.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Groom/GroomCoverage.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomCoverage;

namespace
{
    // The reference rate. 16x16 = 256 samples per pixel quantises coverage to
    // 1/256, which is four times finer than the finest mode under test (8x
    // alpha-to-coverage, 1/8), so the reference cannot be the thing that
    // limits any measurement below.
    constexpr u32 kSupersample = 16;

    // The engine's per-pixel-per-sample G-Buffer cost: the six colour
    // attachments of GBuffer::s_ColorAttachmentFormats (RGBA8 4, RGBA16F 8,
    // RGBA16F 8, RG16F 4, R32I 4, RGBA16F 8 = 36 B) plus a 4 B depth-stencil.
    // Passed into ModeExtraBytes rather than hard-coded there so this number
    // sits next to the table it is derived from.
    constexpr u32 kGBufferBytesPerSamplePerPixel = 40;

    struct CoverageCamera
    {
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
    };

    [[nodiscard]] CoverageCamera MakeCamera(const glm::vec3& eye, const glm::vec3& target, u32 width, u32 height,
                                            f32 fovYDegrees = 45.0f)
    {
        CoverageCamera camera;
        camera.View = glm::lookAt(eye, target, glm::vec3{ 0.0f, 1.0f, 0.0f });
        camera.Projection = glm::perspective(glm::radians(fovYDegrees),
                                             static_cast<f32>(width) / static_cast<f32>(height), 0.01f, 100.0f);
        return camera;
    }

    // One row of the comparison table.
    struct Row
    {
        std::string Mode;
        CoverageError Error;
        f64 TotalCoverageRatio = 0.0; // measured / reference silhouette area
        u64 ExtraBytes = 0;
    };

    void PrintHeader(const char* caseName, const ProjectionStats& projection, u32 width, u32 height,
                     const std::vector<f32>& reference)
    {
        std::printf("\n[groom-coverage] === %s === %ux%u, %u segments, half-width px min %.3f mean %.3f max %.3f, "
                    "reference area %.1f px over %u pixels\n",
                    caseName, width, height, projection.SegmentsProjected, static_cast<f64>(projection.MinHalfWidthPixels),
                    static_cast<f64>(projection.MeanHalfWidthPixels), static_cast<f64>(projection.MaxHalfWidthPixels),
                    TotalCoverage(reference), static_cast<u32>(reference.size()));
        std::printf("[groom-coverage] %-26s %10s %10s %10s %10s %8s %10s\n", "mode", "mean|e|", "rmse", "max|e|",
                    "bias", "area%", "extra KiB");
    }

    void PrintRow(const Row& row)
    {
        std::printf("[groom-coverage] %-26s %10.5f %10.5f %10.5f %+10.5f %8.2f %10.1f\n", row.Mode.c_str(),
                    row.Error.MeanAbsolute, row.Error.Rmse, row.Error.MaxAbsolute, row.Error.MeanSignedBias,
                    row.TotalCoverageRatio * 100.0, static_cast<f64>(row.ExtraBytes) / 1024.0);
    }

    void Record(const std::string& caseName, const Row& row)
    {
        const std::string prefix = caseName + "_" + row.Mode;
        ::testing::Test::RecordProperty(prefix + "_mean_abs", std::format("{:.6f}", row.Error.MeanAbsolute));
        ::testing::Test::RecordProperty(prefix + "_rmse", std::format("{:.6f}", row.Error.Rmse));
        ::testing::Test::RecordProperty(prefix + "_bias", std::format("{:.6f}", row.Error.MeanSignedBias));
        ::testing::Test::RecordProperty(prefix + "_area_ratio", std::format("{:.6f}", row.TotalCoverageRatio));
        ::testing::Test::RecordProperty(prefix + "_extra_bytes", std::to_string(row.ExtraBytes));
    }

    [[nodiscard]] Row Measure(const std::string& label, const std::vector<ScreenSegment>& segments, u32 width,
                              u32 height, GroomCompositionMode mode, const ModeParameters& parameters,
                              const std::vector<f32>& reference)
    {
        Row row;
        row.Mode = label;
        const std::vector<f32> measured = ModeCoverage(segments, width, height, mode, parameters);
        EXPECT_EQ(measured.size(), reference.size()) << label << ": ModeCoverage rejected its parameters";
        if (measured.size() != reference.size())
        {
            return row;
        }
        row.Error = CompareCoverage(measured, reference);
        const f64 referenceArea = TotalCoverage(reference);
        row.TotalCoverageRatio = referenceArea > 0.0 ? TotalCoverage(measured) / referenceArea : 0.0;
        row.ExtraBytes = ModeExtraBytes(mode, width, height, parameters.SampleCount, kGBufferBytesPerSamplePerPixel);
        return row;
    }

    // Everything one case needs, built once and handed to the assertions.
    struct CaseResult
    {
        ProjectionStats Projection;
        std::vector<ScreenSegment> Segments;
        std::vector<f32> Reference;
        Row Opaque;
        std::array<Row, 3> A2C; // 2, 4, 8 samples
        Row StochasticSingleFrame;
        Row StochasticConverged; // 8 frames, TAA's effective history
        Row Oit;
        TemporalStability OpaqueStability;
        TemporalStability StochasticStability;
    };

    [[nodiscard]] CaseResult RunCase(const char* caseName, const GroomAsset& groom, const glm::mat4& model,
                                     const CoverageCamera& camera, u32 width, u32 height)
    {
        CaseResult result;
        result.Projection =
            ProjectGroom(groom, model, camera.View, camera.Projection, width, height, 1.0f, result.Segments);
        result.Reference = ReferenceCoverage(result.Segments, width, height, kSupersample);

        // A case with nothing on screen would pass every relative assertion
        // below trivially, so the framing is checked before anything is
        // concluded from it.
        EXPECT_GT(result.Projection.SegmentsProjected, 0u) << caseName << ": nothing projected";
        EXPECT_EQ(result.Reference.size(), static_cast<sizet>(width) * height) << caseName << ": no reference";
        if (result.Reference.empty())
        {
            return result;
        }

        PrintHeader(caseName, result.Projection, width, height, result.Reference);

        ModeParameters parameters;
        result.Opaque = Measure("OpaqueRibbon", result.Segments, width, height, GroomCompositionMode::OpaqueRibbon,
                                parameters, result.Reference);
        PrintRow(result.Opaque);
        Record(caseName, result.Opaque);

        const std::array<u32, 3> sampleCounts{ 2u, 4u, 8u };
        for (sizet i = 0; i < sampleCounts.size(); ++i)
        {
            ModeParameters a2c = parameters;
            a2c.SampleCount = sampleCounts[i];
            result.A2C[i] = Measure(std::format("AlphaToCoverage x{}", sampleCounts[i]), result.Segments, width, height,
                                    GroomCompositionMode::AlphaToCoverage, a2c, result.Reference);
            PrintRow(result.A2C[i]);
            Record(caseName, result.A2C[i]);
        }

        ModeParameters single = parameters;
        single.TemporalFrames = 1;
        result.StochasticSingleFrame = Measure("StochasticAlpha 1f", result.Segments, width, height,
                                               GroomCompositionMode::StochasticAlpha, single, result.Reference);
        PrintRow(result.StochasticSingleFrame);
        Record(caseName, result.StochasticSingleFrame);

        ModeParameters converged = parameters;
        converged.TemporalFrames = 8;
        result.StochasticConverged = Measure("StochasticAlpha 8f", result.Segments, width, height,
                                             GroomCompositionMode::StochasticAlpha, converged, result.Reference);
        PrintRow(result.StochasticConverged);
        Record(caseName, result.StochasticConverged);

        result.Oit = Measure("WeightedBlendedOIT", result.Segments, width, height,
                             GroomCompositionMode::WeightedBlendedOIT, parameters, result.Reference);
        PrintRow(result.Oit);
        Record(caseName, result.Oit);

        result.OpaqueStability = MeasureTemporalStability(result.Segments, width, height,
                                                          GroomCompositionMode::OpaqueRibbon, parameters, 8,
                                                          result.Reference);
        result.StochasticStability = MeasureTemporalStability(result.Segments, width, height,
                                                              GroomCompositionMode::StochasticAlpha, parameters, 8,
                                                              result.Reference);
        std::printf("[groom-coverage] %-26s frame-to-frame delta %.5f (opaque) vs %.5f (stochastic); "
                    "stochastic converges to mean|e| %.5f\n",
                    "temporal", result.OpaqueStability.MeanFrameToFrameDelta,
                    result.StochasticStability.MeanFrameToFrameDelta,
                    result.StochasticStability.ConvergedMeanAbsolute);
        ::testing::Test::RecordProperty(std::string(caseName) + "_stochastic_frame_delta",
                                        std::format("{:.6f}", result.StochasticStability.MeanFrameToFrameDelta));
        ::testing::Test::RecordProperty(std::string(caseName) + "_opaque_frame_delta",
                                        std::format("{:.6f}", result.OpaqueStability.MeanFrameToFrameDelta));
        return result;
    }
} // namespace

// =============================================================================
// Criterion 1 — the comparison, one case per axis the criterion names.
// =============================================================================

// NEAR SILHOUETTE. A scalp filling the frame at 30 cm — about as close as a
// camera gets to a head without being inside it.
//
// The measured half-widths here are the reason this file exists: even at this
// framing a 70 um hair projects to roughly 0.02 px of half width, so "near
// silhouette" for real hair is still deeply sub-pixel. That is a fact about
// hair and optics rather than about this engine, and it is what makes a mode's
// behaviour BELOW one pixel the whole question instead of an edge case.
TEST(GroomCoverageComparison, NearSilhouette)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(2500u, 8u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 480;
    constexpr u32 kHeight = 270;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.02f, 0.30f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);
    const CaseResult result = RunCase("NearSilhouette", *coat.Groom, glm::mat4(1.0f), camera, kWidth, kHeight);
    ASSERT_FALSE(result.Reference.empty());
    ASSERT_GT(result.Opaque.Error.ReferenceCoveredPixels, 1000u) << "the scalp barely covers the frame";

    // Near the silhouette a strand is around a pixel wide, so the widened
    // alpha is close to 1 and every mode is close to the truth. The claim
    // recorded for this axis is that NO mode is grossly wrong here — which is
    // what makes the sub-pixel and overlap cases, where they diverge, the
    // deciding ones rather than a general statement about strand rendering.
    for (const Row* row : { &result.Opaque, &result.A2C[1], &result.StochasticConverged, &result.Oit })
    {
        EXPECT_LT(row->Error.MeanAbsolute, 0.5) << row->Mode << " is grossly wrong even at the near silhouette";
    }

    // And the one thing that is already visible here: the hard cutoff is the
    // only mode whose silhouette area is not close to the truth, because a
    // strand straddling the half-pixel threshold is either fully kept or fully
    // dropped.
    EXPECT_GT(std::abs(result.Opaque.TotalCoverageRatio - 1.0), std::abs(result.Oit.TotalCoverageRatio - 1.0))
        << "the hard cutoff reproduced the silhouette area as faithfully as WB-OIT did";
}

// DENSE OVERLAP. A pelt at 30 cm: short strands, many of them behind each
// other in the same pixel. This is the case that separates the modes, and it
// is where alpha-to-coverage's DETERMINISTIC sample mask shows: two fragments
// at the same alpha claim the same samples, so the pixel cannot reach the
// union's coverage however many strands land in it.
TEST(GroomCoverageComparison, DenseOverlapExposesAlphaToCoverageSaturation)
{
    const auto coat = Tests::GroomStrandFixture::MakePelt(8000u, 4u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 480;
    constexpr u32 kHeight = 270;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 0.3f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);
    const CaseResult result = RunCase("DenseOverlap", *coat.Groom, glm::mat4(1.0f), camera, kWidth, kHeight);
    ASSERT_FALSE(result.Reference.empty());

    // The discriminating claim: under overlap, alpha-to-coverage LOSES
    // coverage relative to the truth, and does so systematically rather than
    // noisily — a negative signed bias, not merely a large absolute error.
    // A decorrelated (dithered) mask would not do this, which is exactly why
    // GroomCoverage models the hardware's mask and not a convenient one.
    EXPECT_LT(result.A2C[1].Error.MeanSignedBias, 0.0)
        << "alpha-to-coverage did not under-cover under dense overlap; either the case stopped overlapping "
           "or AlphaToCoverageMask stopped modelling the hardware's deterministic mask";

    // Weighted-blended OIT computes 1 - prod(1 - alpha), which IS the union of
    // independent coverages, so it should be the most accurate mode here. That
    // is the finding that makes its rejection interesting: it wins on coverage
    // and is still not selectable, because it writes no depth.
    EXPECT_LT(result.Oit.Error.MeanAbsolute, result.A2C[1].Error.MeanAbsolute)
        << "WB-OIT was expected to beat 4x alpha-to-coverage on coverage accuracy under overlap";
    EXPECT_FALSE(GroomModeWritesDepth(GroomCompositionMode::WeightedBlendedOIT))
        << "if WB-OIT ever writes depth, the rejection recorded in the analysis has to be revisited";
}

// SUB-PIXEL. The same scalp at 2.5 m, where almost every strand is a fraction
// of a pixel wide. This is the axis the whole feature exists for, and the one
// where a hard cutoff is unambiguously wrong.
TEST(GroomCoverageComparison, SubPixelStrandsDefeatTheHardCutoff)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(2500u, 8u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 480;
    constexpr u32 kHeight = 270;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 2.5f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);
    const CaseResult result = RunCase("SubPixel", *coat.Groom, glm::mat4(1.0f), camera, kWidth, kHeight);
    ASSERT_FALSE(result.Reference.empty());

    // The strands really are sub-pixel — otherwise the rest of this case is
    // measuring something else.
    EXPECT_LT(result.Projection.MeanHalfWidthPixels, 0.5f)
        << "the sub-pixel case is no longer sub-pixel; the camera or the strand widths moved";

    // Every mode that keeps the coverage fraction beats the one that throws it
    // away. This is criterion 1's whole question, reduced to four comparisons.
    EXPECT_LT(result.StochasticConverged.Error.MeanAbsolute, result.Opaque.Error.MeanAbsolute)
        << "temporally converged stochastic alpha did not beat a hard cutoff on sub-pixel strands";
    EXPECT_LT(result.Oit.Error.MeanAbsolute, result.Opaque.Error.MeanAbsolute)
        << "WB-OIT did not beat a hard cutoff on sub-pixel strands";

    // ALPHA-TO-COVERAGE DOES NOT, and this is the measurement's most
    // consequential finding. It is asserted as an EQUALITY because that is what
    // it is: the sample mask is round(alpha * S) bits, so a fragment whose
    // alpha is below 1/(2S) claims ZERO samples and the mode degenerates
    // exactly into a hard cutoff — with a multisample target's memory on top.
    // At 8 samples that cliff is alpha 0.0625, i.e. a projected strand
    // half-width of 0.031 px, and a 70 um hair is under it at every framing a
    // person would actually use (the half-widths printed above are ~0.003 px).
    // Raising the sample count moves the cliff; it does not remove it.
    EXPECT_DOUBLE_EQ(result.A2C[2].Error.MeanAbsolute, result.Opaque.Error.MeanAbsolute)
        << "8x alpha-to-coverage no longer collapses onto the hard cutoff here. If AlphaToCoverageMask "
           "gained a dither that is expected, and the analysis doc's rejection of alpha-to-coverage has "
           "to be revisited rather than this assertion relaxed";
    EXPECT_DOUBLE_EQ(result.A2C[2].TotalCoverageRatio, 0.0)
        << "8x alpha-to-coverage recovered silhouette area on strands this thin";

    // And no sample count rescues it HERE: 2, 4 and 8 land on the same number
    // because all three round the same alpha to zero. The monotonic-in-sample-
    // count behaviour that does exist shows up in the near case, where the
    // alphas sit above the cliff.
    EXPECT_DOUBLE_EQ(result.A2C[0].Error.MeanAbsolute, result.A2C[2].Error.MeanAbsolute)
        << "the sample count began to matter on strands below the 1/(2S) cliff";

    // A hard cutoff does not merely err — it DROPS the coat. The signed bias
    // is the statistic that says so, and it is why the failure reads as
    // thinning hair rather than as aliasing.
    EXPECT_LT(result.Opaque.Error.MeanSignedBias, 0.0)
        << "the hard cutoff was expected to lose sub-pixel coverage, not gain it";
}

// =============================================================================
// Criterion 3 — stability, resolution and the temporal claim.
// =============================================================================

// A deterministic mode's frame-to-frame delta on a static camera is exactly
// zero and a stochastic one's is not. Asserted because it is the precise
// statement of what a temporal resolve can and cannot fix: it removes the
// second kind of error and cannot touch the first.
TEST(GroomCoverageStability, StochasticNoiseIsTemporalAndCutoffErrorIsNot)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(1500u, 8u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 320;
    constexpr u32 kHeight = 180;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 1.2f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);

    std::vector<ScreenSegment> segments;
    const ProjectionStats projection =
        ProjectGroom(*coat.Groom, glm::mat4(1.0f), camera.View, camera.Projection, kWidth, kHeight, 1.0f, segments);
    ASSERT_GT(projection.SegmentsProjected, 0u);
    const std::vector<f32> reference = ReferenceCoverage(segments, kWidth, kHeight, kSupersample);
    ASSERT_FALSE(reference.empty());

    const ModeParameters parameters;
    const TemporalStability opaque = MeasureTemporalStability(segments, kWidth, kHeight,
                                                              GroomCompositionMode::OpaqueRibbon, parameters, 8,
                                                              reference);
    const TemporalStability stochastic = MeasureTemporalStability(segments, kWidth, kHeight,
                                                                  GroomCompositionMode::StochasticAlpha, parameters, 8,
                                                                  reference);

    EXPECT_EQ(opaque.MeanFrameToFrameDelta, 0.0)
        << "a deterministic mode moved between frames of a static camera";
    EXPECT_GT(stochastic.MeanFrameToFrameDelta, 0.0)
        << "the stochastic mode produced the same frame twice; the hash is not reseeding per frame";

    // The payoff, and the reason the mode is viable at all: averaging the
    // frames a temporal resolve would average brings the error below the
    // single frame's.
    const CoverageError singleFrame =
        CompareCoverage(ModeCoverage(segments, kWidth, kHeight, GroomCompositionMode::StochasticAlpha, parameters),
                        reference);
    EXPECT_LT(stochastic.ConvergedMeanAbsolute, singleFrame.MeanAbsolute)
        << "averaging eight stochastic frames did not reduce the coverage error";

    std::printf("[groom-coverage] stability: opaque delta %.6f, stochastic delta %.6f, "
                "stochastic 1f mean|e| %.6f -> 8f %.6f\n",
                opaque.MeanFrameToFrameDelta, stochastic.MeanFrameToFrameDelta, singleFrame.MeanAbsolute,
                stochastic.ConvergedMeanAbsolute);
}

// Criterion 3 names varying resolution explicitly. The property that must hold
// is that the silhouette AREA per unit of screen is stable — a coat that loses
// a third of itself at 1080p and keeps it at 4K is the crawl the criterion is
// about, and it is invisible in any single-resolution measurement.
TEST(GroomCoverageStability, SilhouetteAreaFractionIsStableAcrossResolutions)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(2000u, 8u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    struct Resolution
    {
        u32 Width;
        u32 Height;
        const char* Name;
    };
    constexpr std::array<Resolution, 3> kResolutions{ { { 240u, 135u, "low" },
                                                        { 480u, 270u, "native" },
                                                        { 960u, 540u, "high" } } };

    std::array<f64, kResolutions.size()> opaqueFraction{};
    std::array<f64, kResolutions.size()> convergedFraction{};

    for (sizet i = 0; i < kResolutions.size(); ++i)
    {
        const Resolution& resolution = kResolutions[i];
        const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, resolution.Width,
                                                 resolution.Height);
        std::vector<ScreenSegment> segments;
        const ProjectionStats projection = ProjectGroom(*coat.Groom, glm::mat4(1.0f), camera.View, camera.Projection,
                                                        resolution.Width, resolution.Height, 1.0f, segments);
        ASSERT_GT(projection.SegmentsProjected, 0u);

        const std::vector<f32> reference =
            ReferenceCoverage(segments, resolution.Width, resolution.Height, kSupersample);
        ASSERT_FALSE(reference.empty());
        const f64 referenceArea = TotalCoverage(reference);
        ASSERT_GT(referenceArea, 0.0);

        ModeParameters parameters;
        const f64 opaqueArea = TotalCoverage(
            ModeCoverage(segments, resolution.Width, resolution.Height, GroomCompositionMode::OpaqueRibbon,
                         parameters));
        parameters.TemporalFrames = 8;
        const f64 convergedArea = TotalCoverage(
            ModeCoverage(segments, resolution.Width, resolution.Height, GroomCompositionMode::StochasticAlpha,
                         parameters));

        opaqueFraction[i] = opaqueArea / referenceArea;
        convergedFraction[i] = convergedArea / referenceArea;

        std::printf("[groom-coverage] resolution %-7s %4ux%-4u mean half-width %.3f px  opaque area %.1f%%  "
                    "stochastic-8f area %.1f%%\n",
                    resolution.Name, resolution.Width, resolution.Height,
                    static_cast<f64>(projection.MeanHalfWidthPixels), opaqueFraction[i] * 100.0,
                    convergedFraction[i] * 100.0);
        ::testing::Test::RecordProperty(std::string("area_opaque_") + resolution.Name,
                                        std::format("{:.6f}", opaqueFraction[i]));
        ::testing::Test::RecordProperty(std::string("area_stochastic_") + resolution.Name,
                                        std::format("{:.6f}", convergedFraction[i]));
    }

    // The stochastic mode's area fraction is near 1 at every resolution
    // because its expectation IS the coverage. The tolerance is loose on
    // purpose: this asserts the absence of a systematic loss, not a precision.
    for (sizet i = 0; i < kResolutions.size(); ++i)
    {
        EXPECT_NEAR(convergedFraction[i], 1.0, 0.15)
            << "stochastic coverage lost or invented silhouette area at " << kResolutions[i].Name;
    }

    // The hard cutoff, by contrast, reproduces none of the coat at any of the
    // three resolutions — and it is worth being precise about what that does
    // and does not say. The cutoff IS resolution-stable here: stably at zero.
    // So the property asserted is ACCURACY, not spread. A spread comparison
    // would score "loses the whole coat, consistently" as the better result,
    // which is how a measurement ends up flattering the mode it was built to
    // reject.
    for (sizet i = 0; i < kResolutions.size(); ++i)
    {
        EXPECT_GT(std::abs(opaqueFraction[i] - 1.0), std::abs(convergedFraction[i] - 1.0))
            << "the hard cutoff reproduced the silhouette area better than stochastic coverage at "
            << kResolutions[i].Name;
    }

    const f64 opaqueSpread = *std::max_element(opaqueFraction.begin(), opaqueFraction.end()) - *std::min_element(opaqueFraction.begin(), opaqueFraction.end());
    const f64 convergedSpread = *std::max_element(convergedFraction.begin(), convergedFraction.end()) - *std::min_element(convergedFraction.begin(), convergedFraction.end());
    std::printf("[groom-coverage] area-fraction spread across resolutions: opaque %.4f, stochastic %.4f\n",
                opaqueSpread, convergedSpread);
}

// =============================================================================
// The model's own contracts — the things that, if wrong, make every number
// above meaningless.
// =============================================================================

TEST(GroomCoverageModel, ReferenceCoverageIsBoundedAndRejectsATooCoarseRate)
{
    std::vector<ScreenSegment> segments;
    ScreenSegment segment;
    segment.A = { 4.0f, 8.0f };
    segment.B = { 28.0f, 8.0f };
    segment.HalfWidthA = 2.0f;
    segment.HalfWidthB = 2.0f;
    segments.push_back(segment);

    const std::vector<f32> coverage = ReferenceCoverage(segments, 32, 16, kSupersample);
    ASSERT_EQ(coverage.size(), 32u * 16u);
    for (const f32 value : coverage)
    {
        EXPECT_GE(value, 0.0f);
        EXPECT_LE(value, 1.0f);
    }
    // A 24 x 4 px band with SQUARE ends is exactly 96 px of area, and the
    // number being exact rather than approximate is the point: it is what
    // distinguishes the band the GPU draws from the round-capped capsule that
    // would read 96 + pi*4 ~= 108.6 here.
    EXPECT_NEAR(TotalCoverage(coverage), 96.0, 0.6);

    // A rate that is not meaningfully finer than the modes it judges is
    // refused rather than silently used.
    EXPECT_TRUE(ReferenceCoverage(segments, 32, 16, 2).empty());
}

TEST(GroomCoverageModel, StochasticHashIsUniformAndDecorrelatesItsInputs)
{
    // Uniformity, over a grid rather than a single sequence: the hash is
    // indexed by pixel, so a construction that is uniform in one input and
    // degenerate in another would pass a one-dimensional check.
    constexpr u32 kBuckets = 16;
    std::array<u32, kBuckets> histogram{};
    u32 total = 0;
    for (u32 y = 0; y < 64; ++y)
    {
        for (u32 x = 0; x < 64; ++x)
        {
            const f32 value = StochasticHash(x, y, 0u, 7u, 1246u);
            ASSERT_GE(value, 0.0f);
            ASSERT_LT(value, 1.0f);
            histogram[static_cast<sizet>(value * static_cast<f32>(kBuckets))] += 1u;
            ++total;
        }
    }
    const f64 expected = static_cast<f64>(total) / static_cast<f64>(kBuckets);
    for (const u32 count : histogram)
    {
        EXPECT_NEAR(static_cast<f64>(count), expected, expected * 0.25)
            << "the stochastic hash is not uniform over a pixel grid";
    }

    // Each input must move the result on its own. A hash that ignored the
    // frame index would make the stochastic mode deterministic — which is the
    // failure that looks exactly like "TAA is not helping".
    EXPECT_NE(StochasticHash(3u, 5u, 0u, 1u, 1246u), StochasticHash(3u, 5u, 1u, 1u, 1246u)) << "frame index ignored";
    EXPECT_NE(StochasticHash(3u, 5u, 0u, 1u, 1246u), StochasticHash(3u, 5u, 0u, 2u, 1246u)) << "segment id ignored";
    EXPECT_NE(StochasticHash(3u, 5u, 0u, 1u, 1246u), StochasticHash(4u, 5u, 0u, 1u, 1246u)) << "pixel x ignored";
    EXPECT_NE(StochasticHash(3u, 5u, 0u, 1u, 1246u), StochasticHash(3u, 6u, 0u, 1u, 1246u)) << "pixel y ignored";
    EXPECT_NE(StochasticHash(3u, 5u, 0u, 1u, 1246u), StochasticHash(3u, 5u, 0u, 1u, 99u)) << "seed ignored";
}

TEST(GroomCoverageModel, AlphaToCoverageMaskQuantisesToTheSampleCount)
{
    for (const u32 samples : { 2u, 4u, 8u })
    {
        EXPECT_EQ(AlphaToCoverageMask(0.0f, samples), 0u);
        EXPECT_EQ(std::popcount(AlphaToCoverageMask(1.0f, samples)), static_cast<int>(samples));
        // Half alpha takes half the samples, rounded.
        EXPECT_EQ(std::popcount(AlphaToCoverageMask(0.5f, samples)), static_cast<int>(samples / 2u));
        // The defining property: the mask is a function of alpha ALONE, so two
        // overlapping fragments at the same alpha claim identical samples. The
        // dense-overlap case above measures what that costs.
        EXPECT_EQ(AlphaToCoverageMask(0.37f, samples), AlphaToCoverageMask(0.37f, samples));
    }
    // Out of range and non-finite alphas produce an empty mask rather than
    // undefined behaviour or a full one.
    EXPECT_EQ(AlphaToCoverageMask(std::numeric_limits<f32>::quiet_NaN(), 4u), 0u);
    EXPECT_EQ(std::popcount(AlphaToCoverageMask(2.0f, 4u)), 4);
    EXPECT_EQ(AlphaToCoverageMask(-1.0f, 4u), 0u);
}

TEST(GroomCoverageModel, ProjectionDropsRatherThanClampsWhatItCannotSee)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(200u, 4u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 128;
    constexpr u32 kHeight = 128;
    // The camera is INSIDE the scalp looking out, so a large share of the
    // groom is behind the near plane.
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, kWidth, kHeight);

    std::vector<ScreenSegment> segments;
    const ProjectionStats stats =
        ProjectGroom(*coat.Groom, glm::mat4(1.0f), camera.View, camera.Projection, kWidth, kHeight, 1.0f, segments);

    EXPECT_GT(stats.SegmentsDroppedBehindCamera, 0u) << "nothing was behind the camera; the case is not testing drops";
    EXPECT_EQ(stats.SegmentsDroppedNonFinite, 0u) << "a well-formed groom produced non-finite projected points";
    EXPECT_EQ(segments.size(), stats.SegmentsProjected) << "the segment list and the counter disagree";

    // Every emitted segment is usable. A clamped-in segment would show up here
    // as a coordinate far outside the viewport or a negative half width.
    for (const auto& segment : segments)
    {
        EXPECT_TRUE(std::isfinite(segment.A.x) && std::isfinite(segment.A.y));
        EXPECT_GE(segment.HalfWidthA, 0.0f);
        EXPECT_GE(segment.HalfWidthB, 0.0f);
    }
}

TEST(GroomCoverageModel, EveryModeIsIndependentOfSubmissionOrder)
{
    const auto coat = Tests::GroomStrandFixture::MakePelt(800u, 4u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 200;
    constexpr u32 kHeight = 200;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 0.35f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);

    std::vector<ScreenSegment> segments;
    ASSERT_GT(ProjectGroom(*coat.Groom, glm::mat4(1.0f), camera.View, camera.Projection, kWidth, kHeight, 1.0f,
                           segments)
                  .SegmentsProjected,
              0u);

    std::vector<ScreenSegment> reversed(segments.rbegin(), segments.rend());

    for (const auto mode : { GroomCompositionMode::OpaqueRibbon, GroomCompositionMode::StochasticAlpha,
                             GroomCompositionMode::AlphaToCoverage, GroomCompositionMode::WeightedBlendedOIT })
    {
        ASSERT_TRUE(GroomModeIsOrderIndependent(mode));
        const ModeParameters parameters;
        const std::vector<f32> forward = ModeCoverage(segments, kWidth, kHeight, mode, parameters);
        const std::vector<f32> backward = ModeCoverage(reversed, kWidth, kHeight, mode, parameters);
        ASSERT_EQ(forward.size(), backward.size());
        for (sizet i = 0; i < forward.size(); ++i)
        {
            // Exact for the three integer-accumulating modes. WB-OIT
            // multiplies floats, so its product can differ in the last bit
            // between two orderings — which is the honest bound on what
            // "order-independent" means for it.
            EXPECT_NEAR(forward[i], backward[i], 1.0e-5f) << ToString(mode) << " at pixel " << i;
        }
    }
}

// The CPU model and the GPU mesh must label the SAME segment with the SAME
// identity, or the stochastic mode's measured numbers describe a different
// sample set than the one the shader draws.
//
// This is asserted across the two producers rather than each against the
// shared helper, because that is exactly the gap the bug lived in: both called
// GroomSegmentIdentity correctly and passed it different indices. ProjectGroom
// walks segments by their END point (its loop starts at 1) and
// BuildGroomStrandMesh by their START point (its loop starts at 0), so for a
// while segment k was (curve, k+1) on one side and (curve, k) on the other.
TEST(GroomCoverageModel, SegmentIdentityAgreesWithTheGpuStrandMesh)
{
    const auto coat = Tests::GroomStrandFixture::MakeScalp(64u, 6u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    constexpr u32 kWidth = 256;
    constexpr u32 kHeight = 256;
    const CoverageCamera camera = MakeCamera({ 0.0f, 0.0f, 0.6f }, { 0.0f, 0.0f, 0.0f }, kWidth, kHeight);

    std::vector<ScreenSegment> segments;
    const ProjectionStats projection = ProjectGroom(*coat.Groom, glm::mat4(1.0f), camera.View, camera.Projection,
                                                    kWidth, kHeight, 1.0f, segments);
    ASSERT_GT(projection.SegmentsProjected, 0u);
    ASSERT_EQ(projection.SegmentsDroppedBehindCamera, 0u)
        << "a dropped segment would desynchronise the two orderings and make this comparison meaningless";

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats mesh = BuildGroomStrandMesh(*coat.Groom, {}, vertices, indices);
    ASSERT_EQ(mesh.SegmentCount, segments.size())
        << "the two producers did not even emit the same number of segments";

    // Both walk the groom in curve order, so segment i on one side is segment
    // i on the other.
    for (sizet i = 0; i < segments.size(); ++i)
    {
        const u32 meshId = std::bit_cast<u32>(vertices[i * 4u].SegmentId);
        EXPECT_EQ(segments[i].Id, meshId) << "segment " << i;
    }
}

// Coverage in the LAST pixel column and row must survive. The off-screen
// rejection compares against width/height, not against the last pixel INDEX:
// pixel (width - 1) covers [width - 1, width), so a segment starting at
// width - 0.5 is inside it. Testing against the index discarded that segment,
// and with it real coverage along the right and bottom edges of every
// measurement in this file.
TEST(GroomCoverageModel, CoverageAtTheRightAndBottomEdgesIsNotDiscarded)
{
    constexpr u32 kWidth = 32;
    constexpr u32 kHeight = 16;

    // A fat vertical bar whose left edge sits inside the LAST column, and a
    // horizontal one inside the last row.
    std::vector<ScreenSegment> segments;
    ScreenSegment right;
    right.A = { static_cast<f32>(kWidth) - 0.25f, 2.0f };
    right.B = { static_cast<f32>(kWidth) - 0.25f, 12.0f };
    right.HalfWidthA = 0.6f;
    right.HalfWidthB = 0.6f;
    segments.push_back(right);

    ScreenSegment bottom;
    bottom.A = { 4.0f, static_cast<f32>(kHeight) - 0.25f };
    bottom.B = { 20.0f, static_cast<f32>(kHeight) - 0.25f };
    bottom.HalfWidthA = 0.6f;
    bottom.HalfWidthB = 0.6f;
    segments.push_back(bottom);

    const std::vector<f32> reference = ReferenceCoverage(segments, kWidth, kHeight, kSupersample);
    ASSERT_EQ(reference.size(), static_cast<sizet>(kWidth) * kHeight);
    EXPECT_GT(TotalCoverage(reference), 0.0)
        << "a segment inside the last column/row produced no coverage at all";

    // Specifically in the last column and the last row, not merely somewhere.
    f64 lastColumn = 0.0;
    for (u32 y = 0; y < kHeight; ++y)
    {
        lastColumn += static_cast<f64>(reference[static_cast<sizet>(y) * kWidth + (kWidth - 1u)]);
    }
    f64 lastRow = 0.0;
    for (u32 x = 0; x < kWidth; ++x)
    {
        lastRow += static_cast<f64>(reference[static_cast<sizet>(kHeight - 1u) * kWidth + x]);
    }
    EXPECT_GT(lastColumn, 0.0) << "the right-hand column lost its coverage";
    EXPECT_GT(lastRow, 0.0) << "the bottom row lost its coverage";

    // A segment genuinely off the edge still contributes nothing — the
    // rejection must not have been widened into never rejecting.
    std::vector<ScreenSegment> offscreen;
    ScreenSegment away = right;
    away.A.x = static_cast<f32>(kWidth) + 8.0f;
    away.B.x = static_cast<f32>(kWidth) + 8.0f;
    offscreen.push_back(away);
    EXPECT_DOUBLE_EQ(TotalCoverage(ReferenceCoverage(offscreen, kWidth, kHeight, kSupersample)), 0.0)
        << "a segment well past the right edge contributed coverage";
}
// The strand cache keys on (asset handle, build settings). Two properties must
// hold, and the second is the one that bit:
//
//   * different settings must produce different keys, or one entity hands
//     another geometry built for a different budget;
//   * IDENTICAL settings must produce IDENTICAL keys, every time, for objects
//     constructed independently. GroomStrandBuildSettings is 9 bytes of members
//     in 12, and the default member initializers do not touch the padding — so
//     a key hashed over the OBJECT REPRESENTATION could differ between two
//     logically equal settings, miss the cache, rebuild the mesh and leave a
//     duplicate set of GPU buffers behind.
TEST(GroomStrandCacheKey, SeparatesDifferentSettingsAndIgnoresPadding)
{
    const auto makeRequest = [](AssetHandle handle, u32 maxStrands, u32 maxSegments, bool guidesOnly)
    {
        GroomStrandRequest request;
        request.Handle = handle;
        request.Build.MaxStrands = maxStrands;
        request.Build.MaxSegments = maxSegments;
        request.Build.GuidesOnly = guidesOnly;
        return request;
    };

    const u64 base = GroomRenderPass::CacheKey(makeRequest(7u, 1000u, 50000u, false));

    // Every field separates.
    EXPECT_NE(base, GroomRenderPass::CacheKey(makeRequest(8u, 1000u, 50000u, false))) << "handle";
    EXPECT_NE(base, GroomRenderPass::CacheKey(makeRequest(7u, 1001u, 50000u, false))) << "MaxStrands";
    EXPECT_NE(base, GroomRenderPass::CacheKey(makeRequest(7u, 1000u, 50001u, false))) << "MaxSegments";
    EXPECT_NE(base, GroomRenderPass::CacheKey(makeRequest(7u, 1000u, 50000u, true))) << "GuidesOnly";

    // And identical settings agree, across independently constructed objects
    // whose padding bytes are whatever the stack happened to hold. Buffers of
    // deliberately dirtied storage make that concrete rather than hoping the
    // stack differs.
    for (const u8 fill : { u8{ 0x00 }, u8{ 0xCD }, u8{ 0xFF } })
    {
        alignas(GroomStrandRequest) std::array<u8, sizeof(GroomStrandRequest)> storage{};
        storage.fill(fill);
        auto* dirty = new (storage.data()) GroomStrandRequest();
        dirty->Handle = 7u;
        dirty->Build.MaxStrands = 1000u;
        dirty->Build.MaxSegments = 50000u;
        dirty->Build.GuidesOnly = false;
        EXPECT_EQ(GroomRenderPass::CacheKey(*dirty), base)
            << "the key changed with the padding bytes (fill 0x" << std::hex << static_cast<int>(fill) << ")";
        std::destroy_at(dirty);
    }
}
TEST(GroomCoverageModel, MemoryCostIsAPropertyOfTheModeAndTheResolution)
{
    constexpr u32 kWidth = 1920;
    constexpr u32 kHeight = 1080;
    const u64 pixels = static_cast<u64>(kWidth) * kHeight;

    EXPECT_EQ(ModeExtraBytes(GroomCompositionMode::OpaqueRibbon, kWidth, kHeight, 1u, kGBufferBytesPerSamplePerPixel),
              0ull);
    EXPECT_EQ(ModeExtraBytes(GroomCompositionMode::StochasticAlpha, kWidth, kHeight, 1u,
                             kGBufferBytesPerSamplePerPixel),
              0ull);
    EXPECT_EQ(ModeExtraBytes(GroomCompositionMode::WeightedBlendedOIT, kWidth, kHeight, 1u,
                             kGBufferBytesPerSamplePerPixel),
              pixels * 12ull);

    // Alpha-to-coverage is the expensive one, and it is expensive in the
    // resolution, not in the groom: 4x at 1080p is three extra G-Buffer
    // samples per pixel.
    const u64 a2c4 =
        ModeExtraBytes(GroomCompositionMode::AlphaToCoverage, kWidth, kHeight, 4u, kGBufferBytesPerSamplePerPixel);
    EXPECT_EQ(a2c4, pixels * 3ull * kGBufferBytesPerSamplePerPixel);
    const u64 a2c8 =
        ModeExtraBytes(GroomCompositionMode::AlphaToCoverage, kWidth, kHeight, 8u, kGBufferBytesPerSamplePerPixel);
    EXPECT_GT(a2c8, a2c4);
    std::printf("[groom-coverage] memory at 1920x1080: WB-OIT %.1f MiB, A2C x4 %.1f MiB, A2C x8 %.1f MiB\n",
                static_cast<f64>(pixels * 12ull) / (1024.0 * 1024.0), static_cast<f64>(a2c4) / (1024.0 * 1024.0),
                static_cast<f64>(a2c8) / (1024.0 * 1024.0));
}

TEST(GroomStrandSubmission, MovingRequestsPreservesOwnedBufferAddresses)
{
    struct ResetRequests
    {
        ~ResetRequests()
        {
            Renderer3D::SetGroomStrandRequests(TDoubleLinkedList<GroomStrandRequest>{});
        }
    } resetRequests;

    GroomStrandRequest request;
    request.RootTransforms.SetNum(2);
    request.SimulationDisplacements.SetNum(3);
    request.SimulationPrevDisplacements.SetNum(3);
    request.SimulationGuideOffsets = { 0u, 3u };
    request.SimulationGuideOfSlot = { 0u };
    request.SimulationSlotOfGuide = { 0u };
    request.SimulationColliders.SetNum(1);

    const auto* roots = request.RootTransforms.GetData();
    const auto* displacement = request.SimulationDisplacements.GetData();
    const auto* previous = request.SimulationPrevDisplacements.GetData();
    const auto* offsets = request.SimulationGuideOffsets.GetData();
    const auto* guideOfSlot = request.SimulationGuideOfSlot.GetData();
    const auto* slotOfGuide = request.SimulationSlotOfGuide.GetData();
    const auto* colliders = request.SimulationColliders.GetData();

    // Match Scene::PublishGroomStrandRequests: move into the producer list,
    // then publish the whole list. Neither step may copy the groom buffers.
    TDoubleLinkedList<GroomStrandRequest> requests;
    requests.AddTail(std::move(request));
    const auto* producerNode = requests.GetHead();
    Renderer3D::SetGroomStrandRequests(std::move(requests));

    EXPECT_EQ(requests.Num(), 0);
    EXPECT_EQ(requests.GetHead(), nullptr);
    const auto& submitted = Renderer3D::GetGroomStrandRequests();
    ASSERT_EQ(submitted.Num(), 1);
    ASSERT_EQ(submitted.GetHead(), producerNode);
    const auto& actual = submitted.GetHead()->GetValue();
    EXPECT_EQ(actual.RootTransforms.GetData(), roots);
    EXPECT_EQ(actual.SimulationDisplacements.GetData(), displacement);
    EXPECT_EQ(actual.SimulationPrevDisplacements.GetData(), previous);
    EXPECT_EQ(actual.SimulationGuideOffsets.GetData(), offsets);
    EXPECT_EQ(actual.SimulationGuideOfSlot.GetData(), guideOfSlot);
    EXPECT_EQ(actual.SimulationSlotOfGuide.GetData(), slotOfGuide);
    EXPECT_EQ(actual.SimulationColliders.GetData(), colliders);
}
