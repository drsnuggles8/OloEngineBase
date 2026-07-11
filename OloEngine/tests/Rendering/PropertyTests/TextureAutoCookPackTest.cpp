// OLO_TEST_LAYER: L3
//
// End-to-end proof of the asset-pack texture auto-cook (#440). An uncompressed source
// PNG, registered as a Texture2D and run through the production
// TextureSerializer::SerializeToAssetPack / DeserializeFromAssetPack pair:
//   * with the cook policy ON  -> comes back BC7-compressed and loaded, and the packed
//     record is materially smaller than the raw RGBA8 pixels;
//   * with the cook policy OFF -> stays an uncompressed record (no BC format), proving
//     the policy actually gates the behaviour rather than always compressing.
// This is the only test that exercises the write-side wiring (flag gating + record
// format selection + embedded-blob round-trip) as one path. Needs a GL context because
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

    // Write a source PNG, load it as an uncompressed Texture2D, register it, and return
    // its handle + on-disk source size.
    AssetHandle StageSourcePng(u32 w, u32 h, sizet& outRawPixels)
    {
        const std::vector<u8> source = MakeGradientRGBA(w, h);
        outRawPixels = source.size();
        m_PngPath = m_TempDir / "Assets" / "albedo_gradient.png";
        EXPECT_NE(::stbi_write_png(m_PngPath.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                   source.data(), static_cast<int>(w) * 4),
                  0);

        Ref<Texture2D> texture = Texture2D::Create(m_PngPath.string(), /*srgb=*/false);
        EXPECT_TRUE(texture && texture->IsLoaded());
        EXPECT_FALSE(IsCompressedFormat(texture->GetSpecification().Format)) << "source must start uncompressed";
        return AssetManager::AddMemoryOnlyAsset(texture);
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
    fs::path m_PngPath;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(TextureAutoCookPackTest, CompressionOnCooksSourcePngToBC7)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawPixels = 0;
    const AssetHandle handle = StageSourcePng(kW, kH, rawPixels);
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
    EXPECT_LT(recordSize, rawPixels) << "cooked record (" << recordSize << ") should be < raw RGBA8 (" << rawPixels << ")";
}

TEST_F(TextureAutoCookPackTest, CompressionOffLeavesTextureUncompressed)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    sizet rawPixels = 0;
    const AssetHandle handle = StageSourcePng(kW, kH, rawPixels);
    ASSERT_TRUE(handle);

    TextureSerializer::SetAssetPackCompressionEnabled(false);
    sizet recordSize = 0;
    Ref<Texture2D> roundTripped = RoundTrip(handle, recordSize);

    ASSERT_TRUE(roundTripped) << "DeserializeFromAssetPack returned null/non-texture";
    EXPECT_TRUE(roundTripped->IsLoaded());
    EXPECT_FALSE(IsCompressedFormat(roundTripped->GetSpecification().Format))
        << "with compression off the texture must stay uncompressed";
}
