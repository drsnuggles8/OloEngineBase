#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// EnvironmentSerializerTest — asset-registry path-resolution for EnvMap.
//
// Pre-fix EnvironmentSerializer::TryLoadData constructed the EnvironmentMap
// with a default-initialised EnvironmentMapSpecification, discarding the
// file path resolved from metadata.FilePath. The empty spec.FilePath branch
// inside EnvironmentMap's ctor skips the HDR -> cubemap conversion entirely,
// so any .hdr / .exr asset loaded through the editor's asset registry came
// back as a hollow EnvironmentMap with no cubemap data. The asset-pack path
// (DeserializeFromAssetPack) was always correct — the bug was specific to
// TryLoadData.
//
// This test pins the fix at the metadata layer: after TryLoadData succeeds,
// the resulting EnvironmentMap's spec must carry the absolute project-resolved
// file path. The actual HDR -> GPU conversion needs an OpenGL context and the
// IBL shader library; both are out of scope here. A renderer-attached smoke
// test can layer on the end-to-end load case once needed.
// =============================================================================

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/EnvironmentMap.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // Write a placeholder file. We never load it as HDR data — the headless
    // test path stops at spec.FilePath assignment, before stb_image is asked
    // to parse the bytes. A non-empty file is enough to satisfy the
    // filesystem::exists() guard inside TryLoadData.
    void WritePlaceholderFile(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary);
        out << "placeholder";
    }
} // namespace

class EnvironmentSerializerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineEnvironmentSerializer" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: EnvironmentSerializerTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed for temp project at " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        // Filewatcher disabled for the same reason as MeshAssetSerializerYAMLTest —
        // its callback thread races the at-exit dtor of FNamedThreadManager under
        // TSan. Tests write synchronously and never need hot-reload.
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    AssetMetadata MakeMetadata(AssetHandle handle, const fs::path& relativePath) const
    {
        AssetMetadata md;
        md.Handle = handle;
        md.Type = AssetType::EnvMap;
        md.FilePath = fs::path("Assets") / relativePath;
        return md;
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(EnvironmentSerializerTest, TryLoadDataPopulatesSpecFilePath)
{
    const fs::path relative = fs::path("Assets") / "skybox.hdr";
    const fs::path absolute = Project::GetProjectDirectory() / relative;
    WritePlaceholderFile(absolute);

    AssetMetadata metadata;
    metadata.Handle = AssetHandle();
    metadata.Type = AssetType::EnvMap;
    metadata.FilePath = relative;

    EnvironmentSerializer serializer;
    Ref<Asset> loadedAsset;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData should succeed when the source file exists";

    Ref<EnvironmentMap> loaded = loadedAsset.As<EnvironmentMap>();
    ASSERT_TRUE(loaded) << "Loaded asset is not an EnvironmentMap";

    // The core regression: pre-fix this came back empty because the serializer
    // constructed the spec from {} instead of from the resolved path.
    EXPECT_FALSE(loaded->GetSpecification().FilePath.empty())
        << "EnvironmentSerializer dropped the file path before constructing the EnvironmentMap "
           "— any HDR cubemap loaded through the asset registry would be hollow.";

    EXPECT_EQ(fs::path(loaded->GetSpecification().FilePath), absolute)
        << "spec.FilePath should be the absolute project-resolved path so EnvironmentMap "
           "passes it straight to stb_image";

    EXPECT_EQ(loaded->GetHandle(), metadata.Handle)
        << "Handle should be propagated from metadata";
}

TEST_F(EnvironmentSerializerTest, TryLoadDataFailsWhenSourceMissing)
{
    AssetMetadata metadata = MakeMetadata(AssetHandle(), "does_not_exist.hdr");

    EnvironmentSerializer serializer;
    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData must report failure when the source file is missing";
    EXPECT_FALSE(loadedAsset) << "Failure path must not return a half-constructed asset";
}
