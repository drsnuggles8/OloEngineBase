#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomBindingBuilderTest — issue #1249, acceptance criterion 1.
//
// "Binding assets identify roots on the body with explicit topology/version
//  checks and deterministic rebind behavior."
//
// Three separate claims, and this file is one section per claim:
//
//   IDENTIFY   the closest triangle really is the closest one, and the
//              barycentric really is a point on it. Tested against a grid whose
//              geometry is known by hand, so the expected answers are numbers
//              rather than the builder's own output.
//
//   CHECK      a binding refuses a groom or a body it was not built for, by
//              NAME. Every refusal is a different reason, because "it did not
//              attach" is not something anyone can act on.
//
//   DETERMINE  binding the same pair twice produces byte-identical records.
//              The interesting case is a TIE — a root equidistant from two
//              triangles, which happens on every shared edge of a closed mesh —
//              and the tie-break is the triangle index rather than whatever the
//              grid visited first.
// =============================================================================

#include <gtest/gtest.h>

#include "GroomBindingFixture.h"

#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBuilder.h"

#include <glm/glm.hpp>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomBindingTest;

namespace
{
    [[nodiscard]] Ref<GroomAsset> MakeSingleStrandAt(const glm::vec3& root)
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("one", group, reason)) << reason;

        const std::vector<glm::vec3> points{ root, root + glm::vec3(0.0f, 0.05f, 0.0f) };
        const std::vector<f32> widths{ 0.001f, 0.001f };
        GroomCurveInput input;
        input.Points = points;
        input.Widths = widths;
        input.RootUV = { 0.0f, 0.0f };
        input.GroupId = group;
        EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }
} // namespace

// ── IDENTIFY ────────────────────────────────────────────────────────────────

TEST(GroomBindingBuilder, ClosestPointInsideATriangleIsInteriorAndReproducesThePoint)
{
    const glm::vec3 v0{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 v1{ 1.0f, 0.0f, 0.0f };
    const glm::vec3 v2{ 0.0f, 0.0f, 1.0f };

    glm::vec3 barycentric{ 0.0f };
    bool interior = false;
    // Directly above the triangle's middle, one unit up.
    const f32 distanceSquared = GroomBindingBuilder::ClosestPointOnTriangle({ 0.25f, 1.0f, 0.25f }, v0, v1, v2,
                                                                            barycentric, interior);
    EXPECT_TRUE(interior);
    EXPECT_NEAR(distanceSquared, 1.0f, 1.0e-5f);
    EXPECT_NEAR(barycentric.x + barycentric.y + barycentric.z, 1.0f, 1.0e-5f);

    const glm::vec3 reconstructed = v0 * barycentric.x + v1 * barycentric.y + v2 * barycentric.z;
    EXPECT_NEAR(reconstructed.x, 0.25f, 1.0e-5f);
    EXPECT_NEAR(reconstructed.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(reconstructed.z, 0.25f, 1.0e-5f);
}

TEST(GroomBindingBuilder, ClosestPointBeyondAnEdgeIsNotInterior)
{
    // The case a "project onto the plane and clamp" implementation gets wrong.
    // The point projects OUTSIDE the triangle, so the true closest point is on
    // an edge and `interior` must say so — that distinction is exactly what
    // separates GroomRootBindQuality::Exact from Clamped.
    const glm::vec3 v0{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 v1{ 1.0f, 0.0f, 0.0f };
    const glm::vec3 v2{ 0.0f, 0.0f, 1.0f };

    glm::vec3 barycentric{ 0.0f };
    bool interior = false;
    const f32 distanceSquared =
        GroomBindingBuilder::ClosestPointOnTriangle({ 2.0f, 0.0f, 0.0f }, v0, v1, v2, barycentric, interior);
    EXPECT_FALSE(interior);
    EXPECT_NEAR(distanceSquared, 1.0f, 1.0e-5f); // (2,0,0) to (1,0,0)
    EXPECT_NEAR(barycentric.y, 1.0f, 1.0e-5f);   // the whole weight is on v1
}

TEST(GroomBindingBuilder, ARootOnTheSurfaceBindsExactlyWhereItSits)
{
    GridSurface grid = MakeGrid(4u);
    // Deliberately NOT (0.3, 0, 0.7): on a 4-division grid that lands exactly on
    // a cell's diagonal, which is a triangle EDGE, so the projection is
    // legitimately Clamped rather than Exact. It is a real case and it gets its
    // own test below; this one is about the interior.
    Ref<GroomAsset> groom = MakeSingleStrandAt({ 0.3f, 0.0f, 0.55f });
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;
    ASSERT_EQ(binding->GetRootCount(), 1u);

    const GroomRootBinding& record = binding->GetRoot(0);
    EXPECT_EQ(record.Quality, static_cast<u32>(GroomRootBindQuality::Exact));
    EXPECT_NEAR(record.RestDistance, 0.0f, 1.0e-5f);
    EXPECT_NEAR(record.RestOrigin.x, 0.3f, 1.0e-5f);
    EXPECT_NEAR(record.RestOrigin.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(record.RestOrigin.z, 0.55f, 1.0e-5f);

    // The frame's z axis is the geometric normal, which for this grid is +Y.
    const glm::vec3 normal = record.RestRotation * glm::vec3(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(normal.x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(normal.y, 1.0f, 1.0e-4f);
    EXPECT_NEAR(normal.z, 0.0f, 1.0e-4f);

    EXPECT_EQ(stats.RootsExact, 1u);
    EXPECT_EQ(stats.RootsDistant, 0u);
    EXPECT_EQ(stats.RootsOnDegenerateTriangles, 0u);
}

TEST(GroomBindingBuilder, ARootOnATriangleEdgeIsBoundButCountedAsClamped)
{
    // A root on a shared edge is not a corner case, it is arithmetic: on a
    // 4-division grid, (0.3, 0, 0.7) is exactly on a cell's diagonal. The frame
    // is still that triangle's and the strand still deforms correctly — what
    // differs is the QUALITY, and reporting it as Exact would make the editor's
    // "N exact" readout a number that cannot distinguish a coat sitting in the
    // middle of its faces from one sitting along their seams.
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeSingleStrandAt({ 0.3f, 0.0f, 0.7f });
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    EXPECT_EQ(binding->GetRoot(0).Quality, static_cast<u32>(GroomRootBindQuality::Clamped));
    EXPECT_EQ(stats.RootsClamped, 1u);
    EXPECT_EQ(stats.RootsDistant, 0u) << "an edge root is on the body, just not inside a face";
    EXPECT_NEAR(binding->GetRoot(0).RestDistance, 0.0f, 1.0e-5f);
}

TEST(GroomBindingBuilder, ARootFarFromTheBodyIsBoundButCountedAsDistant)
{
    // The wrong-body case, and the reason it is COUNTED rather than dropped:
    // dropping it would leave a bald patch, which reads as a broken import.
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeSingleStrandAt({ 0.5f, 5.0f, 0.5f });
    ASSERT_TRUE(groom);

    GroomBindingBuildSettings settings;
    settings.SearchRadius = 0.25f;

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", settings, binding, stats, reason)) << reason;

    EXPECT_EQ(binding->GetRoot(0).Quality, static_cast<u32>(GroomRootBindQuality::Distant));
    EXPECT_EQ(stats.RootsDistant, 1u);
    EXPECT_EQ(stats.RootsExact, 0u);
    EXPECT_NEAR(stats.MaxRestDistance, 5.0f, 1.0e-3f);
    EXPECT_EQ(binding->GetQualityCount(GroomRootBindQuality::Distant), 1u);
}

TEST(GroomBindingBuilder, EveryCurveGetsARecordSoTheArraysStayParallel)
{
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeCoat(37u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    EXPECT_EQ(binding->GetRootCount(), groom->GetCurveCount());
    EXPECT_EQ(stats.RootsBound, groom->GetCurveCount());
    EXPECT_EQ(binding->GetSourceSignature().CurveCount, groom->GetCurveCount());
    EXPECT_EQ(binding->GetSourceSignature().GuideCount, groom->GetGuideCount());
}

// ── DETERMINE ───────────────────────────────────────────────────────────────

TEST(GroomBindingBuilder, BindingTheSamePairTwiceProducesIdenticalRecords)
{
    GridSurface grid = MakeGrid(6u);
    Ref<GroomAsset> groom = MakeCoat(64u, 4u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> first;
    Ref<GroomBindingAsset> second;
    GroomBindingBuildStats firstStats;
    GroomBindingBuildStats secondStats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, first,
                                           firstStats, reason))
        << reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, second,
                                           secondStats, reason))
        << reason;

    ASSERT_EQ(first->GetRootCount(), second->GetRootCount());
    EXPECT_EQ(firstStats, secondStats);
    // memcmp over the whole array, not a field-by-field walk: the records go to
    // disk as a block, so byte equality is the property that matters and a
    // field comparison would miss a difference in the padding that also ships.
    EXPECT_EQ(0, std::memcmp(first->GetRoots().data(), second->GetRoots().data(),
                             first->GetRoots().size() * sizeof(GroomRootBinding)));
}

TEST(GroomBindingBuilder, AGridResolutionChangeDoesNotChangeTheResult)
{
    // The grid is a build-time ACCELERATOR. If changing its cell size changed
    // which triangle won, the binding would depend on a performance knob — and
    // the tie-break would be traversal order, which is the determinism leak the
    // header names first.
    GridSurface grid = MakeGrid(6u);
    Ref<GroomAsset> groom = MakeCoat(48u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> coarse;
    Ref<GroomBindingAsset> fine;
    GroomBindingBuildStats stats;
    std::string reason;

    GroomBindingBuildSettings coarseSettings;
    coarseSettings.GridResolution = 2u;
    GroomBindingBuildSettings fineSettings;
    fineSettings.GridResolution = 64u;

    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", coarseSettings, coarse, stats, reason))
        << reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", fineSettings, fine, stats, reason))
        << reason;

    ASSERT_EQ(coarse->GetRootCount(), fine->GetRootCount());
    EXPECT_EQ(0, std::memcmp(coarse->GetRoots().data(), fine->GetRoots().data(),
                             coarse->GetRoots().size() * sizeof(GroomRootBinding)));
}

TEST(GroomBindingBuilder, ATieBetweenTwoTrianglesGoesToTheLowerIndex)
{
    // A root directly over a shared edge is equidistant from both triangles
    // that own it. This happens on every interior edge of every closed mesh, so
    // it is the common case rather than a corner one, and the tie-break has to
    // be a property of the DATA rather than of the search.
    GridSurface grid = MakeGrid(1u); // two triangles sharing the diagonal
    ASSERT_EQ(grid.Indices.size(), 6u);

    // The grid's diagonal runs from (1,0,0) to (0,0,1); its midpoint is
    // (0.5, 0, 0.5), which both triangles contain.
    Ref<GroomAsset> groom = MakeSingleStrandAt({ 0.5f, 0.0f, 0.5f });
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;
    EXPECT_EQ(binding->GetRoot(0).TriangleIndex, 0u)
        << "an equidistant tie must resolve to the lower triangle index, not to visit order";
}

TEST(GroomBindingBuilder, ATieIsBrokenByIndexEvenWhenTheTwoTrianglesAreInDifferentGridCells)
{
    // The same tie, moved ACROSS a cell boundary — and this is the case the
    // first implementation got wrong. Keeping only the strictly-closer
    // candidate makes "first seen wins", which inside one cell is "lowest index
    // wins" (the lists are index-sorted) and across cells is "whichever cell the
    // shell walk reached first". That made the cooked bytes depend on
    // GridResolution, which is a performance knob.
    //
    // A fine grid over a dense mesh puts the two triangles sharing an edge in
    // different cells, so the same root must still resolve to the lower index at
    // every resolution.
    GridSurface grid = MakeGrid(8u);
    // Dead centre of the grid, which is a shared vertex of four cells and lies
    // exactly on two triangles' diagonals.
    Ref<GroomAsset> groom = MakeSingleStrandAt({ 0.5f, 0.0f, 0.5f });
    ASSERT_TRUE(groom);

    u32 winner = 0;
    bool first = true;
    for (const u32 resolution : { 1u, 2u, 4u, 16u, 64u, 200u })
    {
        GroomBindingBuildSettings settings;
        settings.GridResolution = resolution;

        Ref<GroomBindingAsset> binding;
        GroomBindingBuildStats stats;
        std::string reason;
        ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", settings, binding, stats, reason))
            << reason;

        const u32 triangle = binding->GetRoot(0).TriangleIndex;
        if (first)
        {
            winner = triangle;
            first = false;
        }
        EXPECT_EQ(triangle, winner) << "grid resolution " << resolution << " picked a different triangle, so the "
                                                                           "cooked bytes depend on a performance knob";
    }

    // ...and the winner is the LOWEST-indexed of the triangles that actually
    // touch the point, not merely a stable arbitrary one. Found by brute force
    // against every triangle, so the expectation is independent of the search.
    const GroomSurfaceView view = grid.View();
    u32 expected = 0;
    f32 bestDistanceSquared = std::numeric_limits<f32>::max();
    for (u32 triangle = 0; triangle < view.TriangleCount(); ++triangle)
    {
        const glm::uvec3 corners = view.TriangleIndices(triangle);
        glm::vec3 barycentric{ 0.0f };
        bool interior = false;
        const f32 distanceSquared = GroomBindingBuilder::ClosestPointOnTriangle(
            { 0.5f, 0.0f, 0.5f }, view.Position(corners.x), view.Position(corners.y), view.Position(corners.z),
            barycentric, interior);
        if (distanceSquared < bestDistanceSquared)
        {
            bestDistanceSquared = distanceSquared;
            expected = triangle;
        }
    }
    EXPECT_EQ(winner, expected);
}

// ── CHECK ───────────────────────────────────────────────────────────────────

TEST(GroomBindingBuilder, ATargetWhoseIndicesOverrunItsVerticesIsRefused)
{
    GridSurface grid = MakeGrid(2u);
    grid.Indices[0] = grid.VertexCount() + 5u; // corrupt
    Ref<GroomAsset> groom = MakeCoat(4u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    EXPECT_FALSE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                            reason));
    EXPECT_FALSE(binding) << "a failed build must leave the caller's handle untouched";
    EXPECT_GT(stats.TrianglesOutOfRange, 0u);
    EXPECT_FALSE(reason.empty());
}

TEST(GroomBindingBuilder, AnEmptySurfaceIsRefusedByName)
{
    GridSurface grid;
    Ref<GroomAsset> groom = MakeCoat(4u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    EXPECT_FALSE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                            reason));
    EXPECT_NE(reason.find("not usable"), std::string::npos) << reason;
}

TEST(GroomBindingBuilder, ANonPositiveSearchRadiusIsRefused)
{
    GridSurface grid = MakeGrid(2u);
    Ref<GroomAsset> groom = MakeCoat(4u, 3u);
    ASSERT_TRUE(groom);

    GroomBindingBuildSettings settings;
    settings.SearchRadius = 0.0f;

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    EXPECT_FALSE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", settings, binding, stats, reason));
    EXPECT_FALSE(reason.empty());
}

TEST(GroomBindingBuilder, ABindingRefusesAGroomWhoseRootsMoved)
{
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> original = MakeCoat(8u, 3u);
    Ref<GroomAsset> moved = MakeCoat(8u, 3u, 0.1f, 0.25f); // same count, roots lifted
    ASSERT_TRUE(original);
    ASSERT_TRUE(moved);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*original, grid.View(), "Grid", GroomBindingBuildSettings{}, binding,
                                           stats, reason))
        << reason;

    const auto targetSignature = GroomBindingBuilder::SignTarget(grid.View());
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*original), targetSignature),
              GroomBindingRejectReason::None);
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*moved), targetSignature),
              GroomBindingRejectReason::SourceSignatureMismatch);
}

TEST(GroomBindingBuilder, ABindingRefusesAGroomWithADifferentCurveCount)
{
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> eight = MakeCoat(8u, 3u);
    Ref<GroomAsset> nine = MakeCoat(9u, 3u);
    ASSERT_TRUE(eight);
    ASSERT_TRUE(nine);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*eight, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*nine),
                                          GroomBindingBuilder::SignTarget(grid.View())),
              GroomBindingRejectReason::RootCountMismatch);
}

TEST(GroomBindingBuilder, ABindingRefusesARetriangulatedBodyEvenAtTheSameVertexCount)
{
    // The failure this check exists for: same counts, different connectivity.
    // A binding addresses triangles by INDEX, so a re-indexed mesh silently
    // moves every root to a different part of the body — a coat that looks
    // authored and is wrong.
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeCoat(8u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    GridSurface retriangulated = grid;
    // Flip the diagonal of the first quad: identical vertices, identical
    // counts, different triangles.
    std::swap(retriangulated.Indices[1], retriangulated.Indices[2]);

    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*groom),
                                          GroomBindingBuilder::SignTarget(retriangulated.View())),
              GroomBindingRejectReason::TargetTopologyMismatch);
}

TEST(GroomBindingBuilder, ABindingRefusesARerigggedBody)
{
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeCoat(8u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(2u, 0x1234u), "Grid", GroomBindingBuildSettings{},
                                           binding, stats, reason))
        << reason;

    // Same geometry, different skeleton identity: the palette the runtime
    // indexes is ordered, so a re-rig is a different deformation through the
    // same triangles.
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*groom),
                                          GroomBindingBuilder::SignTarget(grid.View(2u, 0x5678u))),
              GroomBindingRejectReason::TargetTopologyMismatch);
    // ...and a different bone COUNT too.
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*groom),
                                          GroomBindingBuilder::SignTarget(grid.View(3u, 0x1234u))),
              GroomBindingRejectReason::TargetTopologyMismatch);
}

TEST(GroomBindingBuilder, ABinderVersionMismatchIsRefusedSeparatelyFromEverythingElse)
{
    // A binding built by an older binder is refused with "rebuild it" rather
    // than migrated. It is checked FIRST, before any signature, because such a
    // binding may disagree about every other field for reasons that have
    // nothing to do with the assets in front of it.
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeCoat(8u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;
    EXPECT_EQ(binding->GetBinderVersion(), kGroomBinderVersion);

    // Reached through the serializer in GroomBindingRoundTripTest, where a file
    // can carry an arbitrary version; here the contract is simply that the
    // current binder stamps the current version and that a matching pair
    // attaches.
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*groom),
                                          GroomBindingBuilder::SignTarget(grid.View())),
              GroomBindingRejectReason::None);
}

TEST(GroomBindingBuilder, ATargetThatIsNotReadyIsDistinguishedFromOneThatMismatches)
{
    // Two zeros that must not read alike: a body that has not loaded yet is not
    // the same as a body that is the wrong one, and an editor showing
    // "TargetTopologyMismatch" for a mesh still streaming in sends the reader
    // to rebuild a binding that was never wrong.
    GridSurface grid = MakeGrid(4u);
    Ref<GroomAsset> groom = MakeCoat(8u, 3u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    GroomBindingTargetSignature empty;
    EXPECT_EQ(binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*groom), empty),
              GroomBindingRejectReason::TargetNotReady);
}

// ── The self-check the writer and the reader share ──────────────────────────

TEST(GroomBindingBuilder, AValidatedBindingSurvivesItsOwnValidator)
{
    GridSurface grid = MakeGrid(6u);
    Ref<GroomAsset> groom = MakeCoat(40u, 5u);
    ASSERT_TRUE(groom);

    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(), "Grid", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;

    std::string validationReason;
    EXPECT_TRUE(binding->Validate(validationReason)) << validationReason;
    EXPECT_EQ(binding->GetQualityCount(GroomRootBindQuality::Exact) +
                  binding->GetQualityCount(GroomRootBindQuality::Clamped) +
                  binding->GetQualityCount(GroomRootBindQuality::Distant),
              binding->GetRootCount());
}
