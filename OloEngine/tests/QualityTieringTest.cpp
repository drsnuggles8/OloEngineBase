#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/QualityTiering.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Preset Values
// =============================================================================

TEST(QualityTiering, LowPresetDisablesExpensiveFeatures)
{
    auto s = GetPresetSettings(QualityPreset::Low);
    EXPECT_EQ(s.Preset, QualityPreset::Low);
    EXPECT_FALSE(s.ShadowEnabled);
    EXPECT_FALSE(s.BloomEnabled);
    EXPECT_FALSE(s.FXAAEnabled);
    EXPECT_FALSE(s.DOFEnabled);
    EXPECT_FALSE(s.MotionBlurEnabled);
    EXPECT_FALSE(s.VignetteEnabled);
    EXPECT_FALSE(s.ChromaticAberrationEnabled);
    EXPECT_EQ(s.AO, AOTechnique::None);
}

TEST(QualityTiering, UltraPresetEnablesEverything)
{
    auto s = GetPresetSettings(QualityPreset::Ultra);
    EXPECT_EQ(s.Preset, QualityPreset::Ultra);
    EXPECT_TRUE(s.ShadowEnabled);
    EXPECT_TRUE(s.BloomEnabled);
    EXPECT_TRUE(s.FXAAEnabled);
    EXPECT_TRUE(s.DOFEnabled);
    EXPECT_TRUE(s.MotionBlurEnabled);
    EXPECT_TRUE(s.VignetteEnabled);
    EXPECT_TRUE(s.ChromaticAberrationEnabled);
    EXPECT_EQ(s.AO, AOTechnique::GTAO);
    EXPECT_GE(s.ShadowResolution, 4096u);
}

TEST(QualityTiering, CustomReturnsHighDefaults)
{
    auto custom = GetPresetSettings(QualityPreset::Custom);
    auto high = GetPresetSettings(QualityPreset::High);
    EXPECT_EQ(custom.ShadowResolution, high.ShadowResolution);
    EXPECT_EQ(custom.BloomEnabled, high.BloomEnabled);
    EXPECT_EQ(custom.AO, high.AO);
}

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
