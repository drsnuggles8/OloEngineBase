// OLO_TEST_LAYER: L3
//
// End-to-end proof of the virtualized-geometry ASSET-PACK cook (#629).
//
// The cluster LOD DAG was already cooked into the .omesh dev cache, but the asset
// pack — the only artifact a SHIPPED game reads — never carried it. A packaged build
// therefore rebuilt the whole DAG synchronously on the render thread at first draw,
// every launch. This test pins the fix by driving the production
// MeshSourceSerializer::SerializeToAssetPack / DeserializeFromAssetPack pair:
//
//   * a MeshSource carrying a cooked OVGM blob  -> the blob survives the pack round
//     trip byte-for-byte and still deserializes into an equivalent DAG;
//   * a MeshSource with NO blob                 -> round-trips cleanly (empty, not garbage);
//   * a pre-v4 pack (reader pinned to v3)       -> the trailing blob field is NOT read,
//     which is the desync guard required by docs/agent-rules/binary-format-versioning.md.
//
// Needs a GL context because MeshSource::Build() (called by DeserializeFromAssetPack)
// uploads GPU buffers; SKIPs cleanly on headless CI.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace OloEngine;

namespace
{
    // A grid dense enough to produce a multi-level DAG (>>128 triangles), single submesh,
    // unskinned — the shape VirtualMeshBuilder accepts.
    Ref<MeshSource> MakeGridMesh(u32 gridSize)
    {
        auto const verticesPerSide = gridSize + 1;
        TArray<Vertex> vertices;
        vertices.Reserve(static_cast<i32>(verticesPerSide * verticesPerSide));

        for (u32 z = 0; z < verticesPerSide; ++z)
        {
            for (u32 x = 0; x < verticesPerSide; ++x)
            {
                auto const fx = static_cast<f32>(x) / static_cast<f32>(gridSize);
                auto const fz = static_cast<f32>(z) / static_cast<f32>(gridSize);
                vertices.Add(Vertex({ fx, 0.0f, fz }, { 0.0f, 1.0f, 0.0f }, { fx, fz }));
            }
        }

        TArray<u32> indices;
        for (u32 z = 0; z < gridSize; ++z)
        {
            for (u32 x = 0; x < gridSize; ++x)
            {
                u32 const topLeft = z * verticesPerSide + x;
                u32 const topRight = topLeft + 1;
                u32 const bottomLeft = (z + 1) * verticesPerSide + x;
                u32 const bottomRight = bottomLeft + 1;

                indices.Add(topLeft);
                indices.Add(bottomLeft);
                indices.Add(topRight);
                indices.Add(topRight);
                indices.Add(bottomLeft);
                indices.Add(bottomRight);
            }
        }

        return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    }
} // namespace

class VirtualMeshAssetPackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineVirtualMeshPack" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: VirtualMeshAssetPackTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    // Write the registered MeshSource into a one-record pack, then read it back with the
    // reader pinned to `readerVersion` (defaults to the current pack version).
    Ref<MeshSource> RoundTrip(AssetHandle handle, u32 readerVersion = AssetPackFile::Version)
    {
        const fs::path packPath = m_TempDir / "mesh.pack";
        MeshSourceSerializer serializer;
        AssetSerializationInfo info{};
        {
            FileStreamWriter writer(packPath);
            EXPECT_TRUE(writer.IsStreamGood());
            EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
        }

        AssetPackFile::AssetInfo assetInfo{};
        assetInfo.Handle = static_cast<AssetHandle>(0xC0FFEEULL);
        assetInfo.PackedOffset = info.Offset;
        assetInfo.PackedSize = info.Size;
        assetInfo.Type = AssetType::MeshSource;

        FileStreamReader reader(packPath);
        EXPECT_TRUE(reader.IsStreamGood());
        reader.SetArchiveVersion(readerVersion);
        return serializer.DeserializeFromAssetPack(reader, assetInfo).As<MeshSource>();
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(VirtualMeshAssetPackTest, CookedDagSurvivesThePackRoundTrip)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<MeshSource> source = MakeGridMesh(24); // 24*24*2 = 1152 triangles
    ASSERT_TRUE(source);

    // Cook exactly as Model::CookVirtualMesh does.
    VirtualMesh const built = VirtualMeshBuilder::Build(*source);
    ASSERT_TRUE(built.IsValid()) << "the DAG builder rejected the grid mesh";
    ASSERT_GT(built.LevelCount, 1u) << "expected a multi-level DAG to make the round trip meaningful";

    std::vector<u8> const cooked = VirtualMeshSerializer::SerializeToBlob(built);
    ASSERT_FALSE(cooked.empty());
    source->SetVirtualMeshBlob(cooked);
    ASSERT_TRUE(source->HasVirtualMeshBlob());

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = RoundTrip(handle);
    ASSERT_TRUE(unpacked) << "DeserializeFromAssetPack returned null";

    // The blob shipped, byte-for-byte...
    ASSERT_TRUE(unpacked->HasVirtualMeshBlob())
        << "the packed MeshSource carries no DAG — a shipped game would rebuild it on the render thread";
    EXPECT_EQ(unpacked->GetVirtualMeshBlob(), cooked);

    // ...and still deserializes into the same DAG, which is what the registry consumes.
    VirtualMesh restored;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(unpacked->GetVirtualMeshBlob(), restored));
    EXPECT_TRUE(restored.IsValid());
    EXPECT_EQ(restored.LevelCount, built.LevelCount);
    EXPECT_EQ(restored.Clusters.size(), built.Clusters.size());
}

TEST_F(VirtualMeshAssetPackTest, MeshWithoutADagRoundTripsWithAnEmptyBlob)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // Not every MeshSource is virtualized (skinned / multi-submesh / tiny meshes are
    // rejected by the cook). Those must still pack and unpack cleanly — an absent blob
    // is a zero-length field, never garbage bytes.
    Ref<MeshSource> source = MakeGridMesh(8);
    ASSERT_TRUE(source);
    ASSERT_FALSE(source->HasVirtualMeshBlob());

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = RoundTrip(handle);

    ASSERT_TRUE(unpacked);
    EXPECT_FALSE(unpacked->HasVirtualMeshBlob());
    EXPECT_EQ(unpacked->GetVertices().Num(), source->GetVertices().Num());
    EXPECT_EQ(unpacked->GetIndices().Num(), source->GetIndices().Num());
}

TEST_F(VirtualMeshAssetPackTest, PreV4ReaderDoesNotConsumeTheTrailingBlobField)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // The desync guard. A v1-v3 pack never wrote the trailing blob length, so a reader
    // that sees Header.Version < 4 must not try to read it. Pinning the reader to v3
    // simulates loading an older pack: the geometry must still come back intact and the
    // blob must simply be absent — NOT a misread of whatever bytes follow.
    Ref<MeshSource> source = MakeGridMesh(24);
    ASSERT_TRUE(source);

    VirtualMesh const built = VirtualMeshBuilder::Build(*source);
    ASSERT_TRUE(built.IsValid());
    source->SetVirtualMeshBlob(VirtualMeshSerializer::SerializeToBlob(built));
    ASSERT_TRUE(source->HasVirtualMeshBlob());

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = RoundTrip(handle, AssetPackFile::VirtualMeshPackVersion - 1);

    ASSERT_TRUE(unpacked) << "an older pack must still load";
    EXPECT_FALSE(unpacked->HasVirtualMeshBlob())
        << "a pre-v4 reader must not consume the trailing blob field";
    EXPECT_EQ(unpacked->GetVertices().Num(), source->GetVertices().Num());
    EXPECT_EQ(unpacked->GetIndices().Num(), source->GetIndices().Num());
}
