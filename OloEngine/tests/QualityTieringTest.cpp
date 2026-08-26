#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/QualityTiering.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Preset Values
// =============================================================================

// LowPresetDisablesExpensiveFeatures / UltraPresetEnablesEverything /
// CustomReturnsHighDefaults retired — design-choice pinning on preset
// LUT contents. The monotonicity invariant below is the real contract.
// See docs/testing.md §4.1.

TEST(QualityTiering, PresetsAreOrderedByQuality)
{
    auto low = GetPresetSettings(QualityPreset::Low);
    auto med = GetPresetSettings(QualityPreset::Medium);
    auto high = GetPresetSettings(QualityPreset::High);
    auto ultra = GetPresetSettings(QualityPreset::Ultra);

    EXPECT_LE(low.ShadowResolution, med.ShadowResolution);
    EXPECT_LE(med.ShadowResolution, high.ShadowResolution);
    EXPECT_LE(high.ShadowResolution, ultra.ShadowResolution);
}

// =============================================================================
// ApplyTieringToSettings
// =============================================================================

TEST(QualityTiering, ApplyWritesShadowSettings)
{
    auto tiering = GetPresetSettings(QualityPreset::Ultra);
    PostProcessSettings pp;
    ShadowSettings shadow;

    ApplyTieringToSettings(tiering, pp, shadow);

    EXPECT_EQ(shadow.Resolution, tiering.ShadowResolution);
    EXPECT_FLOAT_EQ(shadow.Softness, tiering.ShadowSoftness);
    EXPECT_EQ(shadow.Enabled, tiering.ShadowEnabled);
}

TEST(QualityTiering, ApplyWritesPostProcessSettings)
{
    auto tiering = GetPresetSettings(QualityPreset::Low);
    PostProcessSettings pp;
    ShadowSettings shadow;

    ApplyTieringToSettings(tiering, pp, shadow);

    EXPECT_EQ(pp.BloomEnabled, tiering.BloomEnabled);
    EXPECT_EQ(pp.FXAAEnabled, tiering.FXAAEnabled);
    EXPECT_EQ(pp.DOFEnabled, tiering.DOFEnabled);
    EXPECT_EQ(pp.MotionBlurEnabled, tiering.MotionBlurEnabled);
    EXPECT_EQ(pp.VignetteEnabled, tiering.VignetteEnabled);
    EXPECT_EQ(pp.ChromaticAberrationEnabled, tiering.ChromaticAberrationEnabled);
}

TEST(QualityTiering, AuthoredNoAOOverridesThePreset)
{
    const auto tiering = GetPresetSettings(QualityPreset::High);
    PostProcessSettings pp;
    ShadowSettings shadow;

    pp.ActiveAOTechnique = AOTechnique::None;
    pp.m_AOTechniqueOverride = true;
    pp.SSAOEnabled = false;
    pp.GTAOEnabled = false;

    ApplyTieringToSettings(tiering, pp, shadow);

    EXPECT_EQ(pp.ActiveAOTechnique, AOTechnique::None);
    EXPECT_FALSE(pp.SSAOEnabled);
    EXPECT_FALSE(pp.GTAOEnabled);
}

TEST(QualityTiering, LegacyAOSelectionRemainsTierControlled)
{
    const auto tiering = GetPresetSettings(QualityPreset::High);
    PostProcessSettings pp;
    ShadowSettings shadow;

    // The technique field alone is not an override: scenes written before the
    // key existed must still adopt the project's tier when they are opened.
    pp.ActiveAOTechnique = AOTechnique::None;
    pp.SSAOEnabled = false;
    pp.GTAOEnabled = false;

    ApplyTieringToSettings(tiering, pp, shadow);

    EXPECT_FALSE(pp.m_AOTechniqueOverride);
    EXPECT_EQ(pp.ActiveAOTechnique, AOTechnique::GTAO);
    EXPECT_FALSE(pp.SSAOEnabled);
    EXPECT_TRUE(pp.GTAOEnabled);
}

TEST(QualityTiering, SavingPreservesAnAuthoredNoAOSelection)
{
    PostProcessSettings renderer;
    renderer.ActiveAOTechnique = AOTechnique::None;
    renderer.m_AOTechniqueOverride = true;
    renderer.SSAOEnabled = false;
    renderer.GTAOEnabled = false;

    // A legacy scene can still carry un-tiered AO values while the renderer has
    // an explicit selection made in the post-process panel. Saving strips the
    // tier overlay through this helper, so it must keep the authored choice.
    PostProcessSettings scene;
    scene.ActiveAOTechnique = AOTechnique::SSAO;
    scene.SSAOEnabled = false;
    scene.GTAOEnabled = false;

    const PostProcessSettings saved = StripTieringOverlay(renderer, scene);

    EXPECT_TRUE(saved.m_AOTechniqueOverride);
    EXPECT_EQ(saved.ActiveAOTechnique, AOTechnique::None);
    EXPECT_FALSE(saved.SSAOEnabled);
    EXPECT_FALSE(saved.GTAOEnabled);
}

// =============================================================================
// String Conversions
// =============================================================================

TEST(QualityTiering, StringRoundTrip)
{
    for (auto preset : { QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra, QualityPreset::Custom })
    {
        auto str = QualityPresetToString(preset);
        EXPECT_FALSE(str.empty());
        auto parsed = QualityPresetFromString(str);
        EXPECT_EQ(parsed, preset);
    }
}

TEST(QualityTiering, UnknownStringDefaultsToHigh)
{
    EXPECT_EQ(QualityPresetFromString("garbage"), QualityPreset::High);
    EXPECT_EQ(QualityPresetFromString(""), QualityPreset::High);
}
