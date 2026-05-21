#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// MeshAssetSerializerTest — YAML roundtrip for the Mesh asset.
//
// The `Mesh` asset is a *lightweight reference*: a MeshSource handle plus a
// single submesh index. Submesh/material containers live on `StaticMesh`,
// which has its own serializer. Pre-fix, the MeshSerializer YAML path
// emitted vertex/index counts (useless on load) and dropped both the
// MeshSource handle and the submesh index, so any Mesh asset written to
// disk was unrecoverable. These tests pin the fix: the YAML must round-trip
// the two fields the asset actually depends on.
// =============================================================================

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    Ref<MeshSource> MakeMeshSourceWithSubmeshes(u32 submeshCount)
    {
        TArray<Vertex> vertices;
        vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
        vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
        vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));

        TArray<u32> indices;
        indices.Add(0);
        indices.Add(1);
        indices.Add(2);

        auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        for (u32 i = 0; i < submeshCount; ++i)
        {
            Submesh sub;
            sub.m_VertexCount = 3;
            sub.m_IndexCount = 3;
            sub.m_NodeName = "Sub" + std::to_string(i);
            source->GetSubmeshes().Add(sub);
        }
        return source;
    }
} // namespace

class MeshAssetSerializerYAMLTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Per-test temp project directory so parallel runs do not clobber each other.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineMeshSerializerYAML" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: MeshSerializerTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed for temp project at " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        m_AssetManager->Initialize();
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    // AssetMetadata::FilePath is the project-root-relative path *including* the "Assets/"
    // prefix — that is the form EditorAssetManager::GetRelativePath emits, and every
    // serializer joins it against Project::GetProjectDirectory(). Pre-fix the test stored a
    // bare filename here, so MeshSerializer wrote the YAML one level above the Assets
    // folder while the test looked for it inside Assets/, and the round-trip case silently
    // failed (file-not-found in place of "does the round-trip survive").
    AssetMetadata MakeMetadata(AssetHandle handle, const fs::path& relativePath) const
    {
        AssetMetadata md;
        md.Handle = handle;
        md.Type = AssetType::Mesh;
        md.FilePath = fs::path("Assets") / relativePath;
        return md;
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(MeshAssetSerializerYAMLTest, RoundTripsMeshSourceHandleAndSubmeshIndex)
{
    auto meshSource = MakeMeshSourceWithSubmeshes(3);
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);
    ASSERT_NE(static_cast<u64>(meshSourceHandle), 0ULL);

    constexpr u32 kSubmeshIndex = 2;
    Ref<Mesh> originalMesh = Ref<Mesh>(new Mesh(meshSource, kSubmeshIndex));
    const AssetHandle meshHandle = AssetManager::AddMemoryOnlyAsset<Mesh>(originalMesh);

    MeshSerializer serializer;
    AssetMetadata metadata = MakeMetadata(meshHandle, "roundtrip.olomesh");

    serializer.Serialize(metadata, originalMesh);

    const auto yamlPath = Project::GetProjectDirectory() / metadata.FilePath;
    ASSERT_TRUE(fs::exists(yamlPath)) << "Serialize did not write the YAML file";

    Ref<Asset> loadedAsset;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loadedAsset)) << "TryLoadData failed";

    Ref<Mesh> loadedMesh = loadedAsset.As<Mesh>();
    ASSERT_TRUE(loadedMesh) << "Loaded asset is not a Mesh";
    ASSERT_TRUE(loadedMesh->GetMeshSource()) << "Loaded Mesh has no MeshSource — TryLoadData failed to resolve the handle";

    EXPECT_EQ(loadedMesh->GetMeshSource()->GetHandle(), meshSourceHandle)
        << "MeshSource handle did not survive YAML round-trip";
    EXPECT_EQ(loadedMesh->GetSubmeshIndex(), kSubmeshIndex)
        << "SubmeshIndex did not survive YAML round-trip";
    EXPECT_EQ(loadedMesh->GetHandle(), meshHandle)
        << "Mesh handle was not assigned from metadata";
}

TEST_F(MeshAssetSerializerYAMLTest, FailsWhenMeshSourceHandleIsMissing)
{
    AssetMetadata metadata = MakeMetadata(AssetHandle(), "missing_source.olomesh");

    // Hand-write a YAML file with a zero MeshSource handle, simulating either
    // a corrupted asset on disk or a Mesh whose MeshSource was deleted.
    const auto yamlPath = Project::GetProjectDirectory() / metadata.FilePath;
    {
        std::ofstream out(yamlPath);
        out << "Mesh:\n"
               "  MeshSource: 0\n"
               "  SubmeshIndex: 0\n";
    }

    MeshSerializer serializer;
    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData must refuse to construct a Mesh with a zero MeshSource handle";
}

TEST_F(MeshAssetSerializerYAMLTest, ClampsOutOfRangeSubmeshIndexInsteadOfAsserting)
{
    // Stale on-disk submesh index (MeshSource has been re-imported with fewer
    // submeshes). Without the clamp, Mesh's ctor assert would crash the editor.
    auto meshSource = MakeMeshSourceWithSubmeshes(2);
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);

    AssetMetadata metadata = MakeMetadata(AssetHandle(), "stale_index.olomesh");
    const auto yamlPath = Project::GetProjectDirectory() / metadata.FilePath;
    {
        std::ofstream out(yamlPath);
        out << "Mesh:\n"
               "  MeshSource: "
            << static_cast<u64>(meshSourceHandle) << "\n"
                                                     "  SubmeshIndex: 99\n";
    }

    MeshSerializer serializer;
    Ref<Asset> loadedAsset;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loadedAsset))
        << "TryLoadData should clamp out-of-range submesh index, not fail";

    Ref<Mesh> loadedMesh = loadedAsset.As<Mesh>();
    ASSERT_TRUE(loadedMesh);
    EXPECT_EQ(loadedMesh->GetSubmeshIndex(), 0u)
        << "Out-of-range SubmeshIndex should be clamped to 0";
}
