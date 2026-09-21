#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomBindingRoundTripTest — issue #1249, the cooked .ologroombinding.
//
// Three things, and the third is an acceptance criterion in its own right:
//
//   ROUND TRIP    every record survives cook -> bytes -> decode UNCHANGED, and
//                 unchanged means bit-for-bit. The pipeline stores IEEE-754 bit
//                 patterns end to end and does no arithmetic on them, so an
//                 epsilon comparison here would hide exactly the bug it was
//                 written to catch.
//
//   DETERMINISM   cooking the same pair twice produces identical bytes. Pinned
//                 the way GroomCookDeterminismTest pins .ologroom's, because
//                 "deterministic rebind behavior" is criterion 1's own wording.
//
//   VERSION       a file carrying a version this build does not read is
//                 REFUSED, by version, with a reason — not mis-parsed. There
//                 are TWO versions in play and they refuse differently: the
//                 container's (the bytes moved) and the binder's (the meaning
//                 moved), and a file can be perfectly readable and still be
//                 refused at attach.
// =============================================================================

#include <gtest/gtest.h>

#include <optional>

#include "GroomBindingFixture.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Serialization/GroomBindingBinaryFormat.h"
#include "OloEngine/Serialization/ZlibSection.h"

#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomBindingTest;

namespace
{
    struct Cooked
    {
        Ref<GroomAsset> Groom;
        Ref<GroomBindingAsset> Binding;
        std::vector<u8> Bytes;
    };

    [[nodiscard]] Cooked CookACoat(const GridSurface& grid, u32 strands = 24u)
    {
        Cooked cooked;
        cooked.Groom = MakeCoat(strands, 4u);
        EXPECT_TRUE(cooked.Groom);
        GroomBindingBuildStats stats;
        std::string reason;
        const bool ok = GroomBindingCooker::CookPair(*cooked.Groom, grid.View(2u, 0xABCDu), "Grids/body.omesh",
                                                     GroomBindingBuildSettings{}, cooked.Bytes, cooked.Binding,
                                                     stats, reason);
        EXPECT_TRUE(ok) << reason;
        return cooked;
    }

    // Rewrites the header VERSION of an otherwise valid file. The checksum
    // covers the payload, not the header, so nothing needs repairing here and
    // the only thing wrong with the result is the field under test — which is
    // what makes a pass mean "the version check fired" rather than "something
    // rejected it".
    [[nodiscard]] std::vector<u8> WithHeaderVersion(const std::vector<u8>& bytes, u32 version)
    {
        std::vector<u8> copy = bytes;
        OloGroomBindingFormat::FileHeader header;
        std::memcpy(&header, copy.data(), sizeof(header));
        header.Version = version;
        std::memcpy(copy.data(), &header, sizeof(header));
        return copy;
    }

    // Rewrites the PAYLOAD of an otherwise valid file and repairs its checksum.
    //
    // Editing the decompressed bytes rather than reaching into the asset is
    // deliberate, and it is the stronger test: a hostile or corrupt file is
    // exactly a payload nobody in this process produced, so this is the real
    // route rather than a test-only back door through a private member. It also
    // means the production header needs no friend declaration for the tests.
    [[nodiscard]] std::vector<u8> WithRewrittenPayload(const std::vector<u8>& bytes,
                                                       const std::function<void(std::vector<u8>&)>& mutate)
    {
        OloGroomBindingFormat::FileHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));

        std::vector<u8> payload = ZlibSection::Decompress(
            bytes.data() + sizeof(header), bytes.size() - sizeof(header), header.UncompressedPayloadSize,
            OloGroomBindingFormat::MaxUncompressedPayloadSize, "GroomBindingRoundTripTest");
        EXPECT_FALSE(payload.empty());
        mutate(payload);

        std::vector<u8> compressed =
            ZlibSection::Compress(payload.data(), payload.size(), "GroomBindingRoundTripTest");
        EXPECT_FALSE(compressed.empty());

        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.UncompressedPayloadSize = payload.size();

        std::vector<u8> out;
        out.resize(sizeof(header) + compressed.size());
        std::memcpy(out.data(), &header, sizeof(header));
        std::memcpy(out.data() + sizeof(header), compressed.data(), compressed.size());
        return out;
    }

    // Byte offset of the Info section's payload inside the decompressed stream.
    // Section 0 is first, so it is exactly one section frame in.
    constexpr sizet kInfoPayloadOffset = sizeof(OloGroomBindingFormat::SectionFrame);
    // ...and the Roots array follows the Info section and its own frame.
    constexpr sizet kRootsPayloadOffset = kInfoPayloadOffset + sizeof(OloGroomBindingFormat::InfoSection) +
                                          sizeof(OloGroomBindingFormat::SectionFrame);
} // namespace

// ── ROUND TRIP ──────────────────────────────────────────────────────────────

TEST(GroomBindingRoundTrip, EveryRecordSurvivesTheCookBitForBit)
{
    GridSurface grid = MakeGrid(6u);
    const Cooked cooked = CookACoat(grid, 40u);
    ASSERT_TRUE(cooked.Binding);
    ASSERT_FALSE(cooked.Bytes.empty());

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(cooked.Bytes.data(), cooked.Bytes.size(), decoded, reason))
        << reason;
    ASSERT_TRUE(decoded);

    ASSERT_EQ(decoded->GetRootCount(), cooked.Binding->GetRootCount());
    EXPECT_EQ(0, std::memcmp(decoded->GetRoots().data(), cooked.Binding->GetRoots().data(),
                             decoded->GetRoots().size() * sizeof(GroomRootBinding)));

    EXPECT_EQ(decoded->GetSourceSignature(), cooked.Binding->GetSourceSignature());
    EXPECT_EQ(decoded->GetTargetSignature(), cooked.Binding->GetTargetSignature());
    EXPECT_EQ(decoded->GetBinderVersion(), cooked.Binding->GetBinderVersion());
    EXPECT_EQ(decoded->GetTargetSourcePath(), "Grids/body.omesh");

    // Derived data is recomputed on decode rather than trusted, so it must
    // agree with what the build produced.
    EXPECT_EQ(decoded->GetQualityCount(GroomRootBindQuality::Exact),
              cooked.Binding->GetQualityCount(GroomRootBindQuality::Exact));
    EXPECT_EQ(std::bit_cast<u32>(decoded->GetMaxRestDistance()),
              std::bit_cast<u32>(cooked.Binding->GetMaxRestDistance()));
}

TEST(GroomBindingRoundTrip, ADecodedBindingStillAttachesToTheSamePair)
{
    // The round trip that actually matters: not "the fields came back" but "the
    // thing that came back is still usable against the assets it was built
    // for". A signature that survived the file but not the recomputation would
    // pass the test above and fail here.
    GridSurface grid = MakeGrid(6u);
    const Cooked cooked = CookACoat(grid);
    ASSERT_TRUE(cooked.Binding);

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(cooked.Bytes.data(), cooked.Bytes.size(), decoded, reason))
        << reason;

    EXPECT_EQ(decoded->CheckCompatibility(GroomBindingBuilder::SignGroom(*cooked.Groom),
                                          GroomBindingBuilder::SignTarget(grid.View(2u, 0xABCDu))),
              GroomBindingRejectReason::None);
}

TEST(GroomBindingRoundTrip, ACookedBindingDeformsIdenticallyToTheOneItWasCookedFrom)
{
    // The `{loose, cooked}` asset axis, as a test rather than as a claim: a
    // binding read back off disk must produce the SAME deformed positions as
    // the in-memory one it was cooked from, bit for bit.
    GridSurface grid = MakeGrid(6u);
    WeightAsHinge(grid);
    const Cooked cooked = CookACoat(grid, 16u);
    ASSERT_TRUE(cooked.Binding);

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(cooked.Bytes.data(), cooked.Bytes.size(), decoded, reason))
        << reason;

    const std::vector<glm::mat4> palette{ glm::mat4(1.0f),
                                          glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 0.0f, 1.0f)) };
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u, 0xABCDu);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;

    TArray<GroomRootTransform> fromMemory;
    TArray<GroomRootTransform> fromDisk;
    (void)EvaluateGroomRootTransforms(*cooked.Groom, *cooked.Binding, inputs, std::nullopt, fromMemory);
    (void)EvaluateGroomRootTransforms(*cooked.Groom, *decoded, inputs, std::nullopt, fromDisk);

    ASSERT_EQ(fromMemory.Num(), fromDisk.Num());
    for (sizet i = 0; i < fromMemory.Num(); ++i)
    {
        EXPECT_EQ(fromMemory[i], fromDisk[i]) << "curve " << i;
    }
}

// ── DETERMINISM ─────────────────────────────────────────────────────────────

TEST(GroomBindingRoundTrip, CookingTheSamePairTwiceProducesIdenticalBytes)
{
    GridSurface grid = MakeGrid(6u);
    const Cooked first = CookACoat(grid, 32u);
    const Cooked second = CookACoat(grid, 32u);
    ASSERT_FALSE(first.Bytes.empty());
    ASSERT_EQ(first.Bytes.size(), second.Bytes.size());
    EXPECT_EQ(0, std::memcmp(first.Bytes.data(), second.Bytes.data(), first.Bytes.size()))
        << "the cook is not deterministic, which is acceptance criterion 1";
}

// ── VERSION ─────────────────────────────────────────────────────────────────

TEST(GroomBindingRoundTrip, AFileFromANewerBuildIsRefusedByVersion)
{
    // The prior/newer on-disk version cell. The file is otherwise perfectly
    // well formed and its CRC is intact — the ONLY thing wrong with it is the
    // version, so a pass here is about the version check and nothing else.
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_FALSE(cooked.Bytes.empty());

    const auto newer = WithHeaderVersion(cooked.Bytes, OloGroomBindingFormat::CurrentVersion + 1u);
    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(newer.data(), newer.size(), decoded, reason));
    EXPECT_FALSE(decoded) << "a refused file must leave the caller's handle untouched";
    EXPECT_NE(reason.find("version"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, AFileBelowTheSupportedFloorIsRefusedByVersion)
{
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_FALSE(cooked.Bytes.empty());
    ASSERT_GT(OloGroomBindingFormat::MinSupportedVersion, 0u);

    const auto older = WithHeaderVersion(cooked.Bytes, OloGroomBindingFormat::MinSupportedVersion - 1u);
    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(older.data(), older.size(), decoded, reason));
    EXPECT_FALSE(decoded);
    EXPECT_NE(reason.find("version"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, ABindingCarryingAnOlderBinderVersionIsRefusedAtAttachNotAtLoad)
{
    // The OTHER version, and the reason the two are separate. A binding written
    // by an older BINDER is a perfectly readable file: the bytes are the
    // current layout, the CRC is right, every record decodes. What is stale is
    // the MEANING of those records, and that is caught where it matters — at
    // attach — with a reason that says "rebuild it" rather than "this file is
    // corrupt". A reader that refused it at LOAD would be wrong in a way that
    // matters: the file is not damaged and its records are worth showing.
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_TRUE(cooked.Binding);
    ASSERT_GT(kGroomBinderVersion, 0u);

    const auto stale = WithRewrittenPayload(cooked.Bytes,
                                            [](std::vector<u8>& payload)
                                            {
                                                OloGroomBindingFormat::InfoSection info;
                                                std::memcpy(&info, payload.data() + kInfoPayloadOffset, sizeof(info));
                                                info.BinderVersion = kGroomBinderVersion - 1u;
                                                std::memcpy(payload.data() + kInfoPayloadOffset, &info, sizeof(info));
                                            });

    Ref<GroomBindingAsset> staleAsset;
    std::string reason;
    ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(stale.data(), stale.size(), staleAsset, reason))
        << "a stale BINDER version is not a damaged file and must still load: " << reason;
    EXPECT_EQ(staleAsset->GetBinderVersion(), kGroomBinderVersion - 1u)
        << "the file's binder version must survive the round trip, or this case proves nothing";

    EXPECT_EQ(staleAsset->CheckCompatibility(GroomBindingBuilder::SignGroom(*cooked.Groom),
                                             GroomBindingBuilder::SignTarget(grid.View(2u, 0xABCDu))),
              GroomBindingRejectReason::BinderVersionMismatch);

    // ...while the file cooked by THIS binder attaches, so the assertion above
    // is about the version and not about the pair.
    Ref<GroomBindingAsset> current;
    ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(cooked.Bytes.data(), cooked.Bytes.size(), current, reason))
        << reason;
    EXPECT_EQ(current->CheckCompatibility(GroomBindingBuilder::SignGroom(*cooked.Groom),
                                          GroomBindingBuilder::SignTarget(grid.View(2u, 0xABCDu))),
              GroomBindingRejectReason::None);
}

// ── Corruption ──────────────────────────────────────────────────────────────

TEST(GroomBindingRoundTrip, ACorruptPayloadIsRefusedByChecksum)
{
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_GT(cooked.Bytes.size(), sizeof(OloGroomBindingFormat::FileHeader) + 4u);

    std::vector<u8> corrupt = cooked.Bytes;
    corrupt[sizeof(OloGroomBindingFormat::FileHeader) + 2u] ^= 0xFFu;

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(corrupt.data(), corrupt.size(), decoded, reason));
    EXPECT_NE(reason.find("checksum"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, AFileWithTheWrongMagicIsRefusedBeforeAnythingElse)
{
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);

    std::vector<u8> foreign = cooked.Bytes;
    const u32 wrongMagic = 0x4D524750u; // .ologroom's magic, which is the likely mix-up
    std::memcpy(foreign.data(), &wrongMagic, sizeof(wrongMagic));

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(foreign.data(), foreign.size(), decoded, reason));
    EXPECT_NE(reason.find("magic"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, ATruncatedFileIsRefusedRatherThanPartiallyRead)
{
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_GT(cooked.Bytes.size(), sizeof(OloGroomBindingFormat::FileHeader) + 8u);

    std::vector<u8> truncated(cooked.Bytes.begin(), cooked.Bytes.end() - 8);
    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(truncated.data(), truncated.size(), decoded, reason));
    EXPECT_FALSE(decoded);
    EXPECT_FALSE(reason.empty());
}

TEST(GroomBindingRoundTrip, ARecordWithImpossibleBarycentricCoordinatesIsRefusedByName)
{
    // A file nobody in this process could have written: the writer and the
    // reader share one Validate, so a barycentric that does not sum to one can
    // only come from outside. The reader must say WHICH invariant broke rather
    // than failing somewhere downstream in the deformation, where the symptom
    // would be a strand at infinity and the cause would be three files away.
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_TRUE(cooked.Binding);

    const auto corrupt = WithRewrittenPayload(cooked.Bytes,
                                              [](std::vector<u8>& payload)
                                              {
                                                  GroomRootBinding record;
                                                  std::memcpy(&record, payload.data() + kRootsPayloadOffset,
                                                              sizeof(record));
                                                  record.Barycentric = { 5.0f, 5.0f, 5.0f };
                                                  std::memcpy(payload.data() + kRootsPayloadOffset, &record,
                                                              sizeof(record));
                                              });

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(corrupt.data(), corrupt.size(), decoded, reason));
    EXPECT_FALSE(decoded);
    EXPECT_NE(reason.find("barycentric"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, ABarycentricTripleThatSumsToOneIsStillRefusedIfAComponentIsNot)
{
    // The SUM was bounded and the COMPONENTS were not, so (1e6, -1e6, 1) passed
    // the check above it and every other check in Validate. That record is not a
    // rounding error: the deformed root is `b.x*v0 + b.y*v1 + b.z*v2`, so it
    // puts one strand a million surface extents from the body -- an
    // infinite-looking spike on screen and a bounding box that swallows the
    // scene. The bound was on the wrong side of the multiplication.
    //
    // Nothing in this process can write such a file, because the builder emits a
    // convex combination; like every case around it, this is a hostile or
    // corrupt file, and the reader has to name the invariant rather than fail
    // three files later inside the deformation.
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_TRUE(cooked.Binding);

    const auto corrupt = WithRewrittenPayload(cooked.Bytes,
                                              [](std::vector<u8>& payload)
                                              {
                                                  GroomRootBinding record;
                                                  std::memcpy(&record, payload.data() + kRootsPayloadOffset,
                                                              sizeof(record));
                                                  record.Barycentric = { 1.0e6f, -1.0e6f, 1.0f };
                                                  std::memcpy(payload.data() + kRootsPayloadOffset, &record,
                                                              sizeof(record));
                                              });

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(corrupt.data(), corrupt.size(), decoded, reason));
    EXPECT_FALSE(decoded);
    EXPECT_NE(reason.find("barycentric component"), std::string::npos) << reason;
}

TEST(GroomBindingRoundTrip, ARecordAddressingATriangleTheTargetDoesNotHaveIsRefusedByName)
{
    GridSurface grid = MakeGrid(4u);
    const Cooked cooked = CookACoat(grid, 8u);
    ASSERT_TRUE(cooked.Binding);

    const auto corrupt = WithRewrittenPayload(cooked.Bytes,
                                              [](std::vector<u8>& payload)
                                              {
                                                  GroomRootBinding record;
                                                  std::memcpy(&record, payload.data() + kRootsPayloadOffset,
                                                              sizeof(record));
                                                  record.TriangleIndex = 1'000'000u;
                                                  std::memcpy(payload.data() + kRootsPayloadOffset, &record,
                                                              sizeof(record));
                                              });

    Ref<GroomBindingAsset> decoded;
    std::string reason;
    EXPECT_FALSE(GroomBindingSerializer::DecodeFromBytes(corrupt.data(), corrupt.size(), decoded, reason));
    EXPECT_NE(reason.find("triangle"), std::string::npos) << reason;
}
