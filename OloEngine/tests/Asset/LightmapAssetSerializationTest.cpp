#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// LightmapAssetSerializationTest — the `.olmap` baked-lightmap round-trip and
// hardening contract (issue #439).
//
// Pins four things:
//   1. A full disk round-trip through LightmapSerializer::Serialize ->
//      TryLoadData preserves every field and every texel BIT-exactly (memcmp,
//      not float comparison — the atlas is physical irradiance data and any
//      re-quantisation is silent energy drift).
//   2. A file whose header claims a version above CurrentVersion is rejected
//      outright — a .olmap is a derived artifact with strict versioning
//      (Min == Current), so "newer build wrote it" means re-bake, never guess.
//   3. A file truncated mid-payload fails cleanly (no crash, no asset).
//   4. Non-finite texels are stopped at SERIALIZE time: writing an asset with
//      a NaN texel is refused (error return, no file) — chosen over patching a
//      NaN into the compressed payload, which would be fiddly to aim; the
//      loader independently re-validates finiteness on every read.
//
// The CPU-only serializer creates no GPU resources, so none of this needs a
// GL context. Serialize/TryLoadData resolve metadata.FilePath against the
// active project, so the fixture mounts a throwaway project in TempDir()
// (same pattern as SoundConfigSerializerTest).
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Serialization/LightmapBinaryFormat.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    constexpr u32 kWidth = 64;
    constexpr u32 kHeight = 64;
    constexpr u64 kBakeKey = 0x123456789ABCDEF0ull;

    // A single-page 64x64 atlas with a texel gradient (every texel distinct
    // from its neighbours, alpha 1.0 = "baked"), three entity entries and a
    // non-zero bake key — every persisted field differs from its default.
    Ref<LightmapAsset> MakeBakedLightmap()
    {
        auto lightmap = Ref<LightmapAsset>::Create();
        lightmap->SetDimensions(kWidth, kHeight, 1);
        lightmap->SetBakeKey(kBakeKey);

        std::vector<f32> texels(static_cast<sizet>(lightmap->GetExpectedTexelCount()));
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                sizet const base = (static_cast<sizet>(y) * kWidth + x) * 4;
                texels[base + 0] = static_cast<f32>(x) / static_cast<f32>(kWidth);
                texels[base + 1] = static_cast<f32>(y) / static_cast<f32>(kHeight);
                texels[base + 2] = 0.25f + static_cast<f32>(x + y) * 0.001f;
                texels[base + 3] = 1.0f; // baked
            }
        }
        lightmap->SetTexelData(std::move(texels));

        std::vector<LightmapEntityEntry> entries;
        entries.push_back({ 0x1111111111111111ull, 0, 0, glm::vec4(0.25f, 0.25f, 0.0f, 0.0f) });
        entries.push_back({ 0x2222222222222222ull, 0, 0, glm::vec4(0.5f, 0.5f, 0.25f, 0.0f) });
        entries.push_back({ 0x3333333333333333ull, 0, 0, glm::vec4(0.125f, 0.125f, 0.75f, 0.875f) });
        lightmap->SetEntries(std::move(entries));

        return lightmap;
    }
} // namespace

class LightmapAssetSerializationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_TempDir = OloEngine::Tests::TempDir();

        std::error_code ec;
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: LightmapAssetSerializationTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed for temp project at " << m_TempDir.string();

        m_Metadata.Handle = AssetHandle();
        m_Metadata.Type = AssetType::Lightmap;
        m_Metadata.FilePath = fs::path("Assets") / "baked.olmap";
    }

    [[nodiscard]] fs::path AbsolutePath() const
    {
        return Project::GetProjectDirectory() / m_Metadata.FilePath;
    }

    fs::path m_TempDir;
    AssetMetadata m_Metadata;
};

// -----------------------------------------------------------------------------
// 1. Round-trip: every field and every texel, bit-exact.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, DiskRoundTripIsBitExact)
{
    auto source = MakeBakedLightmap();
    ASSERT_TRUE(source->Validate());

    LightmapSerializer serializer;
    serializer.Serialize(m_Metadata, source);
    ASSERT_TRUE(fs::exists(AbsolutePath())) << "Serialize did not write the .olmap file";

    Ref<Asset> loadedAsset;
    ASSERT_TRUE(serializer.TryLoadData(m_Metadata, loadedAsset))
        << "TryLoadData should succeed for a freshly written lightmap";

    Ref<LightmapAsset> loaded = loadedAsset.As<LightmapAsset>();
    ASSERT_TRUE(loaded) << "Loaded asset is not a LightmapAsset";

    EXPECT_EQ(loaded->GetAssetType(), AssetType::Lightmap);
    EXPECT_EQ(loaded->GetHandle(), m_Metadata.Handle) << "Handle should be propagated from metadata";
    EXPECT_EQ(loaded->GetWidth(), source->GetWidth());
    EXPECT_EQ(loaded->GetHeight(), source->GetHeight());
    EXPECT_EQ(loaded->GetPageCount(), source->GetPageCount());
    EXPECT_EQ(loaded->GetBakeKey(), source->GetBakeKey());
    EXPECT_TRUE(loaded->Validate());

    // Texels: bit-exact. memcmp over the raw f32 buffer, deliberately not a
    // float comparison — identical bits is the contract.
    ASSERT_EQ(loaded->GetTexelData().size(), source->GetTexelData().size());
    EXPECT_EQ(0, std::memcmp(loaded->GetTexelData().data(), source->GetTexelData().data(),
                             source->GetTexelData().size() * sizeof(f32)))
        << "Texel data did not round-trip bit-exactly";

    // Entity table: field-by-field, ScaleOffset again by bits.
    ASSERT_EQ(loaded->GetEntries().size(), source->GetEntries().size());
    for (sizet i = 0; i < source->GetEntries().size(); ++i)
    {
        const auto& a = source->GetEntries()[i];
        const auto& b = loaded->GetEntries()[i];
        EXPECT_EQ(a.EntityUUID, b.EntityUUID) << "entry " << i;
        EXPECT_EQ(a.Page, b.Page) << "entry " << i;
        EXPECT_EQ(0, std::memcmp(&a.ScaleOffset, &b.ScaleOffset, sizeof(glm::vec4)))
            << "entry " << i << " ScaleOffset did not round-trip bit-exactly";
    }
}

// -----------------------------------------------------------------------------
// 2. A version above CurrentVersion is rejected outright.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, VersionAboveCurrentIsRejected)
{
    auto source = MakeBakedLightmap();
    LightmapSerializer serializer;
    serializer.Serialize(m_Metadata, source);
    ASSERT_TRUE(fs::exists(AbsolutePath()));

    // Patch FileHeader::Version (u32 at byte offset 4) to CurrentVersion + 1.
    // The CRC only covers the payload, so this isolates the version gate.
    {
        std::fstream file(AbsolutePath(), std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        constexpr u32 futureVersion = OLmapFormat::CurrentVersion + 1;
        file.seekp(static_cast<std::streamoff>(offsetof(OLmapFormat::FileHeader, Version)), std::ios::beg);
        file.write(reinterpret_cast<const char*>(&futureVersion), sizeof(futureVersion));
        ASSERT_FALSE(file.fail());
    }

    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(m_Metadata, loadedAsset))
        << "A file claiming a future format version must be rejected";
    EXPECT_FALSE(loadedAsset) << "Rejection must not return a partially-populated asset";
}

// -----------------------------------------------------------------------------
// 3. Truncation mid-payload fails cleanly — no crash, no asset.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, TruncatedFileIsRejectedCleanly)
{
    auto source = MakeBakedLightmap();
    LightmapSerializer serializer;
    serializer.Serialize(m_Metadata, source);
    ASSERT_TRUE(fs::exists(AbsolutePath()));

    auto const fullSize = fs::file_size(AbsolutePath());
    ASSERT_GT(fullSize, sizeof(OLmapFormat::FileHeader))
        << "Written file is implausibly small; the truncation below would only cut the header";

    // Cut the file in half — past the header, mid-way through the compressed
    // payload.
    std::error_code ec;
    fs::resize_file(AbsolutePath(), fullSize / 2, ec);
    ASSERT_FALSE(ec) << "resize_file failed: " << ec.message();

    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(m_Metadata, loadedAsset))
        << "A truncated file must be rejected";
    EXPECT_FALSE(loadedAsset);
}

// -----------------------------------------------------------------------------
// 4. Non-finite texels are refused at serialize time (error return, no file).
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, SerializeRefusesNonFiniteTexels)
{
    auto source = MakeBakedLightmap();
    // Poke a NaN into one texel's green channel.
    source->GetTexelData()[42 * 4 + 1] = std::numeric_limits<f32>::quiet_NaN();
    ASSERT_FALSE(source->Validate());

    // The file-level helper reports the refusal explicitly...
    EXPECT_FALSE(LightmapSerializer::SerializeToFile(AbsolutePath(), source))
        << "SerializeToFile must refuse an asset with a non-finite texel";
    // ...and nothing may have been written: a poisoned atlas on disk would
    // fail every future load of the scene.
    EXPECT_FALSE(fs::exists(AbsolutePath()))
        << "A refused serialize must not leave a file behind";

    // The AssetSerializer-interface path (void return) refuses the same way.
    LightmapSerializer serializer;
    serializer.Serialize(m_Metadata, source);
    EXPECT_FALSE(fs::exists(AbsolutePath()));
}
