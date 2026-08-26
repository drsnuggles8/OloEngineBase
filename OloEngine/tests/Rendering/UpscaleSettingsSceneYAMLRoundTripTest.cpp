// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// UpscaleSettingsSceneYAMLRoundTripTest
//
// Scene-level PostProcessSettings are NOT covered by the component-serializer
// codegen — they are a hand-written block in SceneSerializer.cpp, written in two
// places (the scene writer and the scene-settings writer) and read in one. That
// makes a forgotten field a SILENT drop: the setting exists, the editor edits it,
// the frame honours it, and it vanishes the moment the scene is saved and
// reopened. Nothing else in the suite catches that for this struct.
//
// So this pins the FSR2 upscale settings (#684) through a real
// SerializeToYAML -> DeserializeFromYAML round-trip, alongside the FSR1 fields
// they sit next to (which had no round-trip coverage either — if this file ever
// fails on RCASSharpness, that is a genuine pre-existing gap being caught, not a
// bad expectation).
// =============================================================================

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <string>

using namespace OloEngine;

namespace
{
    // Deliberately NOT the defaults: a writer that emits nothing and a reader
    // that leaves the fresh Scene's defaults in place would both "pass" against
    // default values.
    [[nodiscard]] PostProcessSettings MakeNonDefaultUpscaleSettings()
    {
        PostProcessSettings s;
        s.Upscale = UpscaleMode::Balanced;
        s.RCASSharpness = 0.375f;
        s.Technique = UpscalerTechnique::Temporal;
        s.FSR2SharpeningEnabled = false;
        s.FSR2Sharpness = 0.875f;
        return s;
    }

    [[nodiscard]] Ref<Scene> RoundTrip(const PostProcessSettings& settings)
    {
        Ref<Scene> source = Scene::Create();
        source->SetRenderingEnabled(false);
        source->SetPostProcessSettings(settings);

        SceneSerializer writer(source);
        const std::string yaml = writer.SerializeToYAML();
        if (yaml.empty())
            return nullptr;

        Ref<Scene> restored = Scene::Create();
        restored->SetRenderingEnabled(false);
        SceneSerializer reader(restored);
        if (!reader.DeserializeFromYAML(yaml))
            return nullptr;

        return restored;
    }
} // namespace

TEST(UpscaleSettingsSceneYAMLRoundTripTest, TemporalUpscaleSettingsSurviveASaveAndReopen)
{
    const PostProcessSettings authored = MakeNonDefaultUpscaleSettings();
    const Ref<Scene> restored = RoundTrip(authored);
    ASSERT_TRUE(restored) << "the scene failed to round-trip through YAML at all";

    const PostProcessSettings& loaded = restored->GetPostProcessSettings();

    EXPECT_EQ(loaded.Technique, authored.Technique)
        << "UpscaleTechnique did not survive the round-trip — a scene authored with FSR2 silently "
           "reopens on the FSR1 spatial upscaler, at the same render scale, looking merely 'worse' "
           "with nothing to point at.";
    EXPECT_EQ(loaded.FSR2SharpeningEnabled, authored.FSR2SharpeningEnabled)
        << "FSR2SharpeningEnabled did not survive the round-trip";
    EXPECT_FLOAT_EQ(loaded.FSR2Sharpness, authored.FSR2Sharpness)
        << "FSR2Sharpness did not survive the round-trip";

    // The FSR1 fields the new ones are written beside, so a future edit that
    // rewrites the block cannot drop one half of it unnoticed.
    EXPECT_EQ(loaded.Upscale, authored.Upscale) << "Upscale did not survive the round-trip";
    EXPECT_FLOAT_EQ(loaded.RCASSharpness, authored.RCASSharpness)
        << "RCASSharpness did not survive the round-trip";
}

TEST(UpscaleSettingsSceneYAMLRoundTripTest, AnOlderSceneWithNoUpscaleKeysLoadsAsSpatial)
{
    // Backward compatibility: every scene authored before #684 has no
    // UpscaleTechnique key. The reader must leave the default in place rather
    // than reading a missing node as zero-ish garbage or refusing the scene —
    // and the default must be Spatial, so those scenes render exactly as they
    // did before this feature existed.
    Ref<Scene> source = Scene::Create();
    source->SetRenderingEnabled(false);
    PostProcessSettings legacy;
    legacy.Upscale = UpscaleMode::Quality;
    source->SetPostProcessSettings(legacy);

    SceneSerializer writer(source);
    std::string yaml = writer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());

    // Strip the three keys this feature added, reproducing a pre-#684 file.
    for (const std::string_view key : { "UpscaleTechnique", "FSR2SharpeningEnabled", "FSR2Sharpness" })
    {
        for (;;)
        {
            const std::size_t at = yaml.find(key);
            if (at == std::string::npos)
                break;
            const std::size_t lineStart = yaml.rfind('\n', at);
            const std::size_t lineEnd = yaml.find('\n', at);
            ASSERT_NE(lineStart, std::string::npos);
            yaml.erase(lineStart, (lineEnd == std::string::npos ? yaml.size() : lineEnd) - lineStart);
        }
    }

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer reader(restored);
    ASSERT_TRUE(reader.DeserializeFromYAML(yaml))
        << "a scene without the #684 keys must still load — the reader may not require them";

    const PostProcessSettings& loaded = restored->GetPostProcessSettings();
    EXPECT_EQ(loaded.Technique, UpscalerTechnique::Spatial)
        << "a pre-#684 scene came back on the temporal upscaler — existing scenes must render "
           "exactly as they did before the feature existed";
    EXPECT_EQ(loaded.Upscale, UpscaleMode::Quality)
        << "removing the new keys disturbed the FSR1 render-scale preset next to them";
}

TEST(UpscaleSettingsSceneYAMLRoundTripTest, ExplicitNoAOSurvivesSaveReopenAndSceneCopy)
{
    PostProcessSettings authored;
    authored.ActiveAOTechnique = AOTechnique::None;
    authored.m_AOTechniqueOverride = true;
    authored.SSAOEnabled = false;
    authored.GTAOEnabled = false;

    const Ref<Scene> restored = RoundTrip(authored);
    ASSERT_TRUE(restored) << "an authored AO selection must not make the scene fail to load";

    const PostProcessSettings& loaded = restored->GetPostProcessSettings();
    EXPECT_TRUE(loaded.m_AOTechniqueOverride)
        << "the presence of ActiveAOTechnique must mark the selection as scene-authored";
    EXPECT_EQ(loaded.ActiveAOTechnique, AOTechnique::None);
    EXPECT_FALSE(loaded.SSAOEnabled);
    EXPECT_FALSE(loaded.GTAOEnabled);

    // Entering Play Mode clones the scene. The opt-out must survive that
    // transition too, not merely a disk reopen.
    Ref<Scene> mutableRestored = restored;
    const Ref<Scene> copy = Scene::Copy(mutableRestored);
    ASSERT_TRUE(copy);
    EXPECT_TRUE(copy->GetPostProcessSettings().m_AOTechniqueOverride);
    EXPECT_EQ(copy->GetPostProcessSettings().ActiveAOTechnique, AOTechnique::None);

    PostProcessSettings runtime = loaded;
    ShadowSettings shadow;
    ApplyTieringToSettings(GetPresetSettings(QualityPreset::High), runtime, shadow);
    EXPECT_EQ(runtime.ActiveAOTechnique, AOTechnique::None)
        << "quality tiering must not re-enable AO after an authored scene opt-out";
    EXPECT_FALSE(runtime.GTAOEnabled);
}

TEST(UpscaleSettingsSceneYAMLRoundTripTest, ASceneWithoutAOSelectorKeepsLegacyTierBehavior)
{
    // The writer intentionally omits ActiveAOTechnique unless a scene opted
    // out of tier ownership. That preserves every existing scene's quality
    // preset behavior while allowing new scenes to author None/SSAO/GTAO.
    PostProcessSettings legacy;
    legacy.ActiveAOTechnique = AOTechnique::None;
    legacy.SSAOEnabled = false;
    legacy.GTAOEnabled = false;

    Ref<Scene> source = Scene::Create();
    source->SetRenderingEnabled(false);
    source->SetPostProcessSettings(legacy);
    SceneSerializer writer(source);
    const std::string yaml = writer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());
    EXPECT_EQ(yaml.find("ActiveAOTechnique"), std::string::npos);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    PostProcessSettings stale;
    stale.m_AOTechniqueOverride = true;
    restored->SetPostProcessSettings(stale);
    SceneSerializer reader(restored);
    ASSERT_TRUE(reader.DeserializeFromYAML(yaml));
    EXPECT_FALSE(restored->GetPostProcessSettings().m_AOTechniqueOverride);

    PostProcessSettings runtime = restored->GetPostProcessSettings();
    ShadowSettings shadow;
    ApplyTieringToSettings(GetPresetSettings(QualityPreset::High), runtime, shadow);
    EXPECT_EQ(runtime.ActiveAOTechnique, AOTechnique::GTAO);
    EXPECT_TRUE(runtime.GTAOEnabled);
}

TEST(UpscaleSettingsSceneYAMLRoundTripTest, AnInvalidAOSelectorFallsBackToLegacyTierBehavior)
{
    PostProcessSettings authored;
    authored.ActiveAOTechnique = AOTechnique::None;
    authored.m_AOTechniqueOverride = true;
    authored.SSAOEnabled = false;
    authored.GTAOEnabled = false;

    Ref<Scene> source = Scene::Create();
    source->SetRenderingEnabled(false);
    source->SetPostProcessSettings(authored);
    SceneSerializer writer(source);
    std::string yaml = writer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());

    const std::size_t selector = yaml.find("ActiveAOTechnique: 0");
    ASSERT_NE(selector, std::string::npos);
    yaml.replace(selector, std::string_view("ActiveAOTechnique: 0").size(), "ActiveAOTechnique: 99");

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    PostProcessSettings stale;
    stale.m_AOTechniqueOverride = true;
    restored->SetPostProcessSettings(stale);
    SceneSerializer reader(restored);
    ASSERT_TRUE(reader.DeserializeFromYAML(yaml));
    EXPECT_FALSE(restored->GetPostProcessSettings().m_AOTechniqueOverride);

    PostProcessSettings runtime = restored->GetPostProcessSettings();
    ShadowSettings shadow;
    ApplyTieringToSettings(GetPresetSettings(QualityPreset::High), runtime, shadow);
    EXPECT_EQ(runtime.ActiveAOTechnique, AOTechnique::GTAO);
    EXPECT_TRUE(runtime.GTAOEnabled);
}
