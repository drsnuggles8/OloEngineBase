#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <process.h> // _getpid()
inline int OloGetPid()
{
    return _getpid();
}
#else
#include <unistd.h>
inline int OloGetPid()
{
    return static_cast<int>(getpid());
}
#endif

using namespace OloEngine;

// ============================================================================
// Test fixture with temp file management
// ============================================================================

class AssetPackTest : public ::testing::Test
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

    /// Write a minimal valid asset pack file with the given header values.
    /// Field order must match AssetPack::Load exactly:
    ///   Header → IndexTable → AssetInfos → SceneInfos
    void WriteMinimalPack(const AssetPackFile::FileHeader& header, u32 assetCount, u32 sceneCount)
    {
        FileStreamWriter writer(m_TempPath);
        ASSERT_TRUE(writer.IsStreamGood());

        // Write header
        writer.WriteRaw(header.MagicNumber);
        writer.WriteRaw(header.Version);
        writer.WriteRaw(header.BuildVersion);
        writer.WriteRaw(header.IndexOffset);

        // Write index table
        writer.WriteRaw(assetCount);
        writer.WriteRaw(sceneCount);

        u64 packedAppBinaryOffset = 0;
        u64 packedAppBinarySize = 0;
        writer.WriteRaw(packedAppBinaryOffset);
        writer.WriteRaw(packedAppBinarySize);

        // Write asset infos — Load reads: Handle, PackedOffset, PackedSize, Type, Flags
        for (u32 i = 0; i < assetCount; i++)
        {
            AssetHandle handle = static_cast<AssetHandle>(i + 1);
            u64 packedOffset = 4096;
            u64 packedSize = 64;
            AssetType type = AssetType::Texture2D;
            u16 flags = 0;
            writer.WriteRaw(handle);
            writer.WriteRaw(packedOffset);
            writer.WriteRaw(packedSize);
            writer.WriteRaw(type);
            writer.WriteRaw(flags);
        }

        // Write scene infos — Load reads: Handle, PackedOffset, PackedSize, Flags, u32 assetCount, then per-asset entries
        for (u32 i = 0; i < sceneCount; i++)
        {
            AssetHandle sceneHandle = static_cast<AssetHandle>(1000 + i);
            u64 packedOffset = 8192;
            u64 packedSize = 128;
            u16 sceneFlags = 0;
            u32 sceneAssetCount = 0; // No per-scene assets in minimal pack
            writer.WriteRaw(sceneHandle);
            writer.WriteRaw(packedOffset);
            writer.WriteRaw(packedSize);
            writer.WriteRaw(sceneFlags);
            writer.WriteRaw(sceneAssetCount);
        }

        ASSERT_TRUE(writer.IsStreamGood());
    }

    std::filesystem::path m_TempPath = std::filesystem::temp_directory_path() / ("olo_test_assetpack_" + std::to_string(OloGetPid()) + ".olopack");
};

// ============================================================================
// AssetPackFile::FileHeader layout tests
// ============================================================================

TEST(AssetPackFileTest, HeaderSizeIs24Bytes)
{
    // The header must be exactly 24 bytes for IndexOffset validation to work
    EXPECT_EQ(sizeof(AssetPackFile::FileHeader), 24u);
}

TEST(AssetPackFileTest, DefaultHeaderHasCorrectMagicAndVersion)
{
    AssetPackFile::FileHeader header;
    EXPECT_EQ(header.MagicNumber, 0x504C4F4F);
    EXPECT_EQ(header.Version, 3u);
}

TEST(AssetPackFileTest, IndexOffsetMustBeAtLeastHeaderSize)
{
    // IndexOffset = 0 is invalid — this was the bug that caused the asset pack
    // to fail loading at runtime (AssetPack::Load rejects IndexOffset < 24)
    AssetPackFile::FileHeader header;
    EXPECT_EQ(header.IndexOffset, 0u); // Default is 0 — must be explicitly set

    // The builder MUST set this before writing:
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    EXPECT_GE(header.IndexOffset, sizeof(AssetPackFile::FileHeader));
}

// ============================================================================
// AssetPack::Load validation tests
// ============================================================================

TEST_F(AssetPackTest, LoadFailsIfFileDoesNotExist)
{
    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load("nonexistent_file_12345.olopack");

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::FileNotFound);
}

TEST_F(AssetPackTest, LoadFailsWithInvalidMagicNumber)
{
    AssetPackFile::FileHeader header;
    header.MagicNumber = 0xDEADBEEF;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::InvalidMagicNumber);
}

TEST_F(AssetPackTest, LoadFailsWithZeroIndexOffset)
{
    // This is the exact bug we caught: IndexOffset = 0 means the builder
    // forgot to set it, and the loader correctly rejects it.
    AssetPackFile::FileHeader header;
    header.IndexOffset = 0; // BUG: default value, never set by old builder
    WriteMinimalPack(header, 1, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::CorruptHeader);
}

TEST_F(AssetPackTest, LoadFailsWithZeroAssetCount)
{
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 0, 1);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::CorruptIndex);
}

TEST_F(AssetPackTest, LoadSucceedsWithZeroSceneCount)
{
    // SceneCount == 0 is valid: packs can contain only resources
    // (textures, meshes, etc.) with scenes loaded from disk separately
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_TRUE(result.Success) << "Error: " << result.ErrorMessage;
    EXPECT_TRUE(pack->IsLoaded());
    EXPECT_EQ(pack->GetAllSceneInfos().size(), 0u);
}

TEST_F(AssetPackTest, LoadFailsWithWrongVersion)
{
    AssetPackFile::FileHeader header;
    header.Version = 999;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::UnsupportedVersion);
}

TEST_F(AssetPackTest, LoadFailsWithIndexOffsetBeyondFileSize)
{
    AssetPackFile::FileHeader header;
    header.IndexOffset = 999999; // Way beyond the small file
    WriteMinimalPack(header, 1, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorCode, AssetPackLoadError::CorruptHeader);
}

TEST_F(AssetPackTest, LoadSucceedsWithValidMinimalPack)
{
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 1);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_TRUE(result.Success) << "Error: " << result.ErrorMessage;
    EXPECT_TRUE(pack->IsLoaded());

    const auto& allAssets = pack->GetAllAssetInfos();
    EXPECT_EQ(allAssets.size(), 1u);
}

TEST_F(AssetPackTest, UnloadMakesPackNotLoaded)
{
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 1);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);
    ASSERT_TRUE(result.Success);

    pack->Unload();
    EXPECT_FALSE(pack->IsLoaded());
}

TEST_F(AssetPackTest, IdempotentLoadReturnsTrueOnSecondCall)
{
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 1, 1);

    auto pack = Ref<AssetPack>::Create();
    auto result1 = pack->Load(m_TempPath);
    ASSERT_TRUE(result1.Success);

    // Second load with same path should succeed (idempotent)
    auto result2 = pack->Load(m_TempPath);
    EXPECT_TRUE(result2.Success);
}

TEST_F(AssetPackTest, AssetInfoFieldOrderMatchesLoader)
{
    // Verify the field order written by builder matches what the loader reads.
    // The loader reads: Handle, PackedOffset, PackedSize, Type, Flags
    // A previous bug wrote: Handle, Type, PackedOffset, PackedSize, Flags
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);

    // Write a pack with known asset field values
    const AssetHandle expectedHandle = 42;
    const u64 expectedOffset = 0xDEAD;
    const u64 expectedSize = 0xBEEF;
    const AssetType expectedType = AssetType::Texture2D;
    const u16 expectedFlags = 7;

    {
        FileStreamWriter writer(m_TempPath);
        ASSERT_TRUE(writer.IsStreamGood());

        writer.WriteRaw(header.MagicNumber);
        writer.WriteRaw(header.Version);
        writer.WriteRaw(header.BuildVersion);
        writer.WriteRaw(header.IndexOffset);

        u32 assetCount = 1;
        u32 sceneCount = 0;
        u64 zero64 = 0;
        writer.WriteRaw(assetCount);
        writer.WriteRaw(sceneCount);
        writer.WriteRaw(zero64); // PackedAppBinaryOffset
        writer.WriteRaw(zero64); // PackedAppBinarySize

        // Write fields in the CORRECT order
        writer.WriteRaw(expectedHandle);
        writer.WriteRaw(expectedOffset);
        writer.WriteRaw(expectedSize);
        writer.WriteRaw(expectedType);
        writer.WriteRaw(expectedFlags);
    }

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);
    ASSERT_TRUE(result.Success) << "Error: " << result.ErrorMessage;

    const auto& assets = pack->GetAllAssetInfos();
    ASSERT_EQ(assets.size(), 1u);

    const auto& info = assets[0];
    EXPECT_EQ(info.Handle, expectedHandle);
    EXPECT_EQ(info.PackedOffset, expectedOffset);
    EXPECT_EQ(info.PackedSize, expectedSize);
    EXPECT_EQ(info.Type, expectedType);
    EXPECT_EQ(info.Flags, expectedFlags);
}

TEST_F(AssetPackTest, LoadSucceedsWithMultipleAssetsNoScenes)
{
    // Typical game build: many texture/mesh assets, zero scenes
    // (scenes loaded from .olo files on disk)
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);
    WriteMinimalPack(header, 5, 0);

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);

    EXPECT_TRUE(result.Success) << "Error: " << result.ErrorMessage;
    EXPECT_TRUE(pack->IsLoaded());
    EXPECT_EQ(pack->GetAllAssetInfos().size(), 5u);
    EXPECT_EQ(pack->GetAllSceneInfos().size(), 0u);
}

TEST_F(AssetPackTest, WrongFieldOrderProducesGarbledData)
{
    // Regression test: a previous bug in AssetPackBuilder wrote fields as
    // Handle, TYPE, PackedOffset, PackedSize, Flags (Type before offsets)
    // but the loader reads Handle, PackedOFFSET, PackedSIZE, Type, Flags.
    // This test proves the wrong order corrupts the data.
    AssetPackFile::FileHeader header;
    header.IndexOffset = sizeof(AssetPackFile::FileHeader);

    const AssetHandle handle = 42;
    const u64 correctOffset = 0xAAAA;
    const u64 correctSize = 0xBBBB;
    const AssetType correctType = AssetType::Texture2D;
    const u16 correctFlags = 0;

    {
        FileStreamWriter writer(m_TempPath);
        ASSERT_TRUE(writer.IsStreamGood());

        writer.WriteRaw(header.MagicNumber);
        writer.WriteRaw(header.Version);
        writer.WriteRaw(header.BuildVersion);
        writer.WriteRaw(header.IndexOffset);

        u32 assetCount = 1;
        u32 sceneCount = 0;
        u64 zero64 = 0;
        writer.WriteRaw(assetCount);
        writer.WriteRaw(sceneCount);
        writer.WriteRaw(zero64);
        writer.WriteRaw(zero64);

        // Write in the WRONG order (the old bug): Handle, Type, Offset, Size, Flags
        writer.WriteRaw(handle);
        writer.WriteRaw(correctType);   // Loader reads this as PackedOffset
        writer.WriteRaw(correctOffset); // Loader reads this as PackedSize
        writer.WriteRaw(correctSize);   // Loader reads this as Type
        writer.WriteRaw(correctFlags);
    }

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(m_TempPath);
    ASSERT_TRUE(result.Success);

    const auto& info = pack->GetAllAssetInfos()[0];
    // The loader interprets the fields in its own order, producing garbage
    EXPECT_NE(info.PackedOffset, correctOffset) << "Wrong field order should NOT produce correct offset";
    EXPECT_NE(info.Type, correctType) << "Wrong field order should NOT produce correct type";
}
