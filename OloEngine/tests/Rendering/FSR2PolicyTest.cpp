// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscalePolicy.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

// =============================================================================
// FSR2 temporal upscaling (#684) — CPU contract tests.
//
// These pin the rules in TemporalUpscalePolicy.h without a GL context, so they
// run in headless CI, mirroring how EASUMathTest / RCASMathTest pin the FSR1
// kernels. Per the CLAUDE.md rendering rule they prove the RULES, not the frame;
// the frame is a separate, motion-based job (see docs/agent-rules/notes-renderer.md
// and the PR's evidence) because every failure mode here produces a plausible
// still image.
//
// What each group is actually guarding against is stated at the group, because a
// test whose failure mode is not written down gets "fixed" by adjusting the
// expectation.
// =============================================================================

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::TemporalUpscalePolicy;

    constexpr ActivationInputs MakeActive() noexcept
    {
        ActivationInputs in;
        in.Mode = UpscaleMode::Quality;
        in.Technique = UpscalerTechnique::Temporal;
        in.BackendAvailable = true;
        in.SceneSampleCount = 1u;
        return in;
    }
} // namespace

// -----------------------------------------------------------------------------
// Activation. The one thing the whole integration hangs off: every consumer reads
// this decision instead of re-deriving it, so if it is wrong it is wrong
// EVERYWHERE consistently — which is much harder to spot than one site
// disagreeing.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, TemporalRunsOnlyWhenModeTechniqueAndBackendAllAgree)
{
    EXPECT_TRUE(ShouldRunTemporalUpscale(MakeActive()));

    // Render scale off: there is nothing to reconstruct, so the technique is moot.
    {
        auto in = MakeActive();
        in.Mode = UpscaleMode::Off;
        EXPECT_FALSE(ShouldRunTemporalUpscale(in));
    }
    // Spatial requested.
    {
        auto in = MakeActive();
        in.Technique = UpscalerTechnique::Spatial;
        EXPECT_FALSE(ShouldRunTemporalUpscale(in));
    }
    // Backend cannot do it (Vulkan, non-Windows, OLO_WITH_FSR2=0, driver refusal).
    {
        auto in = MakeActive();
        in.BackendAvailable = false;
        EXPECT_FALSE(ShouldRunTemporalUpscale(in));
    }
}

TEST(FSR2PolicyTest, MSAAIsAHardGuardNotADegradation)
{
    // The acceptance criterion is explicit that MSAA must be guarded rather than
    // silently broken. A resolve has already averaged the per-pixel depth and
    // motion vectors FSR2 reconstructs from, so this must be false for every
    // sample count above one — not merely "documented as unsupported".
    for (const u32 samples : { 2u, 4u, 8u, 16u })
    {
        auto in = MakeActive();
        in.SceneSampleCount = samples;
        EXPECT_FALSE(ShouldRunTemporalUpscale(in)) << "sample count " << samples;
        EXPECT_TRUE(IsMSAAResolved(samples)) << "sample count " << samples;
    }

    // A single sample is not MSAA. (0 is what an uninitialised spec reads as and
    // must not be treated as "resolved" — that would disable FSR2 on every frame
    // before the scene pass has been sized.)
    EXPECT_FALSE(IsMSAAResolved(1u));
    EXPECT_FALSE(IsMSAAResolved(0u));
}

TEST(FSR2PolicyTest, UnavailableTemporalFallsBackToSpatialNotToNative)
{
    // The point of this one: when FSR2 cannot run we must NOT quietly restore
    // native resolution. The user asked for a performance trade; dropping it
    // would make an unsupported configuration silently cost the frame time it was
    // chosen to save, which is the opposite of what a fallback is for.
    auto in = MakeActive();
    in.BackendAvailable = false;

    EXPECT_FALSE(ShouldRunTemporalUpscale(in));
    EXPECT_TRUE(ShouldRunSpatialUpscale(in));
}

TEST(FSR2PolicyTest, ExactlyOneUpscalerOwnsAnyGivenFrame)
{
    // The graph declares its display-resolution colour target under whichever of
    // the two names is active, and the PostProcessColor alias picks "whichever is
    // valid". Both true would declare two targets into one slot; both true while
    // upscaling would leave the alias picking arbitrarily. Exhaustive over the
    // whole input space rather than spot-checked, because this is the invariant
    // the resource declaration relies on.
    for (const auto mode : { UpscaleMode::Off, UpscaleMode::Quality, UpscaleMode::Balanced,
                             UpscaleMode::Performance, UpscaleMode::UltraPerformance })
    {
        for (const auto technique : { UpscalerTechnique::Spatial, UpscalerTechnique::Temporal })
        {
            for (const bool available : { false, true })
            {
                for (const u32 samples : { 1u, 4u })
                {
                    ActivationInputs in;
                    in.Mode = mode;
                    in.Technique = technique;
                    in.BackendAvailable = available;
                    in.SceneSampleCount = samples;

                    const bool temporal = ShouldRunTemporalUpscale(in);
                    const bool spatial = ShouldRunSpatialUpscale(in);

                    EXPECT_FALSE(temporal && spatial)
                        << "both upscalers claimed the frame: mode " << static_cast<i32>(mode)
                        << " technique " << static_cast<i32>(technique) << " available " << available
                        << " samples " << samples;

                    // Upscaling on => exactly one owner; upscaling off => neither.
                    if (mode == UpscaleMode::Off)
                        EXPECT_FALSE(temporal || spatial);
                    else
                        EXPECT_TRUE(temporal || spatial);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Suppression. These two predicates exist because a pass's ENABLE gate and its
// output resource's DECLARATION gate live in different functions and must agree.
// When they disagreed — the late sharpen pass disabled while UpscalerColor stayed
// declared — UICompositePass selected a resource nothing wrote, the render
// graph's read->writer reachability severed the chain there, and every pass back
// to ScenePass was culled. The frame was black, with no error anywhere. These
// tests pin the predicates; both call sites calling them is what pins the pairing.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, TemporalUpscaleSuppressesEngineTAAWithoutMutatingTheSetting)
{
    // Two temporal accumulators over one history is not a configuration.
    EXPECT_FALSE(ShouldRunEngineTAA(/*taaEnabled=*/true, /*temporalUpscaleActive=*/true));

    // ...and the suppression is exactly that — suppression. The moment the
    // temporal upscaler stops owning the frame, TAA is back, because the user's
    // setting was never touched.
    EXPECT_TRUE(ShouldRunEngineTAA(true, false));

    // TAA off stays off either way.
    EXPECT_FALSE(ShouldRunEngineTAA(false, false));
    EXPECT_FALSE(ShouldRunEngineTAA(false, true));
}

TEST(FSR2PolicyTest, TemporalUpscaleSuppressesTheLateSharpenButNotAnExplicitCASRequest)
{
    // FSR2 runs its own RCAS on HDR before tone mapping; the late pass would be a
    // second sharpen over the same edges.
    EXPECT_FALSE(ShouldRunLateSharpen(/*casEnabled=*/false, UpscaleMode::Quality, /*temporalUpscaleActive=*/true));

    // The FSR1 path still needs it — RCAS is the other half of that upscaler.
    EXPECT_TRUE(ShouldRunLateSharpen(false, UpscaleMode::Quality, false));

    // An EXPLICIT CAS request is honoured regardless: the user asked for a
    // sharpen pass by name, which is a different statement from "the upscaler
    // implies one".
    EXPECT_TRUE(ShouldRunLateSharpen(true, UpscaleMode::Quality, true));
    EXPECT_TRUE(ShouldRunLateSharpen(true, UpscaleMode::Off, true));
    EXPECT_TRUE(ShouldRunLateSharpen(true, UpscaleMode::Off, false));

    // Native with no CAS: nothing to do.
    EXPECT_FALSE(ShouldRunLateSharpen(false, UpscaleMode::Off, false));
}

TEST(FSR2PolicyTest, SuppressionIsNeverTrueWhileTheTemporalUpscalerOwnsTheFrame)
{
    // The property the graph depends on, stated over the whole input space: with
    // FSR2 active, engine TAA never runs, and the late sharpen runs ONLY when it
    // was explicitly asked for by CAS. Anything else means a pass and its
    // resource declaration can disagree.
    for (const auto mode : { UpscaleMode::Quality, UpscaleMode::Balanced, UpscaleMode::Performance,
                             UpscaleMode::UltraPerformance })
    {
        for (const bool taa : { false, true })
        {
            EXPECT_FALSE(ShouldRunEngineTAA(taa, /*temporalUpscaleActive=*/true))
                << "engine TAA ran alongside FSR2 (taaEnabled=" << taa << ")";
        }
        for (const bool cas : { false, true })
        {
            EXPECT_EQ(ShouldRunLateSharpen(cas, mode, /*temporalUpscaleActive=*/true), cas)
                << "the late sharpen disagreed with the explicit CAS request under FSR2 (cas=" << cas
                << ", mode=" << static_cast<i32>(mode) << ")";
        }
    }
}

// -----------------------------------------------------------------------------
// Render extent. FSR2 is TOLD which region of its input textures holds the
// frame; a one-texel disagreement with what the scene band was actually sized to
// offsets every reprojection by a fraction of a pixel, which reads as softness.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, RenderExtentMatchesThePipelineSceneBandSizing)
{
    // These are the exact expressions RenderPipeline uses to size the scene band
    // (floor of display * scale). Restated here on purpose: if someone changes one
    // of the two, this fails rather than the image quietly degrading.
    EXPECT_EQ(RenderExtentFromDisplay(1920u, 0.667f), static_cast<u32>(std::floor(1920.0f * 0.667f)));
    EXPECT_EQ(RenderExtentFromDisplay(1080u, 0.667f), static_cast<u32>(std::floor(1080.0f * 0.667f)));
    EXPECT_EQ(RenderExtentFromDisplay(1920u, 0.59f), static_cast<u32>(std::floor(1920.0f * 0.59f)));
    EXPECT_EQ(RenderExtentFromDisplay(1920u, 1.0f), 1920u);
}

TEST(FSR2PolicyTest, RenderExtentNeverCollapsesToZeroOrTakesANonFiniteScale)
{
    // A zero extent would be handed to FSR2 as a render size and to GL as a
    // viewport. A non-finite scale can only arrive from persisted settings, which
    // SanitizeUpscale is supposed to have caught — this is the second line.
    EXPECT_GE(RenderExtentFromDisplay(1u, 0.25f), 1u);
    EXPECT_GE(RenderExtentFromDisplay(0u, 0.5f), 1u);
    EXPECT_EQ(RenderExtentFromDisplay(1920u, std::numeric_limits<f32>::quiet_NaN()), 1920u);
    EXPECT_EQ(RenderExtentFromDisplay(1920u, std::numeric_limits<f32>::infinity()), 1920u);

    // Below the clamp floor the scale saturates rather than shrinking further.
    EXPECT_EQ(RenderExtentFromDisplay(1920u, 0.01f), RenderExtentFromDisplay(1920u, 0.25f));
}

// -----------------------------------------------------------------------------
// Jitter. THE quiet failure: dividing by the display extent instead of the render
// extent leaves a working-looking temporal upscaler that reconstructs nothing.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, JitterIsExpressedAgainstTheRenderExtentNotTheDisplayExtent)
{
    constexpr u32 displayW = 1920u;
    constexpr u32 displayH = 1080u;
    const u32 renderW = RenderExtentFromDisplay(displayW, 0.667f);
    const u32 renderH = RenderExtentFromDisplay(displayH, 0.667f);

    // Half a RENDER pixel of jitter must move the projection by half a render
    // pixel in NDC — i.e. 1 / renderW, not 1 / displayW. At a 0.667 scale the two
    // differ by ~1.5x, which is precisely the amount of sub-pixel coverage that
    // silently goes missing.
    const glm::vec2 ndc = JitterPixelsToNDC(glm::vec2(0.5f, 0.5f), renderW, renderH);
    EXPECT_FLOAT_EQ(ndc.x, 1.0f / static_cast<f32>(renderW));
    EXPECT_FLOAT_EQ(ndc.y, 1.0f / static_cast<f32>(renderH));

    const glm::vec2 wrong = JitterPixelsToNDC(glm::vec2(0.5f, 0.5f), displayW, displayH);
    EXPECT_GT(ndc.x, wrong.x) << "jitter computed against the display extent would be smaller — the bug this guards";
}

TEST(FSR2PolicyTest, JitterIsLinearSignPreservingAndZeroSafe)
{
    EXPECT_EQ(JitterPixelsToNDC(glm::vec2(0.5f, -0.5f), 0u, 720u), glm::vec2(0.0f));
    EXPECT_EQ(JitterPixelsToNDC(glm::vec2(0.5f, -0.5f), 1280u, 0u), glm::vec2(0.0f));

    const glm::vec2 positive = JitterPixelsToNDC(glm::vec2(0.25f, 0.25f), 1280u, 720u);
    const glm::vec2 negative = JitterPixelsToNDC(glm::vec2(-0.25f, -0.25f), 1280u, 720u);
    EXPECT_FLOAT_EQ(positive.x, -negative.x);
    EXPECT_FLOAT_EQ(positive.y, -negative.y);

    const glm::vec2 doubled = JitterPixelsToNDC(glm::vec2(0.5f, 0.5f), 1280u, 720u);
    EXPECT_FLOAT_EQ(doubled.x, 2.0f * positive.x);
    EXPECT_FLOAT_EQ(doubled.y, 2.0f * positive.y);

    EXPECT_EQ(JitterPixelsToNDC(glm::vec2(0.0f), 1280u, 720u), glm::vec2(0.0f));
}

// -----------------------------------------------------------------------------
// Motion vectors. The most fragile number in the integration.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, MotionVectorScaleInvertsTheEngineConventionAndConvertsToPixels)
{
    constexpr u32 renderW = 1280u;
    constexpr u32 renderH = 720u;
    const glm::vec2 mvScale = MotionVectorScale(renderW, renderH);

    // Magnitude: the engine writes UV-space displacement, FSR2 reads pixels.
    EXPECT_FLOAT_EQ(std::abs(mvScale.x), static_cast<f32>(renderW));
    EXPECT_FLOAT_EQ(std::abs(mvScale.y), static_cast<f32>(renderH));

    // Sign: the engine writes (curr - prev); FSR2 wants a vector pointing BACK to
    // the previous position. A positive scale here is the classic sign error that
    // makes every moving object trail in the direction it is travelling instead of
    // reprojecting correctly — the image still looks like an image.
    EXPECT_LT(mvScale.x, 0.0f);
    EXPECT_LT(mvScale.y, 0.0f);

    // Worked example. A surface that moved right by exactly one render pixel
    // between frames gets o_Velocity.x = +1/renderW (curr - prev, UV space); FSR2
    // must be told to look one pixel LEFT for its history.
    const f32 engineVelocityX = 1.0f / static_cast<f32>(renderW);
    EXPECT_FLOAT_EQ(engineVelocityX * mvScale.x, -1.0f);
}

// -----------------------------------------------------------------------------
// Settings validation. Persisted settings reach here from scene YAML, which
// CLAUDE.md requires be validated with std::isfinite.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, SanitizeRejectsAnUnknownTechniqueRatherThanSaturatingIntoTheOtherOne)
{
    // Technique is a DISCRIMINATED value. Clamping an unknown ordinal to the
    // nearest valid one would silently select a real, working, DIFFERENT algorithm
    // — a scene authored against a future third technique would come back running
    // FSR2 with no indication anything was lost. It must fall back to the default.
    PostProcessSettings s;
    s.Technique = static_cast<UpscalerTechnique>(7);
    SanitizeUpscale(s);
    EXPECT_EQ(s.Technique, UpscalerTechnique::Spatial);

    s.Technique = static_cast<UpscalerTechnique>(-3);
    SanitizeUpscale(s);
    EXPECT_EQ(s.Technique, UpscalerTechnique::Spatial);

    // Valid values survive untouched.
    for (const auto technique : { UpscalerTechnique::Spatial, UpscalerTechnique::Temporal })
    {
        s.Technique = technique;
        SanitizeUpscale(s);
        EXPECT_EQ(s.Technique, technique);
    }
}

TEST(FSR2PolicyTest, SanitizeClampsFSR2SharpnessAndRejectsNonFinite)
{
    PostProcessSettings s;

    s.FSR2Sharpness = 5.0f;
    SanitizeUpscale(s);
    EXPECT_FLOAT_EQ(s.FSR2Sharpness, 1.0f);

    s.FSR2Sharpness = -2.0f;
    SanitizeUpscale(s);
    EXPECT_FLOAT_EQ(s.FSR2Sharpness, 0.0f);

    s.FSR2Sharpness = std::numeric_limits<f32>::quiet_NaN();
    SanitizeUpscale(s);
    EXPECT_FLOAT_EQ(s.FSR2Sharpness, 0.5f);

    s.FSR2Sharpness = std::numeric_limits<f32>::infinity();
    SanitizeUpscale(s);
    EXPECT_FLOAT_EQ(s.FSR2Sharpness, 0.5f);
}

TEST(FSR2PolicyTest, DefaultSettingsLeaveTemporalUpscalingOff)
{
    // A default scene must render exactly as it did before this feature existed —
    // no render scale, no technique change, no jitter.
    const PostProcessSettings s;
    EXPECT_EQ(s.Upscale, UpscaleMode::Off);
    EXPECT_EQ(s.Technique, UpscalerTechnique::Spatial);

    ActivationInputs in;
    in.Mode = s.Upscale;
    in.Technique = s.Technique;
    in.BackendAvailable = true;
    EXPECT_FALSE(ShouldRunTemporalUpscale(in));
    EXPECT_FALSE(ShouldRunSpatialUpscale(in));
}

// -----------------------------------------------------------------------------
// The unavailable-backend object. A caller must always be able to ask WHY.
// -----------------------------------------------------------------------------

TEST(FSR2PolicyTest, EveryUpscalerStatusHasADistinctHumanReadableReason)
{
    // The fallback log line is the only thing that tells a user why their FSR2
    // setting did nothing, so an unmapped enumerator reading "unknown" (or two
    // sharing a string) is a real loss of information.
    const std::array statuses = {
        TemporalUpscalerStatus::Available,
        TemporalUpscalerStatus::NotCompiledIn,
        TemporalUpscalerStatus::BackendUnsupported,
        TemporalUpscalerStatus::DeviceUnsupported,
        TemporalUpscalerStatus::NotConfigured,
    };

    for (sizet i = 0; i < statuses.size(); ++i)
    {
        const std::string_view text = ToString(statuses[i]);
        EXPECT_FALSE(text.empty());
        EXPECT_NE(text, "unknown") << "status " << static_cast<i32>(statuses[i]) << " has no reason string";
        for (sizet j = i + 1; j < statuses.size(); ++j)
            EXPECT_NE(text, ToString(statuses[j])) << "two statuses share a reason string";
    }
}
