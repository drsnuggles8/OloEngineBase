#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

// OLO_TEST_LAYER: unit

// =============================================================================
// A failed asset save is reported to whoever asked for it (issue #1635).
//
// AssetSerializer::Serialize used to return void, so a save into a missing or
// unwritable location was a log line and the caller carried on: it registered
// the asset, marked it saved, or packed it. Each case here makes one write fail
// and checks the layer above it sees the failure:
//   * the serializer and AssetImporter return false (a YAML asset and a scene);
//   * EditorAssetManager::CreateOrReplaceAsset registers nothing;
//   * AssetPackBuilder fails the build and names the asset whose record it
//     could not write, instead of shipping an index entry with no bytes.
//
// The write is made to fail by putting a regular FILE where the asset's parent
// directory should be: that fails on Windows and Linux alike, and for root,
// which a read-only bit does not.
// =============================================================================

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/SoundConfigAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Scene/Scene.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    class AssetSerializerFailedWriteTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_TempDir = Tests::TempDir();

            std::error_code ec;
            fs::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << ec.message();
            {
                std::ofstream project(m_TempDir / "Test.oloproj");
                project << "Project:\n"
                           "  Name: AssetSerializerFailedWriteTest\n"
                           "  StartScene: \"\"\n"
                           "  AssetDirectory: \"Assets\"\n"
                           "  ScriptModulePath: \"\"\n";
            }
            // The blocker: "Assets/Blocked" is a file, so nothing can be written under it.
            {
                std::ofstream blocker(m_TempDir / "Assets" / "Blocked");
            }
            ASSERT_TRUE(Project::Load(m_TempDir / "Test.oloproj"));

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void TearDown() override
        {
            m_AssetManager.Reset();
        }

        [[nodiscard]] static AssetMetadata MetadataFor(AssetType type, const fs::path& projectRelativePath)
        {
            AssetMetadata metadata;
            metadata.Handle = AssetHandle();
            metadata.Type = type;
            metadata.FilePath = projectRelativePath;
            return metadata;
        }

        fs::path m_TempDir;
        Ref<EditorAssetManager> m_AssetManager;
    };
} // namespace

TEST_F(AssetSerializerFailedWriteTest, SerializerAndImporterReportTheFailedWrite)
{
    const auto asset = Ref<SoundConfigAsset>::Create();

    const AssetMetadata blocked = MetadataFor(AssetType::SoundConfig, fs::path("Assets") / "Blocked" / "preset.olosoundc");
    EXPECT_FALSE(SoundConfigSerializer().Serialize(blocked, asset));
    EXPECT_FALSE(AssetImporter::Serialize(blocked, asset));

    // The control: the same asset saves where it can.
    const AssetMetadata writable = MetadataFor(AssetType::SoundConfig, fs::path("Assets") / "preset.olosoundc");
    EXPECT_TRUE(AssetImporter::Serialize(writable, asset));
    EXPECT_TRUE(fs::is_regular_file(m_TempDir / writable.FilePath));
}

// SceneAssetSerializer used to log SceneSerializer::Serialize's false and drop it.
TEST_F(AssetSerializerFailedWriteTest, SceneAssetSerializerForwardsTheSceneSaveResult)
{
    auto scene = Ref<Scene>::Create();
    scene->SetName("FailedWriteScene");

    EXPECT_FALSE(SceneAssetSerializer().Serialize(
        MetadataFor(AssetType::Scene, fs::path("Assets") / "Blocked" / "Level.oloscene"), scene));
    EXPECT_TRUE(SceneAssetSerializer().Serialize(MetadataFor(AssetType::Scene, fs::path("Assets") / "Level.oloscene"), scene));
}

TEST_F(AssetSerializerFailedWriteTest, CreateOrReplaceAssetRegistersNothingWhenTheWriteFails)
{
    const fs::path blockedPath = m_TempDir / "Assets" / "Blocked" / "preset.olosoundc";
    EXPECT_FALSE(m_AssetManager->CreateOrReplaceAsset<SoundConfigAsset>(blockedPath));
    EXPECT_EQ(static_cast<u64>(m_AssetManager->GetAssetHandleFromFilePath(blockedPath)), 0u)
        << "an asset whose file was never written must not be in the registry";
    EXPECT_TRUE(m_AssetManager->GetLoadedAssets().empty());

    const fs::path writablePath = m_TempDir / "Assets" / "preset.olosoundc";
    const Ref<SoundConfigAsset> created = m_AssetManager->CreateOrReplaceAsset<SoundConfigAsset>(writablePath);
    ASSERT_TRUE(created);
    EXPECT_EQ(m_AssetManager->GetAssetHandleFromFilePath(writablePath), created->GetHandle());
}

// The pack builder reads each asset it packs through the project's asset
// manager. An editor-side copy that can no longer be encoded (here a lightmap
// whose texel went non-finite after it was saved) fails its record write; the
// build used to log that, record a zero-byte entry, and report success.
TEST_F(AssetSerializerFailedWriteTest, PackBuildFailsAndNamesTheAssetWhoseRecordFailed)
{
    TArray<f32> texels(static_cast<sizet>(8 * 8 * 4));
    for (sizet i = 0; i < static_cast<sizet>(texels.Num()); ++i)
        texels[i] = (i % 4 == 3) ? 1.0f : 0.5f;
    TArray<LightmapEntityEntry> entries;
    LightmapEntityEntry entry;
    entry.EntityUUID = 0x1635;
    entry.ScaleOffset = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    entries.Add(entry);

    Ref<LightmapAsset> stored = m_AssetManager->CreateOrReplaceAsset<LightmapAsset>(
        m_TempDir / "Assets" / "Bake.olmap", 8u, 8u, 1u, 0x1635ull, std::move(texels), std::move(entries));
    ASSERT_TRUE(stored);
    ASSERT_TRUE(m_AssetManager->SerializeAssetRegistry());

    AssetRegistry registry;
    registry.AddAsset(m_AssetManager->GetMetadata(stored->GetHandle()));

    AssetPackBuilder::BuildSettings settings;
    settings.m_OutputPath = m_TempDir / "Failed.olopack";
    settings.m_CompressAssets = false;
    settings.m_IncludeScriptModule = false;

    // Control first: the valid lightmap packs.
    {
        std::atomic<f32> progress = 0.0f;
        const auto built = AssetPackBuilder::BuildFromRegistry(registry, settings, progress);
        ASSERT_TRUE(built.m_Success) << built.m_ErrorMessage;
    }

    stored->GetTexelData()[0] = std::numeric_limits<f32>::quiet_NaN();
    std::atomic<f32> progress = 0.0f;
    const auto built = AssetPackBuilder::BuildFromRegistry(registry, settings, progress);
    EXPECT_FALSE(built.m_Success);
    EXPECT_NE(built.m_ErrorMessage.find("Bake.olmap"), std::string::npos) << built.m_ErrorMessage;
}
