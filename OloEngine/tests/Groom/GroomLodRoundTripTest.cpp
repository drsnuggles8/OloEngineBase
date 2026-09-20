#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomLodRoundTripTest — issue #1252, the cooked format's half of criterion 1.
//
// "LOD assets and selection thresholds are authored/cooked" is a claim about
// BYTES, and the things that can go wrong with bytes are not the things that
// can go wrong with the ladder:
//
//   * a level that does not survive the cook — which would look exactly like a
//     feature that does nothing, because the runtime's answer for a groom with
//     no level is to stay on strands and say LevelNotCooked;
//   * a cook that is not deterministic, which breaks the contract the whole
//     .ologroom format exists to hold (GroomCooker.h);
//   * a FILE FROM BEFORE THIS CHANGE, which the version policy says must be
//     rejected by version rather than mis-parsed — and the readable refusal is
//     the whole cost of that policy, so it is worth a case;
//   * a corrupt or hostile level header, which is the one place in this
//     feature where a file-supplied count sizes an allocation and a
//     file-supplied index reaches an array the renderer dereferences every
//     frame.
//
// The handover's verification grid calls the third of these "prior on-disk
// version" and flags it as the cell most likely to be quietly skipped. It is
// here, and it is an assertion rather than a note.
// =============================================================================

#include <gtest/gtest.h>

#include "GroomLodFixture.h"
#include "GroomStrandFixture.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Serialization/GroomBinaryFormat.h"
#include "OloEngine/Serialization/ZlibSection.h"

#include <cstring>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // A pelt with a card level already attached, and the level it was given.
    struct CookedCoat
    {
        Ref<GroomAsset> Groom;
        GroomLodLevel Level;
    };

    [[nodiscard]] CookedCoat MakeCoatWithCards()
    {
        CookedCoat coat;
        auto source = Tests::GroomStrandFixture::MakePelt(2000u, 4u);
        EXPECT_TRUE(source.Groom) << source.FailureReason;
        if (!source.Groom)
        {
            return coat;
        }
        // Root UVs folded into the unit chart: #1246's generator records an
        // unwrapped `phi / 2pi`, which is exactly the groom GroomLodBuilder
        // refuses (see GroomLodContractTest's case for that refusal).
        std::string reason;
        Ref<GroomAsset> pelt = Tests::GroomLodFixture::RebuildWithWrappedRootUVs(*source.Groom, reason);
        EXPECT_TRUE(pelt) << reason;
        if (!pelt)
        {
            return coat;
        }

        GroomCardSettings settings;
        if (!GroomLodBuilder::BuildCardLevel(*pelt, settings, coat.Level, reason, nullptr))
        {
            ADD_FAILURE() << "card cook failed: " << reason;
            return coat;
        }
        if (!GroomLodBuilder::AttachLodLevels(*pelt, { coat.Level }, reason))
        {
            ADD_FAILURE() << "attach failed: " << reason;
            return coat;
        }
        coat.Groom = pelt;
        return coat;
    }
} // namespace

TEST(GroomLodRoundTrip, ACookedCardLevelComesBackByteIdentical)
{
    const CookedCoat coat = MakeCoatWithCards();
    ASSERT_TRUE(coat.Groom);

    std::vector<u8> bytes;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*coat.Groom, bytes, reason)) << reason;

    Ref<GroomAsset> reloaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), reloaded, reason)) << reason;
    ASSERT_TRUE(reloaded);

    ASSERT_EQ(reloaded->GetLodLevels().size(), 1u) << "the cook dropped the level";
    const GroomLodLevel* cards = reloaded->FindLodLevel(GroomRepresentation::Card);
    ASSERT_NE(cards, nullptr);

    // WHOLE-LEVEL equality, not a field sample. GroomLodLevel::operator==
    // compares the float arrays as BYTES, so a stage that quietly rescaled or
    // rounded one shows here — which is the same reason GroomRoundTripTest
    // compares positions and widths exactly.
    EXPECT_TRUE(*cards == coat.Level) << "the level did not survive the cook unchanged";

    // And the source map still points into the base groom, which is the one
    // invariant a reorder could break silently.
    std::string levelReason;
    EXPECT_TRUE(cards->Validate(reloaded->GetCurveCount(), reloaded->GetGroupCount(), levelReason)) << levelReason;
}

TEST(GroomLodRoundTrip, CookingTwiceProducesTheSameBytes)
{
    // The format's contract, extended to the new section. A sorted vector
    // rather than an iterated hash map in GroomLodBuilder is what makes it
    // true; this is the case that would catch a regression to the latter, from
    // the outside, on the actual file.
    const CookedCoat coat = MakeCoatWithCards();
    ASSERT_TRUE(coat.Groom);

    std::vector<u8> first;
    std::vector<u8> second;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*coat.Groom, first, reason)) << reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*coat.Groom, second, reason)) << reason;
    EXPECT_EQ(first, second);
}

TEST(GroomLodRoundTrip, AFileFromBeforeThisChangeIsRefusedByVersionAndSaysSo)
{
    // .ologroom is a DERIVED artifact, so its minimum supported version moves
    // with its current one (binary-format-versioning.md). The cost of that
    // policy is that every cooked groom on disk must be re-imported — and the
    // benefit is that the failure is a readable version error rather than a
    // reader confidently mis-parsing section 9 as section 10.
    //
    // Forged from a REAL current file by rewriting the version word, so the
    // case tests the version gate rather than a hand-built header that would
    // have failed for some other reason first.
    const CookedCoat coat = MakeCoatWithCards();
    ASSERT_TRUE(coat.Groom);

    std::vector<u8> bytes;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*coat.Groom, bytes, reason)) << reason;
    ASSERT_GE(bytes.size(), sizeof(OloGroomFormat::FileHeader));

    OloGroomFormat::FileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    ASSERT_EQ(header.Version, OloGroomFormat::CurrentVersion);
    header.Version = OloGroomFormat::CurrentVersion - 1u;
    std::memcpy(bytes.data(), &header, sizeof(header));

    Ref<GroomAsset> reloaded;
    reason.clear();
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), reloaded, reason));
    EXPECT_EQ(reloaded, nullptr);
    EXPECT_NE(reason.find("version"), std::string::npos)
        << "the refusal did not name the version, so an artist would have no idea to re-import: " << reason;
}

TEST(GroomLodRoundTrip, AHostileLevelHeaderIsRefusedBeforeItSizesAnything)
{
    // The level's counts are FILE-SUPPLIED and size allocations; its source map
    // is FILE-SUPPLIED and indexes an array the renderer dereferences in its
    // innermost loop. Both are bounded at the reader, and a file that trips
    // either must fail rather than allocate or index.
    //
    // THIS case covers the WRITER's half only: AttachLodLevels refuses the
    // corruption before it can be cooked, and the level's own Validate refuses
    // it directly. Neither is the decoder.
    // AChecksumValidFileWithACorruptSourceMapIsRejectedByTheDECODER below is
    // the other half, and it exists because this one cannot be: nothing here
    // ever hands GroomSerializer::DecodeFromBytes an invalid level.
    CookedCoat coat = MakeCoatWithCards();
    ASSERT_TRUE(coat.Groom);
    GroomLodLevel level = coat.Level;
    std::string reason;

    // Out of range by one. The renderer would read one past the end of the
    // binding's root-transform array for this card, every frame, forever.
    level.SourceCurves.back() = coat.Groom->GetCurveCount();

    // AttachLodLevels validates, so the corruption cannot even reach the cook —
    // which is the first of the two guards, and worth asserting as such. The
    // groom is left holding the level it HAD, which is the good one.
    EXPECT_FALSE(GroomLodBuilder::AttachLodLevels(*coat.Groom, { level }, reason));
    EXPECT_NE(reason.find("maps to base curve"), std::string::npos) << reason;
    ASSERT_EQ(coat.Groom->GetLodLevels().size(), 1u) << "a rejected attach dropped the level the groom had";
    EXPECT_TRUE(coat.Groom->GetLodLevels()[0] == coat.Level);

    // The level's own guard, asserted directly. The decoder's is the next case
    // down, on a genuinely forged and re-checksummed file.
    std::string levelReason;
    EXPECT_FALSE(level.Validate(coat.Groom->GetCurveCount(), coat.Groom->GetGroupCount(), levelReason));
}

TEST(GroomLodRoundTrip, AChecksumValidFileWithACorruptSourceMapIsRejectedByTheDECODER)
{
    // THE CASE THE ONE ABOVE DOES NOT COVER, and the distinction matters: that
    // one proves the WRITER's guard (AttachLodLevels refuses before cooking)
    // and the level's own Validate. Neither is the code a shipped game runs. A
    // file that arrives corrupt — a bad disk, a truncated download, a patcher
    // that wrote half a file — reaches GroomSerializer::DecodeFromBytes with a
    // CRC that matches, because whatever produced it produced a consistent
    // file. That path had no coverage at all.
    //
    // So this builds one: decompress the real payload, corrupt it, recompress,
    // recompute the CRC, and hand the decoder a file it cannot tell from a
    // legitimate one by checksum.
    const CookedCoat coat = MakeCoatWithCards();
    ASSERT_TRUE(coat.Groom);
    ASSERT_FALSE(coat.Level.SourceCurves.empty());

    std::vector<u8> bytes;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*coat.Groom, bytes, reason)) << reason;
    ASSERT_GT(bytes.size(), sizeof(OloGroomFormat::FileHeader));

    OloGroomFormat::FileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    ASSERT_EQ(header.Flags & OloGroomFormat::FlagCompressed, OloGroomFormat::FlagCompressed);

    std::vector<u8> payload = ZlibSection::Decompress(
        bytes.data() + sizeof(header), bytes.size() - sizeof(header), header.UncompressedPayloadSize,
        OloGroomFormat::MaxUncompressedPayloadSize, "GroomLodRoundTripTest");
    ASSERT_EQ(payload.size(), header.UncompressedPayloadSize);
    ASSERT_GE(payload.size(), sizeof(u32));

    // THE LAST FOUR BYTES OF THE PAYLOAD ARE THE LAST CARD'S SOURCE MAP ENTRY.
    // Section 10 is the last section, a level's SourceCurves is its last array,
    // and there is one level — so the tail of the payload is that u32.
    //
    // ASSERTED, NOT ASSUMED. If the layout ever changes this reads back
    // something else and the case fails HERE, naming the reason, rather than
    // silently patching an unrelated field and then "passing" because the
    // decoder rejected the file for a completely different reason. That is the
    // difference between testing the prediction and testing a coincidence.
    u32 tail = 0;
    std::memcpy(&tail, payload.data() + payload.size() - sizeof(u32), sizeof(u32));
    ASSERT_EQ(tail, coat.Level.SourceCurves.back())
        << "the payload no longer ends with the level's source map; this case is patching the wrong bytes";

    // One past the end of the base groom — the index that would read past the
    // binding's root-transform array in the renderer's innermost loop.
    const u32 corrupt = coat.Groom->GetCurveCount();
    std::memcpy(payload.data() + payload.size() - sizeof(u32), &corrupt, sizeof(corrupt));

    std::vector<u8> recompressed = ZlibSection::Compress(payload.data(), payload.size(), "GroomLodRoundTripTest");
    ASSERT_FALSE(recompressed.empty());

    std::vector<u8> forged;
    OloGroomFormat::FileHeader forgedHeader = header;
    forgedHeader.Checksum = Hash::CRC32(recompressed.data(), recompressed.size());
    forgedHeader.UncompressedPayloadSize = payload.size();
    forged.resize(sizeof(forgedHeader) + recompressed.size());
    std::memcpy(forged.data(), &forgedHeader, sizeof(forgedHeader));
    std::memcpy(forged.data() + sizeof(forgedHeader), recompressed.data(), recompressed.size());

    Ref<GroomAsset> reloaded;
    reason.clear();
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(forged.data(), forged.size(), reloaded, reason))
        << "the decoder accepted a level whose source map points past the base groom";
    EXPECT_EQ(reloaded, nullptr);
    EXPECT_NE(reason.find("maps to base curve"), std::string::npos)
        << "the refusal did not name the source map: " << reason;

    // The control: the SAME forging path with the ORIGINAL value put back must
    // produce a file that loads. Without it, this case would pass for a
    // decoder that rejected every re-compressed file — which is a different
    // bug wearing this one's clothes.
    std::memcpy(payload.data() + payload.size() - sizeof(u32), &tail, sizeof(tail));
    std::vector<u8> clean = ZlibSection::Compress(payload.data(), payload.size(), "GroomLodRoundTripTest");
    ASSERT_FALSE(clean.empty());
    OloGroomFormat::FileHeader cleanHeader = header;
    cleanHeader.Checksum = Hash::CRC32(clean.data(), clean.size());
    cleanHeader.UncompressedPayloadSize = payload.size();
    std::vector<u8> rebuilt(sizeof(cleanHeader) + clean.size());
    std::memcpy(rebuilt.data(), &cleanHeader, sizeof(cleanHeader));
    std::memcpy(rebuilt.data() + sizeof(cleanHeader), clean.data(), clean.size());

    Ref<GroomAsset> control;
    reason.clear();
    EXPECT_TRUE(GroomSerializer::DecodeFromBytes(rebuilt.data(), rebuilt.size(), control, reason)) << reason;
    EXPECT_TRUE(control);
}

TEST(GroomLodRoundTrip, AGroomWithNoLevelsStillRoundTrips)
{
    // The common case, and the one that would break if section 10 were written
    // only when there was something to put in it: the reader would then have to
    // decide whether a missing section meant "old file" or "no levels", and the
    // version already answers that.
    auto pelt = Tests::GroomStrandFixture::MakePelt(200u, 4u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    ASSERT_TRUE(pelt.Groom->GetLodLevels().empty());

    std::vector<u8> bytes;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*pelt.Groom, bytes, reason)) << reason;

    Ref<GroomAsset> reloaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), reloaded, reason)) << reason;
    ASSERT_TRUE(reloaded);
    EXPECT_TRUE(reloaded->GetLodLevels().empty());
    EXPECT_EQ(reloaded->FindLodLevel(GroomRepresentation::Card), nullptr);
}
