#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// SceneRenderSettingsActivationTest — unit test (headless, no GL).
//
// Pins SceneTransition::ApplySceneRenderSettings, the one seam every host
// activates a scene's render settings through (issue #1563): the editor's
// scene open, the editor's Play-mode scene switch, and OloRuntime's initial
// load, reload and script-driven switch.
//
// The shipped runtime used to load a scene and start it without publishing any
// of the seven settings blocks Renderer3D draws from, so an authored exposure,
// upscale request or fog loaded fine and was never seen. These tests drive the
// seam the way the runtime does — a scene file through LoadSceneFile — and
// read back the renderer's effective settings:
//
//   * every block reaches Renderer3D, not only post-processing,
//   * a second activation replaces the first scene's values instead of
//     leaving any of them behind,
//   * the quality tier lands ON TOP of the authored values, as in the editor,
//   * the manifest's tier block round-trips, because the shipped game only
//     knows the project's tier through game.manifest.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Project/ProjectSerializer.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneTransition.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>
#include <string>

using namespace OloEngine;

namespace
{
    // A scene whose every renderer-mirrored block differs from its default.
    [[nodiscard]] Ref<Scene> AuthoredScene()
    {
        Ref<Scene> scene = Scene::Create();
        Entity camera = scene->CreateEntity("Camera");
        camera.AddComponent<CameraComponent>().Primary = true;

        auto& pp = scene->GetPostProcessSettings();
        pp.Exposure = 2.5f;
        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;
        pp.BloomEnabled = true;
        pp.DOFEnabled = true;

        scene->GetFogSettings().Enabled = true;
        scene->GetFogSettings().Density = 0.07f;
        scene->GetSnowSettings().Enabled = true;
        scene->GetWindSettings().Enabled = true;
        scene->GetWindSettings().Speed = 11.0f;
        scene->GetSnowAccumulationSettings().Enabled = true;
        scene->GetSnowEjectaSettings().Enabled = true;
        scene->GetPrecipitationSettings().Enabled = true;
        scene->GetPrecipitationSettings().Intensity = 0.9f;
        return scene;
    }

    [[nodiscard]] Ref<Scene> DefaultScene()
    {
        Ref<Scene> scene = Scene::Create();
        Entity camera = scene->CreateEntity("Camera");
        camera.AddComponent<CameraComponent>().Primary = true;
        return scene;
    }
} // namespace

class SceneRenderSettingsActivationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Saved = SceneTransition::SceneRenderSettings::CaptureRenderer();
        m_SavedShadow = Renderer3D::GetShadowMap().GetSettings();
        m_SavedRenderer = Renderer3D::GetRendererSettings();
        // Start from a renderer that has seen no scene, so a value that reaches
        // it can only have come through the seam.
        SceneTransition::SceneRenderSettings{}.PublishToRenderer();
        m_Root = OloEngine::Tests::TempDir("scene-render-settings");
    }

    void TearDown() override
    {
        m_Saved.PublishToRenderer();
        Renderer3D::GetShadowMap().SetSettings(m_SavedShadow);
        Renderer3D::GetRendererSettings() = m_SavedRenderer;
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    // Save `scene` and load it back the way the runtime does.
    [[nodiscard]] Ref<Scene> RoundTrip(const Ref<Scene>& scene, const std::string& name) const
    {
        const auto path = m_Root / (name + ".olo");
        SceneSerializer(scene).Serialize(path);
        auto loaded = SceneTransition::LoadSceneFile(path, /*requirePrimaryCamera=*/true);
        EXPECT_TRUE(loaded) << loaded.Error.ToView();
        return loaded.LoadedScene;
    }

    SceneTransition::SceneRenderSettings m_Saved;
    ShadowSettings m_SavedShadow;
    RendererSettings m_SavedRenderer;
    std::filesystem::path m_Root;
};

TEST_F(SceneRenderSettingsActivationTest, EveryAuthoredBlockReachesTheRenderer)
{
    const Ref<Scene> scene = RoundTrip(AuthoredScene(), "Authored");
    ASSERT_TRUE(scene);

    SceneTransition::ApplySceneRenderSettings(*scene, nullptr);

    const auto& pp = Renderer3D::GetPostProcessSettings();
    EXPECT_FLOAT_EQ(pp.Exposure, 2.5f);
    EXPECT_EQ(pp.Upscale, UpscaleMode::Quality);
    EXPECT_EQ(pp.Technique, UpscalerTechnique::Temporal);
    EXPECT_TRUE(pp.DOFEnabled);
    EXPECT_TRUE(Renderer3D::GetFogSettings().Enabled);
    EXPECT_FLOAT_EQ(Renderer3D::GetFogSettings().Density, 0.07f);
    EXPECT_TRUE(Renderer3D::GetSnowSettings().Enabled);
    EXPECT_TRUE(Renderer3D::GetWindSettings().Enabled);
    EXPECT_FLOAT_EQ(Renderer3D::GetWindSettings().Speed, 11.0f);
    EXPECT_TRUE(Renderer3D::GetSnowAccumulationSettings().Enabled);
    EXPECT_TRUE(Renderer3D::GetSnowEjectaSettings().Enabled);
    EXPECT_TRUE(Renderer3D::GetPrecipitationSettings().Enabled);
    EXPECT_FLOAT_EQ(Renderer3D::GetPrecipitationSettings().Intensity, 0.9f);
}

TEST_F(SceneRenderSettingsActivationTest, SpatialUpscaleRequestReachesTheRenderer)
{
    Ref<Scene> authored = AuthoredScene();
    authored->GetPostProcessSettings().Upscale = UpscaleMode::Performance;
    authored->GetPostProcessSettings().Technique = UpscalerTechnique::Spatial;
    const Ref<Scene> scene = RoundTrip(authored, "Spatial");
    ASSERT_TRUE(scene);

    SceneTransition::ApplySceneRenderSettings(*scene, nullptr);

    EXPECT_EQ(Renderer3D::GetPostProcessSettings().Upscale, UpscaleMode::Performance);
    EXPECT_EQ(Renderer3D::GetPostProcessSettings().Technique, UpscalerTechnique::Spatial);
}

// Reload and script-driven switching both reactivate through the seam; the
// incoming scene's values must replace the outgoing scene's, defaults included.
TEST_F(SceneRenderSettingsActivationTest, SwitchingScenesReplacesTheOutgoingScenesSettings)
{
    const Ref<Scene> first = RoundTrip(AuthoredScene(), "First");
    const Ref<Scene> second = RoundTrip(DefaultScene(), "Second");
    ASSERT_TRUE(first && second);

    SceneTransition::ApplySceneRenderSettings(*first, nullptr);
    ASSERT_FLOAT_EQ(Renderer3D::GetPostProcessSettings().Exposure, 2.5f);

    SceneTransition::ApplySceneRenderSettings(*second, nullptr);

    const PostProcessSettings defaults;
    EXPECT_FLOAT_EQ(Renderer3D::GetPostProcessSettings().Exposure, defaults.Exposure);
    EXPECT_EQ(Renderer3D::GetPostProcessSettings().Upscale, defaults.Upscale);
    EXPECT_FALSE(Renderer3D::GetFogSettings().Enabled);
    EXPECT_FALSE(Renderer3D::GetWindSettings().Enabled);
    EXPECT_FALSE(Renderer3D::GetPrecipitationSettings().Enabled);
}

// The editor has always overlaid the project's tier on a loaded scene; the
// runtime now does the same. The tier owns its fields, and only those.
TEST_F(SceneRenderSettingsActivationTest, QualityTierOverridesOnlyTheFieldsItOwns)
{
    const Ref<Scene> scene = RoundTrip(AuthoredScene(), "Tiered");
    ASSERT_TRUE(scene);

    QualityTieringSettings tier = GetPresetSettings(QualityPreset::High);
    tier.Preset = QualityPreset::Custom;
    tier.BloomEnabled = false;
    tier.DOFEnabled = false;
    tier.ShadowResolution = 1024;

    SceneTransition::ApplySceneRenderSettings(*scene, &tier);

    const auto& pp = Renderer3D::GetPostProcessSettings();
    EXPECT_FALSE(pp.BloomEnabled) << "the tier's bloom switch must win over the authored one";
    EXPECT_FALSE(pp.DOFEnabled) << "the tier's DOF switch must win over the authored one";
    EXPECT_EQ(Renderer3D::GetShadowMap().GetSettings().Resolution, 1024u);
    EXPECT_FLOAT_EQ(pp.Exposure, 2.5f) << "exposure is not a tier field";
    EXPECT_EQ(pp.Upscale, UpscaleMode::Quality) << "upscale is not a tier field";
    EXPECT_TRUE(Renderer3D::GetFogSettings().Enabled);
}

// The shipped game has no .oloproj; game.manifest carries the tier. The block
// must round-trip, and a non-finite value must not get through.
TEST_F(SceneRenderSettingsActivationTest, ManifestTierBlockRoundTrips)
{
    QualityTieringSettings written = GetPresetSettings(QualityPreset::Low);
    written.Preset = QualityPreset::Custom;
    written.BloomEnabled = true;
    written.SSAORadius = 1.25f;
    written.ShadowResolution = 2048;

    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "QualityTiering" << YAML::Value;
    SerializeQualityTiering(out, written);
    out << YAML::EndMap;

    QualityTieringSettings read;
    DeserializeQualityTiering(YAML::Load(out.c_str())["QualityTiering"], read);
    EXPECT_EQ(read.Preset, QualityPreset::Custom);
    EXPECT_TRUE(read.BloomEnabled);
    EXPECT_FLOAT_EQ(read.SSAORadius, 1.25f);
    EXPECT_EQ(read.ShadowResolution, 2048u);

    QualityTieringSettings hostile;
    DeserializeQualityTiering(YAML::Load("Preset: Custom\nSSAORadius: .nan\nGTAOPower: .inf\n"), hostile);
    EXPECT_TRUE(std::isfinite(hostile.SSAORadius));
    EXPECT_TRUE(std::isfinite(hostile.GTAOPower));
}
