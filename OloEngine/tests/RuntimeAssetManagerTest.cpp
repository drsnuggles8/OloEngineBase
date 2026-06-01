#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h> // _getpid()
inline int OloRtGetPid()
{
    return _getpid();
}
#else
#include <unistd.h>
inline int OloRtGetPid()
{
    return static_cast<int>(getpid());
}
#endif

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// ============================================================================
// RuntimeAssetManagerTest — pins the runtime asset-pack metadata-indexing fix.
//
// Regression context: RuntimeAssetManager::LoadAssetPack stored the loaded pack
// in m_LoadedPacks but never indexed its contents into m_AssetMetadata. Because
// every metadata-keyed path (GetAsset, IsAssetValid, IsAssetHandleValid,
// GetAssetType, GetAllAssetsWithType, IsPhysicalAsset, ...) consults
// m_AssetMetadata, a freshly loaded pack was completely invisible: GetAsset
// always failed with "No metadata found" and the standalone runtime could not
// load a single packed asset. These tests assert that loading a pack makes its
// assets discoverable through the metadata-keyed API, and that unloading removes
// them again.
//
// The tests deliberately exercise only the metadata layer (no deserialization):
// the minimal packs carry an index but no asset payload, which is exactly the
// set of queries the indexing fix is responsible for. Full deserialization is
// covered by the per-type *AssetPackSerializer round-trip tests.
// ============================================================================

class RuntimeAssetManagerTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        std::error_code ec;
        if (!m_TempPath.empty() && std::filesystem::exists(m_TempPath))
        {
            std::filesystem::remove(m_TempPath, ec);
        }
    }

    /// Write a minimal valid asset pack containing the given (handle, type) assets.
    /// Field order mirrors AssetPack::Load exactly:
    ///   Header(Magic, Version, BuildVersion, IndexOffset)
    ///   IndexTable(AssetCount, SceneCount, AppBinaryOffset, AppBinarySize)
    ///   per asset: Handle, PackedOffset, PackedSize, Type, Flags
    void WritePack(const std::vector<std::pair<AssetHandle, AssetType>>& assets) const
    {
        FileStreamWriter writer(m_TempPath);
        ASSERT_TRUE(writer.IsStreamGood());

        AssetPackFile::FileHeader header;
        header.IndexOffset = sizeof(AssetPackFile::FileHeader);
        writer.WriteRaw(header.MagicNumber);
        writer.WriteRaw(header.Version);
        writer.WriteRaw(header.BuildVersion);
        writer.WriteRaw(header.IndexOffset);

        const u32 assetCount = static_cast<u32>(assets.size());
        const u32 sceneCount = 0;
        const u64 zero64 = 0;
        writer.WriteRaw(assetCount);
        writer.WriteRaw(sceneCount);
        writer.WriteRaw(zero64); // PackedAppBinaryOffset
        writer.WriteRaw(zero64); // PackedAppBinarySize

        for (const auto& asset : assets)
        {
            AssetHandle handle = asset.first;
            AssetType type = asset.second;
            u64 packedOffset = 4096; // Past the index; never read by the metadata path
            u64 packedSize = 64;
            u16 flags = 0;
            writer.WriteRaw(handle);
            writer.WriteRaw(packedOffset);
            writer.WriteRaw(packedSize);
            writer.WriteRaw(type);
            writer.WriteRaw(flags);
        }

        ASSERT_TRUE(writer.IsStreamGood());
    }

    std::filesystem::path m_TempPath =
        std::filesystem::temp_directory_path() /
        ("olo_test_runtime_assetmgr_" + std::to_string(OloRtGetPid()) + ".olopack");
};

// ----------------------------------------------------------------------------

TEST_F(RuntimeAssetManagerTest, LoadedPackAssetsAreDiscoverable)
{
    const AssetHandle texHandle = 101;
    const AssetHandle meshHandle = 202;
    WritePack({ { texHandle, AssetType::Texture2D }, { meshHandle, AssetType::StaticMesh } });

    RuntimeAssetManager manager;
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath));

    // Before the fix all of these were false / None because m_AssetMetadata was
    // never populated on load.
    EXPECT_TRUE(manager.IsAssetHandleValid(texHandle));
    EXPECT_TRUE(manager.IsAssetHandleValid(meshHandle));
    EXPECT_TRUE(manager.IsAssetValid(texHandle));
    EXPECT_FALSE(manager.IsAssetMissing(texHandle));
    EXPECT_TRUE(manager.IsPhysicalAsset(texHandle));

    EXPECT_EQ(manager.GetAssetType(texHandle), AssetType::Texture2D);
    EXPECT_EQ(manager.GetAssetType(meshHandle), AssetType::StaticMesh);

    const AssetMetadata texMeta = manager.GetAssetMetadata(texHandle);
    EXPECT_TRUE(texMeta.IsValid());
    EXPECT_EQ(texMeta.Handle, texHandle);
    EXPECT_EQ(texMeta.Type, AssetType::Texture2D);
}

TEST_F(RuntimeAssetManagerTest, UnknownHandleIsNotValid)
{
    const AssetHandle known = 101;
    WritePack({ { known, AssetType::Texture2D } });

    RuntimeAssetManager manager;
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath));

    const AssetHandle unknown = 999;
    EXPECT_FALSE(manager.IsAssetHandleValid(unknown));
    EXPECT_TRUE(manager.IsAssetMissing(unknown));
    EXPECT_FALSE(manager.IsPhysicalAsset(unknown));
    EXPECT_EQ(manager.GetAssetType(unknown), AssetType::None);
    EXPECT_FALSE(manager.GetAssetMetadata(unknown).IsValid());
}

TEST_F(RuntimeAssetManagerTest, GetAllAssetsWithTypeReturnsMatchingHandles)
{
    const AssetHandle t1 = 1;
    const AssetHandle t2 = 2;
    const AssetHandle m3 = 3;
    WritePack({
        { t1, AssetType::Texture2D },
        { t2, AssetType::Texture2D },
        { m3, AssetType::StaticMesh },
    });

    RuntimeAssetManager manager;
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath));

    const auto textures = manager.GetAllAssetsWithType(AssetType::Texture2D);
    EXPECT_EQ(textures.size(), 2u);
    EXPECT_TRUE(textures.contains(t1));
    EXPECT_TRUE(textures.contains(t2));
    EXPECT_FALSE(textures.contains(m3));

    const auto meshes = manager.GetAllAssetsWithType(AssetType::StaticMesh);
    EXPECT_EQ(meshes.size(), 1u);
    EXPECT_TRUE(meshes.contains(m3));
}

TEST_F(RuntimeAssetManagerTest, UnloadingPackRemovesItsAssets)
{
    const AssetHandle handle = 55;
    WritePack({ { handle, AssetType::Texture2D } });

    RuntimeAssetManager manager;
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath));
    ASSERT_TRUE(manager.IsAssetHandleValid(handle));

    manager.UnloadAssetPack(m_TempPath);

    EXPECT_FALSE(manager.IsAssetHandleValid(handle));
    EXPECT_TRUE(manager.IsAssetMissing(handle));
    EXPECT_EQ(manager.GetAssetType(handle), AssetType::None);
    EXPECT_TRUE(manager.GetAllAssetsWithType(AssetType::Texture2D).empty());
}

TEST_F(RuntimeAssetManagerTest, MetadataSurvivesUntilExplicitUnload)
{
    // A second LoadAssetPack of the same path is idempotent (AssetPack::Load is)
    // and must not drop the already-indexed metadata.
    const AssetHandle handle = 77;
    WritePack({ { handle, AssetType::Texture2D } });

    RuntimeAssetManager manager;
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath));
    ASSERT_TRUE(manager.LoadAssetPack(m_TempPath)); // idempotent re-load

    EXPECT_TRUE(manager.IsAssetHandleValid(handle));
    EXPECT_EQ(manager.GetAssetType(handle), AssetType::Texture2D);
}
