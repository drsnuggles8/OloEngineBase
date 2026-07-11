// OLO_TEST_LAYER: L3
//
// End-to-end proof of the asset-pack texture auto-cook (#440). An uncompressed source,
// registered as a Texture2D and run through the production
// TextureSerializer::SerializeToAssetPack / DeserializeFromAssetPack pair:
//   * cook ON, LDR PNG source  -> comes back BC7-compressed and loaded, record < raw RGBA8;
//   * cook ON, HDR .hdr source -> comes back BC6H-compressed and loaded, record < raw float;
//   * cook OFF                 -> stays an uncompressed record, proving the policy gates it;
//   * cook ON but source missing -> falls back to an uncompressed record (no throw, no BC).
// This is the only test that exercises the write-side wiring (flag gating + format
// selection + embedded-blob round-trip) as one path. Needs a GL context because
// Texture2D::Create uploads the source pixels; SKIPs cleanly on headless CI.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <stb_image/stb_image_write.h>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace OloEngine;        // NOLINT(google-build-using-namespace)
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    std::vector<u8> MakeGradientRGBA(u32 width, u32 height)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 4];
                p[0] = static_cast<u8>((x * 255) / (width - 1));
                p[1] = static_cast<u8>((y * 255) / (height - 1));
                p[2] = static_cast<u8>(128);
                p[3] = 255;
            }
        }
        return pixels;
    }

    std::vector<f32> MakeGradientHDR(u32 width, u32 height)
    {
        std::vector<f32> pixels(static_cast<sizet>(width) * height * 3);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                f32* p = &pixels[(static_cast<sizet>(y) * width + x) * 3];
                p[0] = 0.05f + 6.0f * (static_cast<f32>(x) / (width - 1)); // > 1.0 -> genuinely HDR
                p[1] = 0.05f + 6.0f * (static_cast<f32>(y) / (height - 1));
                p[2] = 0.5f;
            }
        }
        return pixels;
    }
} // namespace

class TextureAutoCookPackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineTextureAutoCook" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: TextureAutoCookPackTest\n"
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
        // Never leak the cook policy into other tests.
        TextureSerializer::SetAssetPackCompressionEnabled(false);
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    // Load the just-written m_SourcePath as an uncompressed Texture2D and register it.
    // Fatal assertions (surfaced via ASSERT_NO_FATAL_FAILURE at the call site) so a failed
    // write/load never dereferences a null texture or registers a bad asset.
    void RegisterSource(AssetHandle& outHandle)
    {
        outHandle = {};
        Ref<Texture2D> texture = Texture2D::Create(m_SourcePath.string(), /*srgb=*/false);
        ASSERT_TRUE(texture) << "Texture2D::Create returned null for " << m_SourcePath.string();
        ASSERT_TRUE(texture->IsLoaded()) << "source texture failed to load: " << m_SourcePath.string();
        ASSERT_FALSE(IsCompressedFormat(texture->GetSpecification().Format)) << "source must start uncompressed";
        outHandle = AssetManager::AddMemoryOnlyAsset(texture);
    }

    // Write an LDR PNG source, load + register it. `outRawBytes` = raw RGBA8 byte count.
    void StageSourcePng(u32 w, u32 h, sizet& outRawBytes, AssetHandle& outHandle)
    {
        const std::vector<u8> source = MakeGradientRGBA(w, h);
        m_SourcePath = m_TempDir / "Assets" / "albedo_gradient.png";
        ASSERT_NE(::stbi_write_png(m_SourcePath.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                   source.data(), static_cast<int>(w) * 4),
                  0)
            << "failed to write source PNG";
        ASSERT_NO_FATAL_FAILURE(RegisterSource(outHandle));
        outRawBytes = source.size();
    }

    // Write an HDR (.hdr) source, load + register it. `outRawBytes` = raw float byte count.
    void StageSourceHdr(u32 w, u32 h, sizet& outRawBytes, AssetHandle& outHandle)
    {
        const std::vector<f32> source = MakeGradientHDR(w, h);
        m_SourcePath = m_TempDir / "Assets" / "sky.hdr";
        ASSERT_NE(::stbi_write_hdr(m_SourcePath.string().c_str(), static_cast<int>(w), static_cast<int>(h), 3, source.data()), 0)
            << "failed to write source HDR";
        // Texture2D::Create path-loads via stbi_load (8-bit); the cook re-reads the .hdr
        // via stbi_loadf and picks BC6H from stbi_is_hdr — so the live texture is LDR but
        // the packed record is BC6H.
        ASSERT_NO_FATAL_FAILURE(RegisterSource(outHandle));
        outRawBytes = source.size() * sizeof(f32);
    }

    // Round-trip a registered texture through the production serializer and return the
    // reconstructed asset.
    Ref<Texture2D> RoundTrip(AssetHandle handle, sizet& outRecordSize)
    {
        const fs::path packPath = m_TempDir / "texture.pack";
        TextureSerializer serializer;
        AssetSerializationInfo info{};
        {
            FileStreamWriter writer(packPath);
            EXPECT_TRUE(writer.IsStreamGood());
            EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
        }
        outRecordSize = info.Size;

        AssetPackFile::AssetInfo assetInfo{};
        assetInfo.Handle = static_cast<AssetHandle>(0xC0FFEEULL);
        assetInfo.PackedOffset = info.Offset;
        assetInfo.PackedSize = info.Size;
        assetInfo.Type = AssetType::Texture2D;

        FileStreamReader reader(packPath);
        EXPECT_TRUE(reader.IsStreamGood());
        return serializer.DeserializeFromAssetPack(reader, assetInfo).As<Texture2D>();
    }

    fs::path m_TempDir;
    fs::path m_SourcePath;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(TextureAutoCookPackTest, CompressionOnCooksSourcePngToBC7)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawBytes = 0;
    AssetHandle handle{};
    ASSERT_NO_FATAL_FAILURE(StageSourcePng(kW, kH, rawBytes, handle));
    ASSERT_TRUE(handle);

    TextureSerializer::SetAssetPackCompressionEnabled(true);
    sizet recordSize = 0;
    Ref<Texture2D> roundTripped = RoundTrip(handle, recordSize);

    ASSERT_TRUE(roundTripped) << "DeserializeFromAssetPack returned null/non-texture";
    EXPECT_TRUE(roundTripped->IsLoaded());
    EXPECT_EQ(roundTripped->GetWidth(), kW);
    EXPECT_EQ(roundTripped->GetHeight(), kH);
    EXPECT_EQ(roundTripped->GetSpecification().Format, ImageFormat::BC7)
        << "auto-cook should have shipped this colour PNG as BC7";
    EXPECT_EQ(roundTripped->GetHandle(), static_cast<AssetHandle>(0xC0FFEEULL));
    // The embedded BC7 mip chain (+ record header) must beat the raw RGBA8 pixels.
    EXPECT_LT(recordSize, rawBytes) << "cooked record (" << recordSize << ") should be < raw RGBA8 (" << rawBytes << ")";
}

TEST_F(TextureAutoCookPackTest, CompressionOnCooksHdrSourceToBC6H)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawBytes = 0;
    AssetHandle handle{};
    ASSERT_NO_FATAL_FAILURE(StageSourceHdr(kW, kH, rawBytes, handle));
    ASSERT_TRUE(handle);

    TextureSerializer::SetAssetPackCompressionEnabled(true);
    sizet recordSize = 0;
    Ref<Texture2D> roundTripped = RoundTrip(handle, recordSize);

    ASSERT_TRUE(roundTripped) << "DeserializeFromAssetPack returned null/non-texture";
    EXPECT_TRUE(roundTripped->IsLoaded());
    EXPECT_EQ(roundTripped->GetWidth(), kW);
    EXPECT_EQ(roundTripped->GetHeight(), kH);
    EXPECT_EQ(roundTripped->GetSpecification().Format, ImageFormat::BC6H)
        << "auto-cook should have shipped this HDR source as BC6H";
    EXPECT_EQ(roundTripped->GetHandle(), static_cast<AssetHandle>(0xC0FFEEULL));
    // The embedded BC6H mip chain (+ record header) must beat the raw HDR float pixels.
    EXPECT_LT(recordSize, rawBytes) << "cooked BC6H record (" << recordSize << ") should be < raw HDR float bytes (" << rawBytes << ")";
}

TEST_F(TextureAutoCookPackTest, CompressionOffLeavesTextureUncompressed)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawBytes = 0;
    AssetHandle handle{};
    ASSERT_NO_FATAL_FAILURE(StageSourcePng(kW, kH, rawBytes, handle));
    ASSERT_TRUE(handle);

    TextureSerializer::SetAssetPackCompressionEnabled(false);
    sizet recordSize = 0;
    Ref<Texture2D> roundTripped = RoundTrip(handle, recordSize);

    ASSERT_TRUE(roundTripped) << "DeserializeFromAssetPack returned null/non-texture";
    EXPECT_TRUE(roundTripped->IsLoaded());
    EXPECT_FALSE(IsCompressedFormat(roundTripped->GetSpecification().Format))
        << "with compression off the texture must stay uncompressed";
}

TEST_F(TextureAutoCookPackTest, CookFailureFallsBackToUncompressedRecord)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawBytes = 0;
    AssetHandle handle{};
    ASSERT_NO_FATAL_FAILURE(StageSourcePng(kW, kH, rawBytes, handle));
    ASSERT_TRUE(handle);

    // Delete the source file *after* the texture loaded: the cook is enabled but the
    // auto-cook guard's exists() check now fails, so SerializeToAssetPack must fall back
    // to the uncompressed raw record rather than throw or emit a compressed record.
    std::error_code ec;
    fs::remove(m_SourcePath, ec);

    TextureSerializer::SetAssetPackCompressionEnabled(true);
    sizet recordSize = 0;
    Ref<Texture2D> roundTripped = RoundTrip(handle, recordSize);

    // The record must reconstruct a (non-null) texture whose format is NOT block-compressed.
    // (Its IsLoaded may be false since the source file is gone — that's the path-load
    // fallback, not the point here; the point is the record was written uncompressed.)
    ASSERT_TRUE(roundTripped) << "even a fallback record must reconstruct a non-null texture";
    EXPECT_FALSE(IsCompressedFormat(roundTripped->GetSpecification().Format))
        << "cook failure must leave the texture as an uncompressed record, not a BC format";
}
