#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomStrandMeshTest — issue #1246.
//
// The CPU half of the strand geometry: the ribbon quad's construction, the
// budget, and the segment identity. None of it needs a GPU, and all of it is
// the kind of thing that fails silently — a bowtie quad, a budget that takes a
// prefix instead of a stride, or two strands sharing a hash identity all
// produce a picture that looks like hair and is wrong.
//
// The vertex LAYOUT is pinned here too, because on Vulkan there is no vertex
// input state at all (ADR 0011 §5): GroomStrand.glsl indexes the same bytes as
// a flat float array with a hard-coded stride of 16, so a seventeenth float in
// the struct reads every strand's data at the wrong offset — on one backend
// only, with no compile error anywhere. (The stride was 12 until #1249 added
// the previous-frame centreline; both sides moved together.)
// =============================================================================

#include <gtest/gtest.h>

#include "GroomStrandFixture.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomShadowWidening.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace OloEngine;

namespace
{
    // A groom of `count` straight two-point strands, each one unit long, so
    // the expected geometry is countable by hand.
    Ref<GroomAsset> MakeStraightGroom(u32 count, u32 pointsPerCurve = 2u, u32 guideStride = 0u)
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("straight", group, reason)) << reason;

        std::vector<glm::vec3> points(pointsPerCurve);
        std::vector<f32> widths(pointsPerCurve, 0.001f);

        for (u32 c = 0; c < count; ++c)
        {
            for (u32 p = 0; p < pointsPerCurve; ++p)
            {
                points[p] = { static_cast<f32>(c) * 0.01f, static_cast<f32>(p), 0.0f };
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { 0.0f, 0.0f };
            input.GroupId = group;
            input.IsGuide = (guideStride != 0u) && ((c % guideStride) == 0u);
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }
} // namespace

// ── The layout contract ─────────────────────────────────────────────────────

TEST(GroomStrandMesh, VertexIsExactlySixteenFloats)
{
    // The static_assert in the header is the real guard; this states the same
    // fact where a reader looking for the Vulkan pull's stride will find it.
    static_assert(sizeof(GroomStrandVertex) == 16u * sizeof(f32));
    EXPECT_EQ(sizeof(GroomStrandVertex), 64u);
    // No padding holes either: the struct is memcpy'd into a vertex buffer and
    // read back as a flat float array on the Vulkan arm.
    EXPECT_EQ(offsetof(GroomStrandVertex, Position), 0u);
    EXPECT_EQ(offsetof(GroomStrandVertex, Other), 12u);
    EXPECT_EQ(offsetof(GroomStrandVertex, Side), 24u);
    EXPECT_EQ(offsetof(GroomStrandVertex, Radius), 28u);
    EXPECT_EQ(offsetof(GroomStrandVertex, Coords), 32u);
    EXPECT_EQ(offsetof(GroomStrandVertex, SegmentId), 40u);
    EXPECT_EQ(offsetof(GroomStrandVertex, Tint), 44u);
    // #1249: the previous-frame centreline, at float 12. The offsets above are
    // unchanged on purpose — the four floats were APPENDED, so the Vulkan
    // pull's existing reads all still land where they did and only the stride
    // moved.
    EXPECT_EQ(offsetof(GroomStrandVertex, PrevPosition), 48u);
    // #1558: the last float, padding until then, is a card's coverage growth --
    // the Vulkan pull's v[base + 15].
    EXPECT_EQ(offsetof(GroomStrandVertex, CoverageGrowth), 60u);
}

TEST(GroomStrandMesh, AnUnboundGroomWritesPreviousPositionEqualToPosition)
{
    // The widening claim in GroomStrandVertex::PrevPosition, as a test: an
    // unbound groom must emit EXACTLY the velocity it emitted before #1249,
    // which means bit-for-bit equal positions rather than approximately equal
    // ones. Anything else would make every committed #1246 capture
    // incomparable with the one beside it.
    Ref<GroomAsset> groom = MakeStraightGroom(8u, 4u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, GroomStrandBuildSettings{}, vertices, indices);
    ASSERT_GT(stats.SegmentCount, 0u);

    for (const auto& vertex : vertices)
    {
        EXPECT_EQ(std::bit_cast<u32>(vertex.PrevPosition.x), std::bit_cast<u32>(vertex.Position.x));
        EXPECT_EQ(std::bit_cast<u32>(vertex.PrevPosition.y), std::bit_cast<u32>(vertex.Position.y));
        EXPECT_EQ(std::bit_cast<u32>(vertex.PrevPosition.z), std::bit_cast<u32>(vertex.Position.z));
    }
    EXPECT_EQ(stats.StrandsHeldAtRest, 0u) << "an unbound groom holds nothing at rest";
    EXPECT_TRUE(stats.BoundsValid);
}

// ── The quad ────────────────────────────────────────────────────────────────

TEST(GroomStrandMesh, OneSegmentBecomesOneQuadOfTwoTriangles)
{
    const auto groom = MakeStraightGroom(1u, 2u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, {}, vertices, indices);

    EXPECT_EQ(stats.SegmentCount, 1u);
    EXPECT_EQ(stats.VertexCount, 4u);
    EXPECT_EQ(stats.IndexCount, 6u);
    ASSERT_EQ(vertices.size(), 4u);
    ASSERT_EQ(indices.size(), 6u);
    EXPECT_EQ(stats.VertexBytes, 4u * sizeof(GroomStrandVertex));
    EXPECT_EQ(stats.IndexBytes, 6u * sizeof(u32));

    // Two triangles over four corners, sharing an edge — not two independent
    // triangles, which would be six distinct vertices.
    const std::vector<u32> expected{ 0u, 1u, 2u, 0u, 2u, 3u };
    EXPECT_EQ(indices, expected);
}

TEST(GroomStrandMesh, AllFourCornersCarryTheSameTangentSoTheQuadIsNotABowtie)
{
    // THE defect this encoding exists to prevent. If `Other` held "the other
    // end of my segment", the two corners at P1 would see the tangent
    // reversed, widen the opposite way, and every quad would be an hourglass —
    // which renders as hair, just thinner and with a pinch at every joint.
    const auto groom = MakeStraightGroom(1u, 2u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    ASSERT_EQ(BuildGroomStrandMesh(*groom, {}, vertices, indices).SegmentCount, 1u);
    ASSERT_EQ(vertices.size(), 4u);

    const glm::vec3 tangent0 = vertices[0].Other - vertices[0].Position;
    for (sizet i = 1; i < vertices.size(); ++i)
    {
        const glm::vec3 tangent = vertices[i].Other - vertices[i].Position;
        EXPECT_NEAR(tangent.x, tangent0.x, 1.0e-6f) << "corner " << i;
        EXPECT_NEAR(tangent.y, tangent0.y, 1.0e-6f) << "corner " << i;
        EXPECT_NEAR(tangent.z, tangent0.z, 1.0e-6f) << "corner " << i;
    }
    EXPECT_GT(glm::length(tangent0), 0.0f) << "the segment has no direction at all";

    // Corners 0 and 1 sit at the root, 2 and 3 at the tip; the sides alternate
    // so the ring 0-1-2-3 walks the quad's perimeter rather than crossing it.
    EXPECT_EQ(vertices[0].Position, vertices[1].Position);
    EXPECT_EQ(vertices[2].Position, vertices[3].Position);
    EXPECT_FLOAT_EQ(vertices[0].Side, -1.0f);
    EXPECT_FLOAT_EQ(vertices[1].Side, 1.0f);
    EXPECT_FLOAT_EQ(vertices[2].Side, 1.0f);
    EXPECT_FLOAT_EQ(vertices[3].Side, -1.0f);
    // Coords.y is the geometric side and must agree with Side, or the fragment
    // shader's across-ribbon coordinate runs the other way from the geometry.
    for (sizet i = 0; i < vertices.size(); ++i)
    {
        EXPECT_FLOAT_EQ(vertices[i].Coords.y, vertices[i].Side) << "corner " << i;
    }
}

TEST(GroomStrandMesh, WidthsAreHalvedExactlyOnceBecauseTheCookStoresDiameters)
{
    GroomBuilder builder;
    std::string reason;
    u16 group = 0;
    ASSERT_TRUE(builder.AddGroup("g", group, reason)) << reason;

    const std::vector<glm::vec3> points{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const std::vector<f32> widths{ 0.004f, 0.002f }; // DIAMETERS
    GroomCurveInput input;
    input.Points = points;
    input.Widths = widths;
    input.GroupId = group;
    ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
    Ref<GroomAsset> groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    ASSERT_EQ(BuildGroomStrandMesh(*groom, {}, vertices, indices).SegmentCount, 1u);
    ASSERT_EQ(vertices.size(), 4u);

    // Radii, not diameters. A second halving downstream would make every coat
    // exactly half as thick as authored, which looks like a plausible groom.
    EXPECT_FLOAT_EQ(vertices[0].Radius, 0.002f);
    EXPECT_FLOAT_EQ(vertices[1].Radius, 0.002f);
    EXPECT_FLOAT_EQ(vertices[2].Radius, 0.001f);
    EXPECT_FLOAT_EQ(vertices[3].Radius, 0.001f);
}

TEST(GroomStrandMesh, RootToTipParameterRunsFromZeroToOne)
{
    const auto groom = MakeStraightGroom(1u, 5u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    ASSERT_EQ(BuildGroomStrandMesh(*groom, {}, vertices, indices).SegmentCount, 4u);

    // The first corner of the first segment is the root; the last corner of
    // the last segment is the tip. A groom imported tip-first reads inverted
    // here exactly as it does in the debug preview, which is the point.
    EXPECT_FLOAT_EQ(vertices.front().Coords.x, 0.0f);
    EXPECT_FLOAT_EQ(vertices.back().Coords.x, 1.0f);
}

// ── Segment identity ────────────────────────────────────────────────────────

TEST(GroomStrandMesh, EverySegmentGetsADistinctStableIdentity)
{
    // Two strands sharing an id would share the stochastic mode's decision
    // wherever they overlapped, which reads as a coat with correlated holes
    // rather than as noise — a defect that looks like a shading artefact and
    // is really an identity one.
    const auto groom = MakeStraightGroom(64u, 5u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, {}, vertices, indices);
    ASSERT_EQ(stats.SegmentCount, 64u * 4u);

    std::set<u32> identities;
    for (sizet i = 0; i < vertices.size(); i += 4u)
    {
        const u32 id = std::bit_cast<u32>(vertices[i].SegmentId);
        // All four corners of one quad must agree, or the flat-interpolated
        // varying depends on which provoking vertex the driver picked.
        for (sizet corner = 0; corner < 4u; ++corner)
        {
            EXPECT_EQ(std::bit_cast<u32>(vertices[i + corner].SegmentId), id) << "quad at " << i;
        }
        identities.insert(id);
    }
    EXPECT_EQ(identities.size(), stats.SegmentCount) << "two segments collided on one identity";
}

TEST(GroomStrandMesh, TheIdentityMatchesTheOneTheCoverageModelUses)
{
    // Three places must agree on this number: the CPU coverage model, the
    // vertex data, and therefore the shader. Asserting the mesh against the
    // shared helper is what stops the vertex builder drifting on its own.
    const auto groom = MakeStraightGroom(3u, 3u);
    ASSERT_TRUE(groom);

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    ASSERT_EQ(BuildGroomStrandMesh(*groom, {}, vertices, indices).SegmentCount, 6u);

    sizet vertex = 0;
    for (u32 curve = 0; curve < 3u; ++curve)
    {
        for (u32 segment = 0; segment < 2u; ++segment)
        {
            EXPECT_EQ(std::bit_cast<u32>(vertices[vertex].SegmentId), GroomSegmentIdentity(curve, segment))
                << "curve " << curve << " segment " << segment;
            vertex += 4u;
        }
    }
}

// ── The budget ──────────────────────────────────────────────────────────────

TEST(GroomStrandMesh, TheStrandBudgetStridesOverTheGroomRatherThanTakingItsFirstStrands)
{
    // A prefix of a COOKED groom is one contiguous range of curves, and the
    // cook sorts curves by group — so "the first N" is one side of the animal.
    // The visible result is a bald flank, which reads as a broken import
    // rather than as a budget.
    const auto groom = MakeStraightGroom(100u, 2u);
    ASSERT_TRUE(groom);

    GroomStrandBuildSettings settings;
    settings.MaxStrands = 10u;

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, settings, vertices, indices);

    EXPECT_EQ(stats.StrandsAvailable, 100u);
    EXPECT_LE(stats.StrandsSelected, 10u);
    EXPECT_EQ(stats.Stride, 10u);
    EXPECT_EQ(stats.SegmentCount, stats.StrandsSelected);

    // The selection SPANS the groom: the strands are laid out along +X at
    // 0.01 spacing, so a prefix would put every root under x = 0.1 and a
    // stride puts the last one near x = 0.9.
    f32 maxX = 0.0f;
    for (const auto& vertex : vertices)
    {
        maxX = std::max(maxX, vertex.Position.x);
    }
    EXPECT_GT(maxX, 0.5f) << "the budget took a prefix of the groom, not a stride over it";
}

TEST(GroomStrandMesh, TheSegmentBudgetIsEnforcedExactlyAndReported)
{
    // The stride is derived from the AVERAGE strand length, so a groom whose
    // long strands land on stride-aligned indices overshoots it. The cap is
    // enforced at emission and the overshoot is reported rather than silent.
    const auto groom = MakeStraightGroom(50u, 9u); // 8 segments each, 400 total
    ASSERT_TRUE(groom);

    GroomStrandBuildSettings settings;
    settings.MaxStrands = 50u;
    settings.MaxSegments = 100u;

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, settings, vertices, indices);

    EXPECT_LE(stats.SegmentCount, 100u);
    EXPECT_TRUE(stats.SegmentBudgetLimited)
        << "a groom held back by the segment budget did not say so, so raising Max Strands and seeing no "
           "change would look like a bug";
    EXPECT_EQ(stats.VertexCount, stats.SegmentCount * 4u);
    EXPECT_EQ(stats.IndexCount, stats.SegmentCount * 6u);
}

TEST(GroomStrandMesh, GuidesOnlyStridesOverTheGuidesRatherThanTheWholeGroom)
{
    const auto groom = MakeStraightGroom(200u, 2u, 10u); // every 10th is a guide
    ASSERT_TRUE(groom);

    GroomStrandBuildSettings settings;
    settings.GuidesOnly = true;

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, settings, vertices, indices);

    EXPECT_EQ(stats.StrandsAvailable, 20u) << "the available count is over the GUIDES, not the whole groom";
    EXPECT_EQ(stats.StrandsSelected, 20u);
    EXPECT_EQ(stats.SegmentCount, 20u);
}

TEST(GroomStrandMesh, AGuidesOnlyViewOfAGroomWithNoGuidesIsEmptyRatherThanEverything)
{
    const auto groom = MakeStraightGroom(50u, 2u, 0u); // no guides at all
    ASSERT_TRUE(groom);

    GroomStrandBuildSettings settings;
    settings.GuidesOnly = true;

    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats stats = BuildGroomStrandMesh(*groom, settings, vertices, indices);

    EXPECT_EQ(stats.StrandsAvailable, 0u);
    EXPECT_EQ(stats.SegmentCount, 0u);
    EXPECT_TRUE(vertices.empty());
    EXPECT_TRUE(indices.empty());
}

// ── Plan agrees with build ──────────────────────────────────────────────────

TEST(GroomStrandMesh, ThePlanMatchesWhatTheBuildProduces)
{
    // The editor's inspector shows the PLAN on every frame and the pass
    // BUILDS. If the two disagree, the panel confidently reports a coat that
    // is not the one on screen.
    const auto coat = Tests::GroomStrandFixture::MakeScalp(500u, 8u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    for (const u32 budget : { 10u, 137u, 500u, 5000u })
    {
        GroomStrandBuildSettings settings;
        settings.MaxStrands = budget;

        const GroomStrandMeshStats plan = PlanGroomStrandMesh(*coat.Groom, settings);
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        const GroomStrandMeshStats built = BuildGroomStrandMesh(*coat.Groom, settings, vertices, indices);

        EXPECT_EQ(plan.Stride, built.Stride) << "budget " << budget;
        EXPECT_EQ(plan.StrandsAvailable, built.StrandsAvailable) << "budget " << budget;
        EXPECT_EQ(plan.SegmentCount, built.SegmentCount) << "budget " << budget;
        EXPECT_EQ(plan.VertexBytes, built.VertexBytes) << "budget " << budget;
    }
}

TEST(GroomStrandMesh, ThePlanMatchesTheBuildWhenOneCurveStraddlesTheSegmentCap)
{
    // The case the MaxStrands sweep above cannot reach: the budget falls in
    // the MIDDLE of a curve. The build truncates there and says
    // SegmentBudgetLimited; a plan that instead refused the whole curve would
    // report 0 segments and 0 MiB, with no warning, while the pass built five
    // — and the inspector shows the PLAN on every frame, so that is a panel
    // confidently describing a coat that is not the one on screen.
    const auto groom = MakeStraightGroom(1u, 11u); // one curve, 10 segments
    ASSERT_TRUE(groom);

    GroomStrandBuildSettings settings;
    settings.MaxSegments = 5u;

    const GroomStrandMeshStats plan = PlanGroomStrandMesh(*groom, settings);
    std::vector<GroomStrandVertex> vertices;
    std::vector<u32> indices;
    const GroomStrandMeshStats built = BuildGroomStrandMesh(*groom, settings, vertices, indices);

    EXPECT_EQ(built.SegmentCount, 5u);
    EXPECT_EQ(plan.SegmentCount, built.SegmentCount);
    EXPECT_EQ(plan.VertexBytes, built.VertexBytes);
    EXPECT_TRUE(plan.SegmentBudgetLimited)
        << "the plan truncated a curve without saying the budget did it";
    EXPECT_TRUE(built.SegmentBudgetLimited);
    EXPECT_EQ(vertices.size(), 5u * 4u);
}

TEST(GroomStrandMesh, TheBuildIsPureSoACacheMayKeyOnTheGroomAndTheSettings)
{
    const auto coat = Tests::GroomStrandFixture::MakePelt(300u, 4u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    GroomStrandBuildSettings settings;
    settings.MaxStrands = 97u;

    std::vector<GroomStrandVertex> firstVertices;
    std::vector<u32> firstIndices;
    const GroomStrandMeshStats first = BuildGroomStrandMesh(*coat.Groom, settings, firstVertices, firstIndices);

    std::vector<GroomStrandVertex> secondVertices;
    std::vector<u32> secondIndices;
    const GroomStrandMeshStats second = BuildGroomStrandMesh(*coat.Groom, settings, secondVertices, secondIndices);

    EXPECT_EQ(first, second);
    ASSERT_EQ(firstVertices.size(), secondVertices.size());
    EXPECT_EQ(firstIndices, secondIndices);
    EXPECT_EQ(std::memcmp(firstVertices.data(), secondVertices.data(),
                          firstVertices.size() * sizeof(GroomStrandVertex)),
              0)
        << "two builds of the same groom differed, so GroomRenderPass's cache key is unsound";
}

// ── The shadow caster's order (#1533 E1) ────────────────────────────────────
//
// A shadow view whose width floor draws every strand several times its true
// width casts a PREFIX of a hashed strand order. Everything that makes that
// sound is CPU-side: each strand whole, every strand once, a prefix that is a
// uniform share of the coat and not a periodic one, and the mean radius the
// fraction divides by weighted by length.

namespace
{
    // Curve c has 2 + (c % 5) points, so strand boundaries are irregular and a
    // boundary off by one segment shows.
    Ref<GroomAsset> MakeRaggedGroom(u32 count)
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("ragged", group, reason)) << reason;
        for (u32 c = 0; c < count; ++c)
        {
            const u32 pointCount = 2u + (c % 5u);
            std::vector<glm::vec3> points(pointCount);
            std::vector<f32> widths(pointCount, 0.001f);
            for (u32 p = 0; p < pointCount; ++p)
            {
                points[p] = { static_cast<f32>(c) * 0.01f, static_cast<f32>(p) * 0.1f, 0.0f };
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { 0.0f, 0.0f };
            input.GroupId = group;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    struct StreamWithStrands
    {
        std::vector<GroomStrandVertex> Vertices;
        std::vector<u32> Indices;
        std::vector<u32> StrandFirstIndex;
    };

    StreamWithStrands BuildWithStrands(const GroomAsset& groom, std::vector<GroomCasterStrand>* casterStrands = nullptr)
    {
        StreamWithStrands stream;
        (void)BuildGroomStrandMesh(GroomBuildSource::FromAsset(groom), GroomStrandBuildSettings{}, stream.Vertices,
                                   stream.Indices, nullptr, nullptr, nullptr, &stream.StrandFirstIndex, casterStrands);
        return stream;
    }

    // The strand each emitted segment belongs to, by segment ordinal.
    std::vector<u32> SegmentStrands(const StreamWithStrands& stream)
    {
        std::vector<u32> strands(stream.Indices.size() / 6u, 0u);
        for (sizet strand = 0; strand < stream.StrandFirstIndex.size(); ++strand)
        {
            const sizet end = strand + 1u < stream.StrandFirstIndex.size() ? stream.StrandFirstIndex[strand + 1u]
                                                                           : stream.Indices.size();
            for (sizet index = stream.StrandFirstIndex[strand]; index < end; index += 6u)
            {
                strands[index / 6u] = static_cast<u32>(strand);
            }
        }
        return strands;
    }

    // `count` straight strands in `groups` groups: curve c is in group c % groups,
    // points along axis (group % 3) with 1 + (c % 3) segments, and is
    // 1 mm * (1 + group) across -- so each group has its own direction and width.
    Ref<GroomAsset> MakeGroupedGroom(u32 count, u32 groups)
    {
        GroomBuilder builder;
        std::string reason;
        std::vector<u16> ids(groups, 0u);
        for (u32 g = 0; g < groups; ++g)
        {
            EXPECT_TRUE(builder.AddGroup("group" + std::to_string(g), ids[g], reason)) << reason;
        }
        for (u32 c = 0; c < count; ++c)
        {
            const u32 g = c % groups;
            glm::vec3 axis(0.0f);
            axis[static_cast<glm::length_t>(g % 3u)] = 1.0f;
            const u32 pointCount = 2u + (c % 3u);
            std::vector<glm::vec3> points(pointCount);
            std::vector<f32> widths(pointCount, 0.001f * static_cast<f32>(1u + g));
            const glm::vec3 root{ 0.01f * static_cast<f32>(c), 0.0f, 0.02f * static_cast<f32>(g) };
            for (u32 p = 0; p < pointCount; ++p)
            {
                points[p] = root + axis * (0.05f * static_cast<f32>(p));
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { 0.0f, 0.0f };
            input.GroupId = ids[g];
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    // The strands of a caster order, in the order it casts them.
    std::vector<u32> CastStrandSequence(const GroomCasterOrder& order, const std::vector<u32>& segmentStrands)
    {
        std::vector<u32> sequence;
        for (sizet index = 0; index < order.Indices.size(); index += 6u)
        {
            const u32 strand = segmentStrands[order.Indices[index] / 4u];
            if (sequence.empty() || sequence.back() != strand)
            {
                sequence.push_back(strand);
            }
        }
        return sequence;
    }
} // namespace

TEST(GroomStrandMesh, EachStrandsIndicesStartWhereTheBuildSaysTheyDo)
{
    const auto groom = MakeRaggedGroom(40u);
    ASSERT_TRUE(groom);
    const StreamWithStrands stream = BuildWithStrands(*groom);

    ASSERT_EQ(stream.StrandFirstIndex.size(), 40u);
    u32 expected = 0;
    for (u32 strand = 0; strand < 40u; ++strand)
    {
        EXPECT_EQ(stream.StrandFirstIndex[strand], expected) << "strand " << strand;
        // The first index of a strand is its FIRST segment's first corner.
        const GroomStrandVertex& corner = stream.Vertices[stream.Indices[expected]];
        EXPECT_EQ(std::bit_cast<u32>(corner.SegmentId), GroomSegmentIdentity(strand, 0u)) << "strand " << strand;
        expected += 6u * (1u + (strand % 5u));
    }
    EXPECT_EQ(expected, stream.Indices.size());
}

TEST(GroomStrandMesh, TheCasterOrderCastsEveryStrandWholeAndOnceInAHashedOrder)
{
    const auto groom = MakeRaggedGroom(300u);
    ASSERT_TRUE(groom);
    const StreamWithStrands stream = BuildWithStrands(*groom);
    const GroomCasterOrder order = BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex);
    ASSERT_EQ(order.Indices.size(), stream.Indices.size());
    // Without strand summaries: the whole stream, one run.
    ASSERT_EQ(order.Runs.size(), 1u);
    EXPECT_FALSE(order.Runs[0].MomentsKnown) << "no summaries, no moments: the share is assumed, not bounded";
    const auto& prefix = order.Runs[0].Prefix;

    // Each segment keeps its own six indices, and each strand's segments stay
    // together, in their own order, complete.
    const std::vector<u32> segmentStrands = SegmentStrands(stream);
    std::vector<u32> castCount(300u, 0u);
    u32 previousStrand = ~0u;
    u32 nextSegment = 0u;
    for (sizet index = 0; index < order.Indices.size(); index += 6u)
    {
        const u32 base = order.Indices[index];
        ASSERT_EQ(base % 4u, 0u);
        for (u32 k = 0; k < 6u; ++k)
        {
            ASSERT_EQ(order.Indices[index + k], stream.Indices[(base / 4u) * 6u + k]) << "a segment was re-cut";
        }
        const u32 strand = segmentStrands[base / 4u];
        if (strand != previousStrand)
        {
            if (previousStrand != ~0u)
            {
                EXPECT_EQ(nextSegment, 1u + (previousStrand % 5u)) << "strand " << previousStrand << " was split";
            }
            ++castCount[strand];
            previousStrand = strand;
            nextSegment = 0u;
        }
        EXPECT_EQ(base / 4u, stream.StrandFirstIndex[strand] / 6u + nextSegment) << "out of order in a strand";
        ++nextSegment;
    }
    EXPECT_EQ(nextSegment, 1u + (previousStrand % 5u)) << "the last strand was cut short";
    for (u32 strand = 0; strand < 300u; ++strand)
    {
        EXPECT_EQ(castCount[strand], 1u) << "strand " << strand << " cast " << castCount[strand] << " times";
    }

    // Hashed: not the cooked order, and the same order every build.
    const std::vector<u32> sequence = CastStrandSequence(order, segmentStrands);
    std::vector<u32> identity(300u);
    for (u32 strand = 0; strand < 300u; ++strand)
    {
        identity[strand] = strand;
    }
    EXPECT_NE(sequence, identity);
    EXPECT_EQ(BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex).Indices, order.Indices);

    // Level j holds exactly the first ceil(j * 300 / 64) strands of that order.
    EXPECT_EQ(prefix[0], 0u);
    EXPECT_EQ(prefix[kGroomCasterPrefixLevels], order.Indices.size());
    for (u32 level = 1; level <= kGroomCasterPrefixLevels; ++level)
    {
        const u32 strands = (level * 300u + kGroomCasterPrefixLevels - 1u) / kGroomCasterPrefixLevels;
        u32 indexCount = 0;
        for (u32 k = 0; k < strands; ++k)
        {
            indexCount += 6u * (1u + (sequence[k] % 5u));
        }
        EXPECT_EQ(prefix[level], indexCount) << "level " << level;
    }
}

// With the build's strand summaries the order is one RUN per group (#1533):
// each run holds exactly its group's strands, in a hashed order of its own, with
// a prefix table of its own -- so a view can cast a share of each group from
// that group's own density, width and direction.
TEST(GroomStrandMesh, TheCasterRunsAreOnePerGroupEachAHashedShareOfItsOwnStrands)
{
    const auto groom = MakeGroupedGroom(300u, 3u);
    ASSERT_TRUE(groom);
    std::vector<GroomCasterStrand> summaries;
    const StreamWithStrands stream = BuildWithStrands(*groom, &summaries);
    ASSERT_EQ(stream.StrandFirstIndex.size(), 300u);
    ASSERT_EQ(summaries.size(), 300u) << "one summary per emitted strand";
    const GroomCasterOrder order =
        BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex, summaries);
    ASSERT_EQ(order.Indices.size(), stream.Indices.size());
    ASSERT_EQ(order.Runs.size(), 3u);

    const std::vector<u32> segmentStrands = SegmentStrands(stream);
    u32 expectedFirst = 0;
    for (u32 r = 0; r < 3u; ++r)
    {
        const GroomCasterRun& run = order.Runs[r];
        EXPECT_EQ(run.FirstIndex, expectedFirst) << "run " << r << ": the runs cover the order end to end";
        EXPECT_EQ(run.Strands, 100u) << "run " << r;
        EXPECT_TRUE(run.MomentsKnown) << "run " << r;

        // The run's strands, in its order: each of its group's strands, once.
        std::vector<u32> sequence;
        for (u32 index = run.FirstIndex; index < run.FirstIndex + run.Prefix[kGroomCasterPrefixLevels]; index += 6u)
        {
            const u32 strand = segmentStrands[order.Indices[index] / 4u];
            if (sequence.empty() || sequence.back() != strand)
            {
                sequence.push_back(strand);
            }
        }
        ASSERT_EQ(sequence.size(), 100u) << "run " << r;
        const u32 group = sequence.front() % 3u;
        EXPECT_EQ(run.Group, summaries[sequence.front()].Group) << "run " << r;
        for (const u32 strand : sequence)
        {
            EXPECT_EQ(strand % 3u, group) << "run " << r << " holds strand " << strand << " of another group";
        }
        EXPECT_FALSE(std::ranges::is_sorted(sequence)) << "run " << r << " is in the cooked order, not a hashed one";

        // Level j holds exactly the first ceil(j * 100 / 64) of ITS strands.
        EXPECT_EQ(run.Prefix[0], 0u);
        for (u32 level = 1; level <= kGroomCasterPrefixLevels; ++level)
        {
            const u32 strands = (level * 100u + kGroomCasterPrefixLevels - 1u) / kGroomCasterPrefixLevels;
            u32 indexCount = 0;
            for (u32 k = 0; k < strands; ++k)
            {
                indexCount += 6u * (1u + (sequence[k] % 3u));
            }
            EXPECT_EQ(run.Prefix[level], indexCount) << "run " << r << " level " << level;
        }

        // Its own width and its own direction. Every strand of the group lies
        // along one axis, so its moments are its length on that axis and
        // nothing across it: along the axis it projects to nothing, across it
        // to the whole of its length.
        EXPECT_NEAR(run.MeanRadius, 0.0005f * static_cast<f32>(1u + group), 1.0e-8f) << "run " << r;
        glm::vec3 along(0.0f);
        along[static_cast<glm::length_t>(group)] = 1.0f;
        glm::vec3 across(0.0f);
        across[static_cast<glm::length_t>((group + 1u) % 3u)] = 1.0f;
        EXPECT_NEAR(GroomShadowProjectedLengthLowerBound(run.TotalLength, run.Moments, along), 0.0f,
                    1.0e-5f * run.TotalLength)
            << "run " << r;
        EXPECT_NEAR(GroomShadowProjectedLengthLowerBound(run.TotalLength, run.Moments, across), run.TotalLength,
                    1.0e-5f * run.TotalLength)
            << "run " << r;

        // Its box holds every point of every one of its strands.
        for (const u32 strand : sequence)
        {
            const sizet end = strand + 1u < stream.StrandFirstIndex.size() ? stream.StrandFirstIndex[strand + 1u]
                                                                           : stream.Indices.size();
            for (sizet index = stream.StrandFirstIndex[strand]; index < end; ++index)
            {
                const glm::vec3& p = stream.Vertices[stream.Indices[index]].Position;
                EXPECT_TRUE(glm::all(glm::greaterThanEqual(p, run.BoundsMin)) &&
                            glm::all(glm::lessThanEqual(p, run.BoundsMax)))
                    << "run " << r << " strand " << strand;
            }
        }
        expectedFirst += run.Prefix[kGroomCasterPrefixLevels];
    }
    EXPECT_EQ(expectedFirst, order.Indices.size());
    EXPECT_EQ(BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex, summaries).Indices,
              order.Indices)
        << "the same stream gives the same order";
}

// A groom with more groups than a view should draw runs for is split by coat
// role instead -- with no coat, one run -- and keeps its moments.
TEST(GroomStrandMesh, ACasterOrderWithMoreGroupsThanItHasRunsForSplitsByRoleInstead)
{
    for (const u32 groups : { kGroomCasterMaxRuns, kGroomCasterMaxRuns + 1u })
    {
        const auto groom = MakeGroupedGroom(groups * 2u, groups);
        ASSERT_TRUE(groom);
        std::vector<GroomCasterStrand> summaries;
        const StreamWithStrands stream = BuildWithStrands(*groom, &summaries);
        const GroomCasterOrder order =
            BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex, summaries);
        ASSERT_EQ(order.Indices.size(), stream.Indices.size()) << groups << " groups";
        if (groups <= kGroomCasterMaxRuns)
        {
            EXPECT_EQ(order.Runs.size(), groups) << "one run per group, up to the cap";
        }
        else
        {
            ASSERT_EQ(order.Runs.size(), 1u) << "past the cap: one run per ROLE, and with no coat every role is 0";
            EXPECT_TRUE(order.Runs[0].MomentsKnown);
            EXPECT_EQ(order.Runs[0].Strands, groups * 2u);
        }
    }
}

// The decision a view makes for a run (#1533), at its edges: what cannot be
// measured is cast whole, and a dense run lying across the light thins.
TEST(GroomStrandMesh, ACasterRunWithNothingToMeasureIsCastWholeAndADenseOneThins)
{
    GroomCasterRun run;
    for (u32 level = 0; level <= kGroomCasterPrefixLevels; ++level)
    {
        run.Prefix[level] = 6u * level * 100u;
    }
    run.Strands = 6400u;
    run.MeanRadius = 5.0e-5f;
    run.TotalLength = 6400.0f * 0.03f; // 3 cm strands
    run.MomentsKnown = true;
    run.Moments = { run.TotalLength, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // all along +x
    run.BoundsMin = glm::vec3(0.0f);
    run.BoundsMax = glm::vec3(0.1f, 0.03f, 0.1f);
    const u32 whole = run.Prefix[kGroomCasterPrefixLevels];

    // A sun straight down a 16 m cascade at 4096^2: a 4 mm texel, a 0.1 mm
    // strand widened about fortyfold.
    GroomCasterView view;
    view.ViewProjection = glm::ortho(-8.0f, 8.0f, -8.0f, 8.0f, 0.1f, 100.0f) *
                          glm::lookAt(glm::vec3(0.05f, 20.0f, 0.05f), glm::vec3(0.05f, 0.0f, 0.05f),
                                      glm::vec3(1.0f, 0.0f, 0.0f));
    view.ResolutionTexels = 4096.0f;
    GroomCasterPlacement caster;
    caster.CullMin = run.BoundsMin;
    caster.CullMax = run.BoundsMax;

    // Across the light: 192 m of strand over a 10 cm square, the dense case.
    const GroomCasterRunDecision dense = DecideGroomCasterRun(run, caster, view);
    EXPECT_NEAR(dense.ProjectedLength, run.TotalLength, 1.0e-3f * run.TotalLength) << "strands across the light";
    EXPECT_GT(dense.Layers, 4.0f * kGroomCasterMinLayers);
    EXPECT_LT(dense.Fraction, 1.0f);
    EXPECT_LT(dense.IndexCount, whole);
    EXPECT_EQ(dense.IndexCount, GroomCasterIndexCount(run.Prefix, dense.Fraction));

    // The same strands standing ALONG the light project to nothing: nothing to
    // estimate layers from, so the run is cast whole -- where the half share
    // would have credited them with half their length.
    GroomCasterRun standing = run;
    standing.Moments = { 0.0f, run.TotalLength, 0.0f, 0.0f, 0.0f, 0.0f };
    const GroomCasterRunDecision upright = DecideGroomCasterRun(standing, caster, view);
    EXPECT_NEAR(upright.ProjectedLength, 0.0f, 1.0e-3f * run.TotalLength);
    EXPECT_EQ(upright.IndexCount, whole);
    GroomCasterRun assumed = standing;
    assumed.MomentsKnown = false;
    EXPECT_LT(DecideGroomCasterRun(assumed, caster, view).IndexCount, whole)
        << "without moments the half share is assumed, and it thins strands that cast dots";

    // No cull box: nothing to measure the run against.
    GroomCasterPlacement unbounded = caster;
    unbounded.CullMin = glm::vec3(std::numeric_limits<f32>::max());
    unbounded.CullMax = glm::vec3(std::numeric_limits<f32>::lowest());
    EXPECT_EQ(DecideGroomCasterRun(run, unbounded, view).IndexCount, whole);

    // A perspective view whose light sits inside the box: texels down to nothing.
    GroomCasterView spot = view;
    spot.ViewProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.01f, 50.0f) *
                          glm::lookAt(glm::vec3(0.05f, 0.01f, 0.05f), glm::vec3(0.05f, -1.0f, 0.05f),
                                      glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(DecideGroomCasterRun(run, caster, spot).IndexCount, whole);

    // A run with no strands draws nothing.
    EXPECT_EQ(DecideGroomCasterRun(GroomCasterRun{}, caster, view).IndexCount, 0u);
}

TEST(GroomStrandMesh, APrefixOfTheCasterOrderIsAUniformShareAndNotAPeriodicOne)
{
    // 4096 strands, a quarter cast. A STRIDED order (every 4th strand) would put
    // the whole quarter in four of the sixteen residues mod 16 -- the aliasing
    // with "N children per guide" the hash exists to avoid -- and a PREFIX of
    // the cooked order would put it all in the first quarter of the groom.
    const auto groom = MakeStraightGroom(4096u);
    ASSERT_TRUE(groom);
    const StreamWithStrands stream = BuildWithStrands(*groom);
    const GroomCasterOrder order = BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex);
    ASSERT_EQ(order.Runs.size(), 1u);
    const u32 count = GroomCasterIndexCount(order.Runs[0].Prefix, 0.25f);
    ASSERT_EQ(count, 1024u * 6u) << "a quarter of 4096 one-segment strands";

    std::array<u32, 8> perBlock{};
    std::array<u32, 16> perResidue{};
    for (u32 index = 0; index < count; index += 6u)
    {
        const u32 strand = order.Indices[index] / 4u; // one segment a strand
        ++perBlock[strand / 512u];
        ++perResidue[strand % 16u];
    }
    // Binomial: 128 +/- 9.8 a block and 64 +/- 6.9 a residue; the bounds are
    // over three and a half deviations wide, and the hash is fixed, so this is
    // deterministic.
    for (u32 block = 0; block < 8u; ++block)
    {
        EXPECT_GE(perBlock[block], 94u) << "block " << block;
        EXPECT_LE(perBlock[block], 162u) << "block " << block;
    }
    for (u32 residue = 0; residue < 16u; ++residue)
    {
        EXPECT_GE(perResidue[residue], 40u) << "residue " << residue;
        EXPECT_LE(perResidue[residue], 88u) << "residue " << residue;
    }
}

TEST(GroomStrandMesh, TheCasterMeanRadiusIsWeightedByLength)
{
    // Two one-segment strands: 1 m at a radius of 1e-4, and 3 m running from
    // 1e-4 to 3e-4 (2e-4 on average). Weighted by length: (1 * 1 + 3 * 2) / 4.
    const auto quad = [](glm::vec3 p0, glm::vec3 p1, f32 radius0, f32 radius1)
    {
        std::array<GroomStrandVertex, 4> corners{};
        for (u32 k = 0; k < 4u; ++k)
        {
            const bool atP0 = k < 2u;
            corners[k].Position = atP0 ? p0 : p1;
            corners[k].Other = (atP0 ? p0 : p1) + (p1 - p0);
            corners[k].Radius = atP0 ? radius0 : radius1;
        }
        return corners;
    };
    std::vector<GroomStrandVertex> vertices;
    for (const auto& corners :
         { quad({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 1.0e-4f, 1.0e-4f),
           quad({ 0.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, 1.0e-4f, 3.0e-4f) })
    {
        vertices.insert(vertices.end(), corners.begin(), corners.end());
    }
    const std::vector<u32> indices{ 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    const std::vector<u32> firsts{ 0u, 6u };

    const GroomCasterOrder order = BuildGroomCasterOrder(vertices, indices, firsts);
    EXPECT_NEAR(order.MeanRadius, 1.75e-4f, 1.0e-9f)
        << "an unweighted mean (1.5e-4) lets short strands speak for the area long ones cover";
    EXPECT_NEAR(order.TotalLength, 4.0f, 1.0e-6f) << "the drawn length the strand-layer floor divides";
}

TEST(GroomStrandMesh, ACasterCountRoundsUpToTheNextSixtyFourthAndNeverToNothing)
{
    std::array<u32, kGroomCasterPrefixLevels + 1> prefix{};
    for (u32 level = 0; level <= kGroomCasterPrefixLevels; ++level)
    {
        prefix[level] = 6u * level;
    }
    EXPECT_EQ(GroomCasterIndexCount(prefix, 0.25f), 96u);
    EXPECT_EQ(GroomCasterIndexCount(prefix, 0.26f), 102u) << "16.64 sixty-fourths round UP: never fewer strands";
    EXPECT_EQ(GroomCasterIndexCount(prefix, 0.0f), 6u) << "a subset never rounds to nothing";
    EXPECT_EQ(GroomCasterIndexCount(prefix, 1.0f), 384u);
    EXPECT_EQ(GroomCasterIndexCount(prefix, 1.5f), 384u);
    EXPECT_EQ(GroomCasterIndexCount(prefix, std::numeric_limits<f32>::quiet_NaN()), 384u)
        << "NaN casts the whole coat rather than a garbage share of it";
    EXPECT_EQ(GroomCasterIndexCount(std::span<const u32>(prefix.data(), 10u), 0.5f), 0u)
        << "a table of the wrong size is not read";
}

TEST(GroomStrandMesh, AStrandTableThatDoesNotDescribeTheStreamGivesNoOrder)
{
    const auto groom = MakeRaggedGroom(10u);
    ASSERT_TRUE(groom);
    StreamWithStrands stream = BuildWithStrands(*groom);

    EXPECT_TRUE(BuildGroomCasterOrder(stream.Vertices, stream.Indices, {}).Indices.empty());

    std::vector<u32> pastTheEnd = stream.StrandFirstIndex;
    pastTheEnd.back() = static_cast<u32>(stream.Indices.size()) + 6u;
    const GroomCasterOrder broken = BuildGroomCasterOrder(stream.Vertices, stream.Indices, pastTheEnd);
    EXPECT_TRUE(broken.Indices.empty()) << "a caster order of something else would cast the wrong coat";
    EXPECT_TRUE(broken.Runs.empty());

    // Summaries for some other number of strands are not trusted either: the
    // order is the whole stream as one run, with no moments to bound by.
    std::vector<GroomCasterStrand> tooFew(stream.StrandFirstIndex.size() - 1u);
    const GroomCasterOrder unsummarised =
        BuildGroomCasterOrder(stream.Vertices, stream.Indices, stream.StrandFirstIndex, tooFew);
    ASSERT_EQ(unsummarised.Runs.size(), 1u);
    EXPECT_FALSE(unsummarised.Runs[0].MomentsKnown);

    std::vector<u32> descending = stream.StrandFirstIndex;
    std::swap(descending[2], descending[3]);
    EXPECT_TRUE(BuildGroomCasterOrder(stream.Vertices, stream.Indices, descending).Indices.empty());
}
