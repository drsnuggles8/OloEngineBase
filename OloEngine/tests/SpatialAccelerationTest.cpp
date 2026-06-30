#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// ============================================================================
// SpatialAccelerationTest — unit test for SceneSpatialIndex, the uniform-grid
// spatial acceleration structure backing dynamic scene proximity queries
// (issue #430). It pins the three query contracts (radius / AABB / nearest-N)
// and the rebuild/edge-case behaviour directly, without a Scene.
//
// The strongest cases are *property* checks: for randomly scattered points the
// grid query must return exactly the brute-force set, and that set must be
// invariant to the grid's cell size (the cell size is a performance knob, never
// a correctness one). A regression in the cell math (floor, bounding-cell
// range, boundary inclusivity) shows up as a mismatch against brute force.
// ============================================================================

#include "OloEngine/Scene/SpatialAcceleration.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace OloEngine;

namespace
{
    struct Point
    {
        UUID Id;
        glm::vec3 Pos;
    };

    // Deterministic scatter of `count` points in a cube of half-extent `extent`.
    // Seeded so a failure reproduces exactly.
    std::vector<Point> ScatterPoints(u32 count, f32 extent, u32 seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> dist(-extent, extent);
        std::vector<Point> points;
        points.reserve(count);
        for (u32 i = 0; i < count; ++i)
        {
            // UUIDs start at 1 — 0 is the "null" UUID elsewhere in the engine.
            points.push_back(Point{ UUID(static_cast<u64>(i) + 1),
                                    glm::vec3{ dist(rng), dist(rng), dist(rng) } });
        }
        return points;
    }

    std::vector<u64> ToSortedU64(std::vector<UUID> ids)
    {
        std::vector<u64> out;
        out.reserve(ids.size());
        for (UUID id : ids)
        {
            out.push_back(static_cast<u64>(id));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<u64> BruteForceRadius(const std::vector<Point>& points, const glm::vec3& center, f32 radius)
    {
        const f32 radiusSq = radius * radius;
        std::vector<u64> out;
        for (const Point& p : points)
        {
            if (radius >= 0.0f && glm::distance2(center, p.Pos) <= radiusSq)
            {
                out.push_back(static_cast<u64>(p.Id));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    SceneSpatialIndex BuildIndex(const std::vector<Point>& points, f32 cellSize)
    {
        SceneSpatialIndex index(cellSize);
        for (const Point& p : points)
        {
            index.Insert(p.Id, p.Pos);
        }
        return index;
    }
} // namespace

TEST(SpatialAccelerationTest, EmptyIndexReturnsNothing)
{
    SceneSpatialIndex index;
    EXPECT_EQ(index.GetEntityCount(), 0u);
    EXPECT_TRUE(index.QueryRadius({ 0, 0, 0 }, 100.0f).empty());
    EXPECT_TRUE(index.QueryAABB({ -1, -1, -1 }, { 1, 1, 1 }).empty());
    EXPECT_TRUE(index.NearestN({ 0, 0, 0 }, 5).empty());
}

TEST(SpatialAccelerationTest, GetEntityCountTracksInserts)
{
    SceneSpatialIndex index;
    index.Insert(UUID(1), { 0, 0, 0 });
    index.Insert(UUID(2), { 5, 0, 0 });
    EXPECT_EQ(index.GetEntityCount(), 2u);
    index.Clear();
    EXPECT_EQ(index.GetEntityCount(), 0u);
}

TEST(SpatialAccelerationTest, QueryRadiusFindsInsideExcludesOutside)
{
    SceneSpatialIndex index(10.0f);
    index.Insert(UUID(1), { 0, 0, 0 });   // at center
    index.Insert(UUID(2), { 3, 0, 0 });   // inside r=5
    index.Insert(UUID(3), { 0, 4, 0 });   // inside r=5
    index.Insert(UUID(4), { 100, 0, 0 }); // far outside

    const auto got = ToSortedU64(index.QueryRadius({ 0, 0, 0 }, 5.0f));
    EXPECT_EQ(got, (std::vector<u64>{ 1, 2, 3 }));
}

TEST(SpatialAccelerationTest, QueryRadiusBoundaryIsInclusive)
{
    SceneSpatialIndex index(10.0f);
    index.Insert(UUID(1), { 5, 0, 0 }); // exactly on the radius
    const auto got = index.QueryRadius({ 0, 0, 0 }, 5.0f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(static_cast<u64>(got[0]), 1u);
}

TEST(SpatialAccelerationTest, QueryRadiusNegativeRadiusReturnsNothing)
{
    SceneSpatialIndex index;
    index.Insert(UUID(1), { 0, 0, 0 });
    EXPECT_TRUE(index.QueryRadius({ 0, 0, 0 }, -1.0f).empty());
}

TEST(SpatialAccelerationTest, QueryAABBInclusiveBoundsAndExclusion)
{
    SceneSpatialIndex index(4.0f);
    index.Insert(UUID(1), { 0, 0, 0 });
    index.Insert(UUID(2), { 2, 2, 2 });    // inside [-3,3]^3
    index.Insert(UUID(3), { 3, 3, 3 });    // on the max corner (inclusive)
    index.Insert(UUID(4), { 3.1f, 0, 0 }); // just outside on x

    const auto got = ToSortedU64(index.QueryAABB({ -3, -3, -3 }, { 3, 3, 3 }));
    EXPECT_EQ(got, (std::vector<u64>{ 1, 2, 3 }));
}

TEST(SpatialAccelerationTest, QueryAABBInvertedBoxReturnsNothing)
{
    SceneSpatialIndex index;
    index.Insert(UUID(1), { 0, 0, 0 });
    // min > max on one axis → empty.
    EXPECT_TRUE(index.QueryAABB({ 1, -1, -1 }, { -1, 1, 1 }).empty());
}

TEST(SpatialAccelerationTest, NearestNReturnsClosestSortedByDistance)
{
    SceneSpatialIndex index(8.0f);
    index.Insert(UUID(10), { 1, 0, 0 });  // nearest
    index.Insert(UUID(20), { 0, 3, 0 });  // 2nd
    index.Insert(UUID(30), { 0, 0, 7 });  // 3rd
    index.Insert(UUID(40), { 50, 0, 0 }); // far

    const auto got = index.NearestN({ 0, 0, 0 }, 3);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(static_cast<u64>(got[0]), 10u);
    EXPECT_EQ(static_cast<u64>(got[1]), 20u);
    EXPECT_EQ(static_cast<u64>(got[2]), 30u);
}

TEST(SpatialAccelerationTest, NearestNRespectsMaxRadius)
{
    SceneSpatialIndex index(8.0f);
    index.Insert(UUID(1), { 2, 0, 0 });  // within 5
    index.Insert(UUID(2), { 20, 0, 0 }); // outside 5

    const auto got = index.NearestN({ 0, 0, 0 }, 5, 5.0f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(static_cast<u64>(got[0]), 1u);
}

TEST(SpatialAccelerationTest, NearestNCountClampsToSizeAndZeroIsEmpty)
{
    SceneSpatialIndex index;
    index.Insert(UUID(1), { 1, 0, 0 });
    index.Insert(UUID(2), { 2, 0, 0 });

    EXPECT_TRUE(index.NearestN({ 0, 0, 0 }, 0).empty());
    EXPECT_EQ(index.NearestN({ 0, 0, 0 }, 100).size(), 2u); // clamped to available
}

TEST(SpatialAccelerationTest, RejectsNonFinitePositions)
{
    SceneSpatialIndex index;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    index.Insert(UUID(1), { nan, 0, 0 });
    index.Insert(UUID(2), { 0, inf, 0 });
    index.Insert(UUID(3), { 0, 0, 0 }); // the only finite one

    EXPECT_EQ(index.GetEntityCount(), 1u);
    const auto got = index.QueryRadius({ 0, 0, 0 }, 1000.0f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(static_cast<u64>(got[0]), 3u);
}

TEST(SpatialAccelerationTest, SetCellSizeRehashesAndPreservesQueries)
{
    const auto points = ScatterPoints(200, 100.0f, 4242u);
    SceneSpatialIndex index = BuildIndex(points, 10.0f);

    const glm::vec3 center{ 5, -5, 10 };
    const f32 radius = 25.0f;
    const auto before = ToSortedU64(index.QueryRadius(center, radius));

    index.SetCellSize(37.0f); // arbitrary, different resolution
    EXPECT_EQ(index.GetEntityCount(), 200u);
    const auto after = ToSortedU64(index.QueryRadius(center, radius));

    EXPECT_EQ(before, after);
    EXPECT_EQ(before, BruteForceRadius(points, center, radius));
}

// The cell size is a performance knob — query *results* must be identical no
// matter the resolution, and must match the brute-force reference. Sweeps a
// few radii and several cell sizes over the same random cloud.
TEST(SpatialAccelerationTest, RadiusQueryMatchesBruteForceAcrossCellSizes)
{
    const auto points = ScatterPoints(500, 200.0f, 90909u);

    const std::vector<glm::vec3> centers = {
        { 0, 0, 0 }, { 50, -30, 20 }, { -120, 80, -60 }, { 300, 300, 300 }
    };
    const std::vector<f32> radii = { 0.0f, 5.0f, 25.0f, 75.0f, 500.0f };
    // Includes cellSize 1.0 paired with radius 500 — that combo's bounding box
    // is ~10^9 cells, which exercises the linear-scan fallback (without it the
    // query iterates a billion empty cells and times out under TSan; see #430).
    const std::vector<f32> cellSizes = { 1.0f, 10.0f, 33.0f, 100.0f, 250.0f };

    // Build the index once per cell size (it depends only on cell size), then
    // sweep every center/radius against it — the result must be invariant.
    // Building inside the innermost loop would rebuild it 100× for no reason.
    for (f32 cellSize : cellSizes)
    {
        SceneSpatialIndex index = BuildIndex(points, cellSize);
        for (const glm::vec3& center : centers)
        {
            for (f32 radius : radii)
            {
                EXPECT_EQ(ToSortedU64(index.QueryRadius(center, radius)),
                          BruteForceRadius(points, center, radius))
                    << "mismatch at center(" << center.x << "," << center.y << "," << center.z
                    << ") radius " << radius << " cellSize " << cellSize;
            }
        }
    }
}

TEST(SpatialAccelerationTest, NearestNMatchesBruteForceOrdering)
{
    const auto points = ScatterPoints(300, 150.0f, 7u);
    SceneSpatialIndex index = BuildIndex(points, 20.0f);

    const glm::vec3 center{ 10, 10, 10 };
    constexpr u32 kN = 7;

    // Brute-force the closest kN by (distance², UUID) — identical tiebreak to
    // SceneSpatialIndex::NearestN.
    std::vector<std::pair<f32, u64>> ranked;
    ranked.reserve(points.size());
    for (const Point& p : points)
    {
        ranked.emplace_back(glm::distance2(center, p.Pos), static_cast<u64>(p.Id));
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b)
              { return a.first != b.first ? a.first < b.first : a.second < b.second; });

    const auto got = index.NearestN(center, kN);
    ASSERT_EQ(got.size(), kN);
    for (u32 i = 0; i < kN; ++i)
    {
        EXPECT_EQ(static_cast<u64>(got[i]), ranked[i].second) << "rank " << i;
    }
}

// A non-finite query input must be rejected before any cell math runs (an
// integer cast of NaN/Inf is UB, and an Inf radius would floor to a near-
// infinite cell loop). Every query entry point returns empty, never hangs.
TEST(SpatialAccelerationTest, NonFiniteQueryInputsReturnEmpty)
{
    SceneSpatialIndex index(10.0f);
    index.Insert(UUID(1), { 0, 0, 0 });
    index.Insert(UUID(2), { 3, 4, 0 });

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    // QueryRadius: bad center, bad radius.
    EXPECT_TRUE(index.QueryRadius({ nan, 0, 0 }, 100.0f).empty());
    EXPECT_TRUE(index.QueryRadius({ 0, inf, 0 }, 100.0f).empty());
    EXPECT_TRUE(index.QueryRadius({ 0, 0, 0 }, inf).empty());
    EXPECT_TRUE(index.QueryRadius({ 0, 0, 0 }, nan).empty());

    // QueryAABB: bad bounds.
    EXPECT_TRUE(index.QueryAABB({ nan, -1, -1 }, { 1, 1, 1 }).empty());
    EXPECT_TRUE(index.QueryAABB({ -1, -1, -1 }, { 1, inf, 1 }).empty());

    // NearestN: bad center, bad maxRadius.
    EXPECT_TRUE(index.NearestN({ nan, 0, 0 }, 5).empty());
    EXPECT_TRUE(index.NearestN({ 0, 0, 0 }, 5, nan).empty());

    // A finite query still works after the rejected ones (no corrupted state).
    EXPECT_EQ(index.QueryRadius({ 0, 0, 0 }, 100.0f).size(), 2u);
}

// A literal +inf maxRadius means "search everything", same as the FLT_MAX
// sentinel default — it falls through to the unbounded flat scan, not the
// bounded cell walk (which would floor inf to a garbage range).
TEST(SpatialAccelerationTest, NearestNInfiniteRadiusSearchesAll)
{
    SceneSpatialIndex index(8.0f);
    index.Insert(UUID(1), { 2, 0, 0 });
    index.Insert(UUID(2), { 200, 0, 0 });
    index.Insert(UUID(3), { -50, 30, 10 });

    const f32 inf = std::numeric_limits<f32>::infinity();
    EXPECT_EQ(index.NearestN({ 0, 0, 0 }, 10, inf).size(), 3u);
    // Nearest is still correctly ranked.
    const auto got = index.NearestN({ 0, 0, 0 }, 1, inf);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(static_cast<u64>(got[0]), 1u);
}

// Rebuild many times while entities roam across many distinct cells. Beyond
// correctness, this exercises the Clear()-drops-dead-cell-keys path (an earlier
// Clear left empty keys behind, growing m_Cells without bound).
TEST(SpatialAccelerationTest, RepeatedRebuildStaysCorrectAsEntitiesRoam)
{
    SceneSpatialIndex index(5.0f);
    for (u32 tick = 0; tick < 50; ++tick)
    {
        index.Clear();
        // Two entities march far across the world each tick → new cells. Entity
        // 2 keeps a large constant offset so it never lands within entity 1's
        // query radius (they'd otherwise coincide at the origin at tick 0).
        const f32 t = static_cast<f32>(tick);
        index.Insert(UUID(1), { t * 13.0f, 0, 0 });
        index.Insert(UUID(2), { -500.0f - t * 7.0f, t * 3.0f, 50.0f });

        EXPECT_EQ(index.GetEntityCount(), 2u);

        // The two entities are always far apart → exactly two occupied cells.
        // This is the regression guard for Clear(): if it freed the per-cell
        // lists but leaked their (empty) keys, GetCellCount() would climb tick
        // after tick (toward 100+) while GetEntityCount() stayed at 2. A correct
        // rebuild never holds more cells than entries.
        EXPECT_EQ(index.GetCellCount(), 2u) << "leaked dead cell keys at tick " << tick;
        EXPECT_LE(index.GetCellCount(), index.GetEntityCount());

        const auto near1 = index.QueryRadius({ t * 13.0f, 0, 0 }, 1.0f);
        ASSERT_EQ(near1.size(), 1u) << "tick " << tick;
        EXPECT_EQ(static_cast<u64>(near1[0]), 1u);
        // The other entity is far away and must not appear in this query.
        EXPECT_TRUE(index.QueryRadius({ 9999.0f, 9999.0f, 9999.0f }, 1.0f).empty());
    }
}

// A query whose cell bounding box vastly exceeds the entry count (a large
// radius / box on a fine grid) must take the linear entry-scan fallback rather
// than iterating ~10^9 empty cells. This pins both correctness *and* that the
// query returns promptly — without the fallback this case times out (it caused
// the TSan CI timeout that motivated the fallback). All three query kinds.
TEST(SpatialAccelerationTest, HugeQueryOnFineGridUsesFallbackAndMatchesBruteForce)
{
    // Fine 1-unit grid, a handful of points spread across a large world.
    const std::vector<Point> points = {
        { UUID(1), { 0, 0, 0 } },
        { UUID(2), { 250, -100, 50 } },
        { UUID(3), { -400, 300, -200 } },
        { UUID(4), { 100, 100, 100 } },
    };
    SceneSpatialIndex index = BuildIndex(points, 1.0f);

    // Radius 2000 around the origin: box would be 4001^3 ≈ 6.4x10^10 cells.
    const glm::vec3 center{ 0, 0, 0 };
    const f32 radius = 2000.0f;
    EXPECT_EQ(ToSortedU64(index.QueryRadius(center, radius)),
              BruteForceRadius(points, center, radius));

    // AABB covering the whole world on the same fine grid.
    const auto aabb = ToSortedU64(index.QueryAABB({ -1000, -1000, -1000 }, { 1000, 1000, 1000 }));
    EXPECT_EQ(aabb, (std::vector<u64>{ 1, 2, 3, 4 }));

    // NearestN with a large finite maxRadius → bounded path takes the fallback.
    const auto nearest = index.NearestN(center, 2, 5000.0f);
    ASSERT_EQ(nearest.size(), 2u);
    EXPECT_EQ(static_cast<u64>(nearest[0]), 1u); // origin point is closest
    EXPECT_EQ(static_cast<u64>(nearest[1]), 4u); // (100,100,100) next
}

// A NaN/Inf/non-positive cell size must never reach the grid math (it would
// make m_InvCellSize NaN and UB every floor()/cast). The constructor clamps to
// a safe default; SetCellSize ignores it and keeps the prior resolution.
TEST(SpatialAccelerationTest, InvalidCellSizeIsClampedOrIgnored)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    EXPECT_FLOAT_EQ(SceneSpatialIndex(nan).GetCellSize(), 1.0f);
    EXPECT_FLOAT_EQ(SceneSpatialIndex(inf).GetCellSize(), 1.0f);
    EXPECT_FLOAT_EQ(SceneSpatialIndex(0.0f).GetCellSize(), 1.0f);
    EXPECT_FLOAT_EQ(SceneSpatialIndex(-5.0f).GetCellSize(), 1.0f);

    SceneSpatialIndex index(10.0f);
    index.Insert(UUID(1), { 1, 1, 1 });
    index.SetCellSize(nan);
    EXPECT_FLOAT_EQ(index.GetCellSize(), 10.0f);
    index.SetCellSize(inf);
    EXPECT_FLOAT_EQ(index.GetCellSize(), 10.0f);
    // The index is still healthy after the rejected resizes.
    EXPECT_EQ(index.QueryRadius({ 1, 1, 1 }, 1.0f).size(), 1u);
}

// A finite but enormous coordinate makes coord/cellSize exceed the i32 range, so
// the float->int cell cast would be UB. Insert skips such a position; queries
// with extreme finite center/radius/bounds fall back to the linear scan. The
// point of this test is that none of it triggers UB (UBSan would flag the cast).
TEST(SpatialAccelerationTest, ExtremeFiniteCoordinatesAreHandledSafely)
{
    SceneSpatialIndex index(1.0f);
    const f32 huge = 1e30f; // huge / cellSize(1) overflows the i32 cell range

    index.Insert(UUID(1), { huge, 0, 0 }); // unrepresentable cell → skipped
    EXPECT_EQ(index.GetEntityCount(), 0u);

    index.Insert(UUID(2), { 1, 2, 3 });
    index.Insert(UUID(3), { -4, 5, -6 });
    EXPECT_EQ(index.GetEntityCount(), 2u);

    // Enormous finite radius → fallback scan, returns the in-range entities.
    EXPECT_EQ(ToSortedU64(index.QueryRadius({ 0, 0, 0 }, huge)),
              (std::vector<u64>{ 2, 3 }));
    // Centered at an extreme coordinate → fallback, nothing in range, no UB.
    EXPECT_TRUE(index.QueryRadius({ huge, huge, huge }, 1.0f).empty());
    // Extreme AABB bounds → fallback scan.
    EXPECT_EQ(ToSortedU64(index.QueryAABB({ -huge, -huge, -huge }, { huge, huge, huge })),
              (std::vector<u64>{ 2, 3 }));
    // Extreme finite maxRadius in the bounded NearestN path → fallback.
    EXPECT_EQ(index.NearestN({ 0, 0, 0 }, 5, huge).size(), 2u);
}

TEST(SpatialAccelerationTest, ClearAllowsRebuildWithFreshContents)
{
    SceneSpatialIndex index(10.0f);
    index.Insert(UUID(1), { 0, 0, 0 });
    index.Clear();
    index.Insert(UUID(2), { 1, 0, 0 });

    const auto got = index.QueryRadius({ 0, 0, 0 }, 100.0f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(static_cast<u64>(got[0]), 2u) << "stale entry survived Clear()";
}
