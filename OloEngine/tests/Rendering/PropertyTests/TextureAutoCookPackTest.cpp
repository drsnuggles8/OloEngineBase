// OLO_TEST_LAYER: L3
//
// End-to-end proof of the asset-pack texture auto-cook (#440). An uncompressed source,
// registered as a Texture2D and run through the production
// TextureSerializer::SerializeToAssetPack / DeserializeFromAssetPack pair:
//   * cook ON, LDR PNG source  -> comes back BC7-compressed and loaded, record < raw RGBA8;
//   * cook ON, HDR .hdr source -> comes back BC6H-compressed and loaded, record < raw float;
//   * cook OFF                 -> stays an uncompressed record, proving the policy gates it;
//   * cook ON but source missing -> falls back to an uncompressed record (no throw, no BC);
//   * cook ON, an alpha-cutout PNG -> BC7 with the coverage-preserving chain that stops at
//     64 texels (#1453), and an "AlphaMipChain: Box" sidecar reaches the pack cook too;
//   * a hash-named texture -> cooked in the colour space of the material slots that use
//     it, not the filename guess (#1462).
// This is the only test that exercises the write-side wiring (flag gating + format
// selection + embedded-blob round-trip) as one path. Needs a GL context because
// Texture2D::Create uploads the source pixels; SKIPs cleanly on headless CI.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureImportSettings.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <stb_image/stb_image_write.h>

#include <filesystem>
#include <fstream>
#include <unordered_map>
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
        m_TempDir = OloEngine::Tests::TempDir();

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

// #1453: what the asset pack ships for an alpha cutout. A Mask material whose source image
// is not on disk (a packed build) resolves its albedo from this record, so this is the
// chain a shipped game's cutouts sample.
TEST_F(TextureAutoCookPackTest, AnAlphaCutoutShipsItsCoverageChain)
{
    OLO_ENSURE_GPU_OR_SKIP();

#ifndef OLO_TEST_EDITOR_ROOT
    GTEST_SKIP() << "OLO_TEST_EDITOR_ROOT is not defined; the vegetation card cannot be found";
#else
    const fs::path card =
        fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject/Assets/Models/Vegetation/pine/Textures/pine_card.png";
    m_SourcePath = m_TempDir / "Assets" / "pine_card.png";
    std::error_code ec;
    fs::copy_file(card, m_SourcePath, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "cannot stage " << card.string() << ": " << ec.message();

    AssetHandle handle{};
    ASSERT_NO_FATAL_FAILURE(RegisterSource(handle));
    TextureSerializer::SetAssetPackCompressionEnabled(true);

    sizet recordSize = 0;
    Ref<Texture2D> packed = RoundTrip(handle, recordSize);
    ASSERT_TRUE(packed);
    ASSERT_TRUE(packed->IsLoaded());
    EXPECT_EQ(packed->GetSpecification().Format, ImageFormat::BC7);
    EXPECT_TRUE(packed->HasAlphaChannel());
    EXPECT_EQ(packed->GetMipLevelCount(), AlphaCoverageMips::CappedLevelCount(512, 512, 10))
        << "the packed cutout should carry the coverage chain, 512 -> 64";

    // The sidecar is read by the pack cook as well: Box is the plain full chain.
    TextureImportSettings settings;
    settings.AlphaMipChain = TextureImportSettings::AlphaMipChainChoice::Box;
    ASSERT_TRUE(TextureImport::SaveForImage(m_SourcePath.string(), settings));
    Ref<Texture2D> boxed = RoundTrip(handle, recordSize);
    ASSERT_TRUE(boxed && boxed->IsLoaded());
    EXPECT_EQ(boxed->GetMipLevelCount(), 10u);
#endif
}

// #1462: a packed texture is cooked in the colour space of the material SLOTS that use it,
// not the one its file name suggests. The asset system loads a hash-named texture (every
// Sponza texture, "5061699253647017043.png") as LINEAR because nothing in the name says
// albedo; the importer knows it fills a base-colour slot. Loose, the codec re-reads the
// source with the slot's intent; packed, the record's colour space is what ships. The
// cooked record is read back with its source DELETED, so no loose fallback can hide it.
TEST_F(TextureAutoCookPackTest, AHashNamedAlbedoShipsInItsMaterialSlotsColourSpace)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const std::vector<u8> pixels = MakeGradientRGBA(64, 64);
    const auto stage = [&](const char* name, AssetHandle& outHandle)
    {
        m_SourcePath = m_TempDir / "Assets" / name;
        ASSERT_NE(::stbi_write_png(m_SourcePath.string().c_str(), 64, 64, 4, pixels.data(), 64 * 4), 0);
        ASSERT_FALSE(TextureSerializer::IsLikelyColorTextureByName(name))
            << "the fixture needs a name the filename heuristic reads as linear data";
        ASSERT_NO_FATAL_FAILURE(RegisterSource(outHandle));
    };
    AssetHandle albedoHandle{};
    AssetHandle normalHandle{};
    AssetHandle sharedHandle{};
    const fs::path albedoPath = m_TempDir / "Assets" / "5061699253647017043.png";
    ASSERT_NO_FATAL_FAILURE(stage("5061699253647017043.png", albedoHandle));
    ASSERT_NO_FATAL_FAILURE(stage("8773302468495022686.png", normalHandle));
    ASSERT_NO_FATAL_FAILURE(stage("2185308426398714530.png", sharedHandle));

    // One material fills a colour slot and two data slots; a second one uses the third
    // texture as its emissive (colour) map while the first uses it as AO (data).
    Ref<Material> first = Material::CreatePBR("Imported", glm::vec3(1.0f));
    first->SetAlbedoMap(AssetManager::GetAsset<Texture2D>(albedoHandle));
    first->SetNormalMap(AssetManager::GetAsset<Texture2D>(normalHandle));
    first->SetAOMap(AssetManager::GetAsset<Texture2D>(sharedHandle));
    Ref<Material> second = Material::CreatePBR("Emissive", glm::vec3(1.0f));
    second->SetEmissiveMap(AssetManager::GetAsset<Texture2D>(sharedHandle));
    const auto firstAsset = Ref<MaterialAsset>::Create(first);
    const auto secondAsset = Ref<MaterialAsset>::Create(second);
    const AssetHandle firstHandle = AssetManager::AddMemoryOnlyAsset(firstAsset);
    const AssetHandle secondHandle = AssetManager::AddMemoryOnlyAsset(secondAsset);

    const std::unordered_map<AssetHandle, Ref<Asset>> packedAssets = {
        { firstHandle, firstAsset },
        { secondHandle, secondAsset },
        { albedoHandle, AssetManager::GetAsset<Texture2D>(albedoHandle) },
    };
    const auto intents = AssetPackBuilder::CollectTextureColorSpaceIntents(packedAssets);
    ASSERT_TRUE(intents.contains(albedoHandle) && intents.contains(normalHandle) && intents.contains(sharedHandle));
    EXPECT_TRUE(intents.at(albedoHandle)) << "a base-colour slot wants sRGB";
    EXPECT_FALSE(intents.at(normalHandle)) << "a normal-map slot wants linear data";
    EXPECT_TRUE(intents.at(sharedHandle)) << "a colour slot wins over a data slot (the documented rule)";

    struct IntentReset
    {
        IntentReset() = default;
        IntentReset(const IntentReset&) = delete;
        IntentReset& operator=(const IntentReset&) = delete;
        ~IntentReset()
        {
            TextureSerializer::SetAssetPackColorSpaceIntents({});
        }
    } intentReset;

    // Serialize, optionally delete the source, then read the record back.
    const auto packAndRead = [&](AssetHandle handle, bool compress, const fs::path& deleteBeforeRead) -> Ref<Texture2D>
    {
        TextureSerializer::SetAssetPackCompressionEnabled(compress);
        const fs::path packPath = m_TempDir / "slot.pack";
        TextureSerializer serializer;
        AssetSerializationInfo info{};
        {
            FileStreamWriter writer(packPath);
            EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
        }
        if (!deleteBeforeRead.empty())
        {
            std::error_code ec;
            fs::remove(deleteBeforeRead, ec);
            EXPECT_FALSE(fs::exists(deleteBeforeRead)) << "the source must be gone before the record is read";
        }
        AssetPackFile::AssetInfo assetInfo{};
        assetInfo.Handle = handle;
        assetInfo.PackedOffset = info.Offset;
        assetInfo.PackedSize = info.Size;
        assetInfo.Type = AssetType::Texture2D;
        FileStreamReader reader(packPath);
        return serializer.DeserializeFromAssetPack(reader, assetInfo).As<Texture2D>();
    };

    // Negative control: without the intents the pack ships the filename guess.
    TextureSerializer::SetAssetPackColorSpaceIntents({});
    Ref<Texture2D> guessed = packAndRead(albedoHandle, /*compress=*/true, {});
    ASSERT_TRUE(guessed && guessed->IsLoaded());
    EXPECT_FALSE(guessed->GetSpecification().SRGB) << "the negative control no longer reproduces #1462";

    TextureSerializer::SetAssetPackColorSpaceIntents(intents);
    // The uncompressed record re-reads the source, so it runs while the source exists.
    Ref<Texture2D> raw = packAndRead(albedoHandle, /*compress=*/false, {});
    ASSERT_TRUE(raw && raw->IsLoaded());
    EXPECT_FALSE(IsCompressedFormat(raw->GetSpecification().Format));
    EXPECT_TRUE(raw->GetSpecification().SRGB) << "the uncompressed record kept the filename guess";

    Ref<Texture2D> linearData = packAndRead(normalHandle, /*compress=*/true, {});
    ASSERT_TRUE(linearData && linearData->IsLoaded());
    EXPECT_FALSE(linearData->GetSpecification().SRGB) << "a normal map must stay linear";

    Ref<Texture2D> cooked = packAndRead(albedoHandle, /*compress=*/true, albedoPath);
    ASSERT_TRUE(cooked && cooked->IsLoaded());
    EXPECT_EQ(cooked->GetSpecification().Format, ImageFormat::BC7);
    EXPECT_TRUE(cooked->GetSpecification().SRGB) << "the cooked BC7 albedo kept the filename guess (linear)";
}
