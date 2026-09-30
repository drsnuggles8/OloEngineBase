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
//   * a MeshSource carrying a cooked OVGS blob  -> the blob survives the pack round
//     trip byte-for-byte and still deserializes into an equivalent DAG;
//   * a MeshSource with NO blob                 -> round-trips cleanly (empty, not garbage).
//
// Needs a GL context because MeshSource::Build() (called by DeserializeFromAssetPack)
// uploads GPU buffers; SKIPs cleanly on headless CI.

#include "OloEnginePCH.h"

#include <algorithm>
#include <gtest/gtest.h>
#include "TestTempDir.h"

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
        m_TempDir = OloEngine::Tests::TempDir();

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

    // Write the registered MeshSource into a one-record pack, then read it back.
    Ref<MeshSource> RoundTrip(AssetHandle handle)
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

    // Cook exactly as Model::CookVirtualMesh does: a cooked mesh always stores the OVGS set.
    VirtualMeshSet const built = VirtualMeshBuilder::BuildSet(*source);
    ASSERT_TRUE(built.IsValid()) << "the DAG builder rejected the grid mesh";
    ASSERT_EQ(built.Parts.Num(), 1) << "the single-submesh grid should cook to exactly one part";
    ASSERT_GT(built.Parts[0].Dag.LevelCount, 1u) << "expected a multi-level DAG to make the round trip meaningful";

    std::vector<u8> const cooked = VirtualMeshSerializer::SerializeSetToBlob(built);
    ASSERT_FALSE(cooked.empty());
    source->SetVirtualMeshBlob(cooked);
    ASSERT_TRUE(source->HasVirtualMeshBlob());

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = RoundTrip(handle);
    ASSERT_TRUE(unpacked) << "DeserializeFromAssetPack returned null";

    // The blob shipped, byte-for-byte...
    ASSERT_TRUE(unpacked->HasVirtualMeshBlob())
        << "the packed MeshSource carries no DAG — a shipped game would rebuild it on the render thread";
    EXPECT_TRUE(std::ranges::equal(unpacked->GetVirtualMeshBlob(), cooked));

    // ...and still deserializes into the same DAG, which is what the registry consumes.
    VirtualMeshSet restored;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeSetFromBlob(unpacked->GetVirtualMeshBlob(), restored));
    ASSERT_TRUE(restored.IsValid());
    ASSERT_EQ(restored.Parts.Num(), built.Parts.Num());
    EXPECT_EQ(restored.Parts[0].Dag.LevelCount, built.Parts[0].Dag.LevelCount);
    EXPECT_EQ(static_cast<sizet>(restored.Parts[0].Dag.Clusters.Num()),
              static_cast<sizet>(built.Parts[0].Dag.Clusters.Num()));
    EXPECT_EQ(restored.TotalClusters(), built.TotalClusters());
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
