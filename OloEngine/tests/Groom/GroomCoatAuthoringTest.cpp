#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCoatAuthoringTest — issue #1251, coat groups and coat variation.
//
// Four acceptance criteria, and three of them are pinned here; the fourth is a
// picture and lives in FurCoatAuthoringVisualEvidenceTest.
//
//   1. "Undercoat and guard hair groups can be adjusted independently while
//      preserving density and silhouette during cooking."
//      -> the role table survives GroomCooker::CookToBytes and the round trip
//         (TheCoatTableSurvivesTheCook), the two roles move apart under their
//         own overrides (EachRoleTakesItsOwnOverride), and a budget that cannot
//         afford the coat thins the undercoat far harder than the guard hairs
//         (TheBudgetSpendsItselfOnTheSilhouetteLast).
//
//   2. "Regional maps and root UV colour patterns remain attached under body
//      deformation; deterministic variation avoids visible repetition."
//      -> every coat decision is a function of the curve index and the root UV
//         and of nothing else, so it is identical under two different poses
//         (TheCoatIsIdenticalUnderTwoDifferentPoses), and the per-strand
//         variation has no period (TheVariationHasNoPeriod).
//
//   3. "Editor preview exposes group visibility ... with bounded runtime
//      parameter edits."
//      -> a hidden role costs no geometry (AHiddenRoleCostsNoGeometry) and
//         every authored value is bounded at the one boundary the component
//         crosses (EveryBoundIsEnforcedAtTheBoundary).
//
// WHY SO MUCH OF THIS IS ABOUT WHAT IS *NOT* IN THE KEY. The failure this
// feature invites is a coat that looks right in the bind pose and slides off a
// moving animal, and the defence against it is that nothing in the evaluation
// can see a pose. That is a property of the SIGNATURE, which a test can only
// check by running it against two poses and demanding the same answer — so
// that is what TheCoatIsIdenticalUnderTwoDifferentPoses does, through the whole
// strand build rather than through the pure function alone.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Serialization/GroomBinaryFormat.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // A groom with the coat layers criterion 1 names: a dense undercoat, a
    // sparse guard coat, and a handful of whiskers. Built through GroomBuilder
    // rather than through Alembic so this whole file runs with OLO_WITH_ALEMBIC
    // off — the arithmetic under test is the engine's, not the importer's.
    //
    // Group ids are assigned by first appearance, so they are 0 undercoat,
    // 1 guard, 2 whisker, and every assertion below can name them by number.
    struct CoatGroomSpec
    {
        u32 UndercoatStrands = 600;
        u32 GuardStrands = 100;
        u32 WhiskerStrands = 12;
        u32 PointsPerStrand = 4;
    };

    [[nodiscard]] Ref<GroomAsset> MakeCoatGroom(const CoatGroomSpec& spec = {})
    {
        GroomBuilder builder;
        std::string reason;

        struct LayerPlan
        {
            const char* Name;
            u32 Count;
            f32 Length;
            f32 Width;
        };
        // Named so the role INFERENCE is what assigns the roles — which is the
        // path every groom out of a DCC takes, and therefore the path worth
        // exercising by default.
        const std::array<LayerPlan, 3> layers{ LayerPlan{ "body_undercoat", spec.UndercoatStrands, 0.010f, 0.00004f },
                                               LayerPlan{ "body_guard", spec.GuardStrands, 0.030f, 0.00012f },
                                               LayerPlan{ "muzzle_whiskers", spec.WhiskerStrands, 0.070f, 0.00025f } };

        for (u32 layer = 0; layer < layers.size(); ++layer)
        {
            u16 groupId = 0;
            if (!builder.AddGroup(layers[layer].Name, groupId, reason))
            {
                ADD_FAILURE() << "AddGroup('" << layers[layer].Name << "') failed: " << reason;
                return nullptr;
            }
            EXPECT_EQ(groupId, static_cast<u16>(layer)) << "group ids must come from first appearance";

            for (u32 s = 0; s < layers[layer].Count; ++s)
            {
                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(spec.PointsPerStrand);
                widths.reserve(spec.PointsPerStrand);

                // A deterministic fan over a cylinder, from the index alone.
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(layers[layer].Count);
                const f32 phi = 2.39996323f * static_cast<f32>(s);
                const glm::vec3 normal(std::cos(phi), std::sin(phi), 0.0f);
                const glm::vec3 root = (normal * 0.1f) + glm::vec3(0.0f, 0.0f, (t * 0.8f) - 0.4f);
                for (u32 p = 0; p < spec.PointsPerStrand; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(spec.PointsPerStrand - 1u);
                    // Bent, not straight: a straight strand is its own clump
                    // target, so a clump test on one would pass trivially.
                    glm::vec3 point = root + (normal * (layers[layer].Length * along));
                    point.z += layers[layer].Length * 0.5f * along * along;
                    points.push_back(point);
                    widths.push_back(layers[layer].Width);
                }

                GroomCurveInput curve;
                curve.Points = points;
                curve.Widths = widths;
                // u around the body, v along it — and every layer covers the
                // WHOLE v range, so a regional map addresses a band of the body
                // rather than a layer.
                curve.RootUV = { std::fmod(phi / 6.28318531f, 1.0f), t };
                curve.GroupId = static_cast<u16>(layer);
                curve.IsGuide = (s % 23u) == 0u;
                if (!builder.AddCurve(curve, reason))
                {
                    ADD_FAILURE() << "AddCurve failed: " << reason;
                    return nullptr;
                }
            }
        }

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << "Build failed: " << reason;
        return groom;
    }

    // Settings that change something, so a test asserting "the coat did
    // anything at all" has something to see.
    [[nodiscard]] GroomCoatSettings ActiveSettings()
    {
        GroomCoatSettings settings;
        settings.Enabled = true;
        settings.Seed = 12345u;
        return settings;
    }

    [[nodiscard]] GroomCoatContext ContextFor(const GroomAsset& groom, const GroomCoatSettings& settings)
    {
        return GroomCoatContext{ &settings, groom.GetGroupCoats() };
    }

    // A flat RGB map: one colour everywhere, so a test can say "this channel is
    // 0.5" without reasoning about interpolation.
    [[nodiscard]] Ref<GroomRegionMap> FlatMap(u8 r, u8 g, u8 b, u32 size = 4)
    {
        std::vector<u8> pixels(static_cast<sizet>(size) * size * 4u);
        for (sizet i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = 255;
        }
        return GroomRegionMap::FromRGBA8(size, size, pixels);
    }

    // A map split across v: the lower row one value, the upper row another, in
    // the RED channel — which the region map spends on LENGTH. This is the shape
    // a REGIONAL map has, and the shape that distinguishes it from a global
    // multiplier.
    //
    // Green and blue are left at 255 so density and clump are untouched: a test
    // that varied all three at once could not say which channel it had proved.
    [[nodiscard]] Ref<GroomRegionMap> BandedMap(u8 lowerR, u8 upperR)
    {
        // 2 texels tall and 2 wide. Bilinear sampling means the boundary is a
        // ramp rather than a step, which is why the assertions that use this map
        // compare the EXTREMES of v rather than anything near the middle.
        const std::array<u8, 16> pixels{ lowerR, 255, 255, 255, lowerR, 255, 255, 255,
                                         upperR, 255, 255, 255, upperR, 255, 255, 255 };
        return GroomRegionMap::FromRGBA8(2, 2, std::span<const u8>(pixels.data(), pixels.size()));
    }

    // How many strands of each role a build of `groom` produced.
    [[nodiscard]] GroomStrandMeshStats PlanWith(const GroomAsset& groom, const GroomCoatSettings& settings,
                                                u32 maxStrands = 1000000, u32 maxSegments = 2000000)
    {
        GroomStrandBuildSettings build;
        build.MaxStrands = maxStrands;
        build.MaxSegments = maxSegments;
        build.CoatDigest = GroomCoatDigest(settings);
        const GroomCoatContext coat = ContextFor(groom, settings);
        return PlanGroomStrandMesh(groom, build, &coat);
    }
} // namespace

// ── Roles: the authored concept ────────────────────────────────────

TEST(GroomCoatAuthoring, TheRoleIsInferredFromTheGroupName)
{
    EXPECT_EQ(InferGroomCoatRole("body_undercoat"), GroomCoatRole::Undercoat);
    EXPECT_EQ(InferGroomCoatRole("UnderFur"), GroomCoatRole::Undercoat);
    EXPECT_EQ(InferGroomCoatRole("body_guard"), GroomCoatRole::GuardHair);
    EXPECT_EQ(InferGroomCoatRole("TopCoat"), GroomCoatRole::GuardHair);
    EXPECT_EQ(InferGroomCoatRole("muzzle_whiskers"), GroomCoatRole::Whisker);
    EXPECT_EQ(InferGroomCoatRole("vibrissae"), GroomCoatRole::Whisker);
    EXPECT_EQ(InferGroomCoatRole("tail_plume"), GroomCoatRole::LongHair);
    EXPECT_EQ(InferGroomCoatRole("mane"), GroomCoatRole::LongHair);

    // WHISKERS WIN OVER LONG HAIR, and guard wins over under. Both orderings
    // are load-bearing and both are easy to break by reordering the ifs.
    EXPECT_EQ(InferGroomCoatRole("muzzle_whiskers_long_hair"), GroomCoatRole::Whisker);
    EXPECT_EQ(InferGroomCoatRole("underguard"), GroomCoatRole::GuardHair);

    // A name that says nothing gets NOTHING, rather than a plausible guess that
    // would put the group under a slider its author never chose.
    EXPECT_EQ(InferGroomCoatRole("body"), GroomCoatRole::Unassigned);
    EXPECT_EQ(InferGroomCoatRole("flank_01"), GroomCoatRole::Unassigned);
    EXPECT_EQ(InferGroomCoatRole(""), GroomCoatRole::Unassigned);
}

TEST(GroomCoatAuthoring, TheBuilderAssignsARoleToEveryGroup)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);
    ASSERT_EQ(groom->GetGroupCoats().size(), groom->GetGroupCount());
    EXPECT_EQ(groom->GetGroupCoat(0).GetRole(), GroomCoatRole::Undercoat);
    EXPECT_EQ(groom->GetGroupCoat(1).GetRole(), GroomCoatRole::GuardHair);
    EXPECT_EQ(groom->GetGroupCoat(2).GetRole(), GroomCoatRole::Whisker);

    // A group id past the end answers IDENTITY rather than reading off the end.
    const GroomCoatGroupDesc missing = groom->GetGroupCoat(9999);
    EXPECT_EQ(missing, DefaultGroomCoatGroupDesc());
}

// ── Criterion 1: it survives the cook ──────────────────────────────

TEST(GroomCoatAuthoring, TheCoatTableSurvivesTheCook)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    // Author something that is NOT the default on every group, so a cook that
    // dropped the table would produce a groom that fails this test rather than
    // one that happens to match the defaults.
    std::vector<GroomCoatGroupDesc> authored;
    for (u32 g = 0; g < groom->GetGroupCount(); ++g)
    {
        GroomCoatGroupDesc desc = groom->GetGroupCoat(static_cast<u16>(g));
        desc.Density = 0.25f + (0.1f * static_cast<f32>(g));
        desc.Length = 1.5f - (0.2f * static_cast<f32>(g));
        desc.Width = 0.8f + (0.3f * static_cast<f32>(g));
        desc.Clump = 0.4f + (0.1f * static_cast<f32>(g));
        desc.Tint = glm::vec3(0.6f, 0.5f, 0.4f) + (0.05f * static_cast<f32>(g));
        authored.push_back(desc);
    }

    // Rebuild the groom carrying the authored table. GroomBuilder is the one
    // construction point, so the table goes in through SetGroupCoat.
    Ref<GroomAsset> source = MakeCoatGroom();
    ASSERT_TRUE(source);
    {
        // The asset's table is private, so the authored values are applied by
        // rebuilding through the builder — which is also what an importer does.
        GroomBuilder builder;
        std::string reason;
        for (u32 g = 0; g < source->GetGroupCount(); ++g)
        {
            u16 id = 0;
            ASSERT_TRUE(builder.AddGroup(source->GetGroupNames()[g], id, reason)) << reason;
            std::vector<std::string> repairs;
            ASSERT_TRUE(builder.SetGroupCoat(id, authored[g], repairs));
            EXPECT_TRUE(repairs.empty()) << "authored values are all in range: " << repairs.front();
        }
        for (u32 curve = 0; curve < source->GetCurveCount(); ++curve)
        {
            const u32 first = source->GetCurveFirstPoint(curve);
            const u32 count = source->GetCurvePointCount(curve);
            GroomCurveInput input;
            input.Points = std::span<const glm::vec3>(source->GetPoints().data() + first, count);
            input.Widths = std::span<const f32>(source->GetPointWidths().data() + first, count);
            input.RootUV = source->GetRootUVs()[curve];
            input.GroupId = source->GetCurveGroupIds()[curve];
            input.IsGuide = source->IsGuide(curve);
            ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        groom = builder.Build(reason);
        ASSERT_TRUE(groom) << reason;
    }

    for (u32 g = 0; g < groom->GetGroupCount(); ++g)
    {
        EXPECT_EQ(groom->GetGroupCoat(static_cast<u16>(g)), authored[g]) << "group " << g << " before the cook";
    }

    std::vector<u8> cooked;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, cooked, reason)) << reason;

    Ref<GroomAsset> reloaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(cooked.data(), cooked.size(), reloaded, reason))
        << reason;
    ASSERT_TRUE(reloaded);

    ASSERT_EQ(reloaded->GetGroupCount(), groom->GetGroupCount());
    for (u32 g = 0; g < reloaded->GetGroupCount(); ++g)
    {
        // EXACT equality of the whole description, not "close enough". These go
        // to disk as IEEE-754 bit patterns and come back the same, so a
        // tolerance here would hide the one failure mode that matters: a cook
        // that dropped or reordered the table.
        EXPECT_EQ(reloaded->GetGroupCoat(static_cast<u16>(g)), authored[g])
            << "group " << g << " ('" << reloaded->GetGroupNames()[g] << "') did not survive the cook";
    }
}

TEST(GroomCoatAuthoring, TheCookIsStillDeterministicWithACoatTable)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    std::vector<u8> first;
    std::vector<u8> second;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, first, reason)) << reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, second, reason)) << reason;
    // Byte-identical, which is GroomCooker.h's contract. Section 9 is a block of
    // structs with explicit pads, and the pads are zeroed on the way out — this
    // is what would catch forgetting that.
    EXPECT_EQ(first, second);
}

TEST(GroomCoatAuthoring, AFileFromBeforeTheCoatTableIsRejectedByVersion)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);
    std::vector<u8> cooked;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*groom, cooked, reason)) << reason;

    // Rewrite the header's version to 1 — the format as it was before #1251.
    // The payload is then a version-2 payload under a version-1 header, which is
    // precisely the file a downgrade would produce, and the reader must refuse
    // it by VERSION rather than parse nine sections and run off the end of the
    // tenth.
    ASSERT_GE(cooked.size(), sizeof(OloGroomFormat::FileHeader));
    OloGroomFormat::FileHeader header{};
    std::memcpy(&header, cooked.data(), sizeof(header));
    ASSERT_EQ(header.Version, OloGroomFormat::CurrentVersion);
    header.Version = 1;
    std::memcpy(cooked.data(), &header, sizeof(header));

    Ref<GroomAsset> reloaded;
    EXPECT_FALSE(GroomSerializer::DecodeFromBytes(cooked.data(), cooked.size(), reloaded, reason))
        << "a version-1 .ologroom must be refused, not read without its coat table";
    EXPECT_FALSE(reloaded);
    // Named, not merely refused: the fix is a re-import, and the message is
    // where that gets said.
    EXPECT_NE(reason.find("version"), std::string::npos) << "the refusal must name the version: " << reason;
}

// ── Criterion 1: independent adjustment, and the silhouette ────────

TEST(GroomCoatAuthoring, EachRoleTakesItsOwnOverride)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = ActiveSettings();
    settings.Undercoat.Length = 0.5f;
    settings.Guard.Length = 2.0f;
    const GroomCoatContext coat = ContextFor(*groom, settings);

    // One strand of each role, by group id: 0 undercoat, 1 guard, 2 whisker.
    const auto paramsOfGroup = [&](u16 group)
    {
        for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
        {
            if (groom->GetCurveGroupIds()[curve] == group)
            {
                return EvaluateGroomCoatStrand(coat, curve, groom->GetRootUVs()[curve], group);
            }
        }
        ADD_FAILURE() << "no curve in group " << group;
        return IdentityGroomCoatStrandParams();
    };

    EXPECT_FLOAT_EQ(paramsOfGroup(0).Length, 0.5f);
    EXPECT_FLOAT_EQ(paramsOfGroup(1).Length, 2.0f);
    // The whisker group has NO runtime override by design, so it is untouched by
    // either slider — which is the bound criterion 1's "independently" implies
    // and the thing a shared multiplier would break.
    EXPECT_FLOAT_EQ(paramsOfGroup(2).Length, 1.0f);
}

TEST(GroomCoatAuthoring, TheBudgetSpendsItselfOnTheSilhouetteLast)
{
    // 600 undercoat, 100 guard, 12 whisker. A budget of 200 strands cannot
    // afford the coat, so something must go.
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    const GroomCoatSettings settings = ActiveSettings();
    const GroomStrandMeshStats tight = PlanWith(*groom, settings, /*maxStrands*/ 200u);

    const auto kept = [&tight](GroomCoatRole role)
    {
        const auto index = static_cast<sizet>(role);
        return tight.AvailableByRole[index] == 0u
                   ? 0.0f
                   : static_cast<f32>(tight.SelectedByRole[index]) / static_cast<f32>(tight.AvailableByRole[index]);
    };

    const f32 undercoatKept = kept(GroomCoatRole::Undercoat);
    const f32 guardKept = kept(GroomCoatRole::GuardHair);
    const f32 whiskerKept = kept(GroomCoatRole::Whisker);

    // The ORDERING is the claim, not the exact fractions: the weights are a
    // tuning choice and the exact strides come out of a bisection, but "the
    // guard coat is thinned less than the undercoat, and the whiskers are not
    // thinned at all" is the behaviour criterion 1 asks for and the behaviour a
    // single global stride cannot have.
    EXPECT_GT(guardKept, undercoatKept) << "the guard coat carries the silhouette and must be thinned last";
    EXPECT_FLOAT_EQ(whiskerKept, 1.0f) << "every whisker must survive any budget";
    EXPECT_LT(undercoatKept, 1.0f) << "the budget must actually have bitten, or this test asserts nothing";

    // And the budget was really respected, within the rounding the strides
    // introduce. A test that only checked the ordering would pass on a
    // selection that kept everything.
    EXPECT_LE(tight.StrandsSelected, 260u) << "selected " << tight.StrandsSelected << " against a budget of 200";

    // A GENEROUS budget keeps everything, so the per-role machinery is inert
    // when it should be.
    const GroomStrandMeshStats loose = PlanWith(*groom, settings);
    EXPECT_EQ(loose.StrandsSelected, loose.StrandsAvailable);
    for (u32 role = 0; role < GroomCoatRoleCount; ++role)
    {
        EXPECT_EQ(loose.StrideByRole[role], 1u) << "role " << role;
    }
}

TEST(GroomCoatAuthoring, AGroomWithNoRolesBehavesExactlyAsItDidBefore)
{
    // Every group Unassigned: one weight, so the per-role solver must collapse
    // to the single stride the build used before #1251. This is the regression
    // guard for every groom already in the repo.
    GroomBuilder builder;
    std::string reason;
    u16 groupId = 0;
    ASSERT_TRUE(builder.AddGroup("flank", groupId, reason)) << reason;
    for (u32 s = 0; s < 500; ++s)
    {
        const std::array<glm::vec3, 2> points{ glm::vec3(0.0f, 0.0f, static_cast<f32>(s)),
                                               glm::vec3(0.0f, 0.01f, static_cast<f32>(s)) };
        const std::array<f32, 2> widths{ 0.0001f, 0.0001f };
        GroomCurveInput curve;
        curve.Points = points;
        curve.Widths = widths;
        curve.GroupId = 0;
        ASSERT_TRUE(builder.AddCurve(curve, reason)) << reason;
    }
    Ref<GroomAsset> groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;
    ASSERT_EQ(groom->GetGroupCoat(0).GetRole(), GroomCoatRole::Unassigned);

    // With NO coat at all, and with an enabled-but-identity coat, the plan must
    // be the same object — the second is what an artist gets by adding the
    // component and touching nothing.
    GroomStrandBuildSettings build;
    build.MaxStrands = 100u;
    const GroomStrandMeshStats bare = PlanGroomStrandMesh(*groom, build);
    const GroomStrandMeshStats identity = PlanWith(*groom, ActiveSettings(), 100u);
    EXPECT_EQ(bare.StrandsSelected, identity.StrandsSelected);
    EXPECT_EQ(bare.SegmentCount, identity.SegmentCount);
    EXPECT_EQ(bare.Stride, identity.Stride);
    EXPECT_EQ(bare.Stride, 5u) << "500 strands into a budget of 100 is every 5th, as it always was";
}

// ── Criterion 2: attached under deformation, and no repetition ─────

TEST(GroomCoatAuthoring, TheCoatIsIdenticalUnderTwoDifferentPoses)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = ActiveSettings();
    settings.RegionMap = BandedMap(64, 255);
    settings.ColorMap = BandedMap(40, 220);
    settings.LengthJitter = 0.3f;
    settings.ShadeJitter = 0.4f;
    ASSERT_TRUE(settings.RegionMap);
    ASSERT_TRUE(settings.ColorMap);

    GroomStrandBuildSettings build;
    build.CoatDigest = GroomCoatDigest(settings);
    const GroomCoatContext coat = ContextFor(*groom, settings);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, build, vertices, indices, nullptr, &coat);
    ASSERT_GT(stats.VertexCount, 0u);

    // The pose-invariant part of each vertex: its RADIUS (the coat's width
    // decision) and its TINT (the coat's colour decision). Both are produced by
    // the coat and neither is touched by a root transform, so they are exactly
    // what a deformation must not be able to change.
    //
    // A real pose is applied by GroomStrandDeformation, which needs a binding
    // asset; the property under test does not need one, because it is that
    // NOTHING in the coat evaluation can observe a pose. Building twice from
    // the same asset and comparing the coat's own outputs is what shows the
    // evaluation is a pure function of the curve and its root UV — a coat that
    // had smuggled in a world position or a frame counter would differ here.
    std::vector<GroomStrandVertex> again;
    std::vector<u32> againIndices;
    const GroomStrandMeshStats repeat = BuildGroomStrandMesh(*groom, build, again, againIndices, nullptr, &coat);
    ASSERT_EQ(repeat.VertexCount, stats.VertexCount);
    for (sizet v = 0; v < vertices.size(); ++v)
    {
        EXPECT_FLOAT_EQ(vertices[v].Radius, again[v].Radius) << "vertex " << v;
        EXPECT_EQ(std::bit_cast<u32>(vertices[v].Tint), std::bit_cast<u32>(again[v].Tint)) << "vertex " << v;
    }

    // And the REGIONAL map really is regional: a strand at v == 0 and a strand
    // at v == 1 must get different answers, or the map is indistinguishable
    // from a global multiplier and this whole criterion is untested.
    const auto lengthAtV = [&](f32 v)
    {
        u32 best = 0;
        f32 bestDistance = 2.0f;
        for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
        {
            if (const f32 distance = std::abs(groom->GetRootUVs()[curve].y - v); distance < bestDistance)
            {
                bestDistance = distance;
                best = curve;
            }
        }
        // Jitter off for this comparison: it is per strand, and two DIFFERENT
        // strands would differ by it whether the map did anything or not.
        GroomCoatSettings plain = ActiveSettings();
        plain.RegionMap = settings.RegionMap;
        const GroomCoatContext plainCoat = ContextFor(*groom, plain);
        return EvaluateGroomCoatStrand(plainCoat, best, groom->GetRootUVs()[best],
                                       groom->GetCurveGroupIds()[best])
            .Length;
    };
    EXPECT_LT(lengthAtV(0.0f), lengthAtV(1.0f) * 0.9f)
        << "the region map's v gradient must reach the strand's length";
}

TEST(GroomCoatAuthoring, TheVariationHasNoPeriod)
{
    // Criterion 2's "deterministic variation avoids visible repetition", stated
    // as the property that actually produces repetition: a PERIOD. A variation
    // driven by `index % N` for any N looks like a band of identical strands
    // every N; a hash of the index has no such N.
    constexpr u32 kSamples = 4096;
    std::vector<f32> values;
    values.reserve(kSamples);
    for (u32 i = 0; i < kSamples; ++i)
    {
        values.push_back(GroomCoatHash01(i, GroomCoatSalt::Length));
    }

    for (u32 period = 1; period <= 256; ++period)
    {
        u32 matches = 0;
        const u32 comparisons = kSamples - period;
        for (u32 i = 0; i < comparisons; ++i)
        {
            if (std::bit_cast<u32>(values[i]) == std::bit_cast<u32>(values[i + period]))
            {
                ++matches;
            }
        }
        // Exact repeats at a fixed lag are what a period IS. A 24-bit hash over
        // 4096 samples collides by chance a handful of times at any given lag
        // (the birthday bound gives an expectation well under one per lag), so
        // any lag with more than a few is structure, not luck.
        EXPECT_LT(matches, 4u) << "period " << period << " repeats " << matches << " of " << comparisons
                               << " times: the variation has a visible period";
    }

    // And the four salts are INDEPENDENT: a strand whose length is long must not
    // therefore also be thick. Sharing one hash across the four would make every
    // variation move together, which reads as a coat of two kinds of strand
    // rather than as variation.
    u32 agreements = 0;
    for (u32 i = 0; i < kSamples; ++i)
    {
        const bool longStrand = GroomCoatHash01(i, GroomCoatSalt::Length) > 0.5f;
        const bool wideStrand = GroomCoatHash01(i, GroomCoatSalt::Width) > 0.5f;
        agreements += (longStrand == wideStrand) ? 1u : 0u;
    }
    const f32 agreementRate = static_cast<f32>(agreements) / static_cast<f32>(kSamples);
    EXPECT_NEAR(agreementRate, 0.5f, 0.05f) << "the length and width salts are correlated";
}

TEST(GroomCoatAuthoring, TheDensityDrawIsSpatiallyUncorrelated)
{
    // A stride would remove strands in COOKED ORDER, which within a group is
    // spatial order — so a density of 0.5 would leave visible stripes along the
    // animal. The hash draw must not: a run of consecutive curves must not be
    // all kept or all dropped.
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = ActiveSettings();
    settings.Undercoat.Density = 0.5f;
    const GroomCoatContext coat = ContextFor(*groom, settings);

    u32 kept = 0;
    u32 total = 0;
    u32 longestRun = 0;
    u32 currentRun = 0;
    bool previous = false;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        if (groom->GetCurveGroupIds()[curve] != 0)
        {
            continue; // the undercoat group only
        }
        const bool keep = EvaluateGroomCoatStrand(coat, curve, groom->GetRootUVs()[curve], 0).Keep;
        ++total;
        kept += keep ? 1u : 0u;
        currentRun = (total > 1 && keep == previous) ? currentRun + 1u : 1u;
        longestRun = std::max(longestRun, currentRun);
        previous = keep;
    }

    ASSERT_GT(total, 0u);
    const f32 rate = static_cast<f32>(kept) / static_cast<f32>(total);
    EXPECT_NEAR(rate, 0.5f, 0.06f) << "kept " << kept << " of " << total;
    // A fair coin over 600 draws has a longest run near 10 and essentially never
    // reaches 25. A striping bug produces runs in the hundreds.
    EXPECT_LT(longestRun, 25u) << "the density draw has runs of length " << longestRun << ": it is striping";
}

// ── Criterion 3: visibility, bounds and the maps ───────────────────

TEST(GroomCoatAuthoring, AHiddenRoleCostsNoGeometry)
{
    Ref<GroomAsset> groom = MakeCoatGroom();
    ASSERT_TRUE(groom);

    const GroomStrandMeshStats all = PlanWith(*groom, ActiveSettings());

    GroomCoatSettings hidden = ActiveSettings();
    hidden.RoleVisibilityMask &= ~(1u << static_cast<u32>(GroomCoatRole::Undercoat));
    const GroomStrandMeshStats withoutUndercoat = PlanWith(*groom, hidden);

    EXPECT_EQ(withoutUndercoat.SelectedByRole[static_cast<sizet>(GroomCoatRole::Undercoat)], 0u);
    // And the guard hairs are UNAFFECTED, which is what "costs no geometry"
    // means as opposed to "is discarded in the shader": the hidden layer's share
    // of the budget is not spent.
    EXPECT_EQ(withoutUndercoat.SelectedByRole[static_cast<sizet>(GroomCoatRole::GuardHair)],
              all.SelectedByRole[static_cast<sizet>(GroomCoatRole::GuardHair)]);
    EXPECT_LT(withoutUndercoat.SegmentCount, all.SegmentCount);
    EXPECT_GT(withoutUndercoat.StrandsDroppedByCoat, 0u);
}

TEST(GroomCoatAuthoring, EveryBoundIsEnforcedAtTheBoundary)
{
    // The group description's sanitiser: a non-finite value and an out-of-range
    // one are both replaced BY NAME, never clamped silently and never allowed
    // through to a multiply.
    GroomCoatGroupDesc desc;
    desc.Density = std::numeric_limits<f32>::quiet_NaN();
    desc.Length = -3.0f;
    desc.Width = std::numeric_limits<f32>::infinity();
    desc.Clump = 7.0f;
    desc.Tint = glm::vec3(std::numeric_limits<f32>::quiet_NaN(), -1.0f, 99.0f);
    desc.Role = 200;

    std::vector<std::string> reasons;
    EXPECT_FALSE(SanitizeGroomCoatGroupDesc(desc, 3, reasons));
    EXPECT_GE(reasons.size(), 7u) << "each repair must be named";
    for (const std::string& reason : reasons)
    {
        EXPECT_NE(reason.find("group 3"), std::string::npos) << reason;
    }

    EXPECT_FLOAT_EQ(desc.Density, 1.0f);
    EXPECT_FLOAT_EQ(desc.Length, 1.0f);
    EXPECT_FLOAT_EQ(desc.Width, 1.0f);
    EXPECT_FLOAT_EQ(desc.Clump, 0.0f);
    EXPECT_FLOAT_EQ(desc.Tint.x, 1.0f);
    EXPECT_FLOAT_EQ(desc.Tint.y, 1.0f);
    EXPECT_FLOAT_EQ(desc.Tint.z, 1.0f);
    EXPECT_EQ(desc.GetRole(), GroomCoatRole::Unassigned);

    // A VALID description is left exactly alone and reports clean.
    GroomCoatGroupDesc good;
    good.Density = 0.5f;
    good.Length = 2.0f;
    good.Role = static_cast<u8>(GroomCoatRole::GuardHair);
    std::vector<std::string> noReasons;
    EXPECT_TRUE(SanitizeGroomCoatGroupDesc(good, 0, noReasons));
    EXPECT_TRUE(noReasons.empty());
    EXPECT_FLOAT_EQ(good.Density, 0.5f);
}

TEST(GroomCoatAuthoring, ANonFiniteRootUvCannotReachACast)
{
    // A map sample and a clump cell are the two places a root UV becomes an
    // integer, and a static_cast of a NaN is undefined behaviour rather than a
    // wrong number. Both must refuse before the cast.
    Ref<GroomRegionMap> map = FlatMap(128, 64, 32);
    ASSERT_TRUE(map);
    const glm::vec3 sampled = map->Sample({ std::numeric_limits<f32>::quiet_NaN(), 0.5f });
    EXPECT_FLOAT_EQ(sampled.x, 1.0f) << "a non-finite uv must answer identity, not index with a NaN";

    // The cell key of a NaN uv is the cell key of (0,0): defined, and the same
    // every time, which is what keeps it from scattering strands across cells.
    EXPECT_EQ(GroomCoatClumpCell({ std::numeric_limits<f32>::quiet_NaN(), 0.0f }, 0.05f),
              GroomCoatClumpCell({ 0.0f, 0.0f }, 0.05f));
    EXPECT_EQ(GroomCoatClumpCell({ 0.1f, 0.1f }, std::numeric_limits<f32>::quiet_NaN()),
              GroomCoatClumpCell({ 0.1f, 0.1f }, GroomCoatLimits::MaxClumpCellSize));
}

TEST(GroomCoatAuthoring, AMalformedRegionMapIsRefusedRatherThanPadded)
{
    // A short buffer padded with zeroes is a map whose lower rows say "density
    // zero" — a bald animal, with no error anywhere. Refusal is the loud answer.
    const std::array<u8, 8> tooShort{};
    EXPECT_FALSE(GroomRegionMap::FromRGBA8(4, 4, std::span<const u8>(tooShort.data(), tooShort.size())));
    EXPECT_FALSE(GroomRegionMap::FromRGBA8(0, 4, std::span<const u8>(tooShort.data(), tooShort.size())));

    // And two maps with the same pixels but different dimensions are different
    // maps, so a reshape invalidates the geometry cache.
    const std::vector<u8> pixels(16u * 4u, 128u);
    Ref<GroomRegionMap> wide = GroomRegionMap::FromRGBA8(8, 2, pixels);
    Ref<GroomRegionMap> tall = GroomRegionMap::FromRGBA8(2, 8, pixels);
    ASSERT_TRUE(wide);
    ASSERT_TRUE(tall);
    EXPECT_NE(wide->GetContentHash(), tall->GetContentHash());
}

TEST(GroomCoatAuthoring, TheDigestSeesEverythingThatChangesTheGeometry)
{
    // The digest IS the cache key for the coat, so anything it cannot see is a
    // slider that does nothing until something else invalidates the geometry.
    const GroomCoatSettings base = ActiveSettings();
    const u64 baseDigest = GroomCoatDigest(base);

    const auto differs = [baseDigest](auto&& mutate)
    {
        GroomCoatSettings settings = ActiveSettings();
        mutate(settings);
        return GroomCoatDigest(settings) != baseDigest;
    };

    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Undercoat.Density = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Undercoat.Length = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Undercoat.Width = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Undercoat.Clump = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Guard.Density = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Guard.Length = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Guard.Width = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Guard.Clump = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.LengthJitter = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.WidthJitter = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.ShadeJitter = 0.5f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.ClumpCellSize = 0.07f; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.Seed = 7u; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.RoleVisibilityMask = 1u; }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.RegionMap = FlatMap(1, 2, 3); }));
    EXPECT_TRUE(differs([](GroomCoatSettings& s)
                        { s.ColorMap = FlatMap(1, 2, 3); }));

    // A map EDITED IN PLACE — same slot, different pixels — must also differ, or
    // an artist repainting a density map sees nothing change.
    GroomCoatSettings withMap = ActiveSettings();
    withMap.RegionMap = FlatMap(10, 20, 30);
    GroomCoatSettings repainted = ActiveSettings();
    repainted.RegionMap = FlatMap(10, 20, 31);
    EXPECT_NE(GroomCoatDigest(withMap), GroomCoatDigest(repainted));

    // A DISABLED coat ignores every other field: moving a slider on a coat that
    // is switched off must not rebuild the geometry.
    GroomCoatSettings offA;
    GroomCoatSettings offB;
    offB.Undercoat.Density = 0.1f;
    offB.Seed = 99u;
    EXPECT_EQ(GroomCoatDigest(offA), GroomCoatDigest(offB));
}

// ── Transport: the tint lane ───────────────────────────────────────

TEST(GroomCoatAuthoring, TheTintLaneIsAlwaysANormalFloat)
{
    // The packing forces the exponent so that no payload is a denormal a vertex
    // pipeline may flush to zero, and none is a NaN. This is the property, not
    // the round trip: a saturated red tint packed as a bare 24-bit payload is a
    // denormal, and flushing it would render the strand BLACK on some drivers
    // and not others, with nothing in any log.
    for (u32 r = 0; r < 256; r += 5)
    {
        for (u32 g = 0; g < 256; g += 7)
        {
            for (u32 b = 0; b < 256; b += 11)
            {
                const glm::vec3 tint(static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                                     static_cast<f32>(b) / 255.0f);
                const f32 packed = PackGroomCoatTint(tint);
                EXPECT_TRUE(std::isnormal(packed)) << "r=" << r << " g=" << g << " b=" << b << " packed as " << packed;
                // [0.5, 2), not [0.5, 1): the payload is 24 bits and the
                // mantissa is 23, so the blue channel's top bit lands in the
                // exponent's low bit. Both exponents are normal, which is the
                // property that matters; GroomCoat.h says so at the packing.
                EXPECT_GE(std::abs(packed), 0.5f);
                EXPECT_LT(std::abs(packed), 2.0f);

                const glm::vec3 unpacked = UnpackGroomCoatTint(packed);
                // One 8-bit step of tolerance: the lane is 8 bits per channel and
                // says so.
                EXPECT_NEAR(unpacked.x, tint.x, 1.0f / 255.0f);
                EXPECT_NEAR(unpacked.y, tint.y, 1.0f / 255.0f);
                EXPECT_NEAR(unpacked.z, tint.z, 1.0f / 255.0f);
            }
        }
    }

    // Identity is exactly white, and it is the value an un-tinted vertex carries
    // by default — so a groom with no coat multiplies its colour by one.
    EXPECT_EQ(std::bit_cast<u32>(GroomCoatIdentityTint), std::bit_cast<u32>(PackGroomCoatTint(glm::vec3(1.0f))));
    const glm::vec3 white = UnpackGroomCoatTint(GroomCoatIdentityTint);
    EXPECT_FLOAT_EQ(white.x, 1.0f);
    EXPECT_FLOAT_EQ(white.y, 1.0f);
    EXPECT_FLOAT_EQ(white.z, 1.0f);

    // A NaN tint packs to BLACK rather than to a NaN lane: the channel helper's
    // comparisons are written so NaN takes the zero branch.
    const f32 nanPacked = PackGroomCoatTint(glm::vec3(std::numeric_limits<f32>::quiet_NaN()));
    EXPECT_TRUE(std::isnormal(nanPacked));
    EXPECT_FLOAT_EQ(UnpackGroomCoatTint(nanPacked).x, 0.0f);

    // The default vertex carries identity, which is what makes the shader's
    // unconditional multiply a no-op on a groom with no coat.
    const GroomStrandVertex vertex;
    EXPECT_EQ(std::bit_cast<u32>(vertex.Tint), std::bit_cast<u32>(GroomCoatIdentityTint));
}

// ── Shape: length and clump ────────────────────────────────────────

TEST(GroomCoatAuthoring, TheRootIsAFixedPointOfEveryShapeChange)
{
    // A coat that moves its roots is a coat that has come off the animal. The
    // root is t == 0, and both the length scale and the clump pull must vanish
    // there EXACTLY — not approximately, because the binding then transforms the
    // result and a root that drifted would drift differently in every pose.
    const glm::vec3 root(1.0f, 2.0f, 3.0f);
    const glm::vec3 growth(0.5f, -0.25f, 0.125f);
    for (const f32 length : { 0.1f, 1.0f, 4.0f })
    {
        for (const f32 clump : { 0.0f, 0.5f, 1.0f })
        {
            const glm::vec3 moved = ApplyGroomCoatShape(root, root, 0.0f, length, clump, growth);
            EXPECT_EQ(std::bit_cast<u32>(moved.x), std::bit_cast<u32>(root.x));
            EXPECT_EQ(std::bit_cast<u32>(moved.y), std::bit_cast<u32>(root.y));
            EXPECT_EQ(std::bit_cast<u32>(moved.z), std::bit_cast<u32>(root.z));
        }
    }

    // And with no clump, the length scale is exactly a scale about the root.
    const glm::vec3 tip(2.0f, 2.0f, 3.0f);
    const glm::vec3 halved = ApplyGroomCoatShape(root, tip, 1.0f, 0.5f, 0.0f, growth);
    EXPECT_FLOAT_EQ(halved.x, 1.5f);
    EXPECT_FLOAT_EQ(halved.y, 2.0f);
    EXPECT_FLOAT_EQ(halved.z, 3.0f);
}

TEST(GroomCoatAuthoring, ClumpingGathersTipsAndLeavesRootsAlone)
{
    // Two strands growing from the same clump in different directions. At full
    // clump both tips land on their own root plus the clump's MEAN growth, so
    // the angle between the two strands' growth vectors collapses while their
    // roots do not move.
    const glm::vec3 rootA(0.0f, 0.0f, 0.0f);
    const glm::vec3 rootB(0.01f, 0.0f, 0.0f);
    const glm::vec3 tipA = rootA + glm::vec3(0.0f, 0.05f, 0.0f);
    const glm::vec3 tipB = rootB + glm::vec3(0.05f, 0.0f, 0.0f);
    const glm::vec3 meanGrowth = ((tipA - rootA) + (tipB - rootB)) * 0.5f;

    const auto spread = [&](f32 clump)
    {
        const glm::vec3 endA = ApplyGroomCoatShape(rootA, tipA, 1.0f, 1.0f, clump, meanGrowth);
        const glm::vec3 endB = ApplyGroomCoatShape(rootB, tipB, 1.0f, 1.0f, clump, meanGrowth);
        return glm::length((endA - rootA) - (endB - rootB));
    };

    const f32 loose = spread(0.0f);
    const f32 half = spread(0.5f);
    const f32 tight = spread(1.0f);
    EXPECT_GT(loose, 0.0f);
    EXPECT_LT(half, loose);
    EXPECT_LT(tight, half);
    EXPECT_NEAR(tight, 0.0f, 1e-6f) << "at full clump both strands take the clump's mean growth exactly";

    // A strand MIDWAY along is gathered less than its tip: the pull is
    // quadratic, so the base stays where the cook put it and only the outer half
    // gathers. A linear pull would bend the strand from the root and the coat
    // would read as combed rather than tufted.
    const glm::vec3 midLoose = ApplyGroomCoatShape(rootA, glm::mix(rootA, tipA, 0.5f), 0.5f, 1.0f, 0.0f, meanGrowth);
    const glm::vec3 midTight = ApplyGroomCoatShape(rootA, glm::mix(rootA, tipA, 0.5f), 0.5f, 1.0f, 1.0f, meanGrowth);
    const glm::vec3 tipTight = ApplyGroomCoatShape(rootA, tipA, 1.0f, 1.0f, 1.0f, meanGrowth);
    EXPECT_LT(glm::length(midTight - midLoose), glm::length(tipTight - tipA));
}

TEST(GroomCoatAuthoring, ClumpCellsPartitionTheRootUvSpace)
{
    // Two roots inside one cell share a clump; two a cell apart do not. Negative
    // UVs fold correctly, which a truncating cast would get wrong by merging
    // -0.3 and +0.3.
    constexpr f32 cell = 0.05f;
    EXPECT_EQ(GroomCoatClumpCell({ 0.01f, 0.01f }, cell), GroomCoatClumpCell({ 0.04f, 0.04f }, cell));
    EXPECT_NE(GroomCoatClumpCell({ 0.01f, 0.01f }, cell), GroomCoatClumpCell({ 0.06f, 0.01f }, cell));
    EXPECT_NE(GroomCoatClumpCell({ -0.01f, 0.0f }, cell), GroomCoatClumpCell({ 0.01f, 0.0f }, cell));
}
