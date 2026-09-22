// OLO_TEST_LAYER: L1
// =============================================================================
// TemporalReconstructionSequenceTest.cpp — the six minimal reproductions
// #1256's first acceptance criterion names, plus the separation its second
// asks for, measured with TemporalSequenceMetrics.
//
// WHY THESE ARE SEQUENCES AND NOT CAPTURES. Every defect here — ghosting,
// shimmer, detail loss — is invisible in a settled frame, which is what
// criterion 3 says outright. So nothing below asserts on a single frame:
// every test runs a sequence through the shipping history model and asserts
// on a NUMBER measured across it.
//
// WHY THE SEQUENCE TESTS CARRY A CONTROL ARM. A temporal test that also
// passes with the feature disabled is not a test. Every scenario that claims
// a behavioural IMPROVEMENT therefore runs twice in the same process — once
// with the channel under test live, once with it as it was before #1256 (or,
// where there is no settings flag to switch the mechanism off, with an input
// that models the broken behaviour) — and asserts on the DIFFERENCE between
// two measurements rather than against a tuned constant. Where the control
// arm is expected to move, that movement is asserted too: an instrument that
// reads zero on both arms is broken, not passing.
//
// Reproduction 6a and the TemporalReactivitySeparation cases below are the
// exception and have no control arm, deliberately: they assert closed-form
// arithmetic on the model rather than a measured improvement, so there is no
// second arm for them to be compared against. 6b is the sequence half of the
// same condition and does carry one.
//
// It is CPU-only and needs no GL context, deliberately. A live pixel A/B
// cannot answer these questions at all — 69 % of pixels move between two
// captures of the same windy scene — so the reproductions are run headless
// under mock time, where the only thing changing is the thing under test.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/SurfaceHistory.h"
#include "OloEngine/Renderer/TemporalSequenceMetrics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using namespace OloEngine::TemporalSequenceMetrics;

        /// What the scene presents at one pixel on one frame.
        struct PixelState
        {
            SurfaceHistoryRecord Record{};
            f32 Signal = 0.0f;
            glm::vec2 ReprojectedUV{ 0.5f, 0.5f };
        };

        struct ResolveConfig
        {
            f32 Feedback = 0.9f;
            /// false models the engine BEFORE #1256: no coverage channel, so
            /// both records read as fully covered and neither the coverage
            /// rejection nor the coverage reactivity term can ever fire.
            /// This is the control arm for every coverage scenario.
            bool CoverageChannelLive = true;
            SurfaceHistoryValiditySettings Validity{};
            TemporalReactivitySettings Reactivity{};
        };

        /// The shipping blend, mirrored exactly: OloTemporalFeedbackWeight
        /// times a confidence that is the product of the three separated
        /// reactive causes, zeroed outright when the validity model rejects.
        [[nodiscard]] f32 ResolveOnePixel(const PixelState& current, const PixelState& previous, f32 history,
                                          bool historyAvailable, const ResolveConfig& config)
        {
            SurfaceHistoryRecord currentRecord = current.Record;
            SurfaceHistoryRecord previousRecord = previous.Record;
            if (!config.CoverageChannelLive)
            {
                currentRecord.Coverage = 1.0f;
                previousRecord.Coverage = 1.0f;
            }

            SurfaceHistoryValiditySettings validity = config.Validity;
            validity.HistoryAvailable = historyAvailable;

            const SurfaceHistoryValidity verdict =
                EvaluateSurfaceHistory(currentRecord, previousRecord, current.ReprojectedUV, validity);
            const TemporalReactivity reactivity =
                EvaluateTemporalReactivity(currentRecord, previousRecord, config.Reactivity);

            const f32 confidence = verdict.Accepted() ? reactivity.Confidence() : 0.0f;
            const f32 weight = std::clamp(config.Feedback, 0.0f, 0.98f) * std::clamp(confidence, 0.0f, 1.0f);
            return current.Signal * (1.0f - weight) + history * weight;
        }

        /// Runs `frameCount` frames over a field of `pixelCount` pixels and
        /// returns the resolved field for each one. `describe(frame, pixel)`
        /// is the scene: it is a pure function of the frame index, which is
        /// what "under mock time" means here — re-running the sequence
        /// produces identical numbers on any machine.
        using SceneFn = std::function<PixelState(u32, u32)>;

        /// The resolve's own configuration, as a function of the frame index.
        /// Constant for every scenario but the dynamic-resolution one, where
        /// the render scale — and therefore PixelSize — moves mid-sequence.
        using ConfigFn = std::function<ResolveConfig(u32)>;

        [[nodiscard]] std::vector<std::vector<f32>> RunSequence(u32 frameCount, u32 pixelCount,
                                                                const ConfigFn& configure, const SceneFn& describe)
        {
            std::vector<std::vector<f32>> frames;
            frames.reserve(frameCount);

            std::vector<f32> history(pixelCount, 0.0f);
            std::vector<PixelState> previous(pixelCount);

            for (u32 frame = 0u; frame < frameCount; ++frame)
            {
                const ResolveConfig config = configure(frame);
                std::vector<f32> resolved(pixelCount, 0.0f);
                std::vector<PixelState> currentStates(pixelCount);
                for (u32 pixel = 0u; pixel < pixelCount; ++pixel)
                {
                    const PixelState current = describe(frame, pixel);
                    currentStates[pixel] = current;
                    resolved[pixel] = ResolveOnePixel(current, previous[pixel], history[pixel], frame > 0u, config);
                }
                history = resolved;
                previous = currentStates;
                frames.push_back(std::move(resolved));
            }
            return frames;
        }

        [[nodiscard]] std::vector<std::vector<f32>> RunSequence(u32 frameCount, u32 pixelCount,
                                                                const ResolveConfig& config, const SceneFn& describe)
        {
            return RunSequence(frameCount, pixelCount, [&config](u32)
                               { return config; }, describe);
        }

        [[nodiscard]] SurfaceHistoryRecord StableSurface()
        {
            SurfaceHistoryRecord surface{};
            surface.LinearDepth = 10.0f;
            surface.GeometricNormal = { 0.0f, 0.0f, 1.0f };
            surface.ShadingNormal = surface.GeometricNormal;
            surface.Roughness = 0.4f;
            surface.MaterialClass = 2u;
            surface.Instance = { 7u, 1u };
            surface.Primitive = { 11u, 1u };
            surface.Material = { 5u, 1u };
            surface.Coverage = 1.0f;
            surface.MaterialProfile = 0.0f;
            return surface;
        }

        /// A reactivity configuration whose dead bands are stated rather than
        /// defaulted, so a scenario's numbers cannot silently change when the
        /// shipping defaults are retuned.
        [[nodiscard]] TemporalReactivitySettings PinnedReactivity(f32 coverageDeadBand)
        {
            TemporalReactivitySettings settings{};
            settings.MotionDeadZonePixels = 1.0f;
            settings.MotionSaturationPixels = 5.0f;
            settings.MotionMaxReactivity = 0.5f;
            settings.CoverageNoiseDeadBand = coverageDeadBand;
            settings.CoverageSaturation = 0.35f;
            settings.MaterialProfileDeadBand = 0.02f;
            settings.MaterialProfileSaturation = 0.25f;
            settings.PixelSize = { 1.0f / 1280.0f, 1.0f / 720.0f };
            return settings;
        }

        /// Deterministic zero-mean jitter standing in for a stochastic
        /// coverage estimator. A hash rather than a PRNG so the sequence is
        /// identical on every run and every platform.
        [[nodiscard]] f32 CoverageJitter(u32 frame, u32 pixel, f32 amplitude)
        {
            u32 h = frame * 0x9E3779B9u ^ pixel * 0x85EBCA6Bu;
            h ^= h >> 15;
            h *= 0x2545F491u;
            h ^= h >> 13;
            const f32 unit = static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
            return (unit * 2.0f - 1.0f) * amplitude;
        }

        constexpr u32 kPixels = 64u;
    } // namespace

    // -------------------------------------------------------------------------
    // Reproduction 1 — DISOCCLUSION
    // -------------------------------------------------------------------------
    // A different instance arrives at the same depth. Nothing about the pixel's
    // radiance says so; only the identity does.
    TEST(TemporalReconstructionSequence, DisocclusionDropsHistoryInOneFrameAndTheControlGhosts)
    {
        constexpr u32 kFrames = 48u;
        const auto scene = [](u32 frame, u32) -> PixelState
        {
            PixelState state{};
            state.Record = StableSurface();
            if (frame >= 8u)
            {
                state.Record.Instance = { 8u, 1u }; // a different object, same depth
                state.Signal = 0.2f;
            }
            else
            {
                state.Signal = 1.0f;
            }
            return state;
        };

        ResolveConfig live{};
        live.Reactivity = PinnedReactivity(0.05f);

        ResolveConfig control = live;
        control.Validity.TestInstance = false; // the identity channel switched off

        const auto liveFrames = RunSequence(kFrames, kPixels, live, scene);
        const auto controlFrames = RunSequence(kFrames, kPixels, control, scene);

        const std::vector<f32> target(kPixels, 0.2f);
        const auto post = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(8u); };

        const GhostingResult liveGhost = MeasureGhosting(post(liveFrames), target, 0.01);
        const GhostingResult controlGhost = MeasureGhosting(post(controlFrames), target, 0.01);

        // The instrument first: the control arm must actually ghost, or the
        // live arm's fast settle proves nothing.
        EXPECT_GT(controlGhost.SettlingFrames, 20u);
        EXPECT_GT(controlGhost.ResidualArea, 1.0);

        EXPECT_LE(liveGhost.SettlingFrames, 1u);
        EXPECT_LT(liveGhost.ResidualArea, controlGhost.ResidualArea * 0.1);
        EXPECT_LT(liveGhost.FinalResidual, 1.0e-4);
    }

    // -------------------------------------------------------------------------
    // Reproduction 2 — CAMERA CUT
    // -------------------------------------------------------------------------
    // The reprojection lands off screen. History is not old here, it is a
    // different pixel entirely, and keeping any of it drags the old view in.
    TEST(TemporalReconstructionSequence, CameraCutRejectsOffScreenHistoryImmediately)
    {
        constexpr u32 kFrames = 32u;
        // `offScreenOnCut` false is the CONTROL: a resolve that failed to
        // notice the cut and reprojected to a plausible in-view UV instead.
        // The off-screen UV test is the entire mechanism here — there is no
        // settings flag to switch it off — so the control has to be built by
        // feeding it the UV a broken reprojection would produce.
        const auto makeScene = [](bool offScreenOnCut)
        {
            return [offScreenOnCut](u32 frame, u32) -> PixelState
            {
                PixelState state{};
                state.Record = StableSurface();
                state.Signal = frame >= 6u ? 0.15f : 0.9f;
                state.ReprojectedUV = (frame == 6u && offScreenOnCut) ? glm::vec2{ 1.8f, 0.5f }
                                                                      : glm::vec2{ 0.5f, 0.5f };
                return state;
            };
        };

        ResolveConfig config{};
        config.Reactivity = PinnedReactivity(0.12f);

        const auto liveFrames = RunSequence(kFrames, kPixels, config, makeScene(true));
        const auto controlFrames = RunSequence(kFrames, kPixels, config, makeScene(false));

        const std::vector<f32> target(kPixels, 0.15f);
        const auto post = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(6u); };

        const GhostingResult ghost = MeasureGhosting(post(liveFrames), target, 0.01);
        const GhostingResult controlGhost = MeasureGhosting(post(controlFrames), target, 0.01);

        // The instrument: the control must actually drag the old view.
        EXPECT_GT(controlGhost.ComparedPixels, 0u);
        EXPECT_GT(controlGhost.SettlingFrames, 10u);
        EXPECT_GT(controlGhost.ResidualArea, 1.0);

        EXPECT_LE(ghost.SettlingFrames, 1u);
        EXPECT_LT(ghost.FinalResidual, 1.0e-4);
        EXPECT_LT(ghost.ResidualArea, controlGhost.ResidualArea * 0.05);

        // And the cut frame itself is the new view exactly — not a blend of
        // the two, which is what a surviving off-screen history looks like.
        EXPECT_NEAR(liveFrames[6u][0u], 0.15f, 1.0e-5f);
    }

    // -------------------------------------------------------------------------
    // Reproduction 3 — ANIMATED DEFORMATION
    // -------------------------------------------------------------------------
    // Skin under animation: the surface stays the same instance and material
    // while its shading normal rotates. The response must be GRADED — a small
    // deformation keeps its history, a large one does not — because rejecting
    // on every frame of an animation is how skin loses its temporal detail.
    TEST(TemporalReconstructionSequence, AnimatedDeformationRejectsOnlyPastTheNormalThreshold)
    {
        constexpr u32 kFrames = 24u;
        const auto rotatedNormal = [](f32 radians)
        { return glm::vec3{ std::sin(radians), 0.0f, std::cos(radians) }; };

        // The signal ALTERNATES, so the resolve's output is what the
        // assertion reads: a kept history damps the alternation toward the
        // mean, a rejected one reproduces the input exactly. An earlier
        // version of this test asserted on rejections re-derived from
        // EvaluateSurfaceHistory and never looked at RunSequence's output at
        // all — it would have passed had the resolve ignored the verdict
        // entirely, which is the one thing it is here to check.
        const auto run = [&](f32 perFrameRadians)
        {
            const auto scene = [&](u32 frame, u32) -> PixelState
            {
                PixelState state{};
                state.Record = StableSurface();
                state.Record.ShadingNormal = rotatedNormal(static_cast<f32>(frame) * perFrameRadians);
                state.Record.GeometricNormal = state.Record.ShadingNormal;
                state.Signal = (frame % 2u == 0u) ? 0.2f : 0.8f;
                return state;
            };
            ResolveConfig config{};
            config.Reactivity = PinnedReactivity(0.12f);
            return RunSequence(kFrames, kPixels, config, scene);
        };

        const auto tail = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(8u); };

        // 0.01 rad/frame is well inside the 0.75 shading-normal cosine
        // threshold, so history survives and damps the alternation.
        const ShimmerResult slow = MeasureShimmer(tail(run(0.01f)));
        // 0.9 rad/frame is well outside it, so every frame is rejected and
        // the output IS the input — the full 0.6 swing, every frame.
        const ShimmerResult fast = MeasureShimmer(tail(run(0.9f)));

        EXPECT_GT(slow.ComparedPixels, 0u) << "the instrument measured nothing";
        EXPECT_GT(fast.ComparedPixels, 0u);

        EXPECT_NEAR(fast.MeanFrameDelta, 0.6, 1.0e-3)
            << "a fully rejected history must reproduce the input exactly";
        EXPECT_LT(slow.MeanFrameDelta, fast.MeanFrameDelta * 0.25)
            << "a deformation inside the threshold must keep its history";
    }

    // -------------------------------------------------------------------------
    // Reproduction 4 — LOD TRANSITION  (the headline)
    // -------------------------------------------------------------------------
    // A groom or foliage layer thins at a density LOD step. The instance, the
    // primitive, the material and the depth are ALL unchanged — only the
    // fraction of the pixel the subject covers moved. Before #1256 nothing in
    // the model could see that, so the history was kept at full weight and the
    // old, denser value was dragged across the transition.
    TEST(TemporalReconstructionSequence, CoverageStepAtALodTransitionIsCaughtOnlyByTheCoverageChannel)
    {
        constexpr u32 kFrames = 64u;
        const auto scene = [](u32 frame, u32) -> PixelState
        {
            PixelState state{};
            state.Record = StableSurface();
            const bool thinned = frame >= 8u;
            state.Record.Coverage = thinned ? 0.35f : 1.0f;
            state.Signal = thinned ? 0.35f : 1.0f; // radiance tracks coverage
            return state;
        };

        ResolveConfig live{};
        live.Reactivity = PinnedReactivity(0.05f);

        ResolveConfig control = live;
        control.CoverageChannelLive = false; // the engine before #1256

        const auto liveFrames = RunSequence(kFrames, kPixels, live, scene);
        const auto controlFrames = RunSequence(kFrames, kPixels, control, scene);

        const std::vector<f32> target(kPixels, 0.35f);
        const auto post = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(8u); };

        const GhostingResult liveGhost = MeasureGhosting(post(liveFrames), target, 0.01);
        const GhostingResult controlGhost = MeasureGhosting(post(controlFrames), target, 0.01);

        // The control ghosts badly — this is the defect, measured.
        EXPECT_GT(controlGhost.SettlingFrames, 25u);
        EXPECT_GT(controlGhost.ResidualArea, 2.0);

        // The coverage channel catches it. The step of 0.65 is past the 0.5
        // hard rejection threshold, so the history goes entirely.
        EXPECT_LE(liveGhost.SettlingFrames, 1u);
        EXPECT_LT(liveGhost.ResidualArea, controlGhost.ResidualArea * 0.05);
    }

    // -------------------------------------------------------------------------
    // Reproduction 5 — ALPHA COVERAGE  (the trap)
    // -------------------------------------------------------------------------
    // The same channel, driven by the opposite input. A stochastic strand
    // estimator moves its per-pixel coverage EVERY frame by construction, and
    // averaging that away is the entire reason the resolve exists. A coverage
    // term that reacted to the raw delta would drop history exactly where
    // history is working, and hair would sparkle worse with the feature on
    // than off — while every still capture looked fine.
    //
    // So the dead band is asserted to be load-bearing, in the direction that
    // matters: shimmer.
    TEST(TemporalReconstructionSequence, StochasticCoverageNoiseMustNotDriveTheReactiveTerm)
    {
        constexpr u32 kFrames = 48u;
        constexpr f32 kJitter = 0.06f;
        const auto scene = [](u32 frame, u32 pixel) -> PixelState
        {
            PixelState state{};
            state.Record = StableSurface();
            const f32 jittered = 0.5f + CoverageJitter(frame, pixel, kJitter);
            state.Record.Coverage = jittered;
            state.Signal = jittered;
            return state;
        };

        // The dead band must clear the estimator's WORST consecutive delta,
        // not its mean. Two frames of a +/-0.06 jitter can differ by 0.12,
        // so a 0.08 band still lets the largest jumps through — and because
        // those are the frames that move the output most, the arm with the
        // too-small band measures *worse* than no band at all (ratio 1.15,
        // computed before this test was written). Sizing the band to the
        // amplitude rather than to the delta is the mistake to avoid.
        ResolveConfig live{};
        live.Feedback = 0.95f;
        live.Reactivity = PinnedReactivity(0.13f);

        // The control removes the dead band — reacting to raw coverage delta.
        ResolveConfig control = live;
        control.Reactivity.CoverageNoiseDeadBand = 0.0f;

        const auto liveFrames = RunSequence(kFrames, kPixels, live, scene);
        const auto controlFrames = RunSequence(kFrames, kPixels, control, scene);

        const auto tail = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(16u); };

        const ShimmerResult liveShimmer = MeasureShimmer(tail(liveFrames));
        const ShimmerResult controlShimmer = MeasureShimmer(tail(controlFrames));

        // The instrument: the control arm must actually shimmer.
        EXPECT_GT(controlShimmer.MeanFrameDelta, 1.0e-3);

        // The prediction, computed rather than observed: the dead-band arm's
        // coverage term is identically zero (0.12 worst delta < 0.13 band),
        // so it keeps weight 0.95 and passes 5 % of each frame's noise. The
        // control's term averages 0.114, cutting the weight to 0.841 and
        // passing 15.9 %. The predicted ratio is 0.315, so a factor of two
        // is a floor the mechanism clears comfortably — and if the dead band
        // were sized to the jitter AMPLITUDE instead of the worst delta this
        // assertion would fail, which is the point of it.
        EXPECT_LT(liveShimmer.MeanFrameDelta, controlShimmer.MeanFrameDelta * 0.5);

        // The mechanism itself, asserted directly rather than inferred from
        // the shimmer: no frame of the live arm may produce ANY coverage
        // reactivity, and the control must produce some.
        f32 liveWorstCoverageTerm = 0.0f;
        f32 controlPeakCoverageTerm = 0.0f;
        for (u32 frame = 1u; frame < kFrames; ++frame)
        {
            for (u32 pixel = 0u; pixel < kPixels; ++pixel)
            {
                const PixelState current = scene(frame, pixel);
                const PixelState prior = scene(frame - 1u, pixel);
                liveWorstCoverageTerm =
                    std::max(liveWorstCoverageTerm,
                             EvaluateTemporalReactivity(current.Record, prior.Record, live.Reactivity).CoverageChange);
                controlPeakCoverageTerm = std::max(
                    controlPeakCoverageTerm,
                    EvaluateTemporalReactivity(current.Record, prior.Record, control.Reactivity).CoverageChange);
            }
        }
        EXPECT_NEAR(liveWorstCoverageTerm, 0.0f, 1.0e-6f);
        EXPECT_GT(controlPeakCoverageTerm, 0.1f);

        // And it converges to the right mean rather than buying stillness by
        // freezing: the resolved mean must still track the estimator's 0.5.
        const auto& last = liveFrames.back();
        const f64 mean = std::accumulate(last.begin(), last.end(), 0.0) / static_cast<f64>(last.size());
        EXPECT_NEAR(mean, 0.5, 0.02);
    }

    // -------------------------------------------------------------------------
    // Reproduction 6a — DYNAMIC RESOLUTION, as closed-form arithmetic
    // -------------------------------------------------------------------------
    // Ghosting is a PIXEL-space phenomenon, so the motion term must be measured
    // in pixels. The same UV motion at half resolution is half the pixel
    // motion and must be correspondingly less reactive — a model that worked
    // in UV would return the same answer at both resolutions and would be
    // mistuned at every resolution but the one it was tuned at.
    TEST(TemporalReconstructionSequence, MotionReactivityTracksPixelSizeUnderDynamicResolution)
    {
        SurfaceHistoryRecord current = StableSurface();
        SurfaceHistoryRecord previous = current;
        current.Motion = { 0.004f, 0.0f }; // UV motion, identical in both arms

        TemporalReactivitySettings full = PinnedReactivity(0.05f);
        full.PixelSize = { 1.0f / 1280.0f, 1.0f / 720.0f }; // 5.12 px

        TemporalReactivitySettings half = full;
        half.PixelSize = { 1.0f / 640.0f, 1.0f / 360.0f }; // 2.56 px

        const TemporalReactivity atFull = EvaluateTemporalReactivity(current, previous, full);
        const TemporalReactivity atHalf = EvaluateTemporalReactivity(current, previous, half);

        // 5.12 px is past the 5 px saturation, so the ramp is 1.0 and the
        // 0.5 MotionMaxReactivity cap is what the result reports — the same
        // half-feedback floor OloTemporalMotionFeedback already applies.
        // 2.56 px is partway up the ramp at 0.39, below the cap, so the two
        // resolutions genuinely differ rather than both pinning at the cap.
        EXPECT_NEAR(atFull.SurfaceMotion, 0.5f, 1.0e-5f);
        EXPECT_NEAR(atHalf.SurfaceMotion, (2.56f - 1.0f) / 4.0f, 1.0e-2f);
        EXPECT_LT(atHalf.SurfaceMotion, atFull.SurfaceMotion);

        // Neither resolution touched the other two causes.
        EXPECT_NEAR(atFull.CoverageChange, 0.0f, 1.0e-6f);
        EXPECT_NEAR(atFull.MaterialChange, 0.0f, 1.0e-6f);
    }

    // Reproduction 6b — DYNAMIC RESOLUTION, as an actual SEQUENCE.
    //
    // 6a above is closed-form arithmetic on one evaluation, which pins the
    // ramp but says nothing about what a RESOLUTION CHANGE does to a running
    // accumulator — and criterion 1 asks for a sequence. This is that
    // sequence, and it carries the control arm 6a has no room for.
    //
    // THE BUG IT MODELS is a real one and specific to dynamic resolution: a
    // resolve that samples its pixel size ONCE, at init, and never updates it
    // when the render scale moves. Nothing about such a resolve looks wrong
    // at the resolution it was initialised at, which is the resolution
    // everybody tests at.
    //
    // The scene pans at a constant UV velocity throughout. At full resolution
    // that is 5.12 px/frame, past the 5 px saturation, so the motion term
    // sits at its 0.5 cap. At frame 16 the render scale drops to the
    // UltraPerformance preset's 0.333, so the SAME UV motion is only 1.70
    // px/frame — a third of the resampling error, and a resolve that noticed
    // would keep correspondingly more history.
    //
    //   * live    — PixelSize tracks the render scale, so the motion term
    //               falls to 0.176 after the step and the blend keeps more.
    //   * control — PixelSize latched at the full-resolution value, so the
    //               term stays pinned at the 0.5 cap and the resolve throws
    //               away history it should have kept.
    //
    // The signal ALTERNATES, so the arms are separated by how much of that
    // alternation survives — shimmer, measured, rather than a re-derived
    // reactivity number that would pass even if the resolve ignored it.
    TEST(TemporalReconstructionSequence, ADynamicResolutionDropKeepsMoreHistoryAndTheLatchedControlDoesNot)
    {
        constexpr u32 kFrames = 48u;
        constexpr u32 kStepFrame = 16u;
        constexpr f32 kFullWidth = 1280.0f;
        constexpr f32 kFullHeight = 720.0f;
        // The shipping UltraPerformance preset rather than a round number, so
        // this is a resolution the engine can actually be in.
        constexpr f32 kScale = 0.333f;

        const auto scene = [](u32 frame, u32) -> PixelState
        {
            PixelState state{};
            state.Record = StableSurface();
            state.Record.Motion = { 0.004f, 0.0f }; // constant UV pan, both arms
            state.Signal = (frame % 2u == 0u) ? 0.2f : 0.8f;
            return state;
        };

        const auto pixelSizeFor = [](f32 scale)
        { return glm::vec2{ 1.0f / (kFullWidth * scale), 1.0f / (kFullHeight * scale) }; };

        // Live: PixelSize follows the render scale.
        const auto live = [&](u32 frame)
        {
            ResolveConfig config{};
            config.Reactivity = PinnedReactivity(0.12f);
            config.Reactivity.PixelSize = pixelSizeFor(frame >= kStepFrame ? kScale : 1.0f);
            return config;
        };

        // Control: latched at full resolution for the whole sequence.
        const auto control = [&](u32)
        {
            ResolveConfig config{};
            config.Reactivity = PinnedReactivity(0.12f);
            config.Reactivity.PixelSize = pixelSizeFor(1.0f);
            return config;
        };

        const auto liveFrames = RunSequence(kFrames, kPixels, ConfigFn(live), scene);
        const auto controlFrames = RunSequence(kFrames, kPixels, ConfigFn(control), scene);

        // Measured only AFTER the step, and after the accumulator has had a
        // few frames to reach its new steady state. Before the step the two
        // arms are identical by construction, which is asserted below.
        const auto afterStep = [](const std::vector<std::vector<f32>>& frames)
        { return std::span<const std::vector<f32>>(frames).subspan(kStepFrame + 8u); };

        const ShimmerResult liveShimmer = MeasureShimmer(afterStep(liveFrames));
        const ShimmerResult controlShimmer = MeasureShimmer(afterStep(controlFrames));

        EXPECT_GT(liveShimmer.ComparedPixels, 0u) << "the instrument measured nothing";
        EXPECT_GT(controlShimmer.ComparedPixels, 0u);

        // The two arms are the same resolve until the step. If they differ
        // here the scenario is not isolating the resolution change.
        for (u32 frame = 0u; frame < kStepFrame; ++frame)
        {
            EXPECT_NEAR(liveFrames[frame][0u], controlFrames[frame][0u], 1.0e-6f)
                << "arms diverged at frame " << frame << ", before the resolution step";
        }

        // The instrument: the control must actually pass the alternation
        // through, or the live arm's stillness proves nothing.
        EXPECT_GT(controlShimmer.MeanFrameDelta, 0.1);

        // The prediction, computed rather than observed. With the other two
        // causes at zero, confidence is 1 - SurfaceMotion. The input alternates
        // between 0.2 and 0.8, so its AMPLITUDE about the mean is 0.3 (the
        // swing is 0.6 — the two are easy to confuse and differ by the factor
        // of two that MeanFrameDelta then doubles back in). Under blend weight
        // w the steady-state output amplitude is 0.3(1-w)/(1+w), and
        // MeanFrameDelta is twice that:
        //
        //   live    motion 1.704 px -> ramp 0.176, w = 0.9 * 0.824 = 0.742
        //           -> amplitude 0.3 * 0.258/1.742 = 0.0444, delta 0.089
        //   control motion 5.12 px  -> capped 0.5,  w = 0.9 * 0.5   = 0.45
        //           -> amplitude 0.3 * 0.55/1.45   = 0.1138, delta 0.228
        //
        // a predicted ratio of 0.39, so half is a floor the mechanism clears.
        // The control measures 0.2276 against the 0.228 predicted here.
        //
        // NEGATIVE CONTROL, run deliberately: latching the LIVE arm's PixelSize
        // at full resolution too — which is exactly the bug described above —
        // makes both arms measure 0.2276 and fails this assertion. The test
        // catches the defect it names.
        EXPECT_LT(liveShimmer.MeanFrameDelta, controlShimmer.MeanFrameDelta * 0.5)
            << "tracking the render scale did not keep more history: live " << liveShimmer.MeanFrameDelta
            << " vs control " << controlShimmer.MeanFrameDelta;

        // And the mechanism directly: the motion term must genuinely differ
        // between the two arms after the step, rather than the shimmer gap
        // coming from somewhere else.
        const PixelState stepped = scene(kStepFrame, 0u);
        const SurfaceHistoryRecord prior = scene(kStepFrame - 1u, 0u).Record;
        EXPECT_NEAR(EvaluateTemporalReactivity(stepped.Record, prior, live(kStepFrame).Reactivity).SurfaceMotion,
                    (1.704f - 1.0f) / 4.0f, 1.0e-2f);
        EXPECT_NEAR(EvaluateTemporalReactivity(stepped.Record, prior, control(kStepFrame).Reactivity).SurfaceMotion,
                    0.5f, 1.0e-5f);
    }

    // -------------------------------------------------------------------------
    // Criterion 2 — the three causes stay SEPARATED
    // -------------------------------------------------------------------------
    TEST(TemporalReactivitySeparation, EachCauseFiresAloneAndDoesNotDriveTheOthers)
    {
        const TemporalReactivitySettings settings = PinnedReactivity(0.05f);
        const SurfaceHistoryRecord base = StableSurface();

        SurfaceHistoryRecord movedOnly = base;
        movedOnly.Motion = { 0.004f, 0.0f };
        const TemporalReactivity motion = EvaluateTemporalReactivity(movedOnly, base, settings);
        // 5.12 px saturates the ramp, so this reports the 0.5 cap.
        EXPECT_NEAR(motion.SurfaceMotion, 0.5f, 1.0e-5f);
        EXPECT_NEAR(motion.CoverageChange, 0.0f, 1.0e-6f);
        EXPECT_NEAR(motion.MaterialChange, 0.0f, 1.0e-6f);

        SurfaceHistoryRecord thinnedOnly = base;
        thinnedOnly.Coverage = 0.6f;
        const TemporalReactivity coverage = EvaluateTemporalReactivity(thinnedOnly, base, settings);
        EXPECT_GT(coverage.CoverageChange, 0.9f);
        EXPECT_NEAR(coverage.SurfaceMotion, 0.0f, 1.0e-6f);
        EXPECT_NEAR(coverage.MaterialChange, 0.0f, 1.0e-6f);

        SurfaceHistoryRecord reprofiledOnly = base;
        reprofiledOnly.MaterialProfile = 0.4f;
        const TemporalReactivity profile = EvaluateTemporalReactivity(reprofiledOnly, base, settings);
        EXPECT_GT(profile.MaterialChange, 0.9f);
        EXPECT_NEAR(profile.SurfaceMotion, 0.0f, 1.0e-6f);
        EXPECT_NEAR(profile.CoverageChange, 0.0f, 1.0e-6f);
    }

    TEST(TemporalReactivitySeparation, ThreeSmallCausesCompoundRatherThanBeingMasked)
    {
        const TemporalReactivitySettings settings = PinnedReactivity(0.05f);
        const SurfaceHistoryRecord base = StableSurface();

        // A 0.15 coverage drop: past the 0.05 dead band, well short of the
        // 0.35 saturation, so the term is partial rather than saturated.
        SurfaceHistoryRecord oneCause = base;
        oneCause.Coverage = 0.85f;

        SurfaceHistoryRecord threeCauses = oneCause;
        threeCauses.MaterialProfile = 0.09f;
        threeCauses.Motion = { 0.002f, 0.0f };

        const TemporalReactivity single = EvaluateTemporalReactivity(oneCause, base, settings);
        const TemporalReactivity compound = EvaluateTemporalReactivity(threeCauses, base, settings);

        // Each cause on its own is partial, so a max() would report the same
        // number for both. The product must not.
        EXPECT_GT(single.CoverageChange, 0.0f);
        EXPECT_LT(single.CoverageChange, 1.0f);
        EXPECT_LT(compound.Confidence(), single.Confidence());
        EXPECT_GT(compound.Combined(), single.Combined());
    }

    TEST(TemporalReactivitySeparation, NonFiniteMotionKeepsNoHistory)
    {
        const TemporalReactivitySettings settings = PinnedReactivity(0.05f);
        const SurfaceHistoryRecord base = StableSurface();
        SurfaceHistoryRecord broken = base;
        broken.Motion = glm::vec2(std::numeric_limits<f32>::quiet_NaN());

        const TemporalReactivity reactivity = EvaluateTemporalReactivity(broken, base, settings);
        EXPECT_NEAR(reactivity.SurfaceMotion, 1.0f, 1.0e-6f);
        EXPECT_NEAR(reactivity.Confidence(), 0.0f, 1.0e-6f);
    }

    TEST(SurfaceHistoryCoverage, AnOpaqueCallerIsUnaffectedByTheNewChannel)
    {
        // Every existing caller leaves Coverage at its 1.0 default, so the
        // channel must be inert for them — including with TestCoverage on,
        // which is its default.
        const SurfaceHistoryRecord surface = StableSurface();
        const SurfaceHistoryValidity verdict =
            EvaluateSurfaceHistory(surface, surface, { 0.5f, 0.5f }, SurfaceHistoryValiditySettings{});
        EXPECT_TRUE(verdict.Accepted());
        EXPECT_FALSE(verdict.Has(SurfaceHistoryRejection::CoverageMismatch));
    }

    TEST(SurfaceHistoryCoverage, ACollapseBeyondTheHardThresholdIsARejectionNotAGradedResponse)
    {
        SurfaceHistoryRecord current = StableSurface();
        SurfaceHistoryRecord previous = current;
        current.Coverage = 0.1f; // a 0.9 collapse, past the 0.5 threshold

        const SurfaceHistoryValidity verdict =
            EvaluateSurfaceHistory(current, previous, { 0.5f, 0.5f }, SurfaceHistoryValiditySettings{});
        EXPECT_TRUE(verdict.Has(SurfaceHistoryRejection::CoverageMismatch));
        EXPECT_FALSE(verdict.Has(SurfaceHistoryRejection::DepthMismatch));
        EXPECT_FALSE(verdict.Has(SurfaceHistoryRejection::InstanceMismatch));
    }

    // The two coverage mechanisms overlap in the blend weight and differ in
    // what they do to the ACCUMULATOR, and only this test says so.
    //
    // Found by deliberately disabling the hard rejection and noticing that
    // the LOD-transition reproduction still passed: the reactive term
    // saturates at a coverage change of 0.35, below the 0.5 rejection
    // threshold, so the rejection can never be what decides the weight. It
    // is not redundant — it is what resets HistoryLength — but nothing
    // pinned that until this test existed.
    TEST(SurfaceHistoryCoverage, RejectionAndSaturatedReactivityDifferInTheirConsequence)
    {
        const TemporalReactivitySettings reactivitySettings = PinnedReactivity(0.05f);
        const SurfaceHistoryRecord base = StableSurface();

        // 0.40 change: past the 0.35 reactive saturation, short of the 0.5
        // rejection threshold.
        SurfaceHistoryRecord thinned = base;
        thinned.Coverage = 0.60f;
        // 0.70 change: past both.
        SurfaceHistoryRecord collapsed = base;
        collapsed.Coverage = 0.30f;

        const SurfaceHistoryValiditySettings validity{};
        const bool thinnedAccepted =
            EvaluateSurfaceHistory(thinned, base, { 0.5f, 0.5f }, validity).Accepted();
        const bool collapsedAccepted =
            EvaluateSurfaceHistory(collapsed, base, { 0.5f, 0.5f }, validity).Accepted();

        // Both keep no history this frame — the weight is the same.
        EXPECT_NEAR(EvaluateTemporalReactivity(thinned, base, reactivitySettings).Confidence(), 0.0f, 1.0e-6f);
        EXPECT_NEAR(EvaluateTemporalReactivity(collapsed, base, reactivitySettings).Confidence(), 0.0f, 1.0e-6f);

        // They differ in whether the accumulator survives.
        EXPECT_TRUE(thinnedAccepted) << "a thinning short of the rejection threshold must not reset the accumulator";
        EXPECT_FALSE(collapsedAccepted);

        TemporalMoments converged{};
        converged.First = glm::vec4(0.5f);
        converged.Second = glm::vec4(0.25f);
        converged.HistoryLength = 64.0f;
        const glm::vec4 current(0.3f);

        EXPECT_GT(AccumulateTemporalMoments(current, converged, thinnedAccepted).HistoryLength, 60.0f);
        EXPECT_FLOAT_EQ(AccumulateTemporalMoments(current, converged, collapsedAccepted).HistoryLength, 1.0f);
    }

    // -------------------------------------------------------------------------
    // The instruments themselves
    // -------------------------------------------------------------------------
    // A metric nothing validates is a number, not evidence.
    TEST(TemporalSequenceMetricsContract, ShimmerIsZeroOnAStillSequenceAndPositiveOnAMovingOne)
    {
        const std::vector<std::vector<f32>> still(8u, std::vector<f32>(16u, 0.5f));
        const ShimmerResult stillResult = MeasureShimmer(still);
        EXPECT_EQ(stillResult.FramesCompared, 7u);
        EXPECT_NEAR(stillResult.MeanFrameDelta, 0.0, 1.0e-9);

        std::vector<std::vector<f32>> moving;
        for (u32 frame = 0u; frame < 8u; ++frame)
            moving.emplace_back(16u, (frame % 2u == 0u) ? 0.4f : 0.6f);
        const ShimmerResult movingResult = MeasureShimmer(moving);
        EXPECT_NEAR(movingResult.MeanFrameDelta, 0.2, 1.0e-5);
        EXPECT_NEAR(movingResult.PeakPixelDelta, 0.2, 1.0e-5);
    }

    TEST(TemporalSequenceMetricsContract, AShortOrRaggedSequenceReportsNothingRatherThanAPlausibleNumber)
    {
        const std::vector<std::vector<f32>> single(1u, std::vector<f32>(4u, 0.5f));
        EXPECT_EQ(MeasureShimmer(single).FramesCompared, 0u);

        std::vector<std::vector<f32>> ragged;
        ragged.emplace_back(4u, 0.5f);
        ragged.emplace_back(8u, 0.5f);
        EXPECT_EQ(MeasureShimmer(ragged).FramesCompared, 0u);
    }

    TEST(TemporalSequenceMetricsContract, GhostingDoesNotCallADipThroughTheTargetSettled)
    {
        // A history that overshoots crosses the target and comes back out.
        // Scanning forwards for the first frame under tolerance would call
        // frame 1 settled; the backwards scan must not.
        const std::vector<f32> target(4u, 1.0f);
        std::vector<std::vector<f32>> frames;
        frames.emplace_back(4u, 1.0f); // momentarily on target
        frames.emplace_back(4u, 1.6f); // overshoot
        frames.emplace_back(4u, 1.05f);
        frames.emplace_back(4u, 1.0f);

        const GhostingResult result = MeasureGhosting(frames, target, 0.01);
        EXPECT_EQ(result.SettlingFrames, 3u);
        EXPECT_NEAR(result.PeakResidual, 0.6, 1.0e-5);
        EXPECT_NEAR(result.FinalResidual, 0.0, 1.0e-6);
    }

    TEST(TemporalSequenceMetricsContract, GhostingReportsNeverSettledRatherThanTheLastFrame)
    {
        const std::vector<f32> target(4u, 1.0f);
        const std::vector<std::vector<f32>> frames(4u, std::vector<f32>(4u, 0.2f));
        EXPECT_EQ(MeasureGhosting(frames, target, 0.01).SettlingFrames, kNeverSettled);
    }

    // An empty capture reads as "perfectly still, settled at frame 0" on the
    // headline numbers alone, because there was nothing to compare. That is
    // the failure "a difference assertion cannot catch an empty frame" names,
    // so ComparedPixels is what a caller must check — and it must be zero
    // here and non-zero on a real sequence.
    TEST(TemporalSequenceMetricsContract, AnEmptyCaptureIsDistinguishableFromAConvergedOne)
    {
        const std::vector<std::vector<f32>> empty(6u, std::vector<f32>(16u, 0.0f));
        const ShimmerResult emptyShimmer = MeasureShimmer(empty);
        EXPECT_EQ(emptyShimmer.FramesCompared, 5u) << "the frames were well formed";
        EXPECT_EQ(emptyShimmer.ComparedPixels, 0u) << "but no pixel carried signal";
        EXPECT_NEAR(emptyShimmer.MeanFrameDelta, 0.0, 1.0e-12);

        // A genuinely converged sequence looks the same on MeanFrameDelta and
        // different on ComparedPixels. That is the whole point.
        const std::vector<std::vector<f32>> converged(6u, std::vector<f32>(16u, 0.5f));
        const ShimmerResult convergedShimmer = MeasureShimmer(converged);
        EXPECT_NEAR(convergedShimmer.MeanFrameDelta, 0.0, 1.0e-12);
        EXPECT_EQ(convergedShimmer.ComparedPixels, 16u);

        const std::vector<f32> emptyTarget(16u, 0.0f);
        const GhostingResult emptyGhost = MeasureGhosting(empty, emptyTarget, 0.01);
        EXPECT_EQ(emptyGhost.ComparedPixels, 0u);

        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const std::vector<f32> poisoned(16u, nan);
        const DetailResult poisonedDetail = MeasureDetail(poisoned, poisoned);
        EXPECT_EQ(poisonedDetail.ComparedPixels, 0u);
        EXPECT_NEAR(poisonedDetail.RetainedFraction, 1.0, 1.0e-9)
            << "nothing comparable means nothing lost, consistent with the flat-reference case";
    }

    TEST(TemporalSequenceMetricsContract, DetailLossSeparatesBlurFromAFlatReference)
    {
        std::vector<f32> reference(64u);
        for (u32 i = 0u; i < reference.size(); ++i)
            reference[i] = (i % 2u == 0u) ? 0.2f : 0.8f;

        // A blurred measurement keeps the mean and loses the variance.
        const std::vector<f32> blurred(64u, 0.5f);
        const DetailResult blurredResult = MeasureDetail(blurred, reference);
        EXPECT_GT(blurredResult.ReferenceVariance, 0.0);
        EXPECT_NEAR(blurredResult.MeasuredVariance, 0.0, 1.0e-9);
        EXPECT_NEAR(blurredResult.RetainedFraction, 0.0, 1.0e-6);

        // An exact copy keeps all of it.
        const DetailResult exactResult = MeasureDetail(reference, reference);
        EXPECT_NEAR(exactResult.RetainedFraction, 1.0, 1.0e-6);
        EXPECT_NEAR(exactResult.MeanAbsoluteError, 0.0, 1.0e-9);

        // A flat reference has no detail to lose, and must not report that
        // the measurement destroyed it.
        const std::vector<f32> flat(64u, 0.5f);
        EXPECT_NEAR(MeasureDetail(flat, flat).RetainedFraction, 1.0, 1.0e-6);
    }
} // namespace OloEngine::Tests
