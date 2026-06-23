#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundConfigSerializerTest — round-trip + hardening for the SoundConfig asset.
//
// SoundConfig is a reusable, serializable preset of AudioSourceConfig playback
// parameters (volume/pitch, spatialization, attenuation, cone, DSP sends). This
// pins three things:
//   1. A full on-disk round-trip through SoundConfigSerializer::Serialize ->
//      TryLoadData preserves every field (project-backed, the real load/save
//      path the editor uses).
//   2. The YAML deserialiser rejects non-finite floats (NaN / Inf from a
//      corrupt file) in favour of the field default.
//   3. Bounded fields are clamped to their valid range on read.
//
// (2) and (3) drive the private YAML path directly via the Test* hooks so they
// need no project / asset manager. The CPU-only serializer creates no GPU
// resources, so none of this needs an OpenGL context.
// =============================================================================

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/SoundConfigAsset.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Project/Project.h"

#include <glm/trigonometric.hpp>

#include <filesystem>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // A config whose every field differs from the AudioSourceConfig defaults,
    // with all values inside their clamp ranges so the round-trip is lossless.
    AudioSourceConfig MakeDistinctConfig()
    {
        AudioSourceConfig c{};
        c.VolumeMultiplier = 0.75f;
        c.PitchMultiplier = 1.25f;
        c.PlayOnAwake = false;
        c.Looping = true;
        c.Spatialization = true;
        c.AttenuationModel = AttenuationModelType::Exponential;
        c.RollOff = 2.5f;
        c.MinGain = 0.1f;
        c.MaxGain = 0.9f;
        c.MinDistance = 0.5f;
        c.MaxDistance = 500.0f;
        c.ConeInnerAngle = 1.0f;
        c.ConeOuterAngle = 2.0f;
        c.ConeOuterGain = 0.25f;
        c.DopplerFactor = 1.5f;
        c.Spread = 0.5f;
        c.Focus = 0.25f;
        c.LowPassCutoff = 0.8f;
        c.HighPassCutoff = 0.2f;
        c.ReverbSend = 0.4f;
        return c;
    }

    void ExpectConfigEq(const AudioSourceConfig& a, const AudioSourceConfig& b)
    {
        EXPECT_FLOAT_EQ(a.VolumeMultiplier, b.VolumeMultiplier);
        EXPECT_FLOAT_EQ(a.PitchMultiplier, b.PitchMultiplier);
        EXPECT_EQ(a.PlayOnAwake, b.PlayOnAwake);
        EXPECT_EQ(a.Looping, b.Looping);
        EXPECT_EQ(a.Spatialization, b.Spatialization);
        EXPECT_EQ(a.AttenuationModel, b.AttenuationModel);
        EXPECT_FLOAT_EQ(a.RollOff, b.RollOff);
        EXPECT_FLOAT_EQ(a.MinGain, b.MinGain);
        EXPECT_FLOAT_EQ(a.MaxGain, b.MaxGain);
        EXPECT_FLOAT_EQ(a.MinDistance, b.MinDistance);
        EXPECT_FLOAT_EQ(a.MaxDistance, b.MaxDistance);
        EXPECT_FLOAT_EQ(a.ConeInnerAngle, b.ConeInnerAngle);
        EXPECT_FLOAT_EQ(a.ConeOuterAngle, b.ConeOuterAngle);
        EXPECT_FLOAT_EQ(a.ConeOuterGain, b.ConeOuterGain);
        EXPECT_FLOAT_EQ(a.DopplerFactor, b.DopplerFactor);
        EXPECT_FLOAT_EQ(a.Spread, b.Spread);
        EXPECT_FLOAT_EQ(a.Focus, b.Focus);
        EXPECT_FLOAT_EQ(a.LowPassCutoff, b.LowPassCutoff);
        EXPECT_FLOAT_EQ(a.HighPassCutoff, b.HighPassCutoff);
        EXPECT_FLOAT_EQ(a.ReverbSend, b.ReverbSend);
    }
} // namespace

// -----------------------------------------------------------------------------
// Full disk round-trip through the real Serialize -> TryLoadData path.
// -----------------------------------------------------------------------------
class SoundConfigSerializerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineSoundConfigSerializer" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: SoundConfigSerializerTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed for temp project at " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        // Filewatcher disabled: its callback thread races the at-exit dtor of
        // FNamedThreadManager under TSan, and the test writes synchronously.
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(SoundConfigSerializerTest, DiskRoundTripPreservesEveryField)
{
    auto source = Ref<SoundConfigAsset>::Create(MakeDistinctConfig());

    AssetMetadata metadata;
    metadata.Handle = AssetHandle();
    metadata.Type = AssetType::SoundConfig;
    metadata.FilePath = fs::path("Assets") / "preset.olosoundc";

    SoundConfigSerializer serializer;
    serializer.Serialize(metadata, source);

    const fs::path absolute = Project::GetProjectDirectory() / metadata.FilePath;
    ASSERT_TRUE(fs::exists(absolute)) << "Serialize did not write the asset file";

    Ref<Asset> loadedAsset;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData should succeed for a freshly written SoundConfig";

    Ref<SoundConfigAsset> loaded = loadedAsset.As<SoundConfigAsset>();
    ASSERT_TRUE(loaded) << "Loaded asset is not a SoundConfig";

    EXPECT_EQ(loaded->GetAssetType(), AssetType::SoundConfig);
    EXPECT_EQ(loaded->GetHandle(), metadata.Handle) << "Handle should be propagated from metadata";
    ExpectConfigEq(loaded->m_Config, source->m_Config);
}

TEST_F(SoundConfigSerializerTest, TryLoadDataFailsWhenSourceMissing)
{
    AssetMetadata metadata;
    metadata.Handle = AssetHandle();
    metadata.Type = AssetType::SoundConfig;
    metadata.FilePath = fs::path("Assets") / "does_not_exist.olosoundc";

    SoundConfigSerializer serializer;
    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData must report failure when the source file is missing";
    EXPECT_FALSE(loadedAsset) << "Failure path must not return a half-constructed asset";
}

// -----------------------------------------------------------------------------
// YAML path edge cases — driven directly through the Test* hooks (no project).
// -----------------------------------------------------------------------------
TEST(SoundConfigSerializerYAML, RoundTripPreservesEveryField)
{
    SoundConfigSerializer serializer;
    auto source = Ref<SoundConfigAsset>::Create(MakeDistinctConfig());

    const std::string yaml = serializer.TestSerializeToYAML(source);
    ASSERT_FALSE(yaml.empty());

    auto restored = Ref<SoundConfigAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, restored));
    ExpectConfigEq(restored->m_Config, source->m_Config);
}

TEST(SoundConfigSerializerYAML, MissingRootNodeFails)
{
    SoundConfigSerializer serializer;
    auto target = Ref<SoundConfigAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("SomethingElse:\n  Foo: 1\n", target))
        << "Deserialize must fail when the SoundConfig root node is absent";
}

TEST(SoundConfigSerializerYAML, NonFiniteFloatsFallBackToDefaults)
{
    // A file with NaN / Inf for several floats — these must be rejected and the
    // field left at its AudioSourceConfig default rather than propagated.
    const std::string yaml =
        "SoundConfig:\n"
        "  VolumeMultiplier: .nan\n"
        "  PitchMultiplier: .inf\n"
        "  RollOff: -.inf\n"
        "  MaxDistance: .nan\n"
        "  ReverbSend: .inf\n";

    SoundConfigSerializer serializer;
    auto target = Ref<SoundConfigAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, target));

    const AudioSourceConfig defaults{};
    const AudioSourceConfig& c = target->m_Config;

    // Every field must remain finite...
    EXPECT_TRUE(std::isfinite(c.VolumeMultiplier));
    EXPECT_TRUE(std::isfinite(c.PitchMultiplier));
    EXPECT_TRUE(std::isfinite(c.RollOff));
    EXPECT_TRUE(std::isfinite(c.MaxDistance));
    EXPECT_TRUE(std::isfinite(c.ReverbSend));

    // ...and specifically fall back to the defaults.
    EXPECT_FLOAT_EQ(c.VolumeMultiplier, defaults.VolumeMultiplier);
    EXPECT_FLOAT_EQ(c.PitchMultiplier, defaults.PitchMultiplier);
    EXPECT_FLOAT_EQ(c.RollOff, defaults.RollOff);
    EXPECT_FLOAT_EQ(c.MaxDistance, defaults.MaxDistance);
    EXPECT_FLOAT_EQ(c.ReverbSend, defaults.ReverbSend);
}

TEST(SoundConfigSerializerYAML, OutOfRangeValuesAreClamped)
{
    // Bounded fields fed values outside their valid range must be clamped, not
    // stored verbatim. Gains / sends are [0,1]; cone angles are [0, 2*pi].
    const std::string yaml =
        "SoundConfig:\n"
        "  MinGain: -3.0\n"
        "  MaxGain: 5.0\n"
        "  ConeOuterGain: 2.0\n"
        "  Spread: -1.0\n"
        "  Focus: 4.0\n"
        "  LowPassCutoff: 9.0\n"
        "  HighPassCutoff: -2.0\n"
        "  ReverbSend: 7.0\n"
        "  ConeInnerAngle: -1.0\n"
        "  ConeOuterAngle: 100.0\n"
        "  VolumeMultiplier: -2.0\n"
        "  MinDistance: -5.0\n";

    SoundConfigSerializer serializer;
    auto target = Ref<SoundConfigAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, target));

    const AudioSourceConfig& c = target->m_Config;
    constexpr f32 kTwoPi = glm::radians(360.0f);

    EXPECT_FLOAT_EQ(c.MinGain, 0.0f);
    EXPECT_FLOAT_EQ(c.MaxGain, 1.0f);
    EXPECT_FLOAT_EQ(c.ConeOuterGain, 1.0f);
    EXPECT_FLOAT_EQ(c.Spread, 0.0f);
    EXPECT_FLOAT_EQ(c.Focus, 1.0f);
    EXPECT_FLOAT_EQ(c.LowPassCutoff, 1.0f);
    EXPECT_FLOAT_EQ(c.HighPassCutoff, 0.0f);
    EXPECT_FLOAT_EQ(c.ReverbSend, 1.0f);
    EXPECT_FLOAT_EQ(c.ConeInnerAngle, 0.0f);
    EXPECT_FLOAT_EQ(c.ConeOuterAngle, kTwoPi);
    EXPECT_FLOAT_EQ(c.VolumeMultiplier, 0.0f);
    EXPECT_FLOAT_EQ(c.MinDistance, 0.0f);
}
