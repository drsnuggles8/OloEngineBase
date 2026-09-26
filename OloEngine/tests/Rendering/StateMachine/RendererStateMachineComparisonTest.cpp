// OLO_TEST_LAYER: L5

// =============================================================================
// The renderer state-machine harness's capture comparison (issues #1349,
// #1492), pinned on the CPU against answers known in advance.
//
// CompareCaptures is the gate every renderer state-machine pair passes
// through, so a comparison that accepts wrong output hides a regression in
// every pair at once. Each rejection below is a case the comparison used to
// accept:
//
//   * a non-finite value, read as black by the statistics, or passed as
//     "identical" by the exact branch when both captures held the same NaN;
//   * a shape the two captures do not share (format, extent, texel count);
//   * a target the controls could not calibrate, which used to become an
//     unbounded tolerance (a missing control frame) or no floor at all;
//   * a spatial permutation (a half-image swap, a moved patch), invisible to
//     channel means and a luminance histogram.
//
// And one acceptance that must survive all of it: an independently seeded
// noise pair still passes the distribution branch. That is the control that
// stops a fix from being too strict.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererStateMachineHarness.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        constexpr u32 kSize = 64u;

        [[nodiscard]] TargetCapture MakeTarget(std::string name, u32 width, u32 height, f32 value)
        {
            TargetCapture target;
            target.Name = std::move(name);
            target.PinnedPass = "Pass";
            target.Width = width;
            target.Height = height;
            target.Texels.assign(static_cast<sizet>(width) * height * 4u, value);
            return target;
        }

        void Fill(TargetCapture& target, u32 x0, u32 y0, u32 x1, u32 y1, f32 value)
        {
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    for (u32 c = 0; c < 3u; ++c)
                        target.Texels[((static_cast<sizet>(y) * target.Width + x) * 4u) + c] = value;
                }
            }
        }

        // Black left half, white right half; `swapped` mirrors the halves.
        [[nodiscard]] TargetCapture HalfImage(bool swapped)
        {
            TargetCapture target = MakeTarget("SceneColorTexture", kSize, kSize, 1.0f);
            Fill(target, swapped ? kSize / 2u : 0u, 0u, swapped ? kSize : kSize / 2u, kSize, 0.0f);
            return target;
        }

        // A 16x16 bright patch on a dark field, its corner at (x, y).
        [[nodiscard]] TargetCapture PatchImage(u32 x, u32 y)
        {
            TargetCapture target = MakeTarget("SceneColorTexture", kSize, kSize, 1.0f);
            Fill(target, 0u, 0u, kSize, kSize, 0.1f);
            Fill(target, x, y, x + 16u, y + 16u, 0.9f);
            return target;
        }

        // Dense per-texel noise, the shape of a stochastic effect sampled
        // with a different frame index: every colour channel of every texel
        // moves by up to `amplitude`, independently. SplitMix64, so a seed
        // gives the same image on every toolchain.
        [[nodiscard]] TargetCapture WithNoise(TargetCapture target, u64 seed, f32 amplitude)
        {
            u64 state = seed;
            const auto next = [&state]()
            {
                u64 z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31u);
            };
            for (sizet i = 0; i < target.Texels.size(); ++i)
            {
                if (i % 4u == 3u)
                    continue; // alpha is not what a stochastic effect moves
                const f64 unit = static_cast<f64>(next() >> 11u) * (1.0 / 9007199254740992.0); // [0, 1)
                target.Texels[i] += static_cast<f32>(((unit * 2.0) - 1.0) * amplitude);
            }
            return target;
        }

        [[nodiscard]] FrameCapture Frame(TargetCapture target)
        {
            FrameCapture capture;
            capture.Targets.push_back(std::move(target));
            return capture;
        }

        // Controls from two independently seeded noisy frames of `base`: the
        // distribution branch, calibrated the way CheckPairs calibrates it.
        [[nodiscard]] std::vector<ControlFloor> NoisyControls(const TargetCapture& base)
        {
            const std::vector<ControlFloor> controls =
                MeasureControls(Frame(WithNoise(base, 11u, 0.02f)), Frame(WithNoise(base, 12u, 0.02f)));
            EXPECT_TRUE(controls.size() == 1u && controls[0].Calibrated && !controls[0].Exact)
                << "the premise: one calibrated, noisy control";
            return controls;
        }

        [[nodiscard]] bool Mentions(const Comparison& comparison, std::string_view text)
        {
            return comparison.Describe().find(text) != std::string::npos;
        }

        // Every failing target failed a guard, and so was never measured: the
        // texel loop that counts differences runs only after the guards.
        [[nodiscard]] bool RejectedBeforeAnyStatistic(const Comparison& comparison)
        {
            return std::ranges::all_of(comparison.Targets, [](const TargetVerdict& verdict)
                                       { return verdict.Held || (verdict.Rejected && verdict.DifferingTexels == 0u); });
        }
    } // namespace

    // =========================================================================
    // The two criteria (the manifest's, as code)
    // =========================================================================

    TEST(RendererStateMachineComparison, AnExactTargetCatchesOneTexel)
    {
        FrameCapture a;
        a.Targets.push_back(MakeTarget("SceneColorTexture", 8u, 8u, 0.25f));
        FrameCapture b = a;
        const std::vector<ControlFloor> exact = MeasureControls(a, a);
        ASSERT_EQ(exact.size(), 1u);
        EXPECT_TRUE(exact[0].Exact);
        EXPECT_TRUE(CompareCaptures(a, b, exact).Held);

        b.Targets[0].Texels[17] = 0.2500001f;
        const Comparison comparison = CompareCaptures(a, b, exact);
        EXPECT_FALSE(comparison.Held) << "one texel one ulp off must fail an exact target";
        EXPECT_FALSE(comparison.AnyDistributionFallback);
        ASSERT_EQ(comparison.Targets.size(), 1u);
        EXPECT_EQ(comparison.Targets[0].DifferingTexels, 1u);
    }

    TEST(RendererStateMachineComparison, ANoisyControlFallsBackToDistributionAndSaysSo)
    {
        FrameCapture first;
        // 0.3, not 0.5: 0.5 is exactly a log-luminance bin edge, where any
        // downward nudge changes bin and no upward one does.
        first.Targets.push_back(MakeTarget("AOBuffer", 16u, 16u, 0.3f));
        FrameCapture second = first;
        // Frame-index noise: a few texels move a little, both ways, between
        // two frames of one state.
        for (sizet i = 0; i < second.Targets[0].Texels.size(); i += 37u)
            second.Targets[0].Texels[i] += (i / 37u) % 2u == 0u ? 0.01f : -0.01f;
        const std::vector<ControlFloor> controls = MeasureControls(first, second);
        ASSERT_FALSE(controls[0].Exact);

        FrameCapture candidate = second;
        for (sizet i = 3; i < candidate.Targets[0].Texels.size(); i += 41u)
            candidate.Targets[0].Texels[i] -= 0.01f;
        const Comparison noiseLike = CompareCaptures(second, candidate, controls);
        EXPECT_TRUE(noiseLike.Held) << noiseLike.Describe();
        EXPECT_TRUE(noiseLike.AnyDistributionFallback) << "a fallback must be reported, never hidden";

        // A real difference (a black frame where there was grey) is far
        // outside any noise floor and must still fail at distribution level.
        FrameCapture broken = second;
        std::ranges::fill(broken.Targets[0].Texels, 0.0f);
        EXPECT_FALSE(CompareCaptures(second, broken, controls).Held);
    }

    TEST(RendererStateMachineComparison, ATargetCapturedOnOneSideOnlyFails)
    {
        FrameCapture a;
        a.Targets.push_back(MakeTarget("UIComposite", 4u, 4u, 1.0f));
        a.Targets.push_back(MakeTarget("SSRColorTexture", 4u, 4u, 1.0f));
        FrameCapture b;
        b.Targets.push_back(MakeTarget("UIComposite", 4u, 4u, 1.0f));
        const Comparison comparison = CompareCaptures(a, b, MeasureControls(a, a));
        EXPECT_FALSE(comparison.Held) << "a pass that ran in one execution only is a difference";
        EXPECT_NE(comparison.Describe().find("SSRColorTexture"), std::string::npos);
    }

    // A target read after a noisy one cannot be held bit-exact on the strength
    // of its own control: its input moves, and a quantised output that rounded
    // the same way on two control frames can round the other way on a third.
    // Upstream of the noise, exactness still holds.
    TEST(RendererStateMachineComparison, TargetsDownstreamOfANoisyTargetFallBackToDistribution)
    {
        const auto frame = [](f32 early, f32 noisy, f32 late)
        {
            FrameCapture capture;
            capture.Targets.push_back(MakeTarget("Early", 8u, 8u, early));
            capture.Targets.back().ReadOrder = 1u;
            capture.Targets.push_back(MakeTarget("Noisy", 8u, 8u, noisy));
            capture.Targets.back().ReadOrder = 2u;
            capture.Targets.push_back(MakeTarget("Late", 8u, 8u, late));
            capture.Targets.back().ReadOrder = 3u;
            return capture;
        };
        const FrameCapture first = frame(0.25f, 0.5f, 0.75f);
        FrameCapture second = frame(0.25f, 0.5f, 0.75f);
        second.Targets[1].Texels[5] = 0.51f; // the noisy target's own frame-to-frame wobble
        const std::vector<ControlFloor> controls = MeasureControls(first, second);
        ASSERT_TRUE(controls[0].Exact);
        ASSERT_FALSE(controls[1].Exact);
        ASSERT_TRUE(controls[2].Exact) << "the premise: Late's own control happened to be stable";

        // Late moved by one quantisation step at one texel: noise, not a fault.
        FrameCapture candidate = second;
        candidate.Targets[2].Texels[9] = 0.75f + (1.0f / 255.0f);
        const Comparison comparison = CompareCaptures(second, candidate, controls);
        EXPECT_TRUE(comparison.Held) << comparison.Describe();
        EXPECT_TRUE(comparison.AnyDistributionFallback);

        // The same one-texel move BEFORE the noise is still an exact failure.
        FrameCapture early = second;
        early.Targets[0].Texels[9] = 0.25f + (1.0f / 255.0f);
        EXPECT_FALSE(CompareCaptures(second, early, controls).Held) << "a target upstream of the noise stays exact";
    }

    // =========================================================================
    // Non-finite values
    // =========================================================================

    TEST(RendererStateMachineComparisonGuards, NonFiniteValuesFailTheExactBranch)
    {
        const TargetCapture black = MakeTarget("SceneColorTexture", 8u, 8u, 0.0f);
        const std::vector<ControlFloor> exact = MeasureControls(Frame(black), Frame(black));
        ASSERT_EQ(exact.size(), 1u);
        ASSERT_TRUE(exact[0].Exact);

        const TargetCapture nan = MakeTarget("SceneColorTexture", 8u, 8u, std::numeric_limits<f32>::quiet_NaN());
        const Comparison nanVsBlack = CompareCaptures(Frame(nan), Frame(black), exact);
        EXPECT_FALSE(nanVsBlack.Held) << "an all-NaN target is not an all-black one";
        EXPECT_TRUE(Mentions(nanVsBlack, "non-finite")) << nanVsBlack.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(nanVsBlack)) << nanVsBlack.Describe();

        for (const f32 invalid : { std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
                                   -std::numeric_limits<f32>::infinity() })
        {
            // One texel, bit-identical in both captures: memcmp says equal.
            TargetCapture poisoned = black;
            poisoned.Texels[21] = invalid;
            const Comparison same = CompareCaptures(Frame(poisoned), Frame(poisoned), exact);
            EXPECT_FALSE(same.Held) << invalid << " in both captures is invalid output, not agreement";
            EXPECT_TRUE(Mentions(same, "non-finite")) << same.Describe();
            EXPECT_TRUE(RejectedBeforeAnyStatistic(same)) << same.Describe();
        }
    }

    TEST(RendererStateMachineComparisonGuards, NonFiniteValuesFailTheDistributionBranch)
    {
        const TargetCapture grey = MakeTarget("SceneColorTexture", kSize, kSize, 0.3f);
        const std::vector<ControlFloor> noisy = NoisyControls(grey);

        const TargetCapture nan = MakeTarget("SceneColorTexture", kSize, kSize, std::numeric_limits<f32>::quiet_NaN());
        const TargetCapture black = MakeTarget("SceneColorTexture", kSize, kSize, 0.0f);
        // Black against black is a pass, so the NaN side is the only reason
        // the next one can fail.
        ASSERT_TRUE(CompareCaptures(Frame(black), Frame(black), noisy).Held);
        const Comparison nanVsBlack = CompareCaptures(Frame(nan), Frame(black), noisy);
        EXPECT_FALSE(nanVsBlack.Held) << "the statistics read NaN as black";
        EXPECT_TRUE(Mentions(nanVsBlack, "non-finite")) << nanVsBlack.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(nanVsBlack)) << nanVsBlack.Describe();

        for (const f32 invalid : { std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
                                   -std::numeric_limits<f32>::infinity() })
        {
            TargetCapture poisoned = grey;
            poisoned.Texels[400] = invalid;
            const Comparison same = CompareCaptures(Frame(poisoned), Frame(poisoned), noisy);
            EXPECT_FALSE(same.Held) << invalid << " in both captures, distribution branch";
            EXPECT_TRUE(Mentions(same, "non-finite")) << same.Describe();
            EXPECT_TRUE(RejectedBeforeAnyStatistic(same)) << same.Describe();
        }
    }

    // A control frame holding a non-finite value measures nothing.
    TEST(RendererStateMachineComparisonGuards, NonFiniteControlsCalibrateNothing)
    {
        const TargetCapture grey = MakeTarget("SceneColorTexture", 8u, 8u, 0.3f);
        TargetCapture poisoned = grey;
        poisoned.Texels[5] = std::numeric_limits<f32>::quiet_NaN();
        const std::vector<ControlFloor> controls = MeasureControls(Frame(grey), Frame(poisoned));
        const Comparison comparison = CompareCaptures(Frame(grey), Frame(grey), controls);
        EXPECT_FALSE(comparison.Held) << "a target whose control held a NaN was compared against it";
    }

    // =========================================================================
    // Shape
    // =========================================================================

    TEST(RendererStateMachineComparisonGuards, ATexelCountThatDisagreesWithTheExtentFails)
    {
        const TargetCapture grey = MakeTarget("SceneColorTexture", 8u, 8u, 0.3f);
        const std::vector<ControlFloor> exact = MeasureControls(Frame(grey), Frame(grey));

        // Same declared extent, a truncated readback on either side.
        TargetCapture shortFirst = grey;
        shortFirst.Texels.resize(shortFirst.Texels.size() / 2u);
        const Comparison first = CompareCaptures(Frame(shortFirst), Frame(grey), exact);
        EXPECT_FALSE(first.Held) << "half a readback compared as a whole one";
        EXPECT_TRUE(Mentions(first, "texel")) << first.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(first)) << first.Describe();

        TargetCapture shortSecond = grey;
        shortSecond.Texels.resize(shortSecond.Texels.size() - 4u);
        const Comparison second = CompareCaptures(Frame(grey), Frame(shortSecond), exact);
        EXPECT_FALSE(second.Held) << "a readback one texel short compared as a whole one";
        EXPECT_TRUE(RejectedBeforeAnyStatistic(second)) << second.Describe();
    }

    TEST(RendererStateMachineComparisonGuards, AnExtentMismatchFails)
    {
        const TargetCapture wide = MakeTarget("SceneColorTexture", 8u, 4u, 0.3f);
        const TargetCapture tall = MakeTarget("SceneColorTexture", 4u, 8u, 0.3f);
        const Comparison comparison = CompareCaptures(Frame(wide), Frame(tall), MeasureControls(Frame(wide), Frame(wide)));
        EXPECT_FALSE(comparison.Held) << "8x4 and 4x8 hold the same texel count, not the same image";
        EXPECT_TRUE(Mentions(comparison, "8x4")) << comparison.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(comparison)) << comparison.Describe();
    }

    // A float readback of RGBA8 and of RGBA16F can hold the same numbers; the
    // two executions still stored different targets.
    TEST(RendererStateMachineComparisonGuards, AFormatMismatchFails)
    {
        TargetCapture half = MakeTarget("SceneColorTexture", 8u, 8u, 0.5f);
        half.Format = GL_RGBA16F;
        TargetCapture byte = half;
        byte.Format = GL_RGBA8;
        const Comparison comparison = CompareCaptures(Frame(half), Frame(byte), MeasureControls(Frame(half), Frame(half)));
        EXPECT_FALSE(comparison.Held) << "identical texels in different formats compared as one target";
        EXPECT_TRUE(Mentions(comparison, "format")) << comparison.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(comparison)) << comparison.Describe();

        // And a control pair that changed format between its frames measures nothing.
        const std::vector<ControlFloor> controls = MeasureControls(Frame(half), Frame(byte));
        ASSERT_EQ(controls.size(), 1u);
        EXPECT_FALSE(controls[0].Calibrated);
        EXPECT_FALSE(CompareCaptures(Frame(half), Frame(half), controls).Held);
    }

    // =========================================================================
    // Calibration
    // =========================================================================

    // A target missing from one control frame used to get a mean-shift floor
    // of 1e30 and a histogram floor of 2, twice which is 4: an L1 distance
    // between normalised histograms is at most 2, so nothing could fail.
    TEST(RendererStateMachineComparisonGuards, ATargetMissingFromOneControlFrameFails)
    {
        FrameCapture withTarget = Frame(MakeTarget("SceneColorTexture", 8u, 8u, 0.3f));
        withTarget.Targets.push_back(MakeTarget("AOBuffer", 8u, 8u, 0.5f));
        const FrameCapture without = Frame(MakeTarget("SceneColorTexture", 8u, 8u, 0.3f));
        FrameCapture different = withTarget;
        std::ranges::fill(different.Targets[1].Texels, 0.9f);

        for (const bool missingFromFirst : { true, false })
        {
            SCOPED_TRACE(missingFromFirst ? "missing from the first control frame" : "missing from the second");
            const std::vector<ControlFloor> controls =
                missingFromFirst ? MeasureControls(without, withTarget) : MeasureControls(withTarget, without);
            // Identical content: still a failure, because nothing measured how
            // noisy this target is, so no criterion for it exists.
            const Comparison same = CompareCaptures(withTarget, withTarget, controls);
            EXPECT_FALSE(same.Held) << "an uncalibrated target was compared anyway";
            EXPECT_TRUE(Mentions(same, "AOBuffer")) << same.Describe();
            EXPECT_TRUE(RejectedBeforeAnyStatistic(same)) << same.Describe();
            EXPECT_FALSE(CompareCaptures(withTarget, different, controls).Held)
                << "0.5 against 0.9 everywhere passed under a missing-control tolerance";
        }
    }

    // fresh-vs-sequence holds a target to both executions' controls at once.
    TEST(RendererStateMachineComparisonGuards, MergedControlsAreAsStrictAsEitherExecution)
    {
        const TargetCapture grey = MakeTarget("SceneColorTexture", kSize, kSize, 0.3f);
        const std::vector<ControlFloor> exact = MeasureControls(Frame(grey), Frame(grey));
        const std::vector<ControlFloor> noisy = NoisyControls(grey);
        ASSERT_EQ(noisy.size(), 1u);
        ASSERT_GT(noisy[0].TileShift, 0.0);

        const std::vector<ControlFloor> merged = MergeControls(exact, noisy);
        ASSERT_EQ(merged.size(), 1u);
        EXPECT_TRUE(merged[0].Calibrated);
        EXPECT_FALSE(merged[0].Exact) << "exact only if both executions' controls are";
        EXPECT_DOUBLE_EQ(merged[0].TileShift, noisy[0].TileShift);
        EXPECT_DOUBLE_EQ(merged[0].HistogramL1, noisy[0].HistogramL1);

        // A target only one execution's controls measured is not calibrated,
        // in whichever order the two are merged.
        FrameCapture withAO = Frame(grey);
        withAO.Targets.push_back(MakeTarget("AOBuffer", 8u, 8u, 0.5f));
        const std::vector<ControlFloor> wider = MeasureControls(withAO, withAO);
        for (const auto& both : { MergeControls(exact, wider), MergeControls(wider, exact) })
        {
            const auto ao = std::ranges::find(both, std::string("AOBuffer"), &ControlFloor::Name);
            ASSERT_NE(ao, both.end());
            EXPECT_FALSE(ao->Calibrated);
            EXPECT_FALSE(CompareCaptures(withAO, withAO, both).Held);
        }
    }

    TEST(RendererStateMachineComparisonGuards, ATargetWithNoControlAtAllFails)
    {
        const FrameCapture frame = Frame(MakeTarget("SceneColorTexture", 8u, 8u, 0.3f));
        const Comparison comparison = CompareCaptures(frame, frame, {});
        EXPECT_FALSE(comparison.Held) << "a target no control measured was held to a criterion nobody chose";
        EXPECT_TRUE(Mentions(comparison, "SceneColorTexture")) << comparison.Describe();
        EXPECT_TRUE(RejectedBeforeAnyStatistic(comparison)) << comparison.Describe();
    }

    // =========================================================================
    // Spatial correctness under noise
    // =========================================================================

    // The control that keeps the spatial term honest: a third independently
    // seeded frame of the same noisy state is noise, not a fault.
    TEST(RendererStateMachineComparisonSpatial, AnIndependentlySeededNoisePairStillPasses)
    {
        for (const TargetCapture& base : { HalfImage(false), PatchImage(8u, 8u), MakeTarget("SceneColorTexture", kSize, kSize, 0.3f) })
        {
            const std::vector<ControlFloor> controls = NoisyControls(base);
            for (const u64 seed : { 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u })
            {
                const Comparison comparison =
                    CompareCaptures(Frame(WithNoise(base, 12u, 0.02f)), Frame(WithNoise(base, seed, 0.02f)), controls);
                EXPECT_TRUE(comparison.Held) << "seed " << seed << ":\n"
                                             << comparison.Describe();
                EXPECT_TRUE(comparison.AnyDistributionFallback);
            }
        }
    }

    // Channel means and a luminance histogram are both permutation-invariant:
    // swapping the halves of an image moves neither, yet every texel is wrong.
    TEST(RendererStateMachineComparisonSpatial, AHalfImageSwapFailsTheDistributionBranch)
    {
        const std::vector<ControlFloor> controls = NoisyControls(HalfImage(false));
        const Comparison comparison =
            CompareCaptures(Frame(WithNoise(HalfImage(false), 12u, 0.02f)), Frame(WithNoise(HalfImage(true), 13u, 0.02f)), controls);
        EXPECT_FALSE(comparison.Held) << "a mirrored image passed the distribution branch";
        EXPECT_TRUE(comparison.AnyDistributionFallback) << "the premise: this is the distribution branch";
    }

    TEST(RendererStateMachineComparisonSpatial, ATranslatedPatchFailsTheDistributionBranch)
    {
        const std::vector<ControlFloor> controls = NoisyControls(PatchImage(8u, 8u));
        // Moved by a whole patch width, and by less than a patch width at an
        // offset no tile grid is aligned to.
        for (const auto& [x, y] : { std::pair{ 40u, 24u }, std::pair{ 13u, 11u } })
        {
            const Comparison comparison = CompareCaptures(Frame(WithNoise(PatchImage(8u, 8u), 12u, 0.02f)),
                                                          Frame(WithNoise(PatchImage(x, y), 13u, 0.02f)), controls);
            EXPECT_FALSE(comparison.Held) << "a patch moved to (" << x << ", " << y << ") passed the distribution branch";
        }
    }

    // The floor is 1% of the target's own peak, not of max(1, mean): a dim
    // target (an SSR or emissive buffer whose whole signal is below 0.01)
    // must not hide a permutation under an absolute floor.
    TEST(RendererStateMachineComparisonSpatial, ADimTargetStillCatchesAPermutation)
    {
        const auto dim = [](bool swapped, u64 seed)
        {
            TargetCapture target = HalfImage(swapped);
            for (sizet i = 0; i < target.Texels.size(); ++i)
            {
                if (i % 4u != 3u)
                    target.Texels[i] *= 0.005f;
            }
            return WithNoise(std::move(target), seed, 0.0001f);
        };
        const std::vector<ControlFloor> controls = MeasureControls(Frame(dim(false, 11u)), Frame(dim(false, 12u)));
        ASSERT_EQ(controls.size(), 1u);
        ASSERT_FALSE(controls[0].Exact);
        const Comparison noise = CompareCaptures(Frame(dim(false, 12u)), Frame(dim(false, 13u)), controls);
        EXPECT_TRUE(noise.Held) << noise.Describe();
        const Comparison swapped = CompareCaptures(Frame(dim(false, 12u)), Frame(dim(true, 13u)), controls);
        EXPECT_FALSE(swapped.Held) << "a mirrored image with a peak of 0.005 passed under the floor";
    }

    // An extent that is not a multiple of the tile has no sliver tile: a
    // one-texel-wide column would be the noisiest mean and loosen every tile.
    TEST(RendererStateMachineComparisonSpatial, AnOddExtentHasNoSliverTile)
    {
        TargetCapture wide = MakeTarget("BloomColorTexture", 33u, 17u, 0.2f);
        TargetCapture moved = wide;
        // Only the last column differs: under a 16-wide grid it would be a
        // tile of its own and move by the full 0.6; in the 16x17 tile that
        // holds it (x = 17..32) it moves by 0.6 / 16.
        for (u32 y = 0; y < 17u; ++y)
        {
            for (u32 c = 0; c < 3u; ++c)
                moved.Texels[((static_cast<sizet>(y) * 33u + 32u) * 4u) + c] = 0.8f;
        }
        EXPECT_NEAR(TileShift(wide, moved), 0.6 / 16.0, 1.0e-6);
        EXPECT_TRUE(std::isinf(TileShift(wide, MakeTarget("BloomColorTexture", 17u, 33u, 0.2f))))
            << "tiles of different extents cover different texels; comparing them by index is meaningless";
    }

    // A target downstream of a noisy one is held at distribution level even
    // when its own control was bit-stable (the #1349 quantisation case, pinned
    // in RendererStateMachineComparison.TargetsDownstreamOfANoisyTargetFallBackToDistribution).
    // That relaxation must not reopen the permutation hole.
    TEST(RendererStateMachineComparisonSpatial, ADownstreamTargetStillCatchesAPermutation)
    {
        const auto frame = [](const TargetCapture& noisy, TargetCapture late)
        {
            FrameCapture capture;
            capture.Targets.push_back(noisy);
            capture.Targets.back().Name = "Noisy";
            capture.Targets.back().ReadOrder = 1u;
            late.Name = "Late";
            late.ReadOrder = 2u;
            capture.Targets.push_back(std::move(late));
            return capture;
        };
        const TargetCapture grey = MakeTarget("", kSize, kSize, 0.3f);
        const FrameCapture first = frame(WithNoise(grey, 11u, 0.02f), HalfImage(false));
        const FrameCapture second = frame(WithNoise(grey, 12u, 0.02f), HalfImage(false));
        const std::vector<ControlFloor> controls = MeasureControls(first, second);
        ASSERT_EQ(controls.size(), 2u);
        ASSERT_FALSE(controls[0].Exact);
        ASSERT_TRUE(controls[1].Exact) << "the premise: Late's own control is bit-stable";

        const Comparison swapped = CompareCaptures(second, frame(WithNoise(grey, 13u, 0.02f), HalfImage(true)), controls);
        EXPECT_FALSE(swapped.Held) << "a mirrored downstream target passed because an upstream one was noisy";
    }
} // namespace OloEngine::Tests::StateMachine
