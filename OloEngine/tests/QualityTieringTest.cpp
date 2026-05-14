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
