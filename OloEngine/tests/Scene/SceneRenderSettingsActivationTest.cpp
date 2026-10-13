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
//     knows the project's tier through game.manifest,
//   * the authored rendering path (issue #1601) is one of those settings: it
//     survives every form a scene travels in, a file without the key means
//     Forward, and a value this build does not know fails the load.
//
// The graph half of #1601 (the path actually rebuilding the render graph, SSR
// and SSGI engaging) needs a GL context and lives in
// TemporalLongSequenceEvidenceTest.AuthoredPathSelectsTheGraphThroughSceneActivation.
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
#include <fstream>
#include <sstream>
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
        // ApplySceneRenderSettings rebuilds the live graph when Renderer3D is up
        // (a GL suite earlier in the run); put the graph back on the restored path.
        if (Renderer3D::HasInitialized())
            Renderer3D::ApplyRendererSettings();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    [[nodiscard]] std::string ReadText(const std::string& name) const
    {
        std::ifstream in(m_Root / (name + ".olo"));
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void WriteText(const std::string& name, const std::string& text) const
    {
        std::ofstream(m_Root / (name + ".olo"), std::ios::trunc) << text;
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
    written.DDGIEnabled = true;
    written.DDGIBudgetScale = 0.75f;
    written.AnimalSchedulingEnabled = false;
    written.AnimalFrameBudgetUnits = 1234.0f;

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
    EXPECT_TRUE(read.DDGIEnabled) << "Low seeds DDGI off; the written Custom value must win";
    EXPECT_FLOAT_EQ(read.DDGIBudgetScale, 0.75f);
    EXPECT_FALSE(read.AnimalSchedulingEnabled);
    EXPECT_FLOAT_EQ(read.AnimalFrameBudgetUnits, 1234.0f);

    QualityTieringSettings hostile;
    DeserializeQualityTiering(YAML::Load("Preset: Custom\nSSAORadius: .nan\nGTAOPower: .inf\n"), hostile);
    EXPECT_TRUE(std::isfinite(hostile.SSAORadius));
    EXPECT_TRUE(std::isfinite(hostile.GTAOPower));
}

// ---- the authored rendering path (issue #1601) ------------------------------

namespace
{
    // A different path than `path`, so a value that reaches the renderer can
    // only have come from the scene.
    [[nodiscard]] RenderingPath OtherThan(RenderingPath path)
    {
        return path == RenderingPath::Deferred ? RenderingPath::Forward : RenderingPath::Deferred;
    }
} // namespace

TEST_F(SceneRenderSettingsActivationTest, EveryAuthoredPathRoundTripsAndReachesTheRenderer)
{
    for (const RenderingPath path : kRenderingPaths)
    {
        SCOPED_TRACE(ToToken(path));
        Ref<Scene> authored = AuthoredScene();
        authored->SetRenderingPath(path);
        const std::string name = std::string("Path") + ToToken(path);
        const Ref<Scene> scene = RoundTrip(authored, name);
        ASSERT_TRUE(scene);
        EXPECT_NE(ReadText(name).find(std::string("RenderingPath: ") + ToToken(path)), std::string::npos)
            << "the key is written by name, not as an ordinal";
        EXPECT_EQ(scene->GetRenderingPath(), path);

        Renderer3D::GetRendererSettings().Path = OtherThan(path);
        SceneTransition::ApplySceneRenderSettings(*scene, nullptr);
        EXPECT_EQ(Renderer3D::GetRendererSettings().Path, path);
    }
}

// Reload and script-driven switching, in both directions: the incoming scene's
// path replaces the outgoing one's, including back to Forward.
TEST_F(SceneRenderSettingsActivationTest, SwitchingScenesSwitchesThePathBothWays)
{
    Ref<Scene> deferredAuthored = DefaultScene();
    deferredAuthored->SetRenderingPath(RenderingPath::Deferred);
    const Ref<Scene> deferred = RoundTrip(deferredAuthored, "SwitchDeferred");
    const Ref<Scene> forward = RoundTrip(DefaultScene(), "SwitchForward");
    ASSERT_TRUE(deferred && forward);

    SceneTransition::ApplySceneRenderSettings(*forward, nullptr);
    EXPECT_EQ(Renderer3D::GetRendererSettings().Path, RenderingPath::Forward);
    SceneTransition::ApplySceneRenderSettings(*deferred, nullptr);
    EXPECT_EQ(Renderer3D::GetRendererSettings().Path, RenderingPath::Deferred);
    SceneTransition::ApplySceneRenderSettings(*forward, nullptr);
    EXPECT_EQ(Renderer3D::GetRendererSettings().Path, RenderingPath::Forward);
}

// Every scene saved before #1601 carries no key; each must keep rendering
// Forward, whatever path the renderer happened to be on.
TEST_F(SceneRenderSettingsActivationTest, SceneWithoutTheKeyRendersForward)
{
    Ref<Scene> authored = DefaultScene();
    authored->SetRenderingPath(RenderingPath::Deferred);
    SceneSerializer(authored).Serialize(m_Root / "NoKey.olo");
    std::string text = ReadText("NoKey");
    const std::string line = "RenderingPath: Deferred\n";
    const auto at = text.find(line);
    ASSERT_NE(at, std::string::npos);
    text.erase(at, line.size());
    ASSERT_EQ(text.find("RenderingPath"), std::string::npos);
    WriteText("NoKey", text);

    const auto loaded = SceneTransition::LoadSceneFile(m_Root / "NoKey.olo", /*requirePrimaryCamera=*/true);
    ASSERT_TRUE(loaded) << loaded.Error.ToView();
    EXPECT_EQ(loaded.LoadedScene->GetRenderingPath(), RenderingPath::Forward);

    Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
    SceneTransition::ApplySceneRenderSettings(*loaded.LoadedScene, nullptr);
    EXPECT_EQ(Renderer3D::GetRendererSettings().Path, RenderingPath::Forward);
}

// A value this build does not know is a load error, never a quiet Forward: the
// game would boot, draw, and silently lack what the author picked the path for.
TEST_F(SceneRenderSettingsActivationTest, UnknownPathFailsTheLoad)
{
    const Ref<Scene> authored = DefaultScene();
    SceneSerializer(authored).Serialize(m_Root / "Source.olo");
    const std::string source = ReadText("Source");
    const std::string valid = "RenderingPath: Forward";
    ASSERT_NE(source.find(valid), std::string::npos);

    // Spellings a hand edit or another tool could plausibly produce.
    for (const char* value : { "Hybrid", "deferred", "Forward+", "2", "\"\"", "[Deferred]" })
    {
        SCOPED_TRACE(value);
        std::string text = source;
        text.replace(text.find(valid), valid.size(), std::string("RenderingPath: ") + value);
        WriteText("Bad", text);

        const auto loaded = SceneTransition::LoadSceneFile(m_Root / "Bad.olo", /*requirePrimaryCamera=*/true);
        EXPECT_FALSE(loaded);
        EXPECT_FALSE(loaded.Error.IsEmpty());

        // Both readers (the file and the asset-pack form) refuse before changing
        // the settings of a scene they are applied onto: neither its path nor a
        // block ApplySceneSettings applies before the path (post-processing).
        for (const bool fromFile : { true, false })
        {
            Ref<Scene> existing = DefaultScene();
            existing->SetRenderingPath(RenderingPath::Deferred);
            existing->GetPostProcessSettings().Exposure = 3.0f;
            SceneSerializer serializer(existing);
            EXPECT_FALSE(fromFile ? serializer.Deserialize(m_Root / "Bad.olo") : serializer.DeserializeFromYAML(text));
            EXPECT_EQ(existing->GetRenderingPath(), RenderingPath::Deferred) << (fromFile ? "file" : "asset pack");
            EXPECT_FLOAT_EQ(existing->GetPostProcessSettings().Exposure, 3.0f) << (fromFile ? "file" : "asset pack");
        }
    }
}

// The path travels with the scene through every other form it takes: the
// asset-pack YAML string, the Play-mode copy, and the binary sidecar cache a
// second load of the same file is served from.
TEST_F(SceneRenderSettingsActivationTest, PathSurvivesEveryOtherSceneForm)
{
    Ref<Scene> authored = DefaultScene();
    authored->SetRenderingPath(RenderingPath::ForwardPlus);

    const Ref<Scene> fromPack = Scene::Create();
    ASSERT_TRUE(SceneSerializer(fromPack).DeserializeFromYAML(SceneSerializer(authored).SerializeToYAML()));
    EXPECT_EQ(fromPack->GetRenderingPath(), RenderingPath::ForwardPlus);

    EXPECT_EQ(Scene::Copy(authored)->GetRenderingPath(), RenderingPath::ForwardPlus);

    const Ref<Scene> first = RoundTrip(authored, "Sidecar");
    ASSERT_TRUE(first);
    ASSERT_TRUE(std::filesystem::is_regular_file(m_Root / "Sidecar.olo.scenebin"))
        << "the first load writes the sidecar the second one is served from";
    const auto second = SceneTransition::LoadSceneFile(m_Root / "Sidecar.olo", /*requirePrimaryCamera=*/true);
    ASSERT_TRUE(second) << second.Error.ToView();
    EXPECT_EQ(second.LoadedScene->GetRenderingPath(), RenderingPath::ForwardPlus);
}
