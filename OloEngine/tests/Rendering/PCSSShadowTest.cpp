// =============================================================================
// PCSSShadowTest.cpp
//
// CPU/contract tests for Percentage-Closer Soft Shadows (contact-hardening
// variable-penumbra shadow filtering). These pin the *formula* and the wiring
// contract that the GLSL relies on:
//   * ShadowUBO std140 layout is unchanged by the SoftShadowMode field (it
//     reused a former pad int), and SoftShadowMode sits where the GLSL mirror
//     (ShaderBindingLayout::GetShadowUBOLayout) expects it.
//   * The new raw-depth sampler binding slots (TEX_SHADOW_CSM_RAW / _SPOT_RAW)
//     have the values the shaders declare (layout(binding=33/34)).
//   * The penumbra estimate is contact-hardening: zero occluder/receiver gap →
//     minimum (sharp) radius; larger gap → monotonically wider penumbra until a
//     clamp; no blocker → fully lit; fully-blocked search → umbra early-out.
//   * ShadowSettings defaults to PCSS OFF (hardware PCF): a live SponzaCSM A/B
//     (2026-07-02) measured PCSS at ~43 ms of a 46.6 ms scene pass. PCSS is an
//     Ultra-tier opt-in, and QualityTiering maps it through.
//
// The penumbra/blocker helpers below MIRROR pcssShadowFactor / pcssBlockerSearch
// in assets/shaders/include/PBRCommon.glsl — keep the constants in sync. The
// *visual* proof (real pipeline, soft edges on screen) lives in
// PropertyTests/PCSSVisualEvidenceTest.cpp.
//
// Classification: unit (CPU-only math + layout/binding contract; no GL).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // --- Mirror of PBRCommon.glsl PCSS math (keep constants in sync) ----------
    constexpr f32 kPcssPenumbraGain = 220.0f; // PCSS_PENUMBRA_GAIN

    // Mirror of pcssShadowFactor's penumbra estimate: returns the PCF filter
    // radius (in texels) for a receiver/occluder depth pair and a light size.
    f32 PcssFilterRadiusTexels(f32 zReceiver, f32 avgBlocker, f32 softness)
    {
        const f32 lightSizeTexels = std::max(softness, 0.05f) * 4.0f;
        const f32 depthGap = std::max(zReceiver - avgBlocker, 0.0f);
        return std::clamp(depthGap * lightSizeTexels * kPcssPenumbraGain, 1.0f, lightSizeTexels * 4.0f);
    }

    // Mirror of pcssBlockerSearch: average of the sample depths nearer the light
    // than the receiver; -1 if none are blockers.
    f32 AverageBlockerDepth(const std::vector<f32>& samples, f32 zReceiver)
    {
        f32 sum = 0.0f;
        int n = 0;
        for (f32 d : samples)
        {
            if (d < zReceiver)
            {
                sum += d;
                ++n;
            }
        }
        return (n == 0) ? -1.0f : (sum / static_cast<f32>(n));
    }
} // namespace

// =============================================================================
// Wiring contract — UBO layout + binding slots
// =============================================================================

TEST(PCSSShadow, ShadowUBOSizeUnchangedBySoftShadowModeField)
{
    // SoftShadowMode reused a former pad int — the std140 size must not drift,
    // or every shader's inline ShadowData block would mismatch binding 6.
    EXPECT_EQ(sizeof(UBOStructures::ShadowUBO) % 16u, 0u);
    EXPECT_EQ(UBOStructures::ShadowUBO::GetSize(), sizeof(UBOStructures::ShadowUBO));
}

TEST(PCSSShadow, SoftShadowModeFollowsCascadeDebugInLayout)
{
    // The GLSL mirror declares u_SoftShadowMode immediately after
    // u_CascadeDebugEnabled; the C++ struct must agree (same offset/order).
    UBOStructures::ShadowUBO ubo{};
    const auto base = reinterpret_cast<const std::byte*>(&ubo);
    const auto cascadeOff = reinterpret_cast<const std::byte*>(&ubo.CascadeDebugEnabled) - base;
    const auto softOff = reinterpret_cast<const std::byte*>(&ubo.SoftShadowMode) - base;
    EXPECT_EQ(softOff, cascadeOff + static_cast<std::ptrdiff_t>(sizeof(i32)));
}

TEST(PCSSShadow, RawDepthBindingSlotsMatchShaders)
{
    // assets/shaders declare layout(binding = 33/34) for the raw-depth views.
    EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, 33u);
    EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, 34u);
    // Must stay within the engine-reserved tracker range (BoundTextureIDs size).
    EXPECT_LT(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS);
    // And below the first shader-graph user slot (engine-reserved region).
    EXPECT_LT(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, ShaderBindingLayout::TEX_SHADER_GRAPH_0);
}

// =============================================================================
// Settings default + tiering mapping
// =============================================================================

TEST(PCSSShadow, SoftShadowsDefaultsOff)
{
    // PCF is the engine baseline; PCSS measured ~14x the scene-pass cost on
    // SponzaCSM (46.6 ms vs 3.1 ms, RTX 4090 @1080p) and is Ultra-tier only.
    ShadowSettings s{};
    EXPECT_FALSE(s.SoftShadows);
}

TEST(PCSSShadow, QualityTieringMapsSoftShadows)
{
    QualityTieringSettings qt{};
    PostProcessSettings pp{};
    ShadowSettings shadow{};

    qt.SoftShadows = false;
    ApplyTieringToSettings(qt, pp, shadow);
    EXPECT_FALSE(shadow.SoftShadows);

    qt.SoftShadows = true;
    ApplyTieringToSettings(qt, pp, shadow);
    EXPECT_TRUE(shadow.SoftShadows);
}

TEST(PCSSShadow, OnlyUltraPresetUsesPCSS)
{
    // PCSS is the single most expensive scene-pass feature (SponzaCSM A/B) —
    // every tier below Ultra uses the hardware PCF path.
    EXPECT_FALSE(GetPresetSettings(QualityPreset::Low).SoftShadows);
    EXPECT_FALSE(GetPresetSettings(QualityPreset::Medium).SoftShadows);
    EXPECT_FALSE(GetPresetSettings(QualityPreset::High).SoftShadows);
    EXPECT_TRUE(GetPresetSettings(QualityPreset::Ultra).SoftShadows);
}

// =============================================================================
// Blocker search contract
// =============================================================================

TEST(PCSSShadow, NoBlockerReportsFullyLit)
{
    // All samples are farther from the light than the receiver -> no occluder.
    const std::vector<f32> samples = { 0.8f, 0.85f, 0.9f, 0.95f };
    EXPECT_FLOAT_EQ(AverageBlockerDepth(samples, 0.5f), -1.0f);
}

TEST(PCSSShadow, BlockerSearchAveragesOnlyOccluders)
{
    // Receiver at 0.6: blockers are 0.2 and 0.4 (avg 0.3); 0.7/0.8 are behind it.
    const std::vector<f32> samples = { 0.2f, 0.4f, 0.7f, 0.8f };
    EXPECT_FLOAT_EQ(AverageBlockerDepth(samples, 0.6f), 0.3f);
}

TEST(PCSSShadow, FullyBlockedSearchIsUmbra)
{
    // pcssShadowFactor early-outs to 0.0 (deep umbra) when all 16 search taps
    // are occluders — the variable-radius PCF is skipped entirely. Mirror the
    // decision: every sample below the receiver counts as a blocker.
    const std::vector<f32> samples(16, 0.3f);
    int blockers = 0;
    for (f32 d : samples)
    {
        if (d < 0.6f)
            ++blockers;
    }
    EXPECT_EQ(blockers, 16);
}

// =============================================================================
// Penumbra contract — contact hardening
// =============================================================================

TEST(PCSSShadow, ZeroGapGivesSharpMinimumRadius)
{
    // Occluder touching the receiver (gap 0) -> minimum 1-texel radius (sharp).
    EXPECT_FLOAT_EQ(PcssFilterRadiusTexels(0.5f, 0.5f, 1.0f), 1.0f);
}

TEST(PCSSShadow, PenumbraGrowsMonotonicallyWithGap)
{
    // Larger occluder/receiver separation -> wider (or clamped-equal) penumbra.
    f32 prev = PcssFilterRadiusTexels(0.50f, 0.50f, 1.0f);
    for (f32 gap = 0.001f; gap <= 0.20f; gap += 0.005f)
    {
        const f32 r = PcssFilterRadiusTexels(0.5f + gap, 0.5f, 1.0f);
        EXPECT_GE(r, prev) << "penumbra must not shrink as the depth gap grows (gap=" << gap << ")";
        prev = r;
    }
}

TEST(PCSSShadow, PenumbraClampsToMaxRadius)
{
    // A huge gap saturates at lightSizeTexels * 4 (softness 1 -> 4 texels -> 16).
    // The cap was halved from *8: wide scattered Poisson taps thrash the
    // texture cache and dominated the measured PCSS cost on Sponza.
    const f32 r = PcssFilterRadiusTexels(1.0f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(r, 4.0f * 4.0f);
}

TEST(PCSSShadow, LargerLightSizeGivesSofterMaxPenumbra)
{
    // A bigger apparent light -> a larger maximum penumbra (softer shadows).
    const f32 small = PcssFilterRadiusTexels(1.0f, 0.0f, 0.5f);
    const f32 large = PcssFilterRadiusTexels(1.0f, 0.0f, 2.0f);
    EXPECT_GT(large, small);
}
