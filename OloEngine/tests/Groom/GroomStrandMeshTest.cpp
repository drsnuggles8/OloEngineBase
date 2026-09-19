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
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

#include <bit>
#include <cstddef>
#include <cstring>
#include <set>
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
    EXPECT_EQ(offsetof(GroomStrandVertex, Pad1), 60u);
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
