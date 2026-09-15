#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomRejectionTest — issue #1232, acceptance criterion 3, first half.
//
// "Reject malformed curve data and unsupported attributes explicitly."
//
// Every case here asserts TWO things, and the second is the one that matters:
//
//   1. the import or decode FAILED, and
//   2. the diagnostic NAMES the problem.
//
// A test that only checked (1) would pass against an importer that returned
// "failed" for everything, which is not what "explicitly" means — and this
// repo's no-silent-fallbacks rule is specifically about the difference between
// a rejection you can act on and one you cannot.
//
// The substring assertions are deliberately on the DOMAIN WORD ("periodic",
// "variable-order", "vertex counts"), not on whole sentences, so rewording a
// message does not break the suite while dropping the fact does.
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

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // Case-insensitive substring match, so a message that capitalises a word
    // differently still counts as naming it.
    [[nodiscard]] bool Mentions(const std::string& haystack, std::string_view needle)
    {
        std::string lowerHaystack = haystack;
        std::string lowerNeedle(needle);
        for (char& c : lowerHaystack)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (char& c : lowerNeedle)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lowerHaystack.find(lowerNeedle) != std::string::npos;
    }

    // A minimal well-formed builder input, so each case can break exactly one
    // thing and keep the rest valid.
    struct BuilderHarness
    {
        GroomBuilder Builder;
        u16 Group = 0;
        std::string Reason;

        BuilderHarness()
        {
            EXPECT_TRUE(Builder.AddGroup("group", Group, Reason)) << Reason;
        }

        [[nodiscard]] bool TryAdd(const std::vector<glm::vec3>& points, const std::vector<f32>& widths,
                                  glm::vec2 rootUV = { 0.0f, 0.0f })
        {
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = rootUV;
            input.GroupId = Group;
            Reason.clear();
            return Builder.AddCurve(input, Reason);
        }
    };
} // namespace

// ── Malformed curve data, rejected at the builder ───────────────────────────

TEST(GroomRejection, SinglePointCurveIsRejectedAndSaysWhy)
{
    BuilderHarness harness;
    // One point has no direction, so it is not a strand at any later stage.
    EXPECT_FALSE(harness.TryAdd({ { 0.0f, 0.0f, 0.0f } }, { 0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "control points")) << harness.Reason;
    EXPECT_TRUE(Mentions(harness.Reason, "minimum")) << harness.Reason;
}

TEST(GroomRejection, WidthCountMismatchIsRejectedAndSaysWhy)
{
    BuilderHarness harness;
    EXPECT_FALSE(harness.TryAdd({ { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } }, { 0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "widths")) << harness.Reason;
}

TEST(GroomRejection, NonFinitePositionIsRejectedAndSaysWhy)
{
    BuilderHarness harness;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(harness.TryAdd({ { 0.0f, 0.0f, 0.0f }, { nan, 1.0f, 0.0f } }, { 0.001f, 0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "finite")) << harness.Reason;

    const f32 inf = std::numeric_limits<f32>::infinity();
    EXPECT_FALSE(harness.TryAdd({ { 0.0f, 0.0f, 0.0f }, { 0.0f, inf, 0.0f } }, { 0.001f, 0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "finite")) << harness.Reason;
}

TEST(GroomRejection, NonFiniteOrNegativeWidthIsRejectedAndSaysWhy)
{
    BuilderHarness harness;
    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };

    EXPECT_FALSE(harness.TryAdd(points, { 0.001f, std::numeric_limits<f32>::quiet_NaN() }));
    EXPECT_TRUE(Mentions(harness.Reason, "finite")) << harness.Reason;

    // Negative is a separate, more specific message: it is authoring error, not
    // corruption, and a width is a diameter.
    EXPECT_FALSE(harness.TryAdd(points, { 0.001f, -0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "negative")) << harness.Reason;

    // Zero is LEGAL — a strand tapering to nothing at the tip.
    EXPECT_TRUE(harness.TryAdd(points, { 0.001f, 0.0f })) << harness.Reason;
}

TEST(GroomRejection, NonFiniteRootUVIsRejectedAndSaysWhy)
{
    BuilderHarness harness;
    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    EXPECT_FALSE(harness.TryAdd(points, { 0.001f, 0.001f },
                                { std::numeric_limits<f32>::quiet_NaN(), 0.0f }));
    EXPECT_TRUE(Mentions(harness.Reason, "root uv")) << harness.Reason;

    // A UV OUTSIDE the unit square is legal — a UDIM / tiled groom needs it.
    EXPECT_TRUE(harness.TryAdd(points, { 0.001f, 0.001f }, { 3.5f, -1.25f })) << harness.Reason;
}

TEST(GroomRejection, CoordinateFarOutsideTheBoundIsRejectedAndSuggestsUnitScale)
{
    BuilderHarness harness;
    const f32 huge = GroomLimits::MaxCoordinate * 10.0f;
    EXPECT_FALSE(harness.TryAdd({ { 0.0f, 0.0f, 0.0f }, { huge, 0.0f, 0.0f } }, { 0.001f, 0.001f }));
    EXPECT_TRUE(Mentions(harness.Reason, "coordinate bound")) << harness.Reason;
    // The diagnostic should point at the likely cause, not just the symptom.
    EXPECT_TRUE(Mentions(harness.Reason, "scale")) << harness.Reason;
}

TEST(GroomRejection, UnregisteredGroupIsRejectedAndSaysWhy)
{
    GroomBuilder builder;
    std::string reason;
    GroomCurveInput input;
    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const std::vector<f32> widths = { 0.001f, 0.001f };
    input.Points = points;
    input.Widths = widths;
    input.GroupId = 7; // nothing registered
    EXPECT_FALSE(builder.AddCurve(input, reason));
    EXPECT_TRUE(Mentions(reason, "group")) << reason;
}

TEST(GroomRejection, EmptyGroupNameIsRejected)
{
    GroomBuilder builder;
    std::string reason;
    u16 id = 0;
    EXPECT_FALSE(builder.AddGroup("", id, reason));
    EXPECT_TRUE(Mentions(reason, "empty")) << reason;
}

TEST(GroomRejection, AFailedAddCurveLeavesTheBuilderUnchanged)
{
    // A partially-appended strand would corrupt the offset table, and the
    // failure would surface later as an unrelated validation error.
    BuilderHarness harness;
    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    ASSERT_TRUE(harness.TryAdd(points, { 0.001f, 0.001f }));
    ASSERT_EQ(harness.Builder.GetCurveCount(), 1u);

    EXPECT_FALSE(harness.TryAdd(points, { 0.001f, std::numeric_limits<f32>::quiet_NaN() }));
    EXPECT_EQ(harness.Builder.GetCurveCount(), 1u) << "a rejected curve was partially appended";

    std::string reason;
    Ref<GroomAsset> groom = harness.Builder.Build(reason);
    EXPECT_TRUE(groom) << reason;
}

TEST(GroomRejection, ADeclaredButUnusedGroupFailsTheCook)
{
    // Silently dropping the empty group would renumber every later group id,
    // so a groom's group indices would change meaning between two cooks.
    GroomBuilder builder;
    std::string reason;
    u16 used = 0;
    u16 unused = 0;
    ASSERT_TRUE(builder.AddGroup("used", used, reason)) << reason;
    ASSERT_TRUE(builder.AddGroup("unused", unused, reason)) << reason;

    const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const std::vector<f32> widths = { 0.001f, 0.001f };
    GroomCurveInput input;
    input.Points = points;
    input.Widths = widths;
    input.GroupId = used;
    ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;

    Ref<GroomAsset> groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;

    EXPECT_FALSE(GroomCooker::Canonicalize(*groom, reason));
    EXPECT_TRUE(Mentions(reason, "no curves")) << reason;
}

// ── Corrupt cooked files, rejected at the decoder ───────────────────────────

namespace
{
    std::vector<u8> CookASmallGroom()
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("group", group, reason)) << reason;
        for (u32 c = 0; c < 8; ++c)
        {
            const std::vector<glm::vec3> points = { { static_cast<f32>(c), 0.0f, 0.0f },
                                                    { static_cast<f32>(c), 1.0f, 0.0f } };
            const std::vector<f32> widths = { 0.002f, 0.001f };
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.GroupId = group;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;

        std::vector<u8> bytes;
        EXPECT_TRUE(GroomCooker::CookToBytes(*groom, bytes, reason)) << reason;
        return bytes;
    }
} // namespace

TEST(GroomRejection, WrongMagicIsRejectedAndSaysWhy)
{
    std::vector<u8> bytes = CookASmallGroom();
    ASSERT_GE(bytes.size(), sizeof(OloGroomFormat::FileHeader));
    bytes[0] ^= 0xFFu;

    Ref<GroomAsset> groom;
    std::string reason;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason));
    EXPECT_TRUE(Mentions(reason, "magic")) << reason;
    EXPECT_FALSE(groom) << "a rejected file must not leave a half-populated asset behind";
}

TEST(GroomRejection, AFutureVersionIsRejectedWithARecoverableMessage)
{
    std::vector<u8> bytes = CookASmallGroom();
    OloGroomFormat::FileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.Version = OloGroomFormat::CurrentVersion + 1;
    std::memcpy(bytes.data(), &header, sizeof(header));

    Ref<GroomAsset> groom;
    std::string reason;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason));
    EXPECT_TRUE(Mentions(reason, "version")) << reason;
    // A .ologroom is a derived artifact, so the fix is a re-import, and the
    // message should say so rather than leaving the reader to guess.
    EXPECT_TRUE(Mentions(reason, "re-import")) << reason;
}

TEST(GroomRejection, ACorruptPayloadFailsTheChecksumRatherThanDecodingGarbage)
{
    std::vector<u8> bytes = CookASmallGroom();
    ASSERT_GT(bytes.size(), sizeof(OloGroomFormat::FileHeader) + 4);
    // Flip a bit well inside the compressed payload.
    bytes[bytes.size() - 3] ^= 0x01u;

    Ref<GroomAsset> groom;
    std::string reason;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason));
    EXPECT_TRUE(Mentions(reason, "checksum")) << reason;
}

TEST(GroomRejection, ATruncatedFileIsRejected)
{
    std::vector<u8> bytes = CookASmallGroom();
    bytes.resize(bytes.size() / 2);

    Ref<GroomAsset> groom;
    std::string reason;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason));
    EXPECT_FALSE(reason.empty());
}

TEST(GroomRejection, AnEmptyBufferIsRejectedRatherThanReadPastTheEnd)
{
    Ref<GroomAsset> groom;
    std::string reason;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(nullptr, 0, groom, reason));
    EXPECT_FALSE(reason.empty());

    const std::vector<u8> tiny(4, 0u);
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(tiny.data(), tiny.size(), groom, reason));
    EXPECT_TRUE(Mentions(reason, "too small")) << reason;
}

// ── Malformed and unsupported Alembic input ─────────────────────────────────

#if defined(OLO_WITH_ALEMBIC)

TEST(GroomRejection, PeriodicCurvesAreRejectedAndSayWhy)
{
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "loops";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 3 };
    prim.Wrap = Fixture::AbcG::kPeriodic;

    const std::filesystem::path path = Tests::TempFile("periodic.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "periodic")) << result.Diagnostic;
    EXPECT_TRUE(Mentions(result.Diagnostic, "root")) << result.Diagnostic;
}

TEST(GroomRejection, VariableOrderCurvesAreRejectedAndSayWhy)
{
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "varorder";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 3 };
    prim.Type = Fixture::AbcG::kVariableOrder;

    const std::filesystem::path path = Tests::TempFile("varorder.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "variable-order")) << result.Diagnostic;
}

TEST(GroomRejection, VertexCountsThatDisagreeWithThePointArrayAreRejected)
{
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "mismatch";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 5 }; // claims five points, holds three

    const std::filesystem::path path = Tests::TempFile("mismatch.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "vertex counts")) << result.Diagnostic;
}

TEST(GroomRejection, AnUnsupportedGroomAttributeIsRejectedByName)
{
    // This is the "unsupported attributes" half of AC 3. Importing the file
    // anyway would produce a groom quietly missing authored intent, which is
    // worse than a refusal because nothing downstream can detect it.
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "future";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 2 };
    prim.ExtraIntParamNames = { "groom_clumpid" };

    const std::filesystem::path path = Tests::TempFile("unsupported-attr.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "groom_clumpid"))
        << "the diagnostic must NAME the attribute it refused: " << result.Diagnostic;
}

TEST(GroomRejection, ANonGroomArbGeomParamIsIgnoredNotRejected)
{
    // The mirror image of the case above. A DCC's own bookkeeping attributes
    // are not groom semantics, and refusing them would make ordinary exports
    // un-importable.
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "dccnoise";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 2 };
    prim.ExtraIntParamNames = { "houdini_primid" };

    const std::filesystem::path path = Tests::TempFile("dcc-attr.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_TRUE(result.Succeeded()) << result.Diagnostic;
}

TEST(GroomRejection, APolygonOnlyArchiveIsRejectedWithTheCurveSupportDistinction)
{
    // The issue calls this out by name: "do not treat existing polygon Alembic
    // import as curve support". An archive with no ICurves must fail here and
    // say so, rather than falling back to the mesh importer.
    namespace Fixture = Tests::GroomFixture;

    // An archive with a prim that is not ICurves: write zero prims, which
    // produces a valid but curve-free archive.
    const std::filesystem::path path = Tests::TempFile("nocurves.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, {}));

    EXPECT_FALSE(AlembicGroomImporter::ArchiveContainsCurves(path));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "icurves")) << result.Diagnostic;
    EXPECT_TRUE(Mentions(result.Diagnostic, "not curve support")) << result.Diagnostic;
}

TEST(GroomRejection, AMissingFileIsRejectedWithItsPath)
{
    const std::filesystem::path path = Tests::TempFile("does-not-exist.abc");
    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "does not exist")) << result.Diagnostic;
}

TEST(GroomRejection, MixedBasesInOneArchiveAreRejected)
{
    // One cooked groom carries one basis. Accepting a mixed archive would mean
    // silently reinterpreting half the strands under the wrong basis.
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim linear = Fixture::MakeHumanScalpGroom(8, 4, 0, "linearPrim");
    Fixture::CurvesPrim cubic = Fixture::MakeAnimalFurGroom(8, 4, 1, "cubicPrim");

    const std::filesystem::path path = Tests::TempFile("mixedbasis.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { linear, cubic }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "basis")) << result.Diagnostic;
}

TEST(GroomRejection, AWidthsParamWithTheWrongValueCountIsRejected)
{
    namespace Fixture = Tests::GroomFixture;

    Fixture::CurvesPrim prim;
    prim.Name = "badwidths";
    prim.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
    prim.VertexCounts = { 2, 2 };
    // Vertex scope demands four values; three is malformed.
    prim.Widths = { 0.001f, 0.001f, 0.001f };
    prim.WidthScope = Fixture::AbcG::kVertexScope;

    const std::filesystem::path path = Tests::TempFile("badwidths.abc");
    ASSERT_TRUE(Fixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(Mentions(result.Diagnostic, "widths")) << result.Diagnostic;
}

#endif // OLO_WITH_ALEMBIC
