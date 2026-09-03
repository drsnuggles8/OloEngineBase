#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// LightmapAssetSerializationTest — the `.olmap` baked-lightmap round-trip and
// hardening contract (issue #439).
//
// Pins five things:
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
//   5. Hostile-header hardening: a tiny file whose header/section fields claim
//      huge sizes (32 GiB of texels via atlas parameters at the caps, an
//      entity table far beyond the actual payload, a 2 GB uncompressed-size
//      claim on a few-KB compressed stream) is rejected cleanly BEFORE any
//      allocation is sized from those untrusted values — the tests below run
//      in milliseconds precisely because nothing giant is ever allocated.
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
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Serialization/LightmapBinaryFormat.h"
#include "OloEngine/Serialization/ZlibSection.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
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
        // Sub-keys on purpose (issue #867), and two entries SHARING a UUID: that
        // is the shape an InstancedMeshComponent produces, and it is exactly what
        // a reader still assuming one region per entity would collapse.
        const auto makeEntry = [](u64 uuid, u64 subKey, const glm::vec4& region)
        {
            LightmapEntityEntry entry;
            entry.EntityUUID = uuid;
            entry.SubKey = subKey;
            entry.ScaleOffset = region;
            return entry;
        };
        entries.push_back(makeEntry(0x1111111111111111ull, 0, glm::vec4(0.25f, 0.25f, 0.0f, 0.0f)));
        entries.push_back(makeEntry(0x2222222222222222ull, 0x8000000000000004ull,
                                    glm::vec4(0.5f, 0.5f, 0.25f, 0.0f)));
        entries.push_back(makeEntry(0x2222222222222222ull, 5ull,
                                    glm::vec4(0.125f, 0.125f, 0.75f, 0.875f)));
        lightmap->SetEntries(std::move(entries));

        return lightmap;
    }

    void AppendRaw(std::vector<u8>& out, const void* data, sizet size)
    {
        const auto* bytes = static_cast<const u8*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    // Wraps a hand-crafted payload in a syntactically valid UNCOMPRESSED
    // .olmap byte stream: correct magic/version, Flags = 0, CRC32 over the
    // payload bytes and UncompressedPayloadSize == payload size. Every outer
    // header check passes, so the decode reaches the framed sections and the
    // hostile values under test.
    std::vector<u8> WrapUncompressedPayload(const std::vector<u8>& payload)
    {
        OLmapFormat::FileHeader header;
        header.Flags = 0;
        header.Checksum = Hash::CRC32(payload.data(), payload.size());
        header.UncompressedPayloadSize = payload.size();

        std::vector<u8> bytes;
        bytes.reserve(sizeof(header) + payload.size());
        AppendRaw(bytes, &header, sizeof(header));
        AppendRaw(bytes, payload.data(), payload.size());
        return bytes;
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
        // The sub-key is half the region's ADDRESS (issue #867). A reader that
        // dropped or truncated it would not fail — it would hand every instance
        // of a batch the same region, so the batch shades from one instance's
        // charts and merely looks a bit flat.
        EXPECT_EQ(a.SubKey, b.SubKey) << "entry " << i;
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
// 2b. A version BELOW MinSupportedVersion is rejected outright (issue #867).
//
// v1 wrote 32-byte entity entries; v2 writes 48-byte ones with a SubKey. The
// two strides differ, so a v1 file read as v2 would not fail a size check by
// luck alone — the section frame's exact-size test catches it, but the version
// gate is the guard that is SUPPOSED to, and this pins that it is the one doing
// the work. A .olmap is a derived artifact: the cost of rejecting it is one
// re-bake, which is why MinSupportedVersion moves with CurrentVersion instead of
// a migration chain existing at all.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, VersionBelowMinSupportedIsRejected)
{
    static_assert(OLmapFormat::MinSupportedVersion > 1,
                  "this test patches a v1 header; if v1 became supported again it must be rewritten");

    auto source = MakeBakedLightmap();
    LightmapSerializer serializer;
    serializer.Serialize(m_Metadata, source);
    ASSERT_TRUE(fs::exists(AbsolutePath()));

    {
        std::fstream file(AbsolutePath(), std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        constexpr u32 legacyVersion = 1;
        file.seekp(static_cast<std::streamoff>(offsetof(OLmapFormat::FileHeader, Version)), std::ios::beg);
        file.write(reinterpret_cast<const char*>(&legacyVersion), sizeof(legacyVersion));
        ASSERT_FALSE(file.fail());
    }

    Ref<Asset> loadedAsset;
    EXPECT_FALSE(serializer.TryLoadData(m_Metadata, loadedAsset))
        << "A pre-#867 v1 file must be rejected, not read with the wrong entry stride";
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

// -----------------------------------------------------------------------------
// 5a. Hostile atlas parameters: a ~100-byte file whose Info section sits at
//     the format caps (16384 x 16384 x 8 pages) implies a 32 GiB texel
//     buffer. The load must fail cleanly and FAST — before the allocation
//     guard, this reached `std::vector<f32> texels(...)` and asked the
//     allocator for 32 GiB (bad_alloc at best, a machine-freezing zero-fill
//     at worst). This test completing in milliseconds is itself the proof
//     that no giant allocation happens.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, HostileAtlasDimensionsAreRejectedWithoutAllocation)
{
    std::vector<u8> payload;

    OLmapFormat::SectionFrame infoFrame;
    infoFrame.SectionId = std::to_underlying(OLmapFormat::SectionType::Info);
    infoFrame.ByteCount = sizeof(OLmapFormat::InfoSection);
    AppendRaw(payload, &infoFrame, sizeof(infoFrame));

    OLmapFormat::InfoSection info;
    info.Width = OLmapFormat::MaxDimension;
    info.Height = OLmapFormat::MaxDimension;
    info.PageCount = OLmapFormat::MaxPageCount;
    info.BakeKey = kBakeKey;
    AppendRaw(payload, &info, sizeof(info));

    // Texels frame whose ByteCount matches the (hostile) atlas parameters
    // exactly — 32 GiB claimed — with zero payload bytes actually present.
    u64 const hugeTexelBytes = static_cast<u64>(info.PageCount) * info.Width * info.Height * 4u * sizeof(f32);
    ASSERT_GT(hugeTexelBytes, u64{ 32 } * 1024u * 1024u * 1024u - 1u)
        << "caps changed — this test should keep claiming a multi-GiB buffer";
    OLmapFormat::SectionFrame texelFrame;
    texelFrame.SectionId = std::to_underlying(OLmapFormat::SectionType::Texels);
    texelFrame.ByteCount = hugeTexelBytes;
    AppendRaw(payload, &texelFrame, sizeof(texelFrame));

    auto const bytes = WrapUncompressedPayload(payload);

    Ref<LightmapAsset> loaded;
    EXPECT_FALSE(LightmapSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, "hostile-dimensions"))
        << "A texel section claiming more bytes than the file holds must be rejected";
    EXPECT_FALSE(loaded) << "Rejection must not return a partially-populated asset";
}

// -----------------------------------------------------------------------------
// 5b. Hostile section ByteCount: an EntityTable frame declaring the maximum
//     entry count (32 MB of entries) in a file that carries none of those
//     bytes. The declared ByteCount is internally consistent with its
//     EntryCount — only the comparison against the bytes ACTUALLY remaining
//     can reject it, and it must do so before the entries vector is sized.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, HostileEntityTableByteCountIsRejected)
{
    constexpr u32 kTinyDim = 2;
    std::vector<u8> payload;

    OLmapFormat::SectionFrame infoFrame;
    infoFrame.SectionId = std::to_underlying(OLmapFormat::SectionType::Info);
    infoFrame.ByteCount = sizeof(OLmapFormat::InfoSection);
    AppendRaw(payload, &infoFrame, sizeof(infoFrame));

    OLmapFormat::InfoSection info;
    info.Width = kTinyDim;
    info.Height = kTinyDim;
    info.PageCount = 1;
    info.BakeKey = kBakeKey;
    AppendRaw(payload, &info, sizeof(info));

    // Valid, fully present texels (all zeroes = finite).
    u64 const texelBytes = static_cast<u64>(info.PageCount) * info.Width * info.Height * 4u * sizeof(f32);
    OLmapFormat::SectionFrame texelFrame;
    texelFrame.SectionId = std::to_underlying(OLmapFormat::SectionType::Texels);
    texelFrame.ByteCount = texelBytes;
    AppendRaw(payload, &texelFrame, sizeof(texelFrame));
    std::vector<u8> const zeroTexels(static_cast<sizet>(texelBytes), 0);
    AppendRaw(payload, zeroTexels.data(), zeroTexels.size());

    // EntityTable frame claiming MaxEntryCount entries — none present.
    u64 const claimedEntryBytes = static_cast<u64>(OLmapFormat::MaxEntryCount) * sizeof(LightmapEntityEntry);
    OLmapFormat::SectionFrame tableFrame;
    tableFrame.SectionId = std::to_underlying(OLmapFormat::SectionType::EntityTable);
    tableFrame.ByteCount = sizeof(OLmapFormat::EntityTableHeader) + claimedEntryBytes;
    AppendRaw(payload, &tableFrame, sizeof(tableFrame));

    OLmapFormat::EntityTableHeader tableHeader;
    tableHeader.EntryCount = OLmapFormat::MaxEntryCount;
    AppendRaw(payload, &tableHeader, sizeof(tableHeader));

    auto const bytes = WrapUncompressedPayload(payload);

    Ref<LightmapAsset> loaded;
    EXPECT_FALSE(LightmapSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, "hostile-entity-table"))
        << "An entity table claiming more bytes than the file holds must be rejected";
    EXPECT_FALSE(loaded);
}

// -----------------------------------------------------------------------------
// 5c. Hostile UncompressedPayloadSize: the CRC only covers the stored
//     (compressed) payload, so a header patched to claim the full 2 GB cap
//     sails past the checksum. The size-plausibility gate must catch it
//     instead — a few-KB compressed stream cannot inflate to 2 GB (deflate's
//     hard ceiling is 1032:1) — and reject BEFORE a 2 GB destination buffer
//     is allocated.
// -----------------------------------------------------------------------------
TEST_F(LightmapAssetSerializationTest, HostileUncompressedSizeClaimIsRejected)
{
    auto source = MakeBakedLightmap();

    std::vector<u8> bytes;
    ASSERT_TRUE(LightmapSerializer::EncodeToBytes(*source, bytes, "hostile-size-claim"));
    ASSERT_GT(bytes.size(), sizeof(OLmapFormat::FileHeader));
    // Sanity: the compressed payload is small enough that a 2 GB claim is
    // physically impossible under deflate's 1032:1 ceiling.
    ASSERT_LT(bytes.size() - sizeof(OLmapFormat::FileHeader),
              static_cast<sizet>(OLmapFormat::MaxUncompressedPayloadSize / ZlibSection::MaxInflateRatio));

    // Patch FileHeader::UncompressedPayloadSize (u64 at byte offset 16) to
    // the format cap — the largest value that passes the plain cap check.
    u64 const hostileClaim = OLmapFormat::MaxUncompressedPayloadSize;
    std::memcpy(bytes.data() + offsetof(OLmapFormat::FileHeader, UncompressedPayloadSize),
                &hostileClaim, sizeof(hostileClaim));

    Ref<LightmapAsset> loaded;
    EXPECT_FALSE(LightmapSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, "hostile-size-claim"))
        << "An implausible uncompressed-size claim must be rejected";
    EXPECT_FALSE(loaded);
}
