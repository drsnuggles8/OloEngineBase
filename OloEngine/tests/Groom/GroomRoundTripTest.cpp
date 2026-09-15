#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomRoundTripTest — issue #1232, acceptance criterion 2.
//
// "Root UVs, widths, curve offsets, groups, guide designation and bounds
//  survive import / reimport / cook / save / load."
//
// That sentence is the test. Each case walks one more step of the chain and
// asserts the SAME six things at the end of it:
//
//   import            .abc -> GroomAsset
//   reimport          import the same .abc twice, compare the two assets
//   cook              GroomAsset -> .ologroom bytes
//   save + load       bytes -> file -> bytes -> GroomAsset
//
// Widths and positions are compared EXACTLY (==) on purpose. The pipeline
// stores IEEE-754 bit patterns end to end and performs no arithmetic on them
// once imported, so an epsilon comparison here would hide precisely the bug
// this criterion exists to catch — a stage that quietly rescales or rounds.
// The one exception is the import step itself, which applies the source
// transform; those cases use an identity transform so exactness still holds.
// =============================================================================

#include <gtest/gtest.h>

#include "Groom/GroomAlembicFixture.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"

#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"
#endif

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // The six properties AC 2 names, checked as one unit so no case can pass by
    // asserting a subset. Reported through gtest's non-fatal assertions so a
    // failure names every property that drifted, not just the first.
    void ExpectSameGroom(const GroomAsset& expected, const GroomAsset& actual, const char* stage)
    {
        SCOPED_TRACE(stage);

        ASSERT_EQ(expected.GetCurveCount(), actual.GetCurveCount()) << "curve count";
        ASSERT_EQ(expected.GetPointCount(), actual.GetPointCount()) << "point count";

        // Curve offsets — the strand topology itself.
        EXPECT_EQ(expected.GetCurveOffsets(), actual.GetCurveOffsets()) << "curve offsets";

        // Positions and widths, exactly.
        EXPECT_EQ(expected.GetPoints(), actual.GetPoints()) << "control points";
        EXPECT_EQ(expected.GetPointWidths(), actual.GetPointWidths()) << "widths";

        // Root UVs.
        EXPECT_EQ(expected.GetRootUVs(), actual.GetRootUVs()) << "root UVs";

        // Groups: ids, names and derived ranges.
        EXPECT_EQ(expected.GetCurveGroupIds(), actual.GetCurveGroupIds()) << "curve group ids";
        EXPECT_EQ(expected.GetGroupNames(), actual.GetGroupNames()) << "group names";
        EXPECT_EQ(expected.GetGroupRanges(), actual.GetGroupRanges()) << "group ranges";

        // Guide designation.
        EXPECT_EQ(expected.GetCurveFlags(), actual.GetCurveFlags()) << "curve flags";
        EXPECT_EQ(expected.GetGuideCount(), actual.GetGuideCount()) << "guide count";

        // Bounds.
        EXPECT_EQ(expected.GetBoundsMin(), actual.GetBoundsMin()) << "bounds min";
        EXPECT_EQ(expected.GetBoundsMax(), actual.GetBoundsMax()) << "bounds max";

        // Basis and provenance ride along; a groom that lost its provenance
        // still fails AC 4, so it is checked here rather than only there.
        EXPECT_EQ(expected.GetBasis(), actual.GetBasis()) << "curve basis";
        EXPECT_EQ(expected.GetProvenance(), actual.GetProvenance()) << "provenance";
    }

    // Builds a groom directly, with no Alembic involved — so the cook/save/load
    // half of the chain is testable in a build with OLO_WITH_ALEMBIC off.
    Ref<GroomAsset> BuildSyntheticGroom()
    {
        GroomBuilder builder;
        std::string reason;

        u16 groupA = 0;
        u16 groupB = 0;
        EXPECT_TRUE(builder.AddGroup("scalp/front", groupA, reason)) << reason;
        EXPECT_TRUE(builder.AddGroup("scalp/crown", groupB, reason)) << reason;

        for (u32 c = 0; c < 32; ++c)
        {
            const u32 points = 4u + (c % 3u);
            std::vector<glm::vec3> positions;
            std::vector<f32> widths;
            for (u32 p = 0; p < points; ++p)
            {
                const f32 fp = static_cast<f32>(p);
                const f32 fc = static_cast<f32>(c);
                positions.emplace_back(fc * 0.01f, fp * 0.02f, (fc * 0.001f) - (fp * 0.003f));
                widths.push_back(0.0001f * (1.0f - (fp / static_cast<f32>(points))));
            }

            GroomCurveInput input;
            input.Points = positions;
            input.Widths = widths;
            input.RootUV = { static_cast<f32>(c) / 32.0f, 0.25f };
            // Interleaved so the cook's grouping sort actually permutes.
            input.GroupId = (c % 2 == 0) ? groupA : groupB;
            input.IsGuide = (c % 5) == 0;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }

        builder.SetBasis(GroomCurveBasis::BSpline);
        builder.SetName("synthetic");

        GroomProvenance provenance;
        provenance.SourcePath = "grooms/synthetic.abc";
        provenance.SourceFormat = "AlembicCurves";
        provenance.SourceContentHash = 0xDEADBEEFCAFEF00Dull;
        provenance.ImporterVersion = 1;
        builder.SetProvenance(provenance);

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }
} // namespace

// ── Cook / save / load, with no Alembic dependency ──────────────────────────

TEST(GroomRoundTrip, CookSaveLoadPreservesEverySurvivingProperty)
{
    Ref<GroomAsset> source = BuildSyntheticGroom();
    ASSERT_TRUE(source);

    std::string reason;
    ASSERT_TRUE(GroomCooker::Canonicalize(*source, reason)) << reason;

    std::vector<u8> cooked;
    ASSERT_TRUE(GroomCooker::CookToBytes(*source, cooked, reason)) << reason;
    ASSERT_FALSE(cooked.empty());

    // Save to a real file and read it back, so the test covers the file layer
    // and not only the in-memory buffer.
    const std::filesystem::path path = Tests::TempFile("roundtrip.ologroom");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
    }

    std::vector<u8> readBack;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.is_open());
        readBack.resize(static_cast<sizet>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(readBack.data()), static_cast<std::streamsize>(readBack.size()));
    }
    EXPECT_EQ(cooked, readBack) << "the bytes on disk differ from the bytes cooked";

    Ref<GroomAsset> loaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(readBack.data(), readBack.size(), loaded, reason)) << reason;
    ASSERT_TRUE(loaded);

    ExpectSameGroom(*source, *loaded, "cook -> save -> load");
}

TEST(GroomRoundTrip, CookSortsCurvesIntoContiguousGroupRanges)
{
    Ref<GroomAsset> groom = BuildSyntheticGroom();
    ASSERT_TRUE(groom);

    // Before: the builder kept source order, which interleaves the two groups.
    const auto& idsBefore = groom->GetCurveGroupIds();
    EXPECT_FALSE(std::is_sorted(idsBefore.begin(), idsBefore.end()))
        << "the fixture must interleave groups or this test proves nothing";

    std::string reason;
    ASSERT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;

    const auto& idsAfter = groom->GetCurveGroupIds();
    EXPECT_TRUE(std::is_sorted(idsAfter.begin(), idsAfter.end())) << "group ids are not contiguous after the cook";

    // Every declared group must be a non-empty range, and the ranges must tile
    // the curve array exactly once.
    u32 covered = 0;
    for (u32 group = 0; group < groom->GetGroupCount(); ++group)
    {
        const GroomGroupRange& range = groom->GetGroupRanges()[group];
        EXPECT_GT(range.CurveCount, 0u) << "group " << group << " is empty";
        EXPECT_EQ(range.FirstCurve, covered) << "group " << group << " does not start where the previous one ended";
        for (u32 i = 0; i < range.CurveCount; ++i)
        {
            EXPECT_EQ(groom->GetCurveGroupIds()[range.FirstCurve + i], static_cast<u16>(group));
        }
        covered += range.CurveCount;
    }
    EXPECT_EQ(covered, groom->GetCurveCount()) << "the group ranges do not cover every curve";
}

TEST(GroomRoundTrip, BoundsIncludeStrandRadiusNotJustTheCentreline)
{
    GroomBuilder builder;
    std::string reason;
    u16 group = 0;
    ASSERT_TRUE(builder.AddGroup("g", group, reason)) << reason;

    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const std::vector<f32> widths = { 0.5f, 0.25f }; // diameters -> max radius 0.25

    GroomCurveInput input;
    input.Points = points;
    input.Widths = widths;
    input.GroupId = group;
    ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;

    Ref<GroomAsset> groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;

    // A groom culled on its centreline pops at the screen edge; the widening is
    // the whole reason the bounds are recomputed rather than copied from P.
    EXPECT_EQ(groom->GetBoundsMin(), glm::vec3(-0.25f, -0.25f, -0.25f));
    EXPECT_EQ(groom->GetBoundsMax(), glm::vec3(0.25f, 1.25f, 0.25f));
}

TEST(GroomRoundTrip, EmptyGroomGetsAZeroBoxNotAnInvertedOne)
{
    // An inverted-infinity box is the value that silently makes an empty asset
    // "visible from everywhere" in a culling test.
    GroomAsset groom;
    groom.RecomputeDerivedData();
    EXPECT_EQ(groom.GetBoundsMin(), glm::vec3(0.0f));
    EXPECT_EQ(groom.GetBoundsMax(), glm::vec3(0.0f));

    std::string reason;
    EXPECT_FALSE(groom.Validate(reason)) << "an empty groom must not validate";
    EXPECT_FALSE(reason.empty());
}

// ── Import and reimport, through a real Alembic ICurves archive ─────────────

#if defined(OLO_WITH_ALEMBIC)

TEST(GroomRoundTrip, ImportReimportCookSaveLoadPreservesEverything)
{
    namespace Fixture = Tests::GroomFixture;

    const std::filesystem::path abcPath = Tests::TempFile("scalp.abc");
    const Fixture::CurvesPrim prim = Fixture::MakeHumanScalpGroom(/*strandCount*/ 256, /*pointsPerStrand*/ 6,
                                                                  /*guideStride*/ 8);
    ASSERT_TRUE(Fixture::WriteArchive(abcPath, { prim })) << "failed to author the Alembic fixture";

    AlembicGroomImporter::Options options;
    options.ProvenancePath = "grooms/scalp.abc";

    const auto firstImport = AlembicGroomImporter::Import(abcPath, options);
    ASSERT_TRUE(firstImport.Succeeded()) << firstImport.Diagnostic;
    ASSERT_TRUE(firstImport.Groom);

    // The authored data actually arrived — a round-trip test that round-trips
    // an empty groom passes for the wrong reason.
    EXPECT_EQ(firstImport.Groom->GetCurveCount(), 256u);
    EXPECT_EQ(firstImport.Groom->GetPointCount(), 256u * 6u);
    EXPECT_EQ(firstImport.Groom->GetGuideCount(), 32u) << "every 8th of 256 strands was authored as a guide";
    EXPECT_EQ(firstImport.Groom->GetBasis(), GroomCurveBasis::Linear);
    EXPECT_EQ(firstImport.Groom->GetProvenance().SourcePath, "grooms/scalp.abc");
    EXPECT_EQ(firstImport.Groom->GetProvenance().SourceFormat, "AlembicCurves");
    EXPECT_NE(firstImport.Groom->GetProvenance().SourceContentHash, 0ull);

    // REIMPORT: the same file, imported again, must be the same groom.
    const auto secondImport = AlembicGroomImporter::Import(abcPath, options);
    ASSERT_TRUE(secondImport.Succeeded()) << secondImport.Diagnostic;
    ExpectSameGroom(*firstImport.Groom, *secondImport.Groom, "import -> reimport");

    // COOK -> SAVE -> LOAD.
    std::string reason;
    std::vector<u8> cooked;
    ASSERT_TRUE(GroomCooker::CookToBytes(*firstImport.Groom, cooked, reason)) << reason;

    const std::filesystem::path cookedPath = Tests::TempFile("scalp.ologroom");
    {
        std::ofstream out(cookedPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
    }
    std::vector<u8> readBack;
    {
        std::ifstream in(cookedPath, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.is_open());
        readBack.resize(static_cast<sizet>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(readBack.data()), static_cast<std::streamsize>(readBack.size()));
    }

    Ref<GroomAsset> loaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(readBack.data(), readBack.size(), loaded, reason)) << reason;
    ExpectSameGroom(*firstImport.Groom, *loaded, "import -> cook -> save -> load");
}

TEST(GroomRoundTrip, ImportPreservesRootUVsAndPerVertexWidthsExactly)
{
    namespace Fixture = Tests::GroomFixture;

    // Small, hand-checkable: two strands, known UVs, known per-vertex widths.
    Fixture::CurvesPrim prim;
    prim.Name = "tiny";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.1f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.1f, 0.0f } };
    prim.VertexCounts = { 3, 2 };
    prim.Widths = { 0.004f, 0.003f, 0.002f, 0.009f, 0.008f };
    prim.WidthScope = Fixture::AbcG::kVertexScope;
    prim.UVs = { { 0.125f, 0.25f }, { 0.75f, 0.5f } };
    prim.UVScope = Fixture::AbcG::kUniformScope;
    prim.GuideFlags = { 0, 1 };

    const std::filesystem::path abcPath = Tests::TempFile("tiny.abc");
    ASSERT_TRUE(Fixture::WriteArchive(abcPath, { prim }));

    const auto result = AlembicGroomImporter::Import(abcPath);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;
    const GroomAsset& groom = *result.Groom;

    ASSERT_EQ(groom.GetCurveCount(), 2u);
    EXPECT_EQ(groom.GetCurveOffsets(), (std::vector<u32>{ 0u, 3u, 5u }));

    EXPECT_EQ(groom.GetRootUVs()[0], glm::vec2(0.125f, 0.25f));
    EXPECT_EQ(groom.GetRootUVs()[1], glm::vec2(0.75f, 0.5f));

    // Widths under an identity transform pass through untouched.
    EXPECT_EQ(groom.GetPointWidths(), (std::vector<f32>{ 0.004f, 0.003f, 0.002f, 0.009f, 0.008f }));

    EXPECT_FALSE(groom.IsGuide(0));
    EXPECT_TRUE(groom.IsGuide(1));
    EXPECT_EQ(groom.GetGuideCount(), 1u);

    // Root first: the first point of curve 0 is the authored root.
    EXPECT_EQ(groom.GetPoints()[groom.GetCurveFirstPoint(0)], glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(groom.GetPoints()[groom.GetCurveFirstPoint(1)], glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST(GroomRoundTrip, SubGroupsBecomeNamedGroupsAndSurviveTheCook)
{
    namespace Fixture = Tests::GroomFixture;

    const std::filesystem::path abcPath = Tests::TempFile("pelt.abc");
    const Fixture::CurvesPrim prim = Fixture::MakeAnimalFurGroom(/*strandCount*/ 120, /*pointsPerStrand*/ 4,
                                                                 /*subGroupCount*/ 3);
    ASSERT_TRUE(Fixture::WriteArchive(abcPath, { prim }));

    const auto result = AlembicGroomImporter::Import(abcPath);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;
    const GroomAsset& groom = *result.Groom;

    ASSERT_EQ(groom.GetGroupCount(), 3u);
    for (const std::string& name : groom.GetGroupNames())
    {
        EXPECT_NE(name.find("#"), std::string::npos) << "a sub-grouped prim names its groups '<path>#<index>': " << name;
    }
    EXPECT_EQ(groom.GetBasis(), GroomCurveBasis::BSpline) << "the fixture authors kCubic curves";

    // 120 strands across 3 sub-groups, interleaved -> 40 each.
    for (u32 group = 0; group < 3; ++group)
    {
        EXPECT_EQ(groom.GetGroupRanges()[group].CurveCount, 40u);
    }

    std::string reason;
    std::vector<u8> cooked;
    ASSERT_TRUE(GroomCooker::CookToBytes(groom, cooked, reason)) << reason;

    Ref<GroomAsset> loaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(cooked.data(), cooked.size(), loaded, reason)) << reason;
    ExpectSameGroom(groom, *loaded, "sub-grouped animal groom");
}

TEST(GroomRoundTrip, SourceTransformIsBakedIntoPointsAndWidths)
{
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "xformed";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 2 };
    prim.Widths = { 0.1f, 0.1f };
    prim.WidthScope = Fixture::AbcG::kVertexScope;

    // Uniform 2x scale plus a translation, in Alembic's row-vector convention.
    Imath::M44d transform;
    transform.setScale(Imath::V3d(2.0, 2.0, 2.0));
    transform[3][0] = 5.0;
    transform[3][1] = 0.0;
    transform[3][2] = -3.0;
    prim.Transform = transform;

    const std::filesystem::path abcPath = Tests::TempFile("xformed.abc");
    ASSERT_TRUE(Fixture::WriteArchive(abcPath, { prim }));

    const auto result = AlembicGroomImporter::Import(abcPath);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;
    const GroomAsset& groom = *result.Groom;

    ASSERT_EQ(groom.GetPointCount(), 2u);
    constexpr f32 kEpsilon = 1e-5f;
    EXPECT_NEAR(groom.GetPoints()[0].x, 5.0f, kEpsilon);
    EXPECT_NEAR(groom.GetPoints()[0].y, 0.0f, kEpsilon);
    EXPECT_NEAR(groom.GetPoints()[0].z, -3.0f, kEpsilon);
    EXPECT_NEAR(groom.GetPoints()[1].y, 2.0f, kEpsilon) << "a 2x scale must move the tip to y=2";

    // A 2x scale makes a 2x-thick strand — what every DCC means by it.
    EXPECT_NEAR(groom.GetPointWidths()[0], 0.2f, kEpsilon);
}

#endif // OLO_WITH_ALEMBIC
