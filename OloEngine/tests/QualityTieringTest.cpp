#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/CVar.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h" // RendererSettings, for the tier-to-renderer copy below

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

// =============================================================================
// Multi-animal scheduling budget across tiers (issue #1258, criterion 4)
// =============================================================================

TEST(QualityTiering, TheAnimalFrameBudgetIsOrderedByTier)
{
    // A TIER IS A BUDGET HERE, NOT A FEATURE SWITCH, and the ordering is what
    // makes "tested across quality tiers" mean anything: the same population
    // is scheduled against a different allowance on each tier, so the tiers
    // differ by how much work the herd may spend rather than by something
    // being turned off. A flat ladder would make every per-tier capture the
    // same picture.
    const auto low = GetPresetSettings(QualityPreset::Low);
    const auto med = GetPresetSettings(QualityPreset::Medium);
    const auto high = GetPresetSettings(QualityPreset::High);
    const auto ultra = GetPresetSettings(QualityPreset::Ultra);

    EXPECT_LT(low.AnimalFrameBudgetUnits, med.AnimalFrameBudgetUnits);
    EXPECT_LT(med.AnimalFrameBudgetUnits, high.AnimalFrameBudgetUnits);
    EXPECT_LT(high.AnimalFrameBudgetUnits, ultra.AnimalFrameBudgetUnits);
}

TEST(QualityTiering, SchedulingIsOnAtEveryTierIncludingLow)
{
    // NOT the DDGI rule, and the difference is worth stating. Realtime GI is a
    // whole feature to shed, so Low turns it off. The population budget is the
    // opposite kind of knob: it is what makes a herd affordable at all, so the
    // weakest tier is the one that needs it MOST. Turning it off on Low would
    // run every animal at full rate on the machine least able to.
    for (const auto preset :
         { QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra })
    {
        EXPECT_TRUE(GetPresetSettings(preset).AnimalSchedulingEnabled)
            << QualityPresetToString(preset) << " leaves the population unbudgeted";
    }
}

TEST(QualityTiering, TheAnimalBudgetReachesRendererSettings)
{
    // ApplyTieringToRendererSettings is a hand-written field-by-field copy, so
    // a field added to the tier struct and not to that function is a slider
    // that does nothing — silently, and only for the fields nobody checked.
    // Same failure the DDGI knobs above are guarded against.
    RendererSettings renderer;
    ApplyTieringToRendererSettings(GetPresetSettings(QualityPreset::Ultra), renderer);
    EXPECT_FLOAT_EQ(renderer.AnimalFrameBudgetUnits,
                    GetPresetSettings(QualityPreset::Ultra).AnimalFrameBudgetUnits);
    EXPECT_TRUE(renderer.AnimalSchedulingEnabled);

    ApplyTieringToRendererSettings(GetPresetSettings(QualityPreset::Low), renderer);
    EXPECT_FLOAT_EQ(renderer.AnimalFrameBudgetUnits,
                    GetPresetSettings(QualityPreset::Low).AnimalFrameBudgetUnits);
}

// =============================================================================
// Groom detail bias across tiers (issue #1616)
// =============================================================================

TEST(QualityTiering, TheGroomDetailBiasIsOrderedByTierAndHighIsAsAuthored)
{
    // High is EXACTLY 1, the bias that is bit-identical to none: every groom
    // capture committed before the bias existed was taken at High's numbers.
    EXPECT_FLOAT_EQ(GetPresetSettings(QualityPreset::High).GroomDetailBias, 1.0f);
    EXPECT_FLOAT_EQ(GetPresetSettings(QualityPreset::Custom).GroomDetailBias, 1.0f);
    EXPECT_FLOAT_EQ(QualityTieringSettings{}.GroomDetailBias, 1.0f) << "the struct default is High's value";

    const f32 low = GetPresetSettings(QualityPreset::Low).GroomDetailBias;
    const f32 med = GetPresetSettings(QualityPreset::Medium).GroomDetailBias;
    const f32 high = GetPresetSettings(QualityPreset::High).GroomDetailBias;
    const f32 ultra = GetPresetSettings(QualityPreset::Ultra).GroomDetailBias;
    EXPECT_LT(low, med);
    EXPECT_LT(med, high);
    EXPECT_LT(high, ultra);
    for (const f32 bias : { low, med, high, ultra })
    {
        EXPECT_FLOAT_EQ(SanitizeGroomDetailBias(bias), bias) << "a preset outside the range the sanitiser keeps";
    }
}

TEST(QualityTiering, TheGroomDetailBiasReachesRendererSettings)
{
    // The same hand-written copy the animal budget is guarded against.
    RendererSettings renderer;
    ApplyTieringToRendererSettings(GetPresetSettings(QualityPreset::Low), renderer);
    EXPECT_FLOAT_EQ(renderer.GroomDetailBias, GetPresetSettings(QualityPreset::Low).GroomDetailBias);
    ApplyTieringToRendererSettings(GetPresetSettings(QualityPreset::Ultra), renderer);
    EXPECT_FLOAT_EQ(renderer.GroomDetailBias, GetPresetSettings(QualityPreset::Ultra).GroomDetailBias);
}

TEST(QualityTiering, TheGroomDetailBiasConsoleVariableIsAViewOfTheRendererSetting)
{
    RegisterGroomDetailBiasCVar();
    RegisterGroomDetailBiasCVar(); // idempotent: one cvar, not two

    RendererSettings& settings = Renderer3D::GetRendererSettings();
    const f32 saved = settings.GroomDetailBias;
    settings.GroomDetailBias = 1.0f;

    const auto found = CVars::Find("olo_groom_detail_bias");
    ASSERT_TRUE(found.has_value()) << "the console variable is not registered";
    EXPECT_EQ(found->Type, CVars::CVarType::Float);
    EXPECT_TRUE(found->IsDefault);
    EXPECT_EQ(found->Value, "1");

    const CVars::SetResult set = CVars::SetFromString("OLO_GROOM_DETAIL_BIAS", "2");
    EXPECT_TRUE(set.Ok) << set.Error;
    EXPECT_FLOAT_EQ(settings.GroomDetailBias, 2.0f) << "a console write must land in the setting the frame reads";
    EXPECT_FALSE(CVars::Find("OLO_GROOM_DETAIL_BIAS")->IsDefault);

    // A preset applied afterwards wins: the cvar has no copy of its own.
    ApplyTieringToRendererSettings(GetPresetSettings(QualityPreset::Low), settings);
    EXPECT_EQ(CVars::Find("OLO_GROOM_DETAIL_BIAS")->Value, "0.5");

    // Refused by name rather than clamped or swallowed.
    for (const char* bad : { "nan", "inf", "9", "0", "-1", "two" })
    {
        const CVars::SetResult refused = CVars::SetFromString("OLO_GROOM_DETAIL_BIAS", bad);
        EXPECT_FALSE(refused.Ok) << "'" << bad << "' was accepted";
        EXPECT_FALSE(refused.Error.empty()) << "'" << bad << "' was refused without a reason";
    }
    EXPECT_FLOAT_EQ(settings.GroomDetailBias, 0.5f) << "a refused write moved the setting";

    settings.GroomDetailBias = saved;
}
