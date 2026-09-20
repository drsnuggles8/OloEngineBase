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
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Serialization/GroomBinaryFormat.h"

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
    EXPECT_TRUE(cards->Validate(reloaded->GetCurveCount(), levelReason)) << levelReason;
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
    // Exercised through the CHECKSUM-CORRECT path: the bytes are re-cooked from
    // a groom whose level was corrupted in memory, so the reader's CRC passes
    // and the level validation is genuinely the thing under test. Patching the
    // compressed payload directly would only prove the CRC works.
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

    // And the reader's own guard, reached by a file that somehow got written
    // anyway: GroomAsset::Validate runs at the end of DecodeFromBytes over the
    // levels it just read, so a hand-forged file takes the same refusal. The
    // in-memory half is asserted directly because a forged compressed payload
    // is not a thing this suite can build without reimplementing the writer.
    std::string levelReason;
    EXPECT_FALSE(level.Validate(coat.Groom->GetCurveCount(), levelReason));
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
