#include <gtest/gtest.h>

#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;

// Temp file helper
class SaveGameFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string testName = std::string(testInfo->test_suite_name()) + "_" + testInfo->name();
        m_TempPath = std::filesystem::temp_directory_path() / ("olo_test_" + testName + ".olosave");
        // Clean up any leftover
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    std::filesystem::path m_TempPath;
};

// ========================================================================
// Header
// ========================================================================

TEST(SaveGameHeaderTest, DefaultValues)
{
    SaveGameHeader header;
    EXPECT_EQ(header.Magic, kSaveGameMagic);
    EXPECT_EQ(header.FormatVersion, kSaveGameFormatVersion);
    EXPECT_TRUE(header.IsValid());
    EXPECT_EQ(header.GetCompression(), SaveGameCompression::None);
}

TEST(SaveGameHeaderTest, SizeIs128Bytes)
{
    EXPECT_EQ(sizeof(SaveGameHeader), kSaveGameHeaderSize);
    EXPECT_EQ(sizeof(SaveGameHeader), 128u);
}

TEST(SaveGameHeaderTest, CompressionFlags)
{
    SaveGameHeader header;
    header.SetCompression(SaveGameCompression::Zlib);
    EXPECT_EQ(header.GetCompression(), SaveGameCompression::Zlib);

    header.SetCompression(SaveGameCompression::None);
    EXPECT_EQ(header.GetCompression(), SaveGameCompression::None);
}

TEST(SaveGameHeaderTest, InvalidMagic)
{
    SaveGameHeader header;
    header.Magic = 0xDEADBEEF;
    EXPECT_FALSE(header.IsValid());
}

// ========================================================================
// Metadata Serialization
// ========================================================================

TEST(SaveGameMetadataTest, RoundTrip)
{
    SaveGameMetadata original;
    original.DisplayName = "Test Save";
    original.SceneName = "TestScene";
    original.TimestampUTC = 1700000000;
    original.PlaytimeSeconds = 123.45f;
    original.SlotType = SaveSlotType::QuickSave;
    original.EntityCount = 42;
    original.ThumbnailAvailable = true;

    // Serialize
    std::vector<u8> buffer;
    {
        FMemoryWriter writer(buffer);
        writer.ArIsSaveGame = true;
        writer << original;
    }

    // Deserialize
    SaveGameMetadata loaded;
    {
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        reader << loaded;
    }

    EXPECT_EQ(loaded.DisplayName, "Test Save");
    EXPECT_EQ(loaded.SceneName, "TestScene");
    EXPECT_EQ(loaded.TimestampUTC, 1700000000);
    EXPECT_FLOAT_EQ(loaded.PlaytimeSeconds, 123.45f);
    EXPECT_EQ(loaded.SlotType, SaveSlotType::QuickSave);
    EXPECT_EQ(loaded.EntityCount, 42u);
    EXPECT_TRUE(loaded.ThumbnailAvailable);
}

// ========================================================================
// Compression
// ========================================================================

TEST(SaveGameCompressionTest, CompressDecompress)
{
    // Create some compressible data
    std::vector<u8> original(4096);
    for (sizet i = 0; i < original.size(); ++i)
    {
        original[i] = static_cast<u8>(i % 64);
    }

    std::vector<u8> compressed;
    ASSERT_TRUE(SaveGameFile::Compress(original, compressed));
    EXPECT_GT(compressed.size(), 0u);
    EXPECT_LT(compressed.size(), original.size()); // Repetitive data should compress

    std::vector<u8> decompressed;
    ASSERT_TRUE(SaveGameFile::Decompress(compressed, original.size(), decompressed));
    EXPECT_EQ(decompressed.size(), original.size());
    EXPECT_EQ(decompressed, original);
}

TEST(SaveGameCompressionTest, EmptyData)
{
    std::vector<u8> empty;
    std::vector<u8> compressed;
    // Empty input is valid — returns true with empty output
    EXPECT_TRUE(SaveGameFile::Compress(empty, compressed));
    EXPECT_TRUE(compressed.empty());
}

// ========================================================================
// File Write/Read Round-Trip
// ========================================================================

TEST_F(SaveGameFileTest, WriteAndReadHeader)
{
    SaveGameHeader writeHeader;
    writeHeader.EntityCount = 10;
    writeHeader.SetCompression(SaveGameCompression::Zlib);

    SaveGameMetadata meta;
    meta.DisplayName = "TestSave";
    meta.SceneName = "TestScene";

    std::vector<u8> thumbnail = { 0x89, 0x50, 0x4E, 0x47 }; // PNG magic
    std::vector<u8> payload = { 1, 2, 3, 4, 5 };

    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, writeHeader, meta, thumbnail, payload));
    ASSERT_TRUE(std::filesystem::exists(m_TempPath));

    SaveGameHeader readHeader;
    ASSERT_TRUE(SaveGameFile::ReadHeader(m_TempPath, readHeader));
    EXPECT_TRUE(readHeader.IsValid());
    EXPECT_EQ(readHeader.EntityCount, 10u);
    EXPECT_EQ(readHeader.GetCompression(), SaveGameCompression::Zlib);
}

TEST_F(SaveGameFileTest, WriteAndReadMetadata)
{
    SaveGameHeader header;
    SaveGameMetadata writeMeta;
    writeMeta.DisplayName = "My Save";
    writeMeta.SceneName = "Level1";
    writeMeta.TimestampUTC = 1700000000;
    writeMeta.PlaytimeSeconds = 60.0f;
    writeMeta.EntityCount = 5;

    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, writeMeta, {}, { 1, 2, 3 }));

    SaveGameHeader readHeader;
    SaveGameMetadata readMeta;
    ASSERT_TRUE(SaveGameFile::ReadMetadata(m_TempPath, readHeader, readMeta));

    EXPECT_EQ(readMeta.DisplayName, "My Save");
    EXPECT_EQ(readMeta.SceneName, "Level1");
    EXPECT_EQ(readMeta.TimestampUTC, 1700000000);
    EXPECT_FLOAT_EQ(readMeta.PlaytimeSeconds, 60.0f);
    EXPECT_EQ(readMeta.EntityCount, 5u);
}

TEST_F(SaveGameFileTest, WriteAndReadThumbnail)
{
    SaveGameHeader header;
    SaveGameMetadata meta;
    std::vector<u8> thumbnail = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A };

    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, thumbnail, { 1 }));

    std::vector<u8> readThumb;
    ASSERT_TRUE(SaveGameFile::ReadThumbnail(m_TempPath, readThumb));
    EXPECT_EQ(readThumb, thumbnail);
}

TEST_F(SaveGameFileTest, WriteAndReadPayload)
{
    SaveGameHeader header;
    SaveGameMetadata meta;
    std::vector<u8> payload = { 10, 20, 30, 40, 50, 60, 70, 80 };

    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, {}, payload));

    std::vector<u8> readPayload;
    ASSERT_TRUE(SaveGameFile::ReadPayload(m_TempPath, readPayload));
    EXPECT_EQ(readPayload, payload);
}

TEST_F(SaveGameFileTest, ChecksumValidation)
{
    SaveGameHeader header;
    SaveGameMetadata meta;
    meta.DisplayName = "ChecksumTest";

    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, {}, { 1, 2, 3, 4 }));
    EXPECT_TRUE(SaveGameFile::ValidateChecksum(m_TempPath));

    // Corrupt a byte after the header
    {
        std::fstream file(m_TempPath, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(kSaveGameHeaderSize + 1);
        char c = '\xFF';
        file.write(&c, 1);
    }

    EXPECT_FALSE(SaveGameFile::ValidateChecksum(m_TempPath));
}

TEST_F(SaveGameFileTest, ReadNonExistentFile)
{
    auto missingPath = m_TempPath.parent_path() / (m_TempPath.stem().string() + "_missing.olosave");
    std::error_code ec;
    std::filesystem::remove(missingPath, ec);

    SaveGameHeader header;
    EXPECT_FALSE(SaveGameFile::ReadHeader(missingPath, header));
}

TEST_F(SaveGameFileTest, EmptyThumbnail)
{
    SaveGameHeader header;
    SaveGameMetadata meta;
    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, {}, { 1 }));

    std::vector<u8> readThumb;
    ASSERT_TRUE(SaveGameFile::ReadThumbnail(m_TempPath, readThumb));
    EXPECT_TRUE(readThumb.empty());
}
