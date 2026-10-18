#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCookDeterminismTest — issue #1232, the word "deterministic" in its title.
//
// Determinism is treated here as a CONTRACT, not an aspiration: the same source
// must cook to byte-identical .ologroom output. These cases attack the specific
// leaks that make a cook non-deterministic in practice, rather than only
// cooking twice in a row and calling it proved:
//
//   * cook twice from the same asset                  -> identical bytes
//   * cook two SEPARATELY IMPORTED assets             -> identical bytes
//     (catches anything keyed on an address or an allocation order)
//   * cook an already-canonical vs a shuffled input   -> identical bytes
//     (catches an unstable sort, the classic leak)
//   * import one archive written in two OBJECT orders -> the same groom
//     (catches group ids that follow archive order, #1560)
//   * uninitialised padding                           -> every byte written is
//     a byte some field owns
//
// If one of these fails, the failure message says WHICH byte differs, because
// "the buffers are not equal" on a 2 MB cook is not a debuggable message.
// =============================================================================

#include <gtest/gtest.h>

#include "Groom/GroomAlembicFixture.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Serialization/GroomBinaryFormat.h"

#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"
#endif

#include <cstring>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // Names the first differing byte. A cook is tens of kilobytes at minimum,
    // so "not equal" alone would send the next reader to a hex editor.
    void ExpectIdenticalBytes(const std::vector<u8>& lhs, const std::vector<u8>& rhs, const char* what)
    {
        SCOPED_TRACE(what);
        ASSERT_EQ(lhs.size(), rhs.size()) << "cooked sizes differ";
        for (sizet i = 0; i < lhs.size(); ++i)
        {
            if (lhs[i] != rhs[i])
            {
                FAIL() << "cooked bytes diverge at offset " << i << " (" << static_cast<int>(lhs[i]) << " vs "
                       << static_cast<int>(rhs[i]) << "); the .ologroom header is "
                       << sizeof(OloGroomFormat::FileHeader) << " bytes, so this offset is "
                       << (i < sizeof(OloGroomFormat::FileHeader) ? "in the header" : "in the compressed payload");
            }
        }
    }

    // Bit equality of two float arrays: what a determinism check means, and the
    // repo's rule against `==` on floats and glm types.
    template<typename T>
    [[nodiscard]] bool SameBits(const std::vector<T>& lhs, const std::vector<T>& rhs)
    {
        return lhs.size() == rhs.size() &&
               (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(T)) == 0);
    }

    // `shuffleGroups` interleaves the two groups; otherwise the curves are
    // emitted group by group. Both must cook to the same bytes — that is what
    // makes the cook's sort a canonicalisation rather than a reordering.
    Ref<GroomAsset> BuildTwoGroupGroom(bool shuffleGroups)
    {
        GroomBuilder builder;
        std::string reason;
        u16 groupA = 0;
        u16 groupB = 0;
        EXPECT_TRUE(builder.AddGroup("alpha", groupA, reason)) << reason;
        EXPECT_TRUE(builder.AddGroup("beta", groupB, reason)) << reason;

        constexpr u32 kCurvesPerGroup = 24;
        // The SAME logical curve set in both orders: curve `c` of group g has
        // the same points either way, so any byte difference is ordering.
        auto emit = [&](u16 group, u32 c)
        {
            std::vector<glm::vec3> points;
            std::vector<f32> widths;
            for (u32 p = 0; p < 5; ++p)
            {
                const f32 fp = static_cast<f32>(p);
                const f32 fc = static_cast<f32>(c);
                points.emplace_back(fc * 0.013f + static_cast<f32>(group), fp * 0.021f, fc * 0.007f);
                widths.push_back(0.0002f - (fp * 0.00003f));
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { static_cast<f32>(c) / 24.0f, static_cast<f32>(group) };
            input.GroupId = group;
            input.IsGuide = (c % 4) == 0;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        };

        if (shuffleGroups)
        {
            for (u32 c = 0; c < kCurvesPerGroup; ++c)
            {
                emit(groupA, c);
                emit(groupB, c);
            }
        }
        else
        {
            for (u32 c = 0; c < kCurvesPerGroup; ++c)
            {
                emit(groupA, c);
            }
            for (u32 c = 0; c < kCurvesPerGroup; ++c)
            {
                emit(groupB, c);
            }
        }

        builder.SetBasis(GroomCurveBasis::Linear);
        GroomProvenance provenance;
        provenance.SourcePath = "grooms/determinism.abc";
        provenance.SourceFormat = "AlembicCurves";
        provenance.SourceContentHash = 0x0123456789ABCDEFull;
        provenance.ImporterVersion = 1;
        builder.SetProvenance(provenance);

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }
} // namespace

TEST(GroomCookDeterminism, CookingTheSameAssetTwiceProducesIdenticalBytes)
{
    Ref<GroomAsset> groom = BuildTwoGroupGroom(/*shuffleGroups*/ true);
    ASSERT_TRUE(groom);

    std::string reason;
    std::vector<u8> first;
    std::vector<u8> second;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, first, reason)) << reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, second, reason)) << reason;

    ExpectIdenticalBytes(first, second, "same asset, cooked twice");
}

TEST(GroomCookDeterminism, CookIsIndependentOfTheSourceCurveOrder)
{
    // The leak this catches: an UNSTABLE sort in Canonicalize. With a stable
    // sort these two inputs converge on the same permutation; with std::sort
    // they do not, and the difference only shows up when the input order
    // changes — which is exactly what a DCC re-export does.
    Ref<GroomAsset> ordered = BuildTwoGroupGroom(/*shuffleGroups*/ false);
    Ref<GroomAsset> shuffled = BuildTwoGroupGroom(/*shuffleGroups*/ true);
    ASSERT_TRUE(ordered);
    ASSERT_TRUE(shuffled);

    std::string reason;
    std::vector<u8> orderedBytes;
    std::vector<u8> shuffledBytes;
    ASSERT_TRUE(GroomCooker::CookToBytes(*ordered, orderedBytes, reason)) << reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*shuffled, shuffledBytes, reason)) << reason;

    ExpectIdenticalBytes(orderedBytes, shuffledBytes, "group-ordered vs interleaved source");
}

TEST(GroomCookDeterminism, CookingDoesNotMutateTheSourceAsset)
{
    // CookToBytes works on a copy. If it canonicalised in place, a caller that
    // cooked mid-session would see its groom's curve order change underneath a
    // preview that was already drawing it.
    Ref<GroomAsset> groom = BuildTwoGroupGroom(/*shuffleGroups*/ true);
    ASSERT_TRUE(groom);

    const std::vector<u16> idsBefore = groom->GetCurveGroupIds();
    const std::vector<glm::vec3> pointsBefore = groom->GetPoints();

    std::string reason;
    std::vector<u8> bytes;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, bytes, reason)) << reason;

    EXPECT_EQ(groom->GetCurveGroupIds(), idsBefore) << "CookToBytes reordered its input";
    EXPECT_EQ(groom->GetPoints(), pointsBefore) << "CookToBytes rewrote its input's points";
}

TEST(GroomCookDeterminism, WireFormatStructsHaveNoImplicitPadding)
{
    // Implicit padding is uninitialised stack memory that reaches the file, so
    // two cooks of the same data differ in bytes no field owns. The sizes are
    // static_assert'ed in GroomBinaryFormat.h; this checks the FIELD SUM, which
    // is the property that actually rules padding out.
    EXPECT_EQ(sizeof(OloGroomFormat::FileHeader), 4u + 4u + 4u + 4u + 8u);
    EXPECT_EQ(sizeof(OloGroomFormat::SectionFrame), 2u + 2u + 4u + 8u);
    EXPECT_EQ(sizeof(OloGroomFormat::InfoSection), (4u * 4u) + (3u * 4u) + (3u * 4u) + 1u + 1u + 2u + 4u);
    EXPECT_EQ(sizeof(OloGroomFormat::ProvenanceHeader), 8u + 4u + 4u + 4u + 4u);
    // Section 9's entry (format version 4, #1533): root and tip tint, density,
    // length, width, clump, curl radius and frequency, wave amplitude and
    // frequency, stiffness scale, then the role and its three pads.
    EXPECT_EQ(sizeof(GroomCoatGroupDesc), (3u * 4u) + (3u * 4u) + (9u * 4u) + 1u + 1u + 1u + 1u);
}

TEST(GroomCookDeterminism, EveryEnumeratedSectionIsWrittenExactlyOnce)
{
    // A section added to the enum but not to the encoder would make the payload
    // size arithmetic in EncodeToBytes wrong — which reserves the wrong amount
    // and, more importantly, means the decoder's fixed section order no longer
    // matches the encoder's.
    Ref<GroomAsset> groom = BuildTwoGroupGroom(false);
    ASSERT_TRUE(groom);

    std::string reason;
    std::vector<u8> bytes;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, bytes, reason)) << reason;

    // Decoding enforces the exact section order and rejects trailing bytes, so
    // a successful decode IS the assertion that every section is present once.
    Ref<GroomAsset> decoded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), decoded, reason)) << reason;
    // 11 since #1252 added section 10, the cooked LOD levels (section 9 is
    // #1251's per-group coat table). This constant is pinned deliberately: bumping it is
    // the moment to check that the new section is written by EncodeToBytes, read
    // by DecodeFromBytes in the same position, counted in the payload-size
    // arithmetic, and that CurrentVersion and MinSupportedVersion both moved.
    EXPECT_EQ(OloGroomFormat::kSectionCount, 11);
}

#if defined(OLO_WITH_ALEMBIC)

TEST(GroomCookDeterminism, TwoSeparateImportsOfOneArchiveCookIdentically)
{
    // The strongest form of the contract available in one process: two
    // independent import runs, two independent allocations, two independent
    // hash maps in the builder — and the same bytes out. Anything keyed on a
    // pointer value or on allocation order fails here and nowhere else.
    namespace Fixture = Tests::GroomFixture;

    const std::filesystem::path abcPath = Tests::TempFile("determinism.abc");
    const Fixture::CurvesPrim scalp = Fixture::MakeHumanScalpGroom(128, 5, 6, "scalp");
    Fixture::CurvesPrim pelt = Fixture::MakeAnimalFurGroom(96, 4, 3, "pelt");
    // One cooked groom carries one basis, and the importer refuses an archive
    // that mixes them (GroomRejection.MixedBasesInOneArchiveAreRejected pins
    // that). This case is about determinism across two prims, so make the pelt
    // linear too rather than exercising the rejection by accident.
    pelt.Type = Fixture::AbcG::kLinear;
    pelt.Basis = Fixture::AbcG::kNoBasis;
    ASSERT_TRUE(Fixture::WriteArchive(abcPath, { scalp, pelt }));

    AlembicGroomImporter::Options options;
    options.ProvenancePath = "grooms/determinism.abc";

    const auto firstImport = AlembicGroomImporter::Import(abcPath, options);
    ASSERT_TRUE(firstImport.Succeeded()) << firstImport.Diagnostic;
    const auto secondImport = AlembicGroomImporter::Import(abcPath, options);
    ASSERT_TRUE(secondImport.Succeeded()) << secondImport.Diagnostic;

    std::string reason;
    std::vector<u8> firstBytes;
    std::vector<u8> secondBytes;
    ASSERT_TRUE(GroomCooker::CookToBytes(*firstImport.Groom, firstBytes, reason)) << reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*secondImport.Groom, secondBytes, reason)) << reason;

    ExpectIdenticalBytes(firstBytes, secondBytes, "two independent imports of one archive");

    // Both prims present: the archive holds a linear scalp and a cubic pelt, so
    // a build that silently dropped one would still cook deterministically.
    EXPECT_EQ(firstImport.PrimsRead, 2u);
    EXPECT_EQ(firstImport.CurvesRead, 128u + 96u);
}

TEST(GroomCookDeterminism, ProvenanceHashIsAFunctionOfTheSourceBytesOnly)
{
    namespace Fixture = Tests::GroomFixture;

    const Fixture::CurvesPrim prim = Fixture::MakeHumanScalpGroom(32, 4, 4);

    // The same content written to two different paths must hash the same: the
    // hash identifies the SOURCE, not where it happened to be on disk. A hash
    // that folded in the path would make a cooked groom differ between a
    // developer's tree and CI's.
    const std::filesystem::path pathA = Tests::TempFile("hash-a.abc");
    const std::filesystem::path pathB = Tests::TempFile("hash-b.abc");
    ASSERT_TRUE(Fixture::WriteArchive(pathA, { prim }));
    ASSERT_TRUE(Fixture::WriteArchive(pathB, { prim }));

    const auto importA = AlembicGroomImporter::Import(pathA);
    const auto importB = AlembicGroomImporter::Import(pathB);
    ASSERT_TRUE(importA.Succeeded()) << importA.Diagnostic;
    ASSERT_TRUE(importB.Succeeded()) << importB.Diagnostic;

    // Alembic stamps its own writer version into the archive, but two writes of
    // identical data by the same build produce identical files — if that ever
    // stops being true this assertion is the thing that says so, rather than a
    // confusing cooked-byte mismatch somewhere downstream.
    EXPECT_EQ(importA.Groom->GetProvenance().SourceContentHash,
              importB.Groom->GetProvenance().SourceContentHash);

    // And the importer's own version is recorded, so a cooked groom from an
    // older importer is identifiable without re-importing it.
    EXPECT_EQ(importA.Groom->GetProvenance().ImporterVersion, AlembicGroomImporter::kImporterVersion);
}

TEST(GroomCookDeterminism, ArchiveObjectOrderDoesNotReachTheCook)
{
    // #1560: Blender writes a groom's objects in pointer order, which changes on
    // every export. Group ids follow the importer's visit order and the cook
    // sorts curves by group id, so in archive order a re-export of the SAME
    // strands cooked to a different curve order -- moving every stride-based
    // LOD subset and every curve-keyed jitter. The same three objects written in
    // two orders must cook to the same groom.
    namespace Fixture = Tests::GroomFixture;

    // Three top-level objects, written in two orders NEITHER of which is name
    // order -- were one of them, an archive-order importer would agree with
    // this one on that archive and the case would rest on the other alone.
    // `scalp` sits under an xform, so its top-level object is `scalp_xf`; the
    // pelt carries sub-groups and roles, which must follow the same order.
    Fixture::CurvesPrim scalp = Fixture::MakeHumanScalpGroom(96, 5, 6, "scalp");
    scalp.Transform.setTranslation(Imath::V3d(0.0, 1.5, 0.0));
    Fixture::CurvesPrim pelt = Fixture::MakeAnimalFurGroom(72, 4, 3, "pelt");
    pelt.Type = Fixture::AbcG::kLinear; // one basis per archive
    pelt.Basis = Fixture::AbcG::kNoBasis;
    pelt.Roles.assign(pelt.SubGroups.begin(), pelt.SubGroups.end()); // constant per sub-group
    const Fixture::CurvesPrim beard = Fixture::MakeHumanScalpGroom(40, 4, 5, "beard");

    const std::filesystem::path pathA = Tests::TempFile("order-a.abc");
    const std::filesystem::path pathB = Tests::TempFile("order-b.abc");
    ASSERT_TRUE(Fixture::WriteArchive(pathA, { scalp, pelt, beard }));
    ASSERT_TRUE(Fixture::WriteArchive(pathB, { pelt, scalp, beard }));

    AlembicGroomImporter::Options options;
    options.ProvenancePath = "grooms/order.abc";
    const auto importA = AlembicGroomImporter::Import(pathA, options);
    const auto importB = AlembicGroomImporter::Import(pathB, options);
    ASSERT_TRUE(importA.Succeeded()) << importA.Diagnostic;
    ASSERT_TRUE(importB.Succeeded()) << importB.Diagnostic;
    const GroomAsset& a = *importA.Groom;
    const GroomAsset& b = *importB.Groom;

    // Ids in name order, whatever order the file holds them in.
    const std::vector<std::string> expectedNames = { "/beard", "/pelt#0", "/pelt#1", "/pelt#2", "/scalp_xf/scalp" };
    EXPECT_EQ(a.GetGroupNames(), expectedNames);
    EXPECT_EQ(b.GetGroupNames(), expectedNames);

    // The cooked bytes cannot be compared whole: the provenance hash is over
    // the SOURCE bytes, and the two archives are different files. Everything
    // the cook writes apart from that hash is compared instead, section by
    // section, and float data as bits (the CLAUDE.md rule: no float `==`).
    EXPECT_NE(a.GetProvenance().SourceContentHash, b.GetProvenance().SourceContentHash)
        << "the archives were meant to differ in byte order";
    ASSERT_EQ(a.GetCurveCount(), b.GetCurveCount());
    EXPECT_EQ(a.GetCurveOffsets(), b.GetCurveOffsets()) << "curve offsets";
    EXPECT_TRUE(SameBits(a.GetPoints(), b.GetPoints())) << "control points";
    EXPECT_TRUE(SameBits(a.GetPointWidths(), b.GetPointWidths())) << "widths";
    EXPECT_TRUE(SameBits(a.GetRootUVs(), b.GetRootUVs())) << "root UVs";
    EXPECT_EQ(a.GetCurveGroupIds(), b.GetCurveGroupIds()) << "curve group ids";
    EXPECT_EQ(a.GetCurveFlags(), b.GetCurveFlags()) << "curve flags";
    EXPECT_EQ(a.GetGuideCount(), b.GetGuideCount()) << "guide count";
    EXPECT_EQ(a.GetGroupNames(), b.GetGroupNames()) << "group names";
    EXPECT_EQ(a.GetGroupRanges(), b.GetGroupRanges()) << "group ranges";
    ASSERT_EQ(a.GetGroupCoats().size(), b.GetGroupCoats().size());
    for (sizet g = 0; g < a.GetGroupCoats().size(); ++g)
    {
        // GroomCoatGroupDesc is explicitly padded (its size is pinned above), so
        // its bytes are its fields.
        EXPECT_EQ(std::memcmp(&a.GetGroupCoats()[g], &b.GetGroupCoats()[g], sizeof(GroomCoatGroupDesc)), 0)
            << "coat of group " << a.GetGroupNames()[g];
    }
    EXPECT_EQ(a.GetLodLevels(), b.GetLodLevels()) << "cooked LOD levels"; // bitwise on its floats
    EXPECT_EQ(a.GetBasis(), b.GetBasis());
    EXPECT_EQ(std::memcmp(&a.GetBoundsMin(), &b.GetBoundsMin(), sizeof(glm::vec3)), 0) << "bounds min";
    EXPECT_EQ(std::memcmp(&a.GetBoundsMax(), &b.GetBoundsMax(), sizeof(glm::vec3)), 0) << "bounds max";
    EXPECT_EQ(a.GetProvenance().SourcePath, b.GetProvenance().SourcePath);
    EXPECT_EQ(a.GetProvenance().ImporterVersion, b.GetProvenance().ImporterVersion);
}

#endif // OLO_WITH_ALEMBIC
