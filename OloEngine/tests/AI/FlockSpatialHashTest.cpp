#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// ============================================================================
// FlockSpatialHashTest — unit test for the uniform-grid spatial hash backing
// boid neighbour queries (issue #731).
//
// This is where the flocking feature's performance claim is actually won, so
// it is tested in isolation from any Scene: a Scene tick can only tell you
// "the flock moved", not "the query pruned correctly". Three properties are
// pinned here, each of which fails silently in the real system:
//
//   1. EXACTNESS — the accelerated query returns precisely the set a
//      brute-force scan would. A wrong bounding-cell range or a missing
//      exact-cell re-check (two cells can share a bucket) drops or invents
//      neighbours; the flock still flocks, just wrongly. Every correctness
//      test here is written as a differential against an independent O(n)
//      reference rather than against hand-picked expectations.
//
//   2. PRUNING — the query visits a bounded number of candidates rather than
//      degenerating into that same O(n) scan. A hash that is merely correct
//      would pass every exactness test above while being no faster than the
//      loop it replaced, so the pruning test counts candidate visits (a
//      hardware-independent proxy) instead of timing anything.
//
//   3. DETERMINISM — identical inputs produce an identical VISIT ORDER, not
//      just an identical set. Acceptance criterion 2 (deterministic under the
//      fixed-timestep loop) rests on this: the steering kernel accumulates
//      floats over neighbours, so a reordered traversal changes the rounding
//      and the flock diverges. It is also what makes moving the steering pass
//      onto a worker thread a no-op for results.
// ============================================================================

#include "OloEngine/AI/Flocking/FlockSpatialHash.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;

namespace
{
    // Deterministic scatter — a fixed LCG, not the engine RNG, so this file
    // owns its inputs completely and can never be perturbed by a seed change
    // elsewhere.
    std::vector<glm::vec3> ScatterPoints(u32 count, f32 extent, u32 seed = 12345u)
    {
        std::vector<glm::vec3> points;
        points.reserve(count);
        u32 state = seed;
        const auto next = [&state]() -> f32
        {
            state = state * 1664525u + 1013904223u;
            // [-1, 1)
            return (static_cast<f32>(state >> 8) / static_cast<f32>(1u << 24)) * 2.0f - 1.0f;
        };
        for (u32 i = 0; i < count; ++i)
            points.push_back({ next() * extent, next() * extent, next() * extent });
        return points;
    }

    // Independent O(n) reference: every index within `radius` of `center`,
    // ascending.
    std::vector<u32> BruteForce(const std::vector<glm::vec3>& points, const glm::vec3& center, f32 radius)
    {
        std::vector<u32> hits;
        const f32 radiusSq = radius * radius;
        for (u32 i = 0; i < static_cast<u32>(points.size()); ++i)
        {
            const glm::vec3 delta = points[i] - center;
            if (glm::dot(delta, delta) <= radiusSq)
                hits.push_back(i);
        }
        return hits;
    }

    std::vector<u32> QuerySorted(const FlockSpatialHash& hash, const glm::vec3& center, f32 radius)
    {
        std::vector<u32> hits;
        hash.ForEachInRadius(center, radius, [&hits](u32 index, const glm::vec3&, f32)
                             { hits.push_back(index); });
        std::sort(hits.begin(), hits.end());
        return hits;
    }
} // namespace

// ── 1. Exactness ────────────────────────────────────────────────────────────

TEST(FlockSpatialHashTest, EmptyGridReturnsNothing)
{
    FlockSpatialHash hash;
    u32 visits = 0;
    hash.ForEachInRadius({ 0.0f, 0.0f, 0.0f }, 100.0f, [&visits](u32, const glm::vec3&, f32)
                         { ++visits; });
    EXPECT_EQ(visits, 0u);
    EXPECT_EQ(hash.GetItemCount(), 0u);
    EXPECT_EQ(hash.GetIndexedItemCount(), 0u);
}

TEST(FlockSpatialHashTest, MatchesBruteForceAcrossRadiiAndCentres)
{
    // Points straddle the origin so negative cell coordinates (the classic
    // floor-vs-truncate bug) are exercised throughout.
    const std::vector<glm::vec3> points = ScatterPoints(600, 40.0f);

    FlockSpatialHash hash;
    hash.Rebuild(points, 4.0f);
    ASSERT_EQ(hash.GetIndexedItemCount(), 600u);

    // Radii deliberately span below, around, and well above the cell size, so
    // the 1-cell, 3-cell and many-cell sweep widths are all covered.
    const f32 radii[] = { 0.0f, 0.5f, 2.0f, 4.0f, 7.5f, 15.0f };
    const glm::vec3 centres[] = {
        { 0.0f, 0.0f, 0.0f },       // origin — the cell-coordinate sign flip
        { 12.0f, -7.0f, 3.5f },     // interior, off-axis
        { -40.0f, -40.0f, -40.0f }, // a corner of the cloud
        { 200.0f, 0.0f, 0.0f },     // far outside — must find nothing
        { 4.0f, 4.0f, 4.0f },       // exactly on a cell boundary
    };

    for (const glm::vec3& centre : centres)
    {
        for (const f32 radius : radii)
        {
            EXPECT_EQ(QuerySorted(hash, centre, radius), BruteForce(points, centre, radius))
                << "centre (" << centre.x << ", " << centre.y << ", " << centre.z << ") radius " << radius;
        }
    }
}

TEST(FlockSpatialHashTest, MatchesBruteForceWhenEveryPointSharesOneCell)
{
    // Degenerate density: the whole flock stacked inside a single cell. This is
    // the case a bucket-collision bug hides in, because every candidate happens
    // to be a genuine hit.
    std::vector<glm::vec3> points;
    for (u32 i = 0; i < 64; ++i)
        points.push_back({ 0.01f * static_cast<f32>(i), 0.0f, 0.0f });

    FlockSpatialHash hash;
    hash.Rebuild(points, 10.0f);

    EXPECT_EQ(hash.GetMaxBucketLoad(), 64u) << "all points should land in one bucket at this cell size";
    EXPECT_EQ(QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, 0.2f), BruteForce(points, { 0.0f, 0.0f, 0.0f }, 0.2f));
}

TEST(FlockSpatialHashTest, VisitsEachItemAtMostOnce)
{
    // Two distinct cells can hash to the same bucket. Without the exact-cell
    // re-check that would show up as a duplicate visit — which the steering
    // kernel would silently double-count into its cohesion/alignment averages.
    const std::vector<glm::vec3> points = ScatterPoints(500, 25.0f, 99u);

    FlockSpatialHash hash;
    hash.Rebuild(points, 3.0f);

    std::vector<u32> hits;
    hash.ForEachInRadius({ 0.0f, 0.0f, 0.0f }, 30.0f, [&hits](u32 i, const glm::vec3&, f32)
                         { hits.push_back(i); });

    std::vector<u32> unique = hits;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    EXPECT_EQ(hits.size(), unique.size()) << "an item was visited more than once";
}

TEST(FlockSpatialHashTest, ReportsSquaredDistanceOfEachHit)
{
    const std::vector<glm::vec3> points = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, -2.0f } };
    FlockSpatialHash hash;
    hash.Rebuild(points, 2.0f);

    hash.ForEachInRadius({ 0.0f, 0.0f, 0.0f }, 5.0f,
                         [&points](u32 index, const glm::vec3& position, f32 distanceSq)
                         {
                             EXPECT_FLOAT_EQ(position.x, points[index].x);
                             EXPECT_FLOAT_EQ(position.y, points[index].y);
                             EXPECT_FLOAT_EQ(position.z, points[index].z);
                             EXPECT_FLOAT_EQ(distanceSq, glm::dot(points[index], points[index]));
                         });
}

TEST(FlockSpatialHashTest, NonFinitePositionsAreExcludedNotCrashing)
{
    // A physics blow-up upstream must not be able to corrupt the cell maths or
    // surface as a neighbour with a NaN position.
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    const std::vector<glm::vec3> points = {
        { 0.0f, 0.0f, 0.0f },
        { nan, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, inf, 0.0f },
        { 0.0f, 0.0f, -inf },
    };

    FlockSpatialHash hash;
    hash.Rebuild(points, 2.0f);

    EXPECT_EQ(hash.GetItemCount(), 5u);
    EXPECT_EQ(hash.GetIndexedItemCount(), 2u) << "only the finite points belong in the grid";

    const std::vector<u32> hits = QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, 1000.0f);
    EXPECT_EQ(hits, (std::vector<u32>{ 0u, 2u }));
}

TEST(FlockSpatialHashTest, DegenerateQueryParametersReturnNothing)
{
    const std::vector<glm::vec3> points = ScatterPoints(50, 10.0f);
    FlockSpatialHash hash;
    hash.Rebuild(points, 2.0f);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_TRUE(QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, -1.0f).empty());
    EXPECT_TRUE(QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, nan).empty());
    EXPECT_TRUE(QuerySorted(hash, { nan, 0.0f, 0.0f }, 5.0f).empty());
}

TEST(FlockSpatialHashTest, HugeRadiusFallsBackToLinearScanWithIdenticalResults)
{
    // A radius spanning more than kMaxQueryCells cells takes the linear-scan
    // fallback. It must be a pure cost trade-off — same answer, same order.
    const std::vector<glm::vec3> points = ScatterPoints(300, 50.0f, 7u);
    FlockSpatialHash hash;
    hash.Rebuild(points, 0.25f); // tiny cells => a wide query blows the cell budget

    constexpr f32 kWideRadius = 60.0f;
    const auto lo = static_cast<i64>(std::floor(-kWideRadius / 0.25f));
    const auto hi = static_cast<i64>(std::floor(kWideRadius / 0.25f));
    ASSERT_GT((hi - lo + 1) * (hi - lo + 1) * (hi - lo + 1), static_cast<i64>(FlockSpatialHash::kMaxQueryCells))
        << "test no longer exercises the fallback path";

    EXPECT_EQ(QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, kWideRadius),
              BruteForce(points, { 0.0f, 0.0f, 0.0f }, kWideRadius));
}

TEST(FlockSpatialHashTest, CellSizeIsClampedToTheMinimum)
{
    const std::vector<glm::vec3> points = ScatterPoints(20, 5.0f);
    FlockSpatialHash hash;

    hash.Rebuild(points, 0.0f);
    EXPECT_FLOAT_EQ(hash.GetCellSize(), FlockSpatialHash::kMinCellSize);

    hash.Rebuild(points, -3.0f);
    EXPECT_FLOAT_EQ(hash.GetCellSize(), FlockSpatialHash::kMinCellSize);

    hash.Rebuild(points, std::numeric_limits<f32>::quiet_NaN());
    EXPECT_FLOAT_EQ(hash.GetCellSize(), FlockSpatialHash::kMinCellSize);
}

TEST(FlockSpatialHashTest, RebuildFullyReplacesThePreviousContents)
{
    // The per-tick rebuild reuses one instance, so a stale item surviving into
    // the next tick would be a neighbour that no longer exists.
    FlockSpatialHash hash;
    hash.Rebuild(ScatterPoints(400, 30.0f, 3u), 3.0f);
    ASSERT_EQ(hash.GetIndexedItemCount(), 400u);

    const std::vector<glm::vec3> second = { { 100.0f, 100.0f, 100.0f } };
    hash.Rebuild(second, 3.0f);

    EXPECT_EQ(hash.GetItemCount(), 1u);
    EXPECT_EQ(hash.GetIndexedItemCount(), 1u);
    EXPECT_TRUE(QuerySorted(hash, { 0.0f, 0.0f, 0.0f }, 50.0f).empty()) << "an item from the previous build survived";
    EXPECT_EQ(QuerySorted(hash, { 100.0f, 100.0f, 100.0f }, 1.0f), (std::vector<u32>{ 0u }));

    hash.Clear();
    EXPECT_EQ(hash.GetItemCount(), 0u);
    EXPECT_TRUE(QuerySorted(hash, { 100.0f, 100.0f, 100.0f }, 1.0f).empty());
}

// ── 2. Pruning ──────────────────────────────────────────────────────────────

TEST(FlockSpatialHashTest, QueryPrunesInsteadOfScanningEverything)
{
    // The whole point of the structure. Counting *visited candidates* rather
    // than wall time keeps this meaningful on any machine and under any build
    // config: a hash that quietly degenerated to a full scan would still pass
    // every exactness test above.
    constexpr u32 kCount = 4000;
    constexpr f32 kExtent = 40.0f;
    constexpr f32 kRadius = 4.0f;

    const std::vector<glm::vec3> points = ScatterPoints(kCount, kExtent, 555u);
    FlockSpatialHash hash;
    hash.Rebuild(points, kRadius);

    u64 totalHits = 0;
    for (u32 i = 0; i < kCount; ++i)
    {
        hash.ForEachInRadius(points[i], kRadius, [&totalHits](u32, const glm::vec3&, f32)
                             { ++totalHits; });
    }
    ASSERT_GT(totalHits, kCount) << "the scene is too sparse to be a meaningful flock";

    // Cell size == query radius, so one query sweeps at most 3x3x3 cells and
    // therefore touches at most 27 * (largest bucket load) candidates. That
    // product is the honest upper bound on the work one agent does, and it is
    // what must stay far below kCount — a hash that had silently degenerated
    // into a full scan would blow it while still passing every exactness test
    // above.
    const u64 worstCaseCandidates = 27ull * static_cast<u64>(hash.GetMaxBucketLoad());
    EXPECT_LT(worstCaseCandidates, static_cast<u64>(kCount) / 10ull)
        << "worst-case candidates per query (" << worstCaseCandidates << " of " << kCount << ") — not pruning";

    // And the average result count, which is what the steering kernel actually
    // folds in, stays a small constant rather than scaling with the flock.
    const f64 averageHits = static_cast<f64>(totalHits) / static_cast<f64>(kCount);
    EXPECT_LT(averageHits, static_cast<f64>(kCount) / 100.0)
        << "average neighbours per query (" << averageHits << ")";

    // Occupancy sanity: a sane cell size spreads the flock over many buckets.
    EXPECT_GT(hash.GetOccupiedBucketCount(), kCount / 10u);
}

TEST(FlockSpatialHashTest, EarlyOutStopsTheTraversal)
{
    // The steering kernel caps how many neighbours it folds in; the cap is
    // implemented by returning false from the callback.
    const std::vector<glm::vec3> points = ScatterPoints(300, 5.0f, 21u);
    FlockSpatialHash hash;
    hash.Rebuild(points, 5.0f);

    constexpr u32 kCap = 7;
    u32 visits = 0;
    hash.ForEachInRadius({ 0.0f, 0.0f, 0.0f }, 100.0f,
                         [&visits](u32, const glm::vec3&, f32) -> bool
                         {
                             ++visits;
                             return visits < kCap;
                         });
    EXPECT_EQ(visits, kCap);
}

// ── 3. Determinism ──────────────────────────────────────────────────────────

TEST(FlockSpatialHashTest, VisitOrderIsIdenticalAcrossInstancesAndRebuilds)
{
    // Acceptance criterion 2 leans on this: the steering kernel sums floats
    // over neighbours, so a different traversal order is a different result.
    const std::vector<glm::vec3> points = ScatterPoints(800, 30.0f, 4242u);

    const auto orderedHits = [&points](const FlockSpatialHash& hash)
    {
        std::vector<u32> order;
        for (u32 i = 0; i < static_cast<u32>(points.size()); i += 37)
        {
            hash.ForEachInRadius(points[i], 6.0f, [&order](u32 index, const glm::vec3&, f32)
                                 { order.push_back(index); });
        }
        return order;
    };

    FlockSpatialHash first;
    first.Rebuild(points, 4.0f);
    const std::vector<u32> firstOrder = orderedHits(first);
    ASSERT_FALSE(firstOrder.empty());

    // A second, independently constructed instance.
    FlockSpatialHash second;
    second.Rebuild(points, 4.0f);
    EXPECT_EQ(orderedHits(second), firstOrder);

    // The same instance, rebuilt after being used for something else — the
    // per-tick reuse pattern.
    first.Rebuild(ScatterPoints(120, 8.0f, 1u), 2.0f);
    first.Rebuild(points, 4.0f);
    EXPECT_EQ(orderedHits(first), firstOrder);
}

TEST(FlockSpatialHashTest, TheTwoTraversalPathsShareASetButNotAnOrder)
{
    // The exact contract at the kMaxQueryCells declaration, pinned so nobody
    // "simplifies" either path into the other's ordering by accident.
    //
    // Points are laid out so global index order and cell-major order genuinely
    // disagree: index 0 sits in a HIGHER cell than index 1, so the cell sweep
    // must emit 1 before 0 while the linear fallback emits 0 before 1.
    constexpr f32 kCell = 1.0f;
    const std::vector<glm::vec3> points = {
        { 1.5f, 0.5f, 0.5f }, // index 0 -> cell (1, 0, 0)
        { 0.5f, 0.5f, 0.5f }, // index 1 -> cell (0, 0, 0)
    };

    FlockSpatialHash hash;
    hash.Rebuild(points, kCell);

    const auto visitOrder = [&hash](f32 radius)
    {
        std::vector<u32> order;
        hash.ForEachInRadius({ 1.0f, 0.5f, 0.5f }, radius,
                             [&order](u32 index, const glm::vec3&, f32)
                             { order.push_back(index); });
        return order;
    };

    // Small radius -> cell sweep: cell (0,0,0) precedes cell (1,0,0).
    const std::vector<u32> sweep = visitOrder(1.0f);
    EXPECT_EQ(sweep, (std::vector<u32>{ 1u, 0u })) << "cell sweep is not cell-major";

    // A radius far past the cell budget -> linear fallback: ascending index.
    const f32 hugeRadius = kCell * static_cast<f32>(FlockSpatialHash::kMaxQueryCells);
    const std::vector<u32> fallback = visitOrder(hugeRadius);
    EXPECT_EQ(fallback, (std::vector<u32>{ 0u, 1u })) << "fallback is not ascending-index";

    // Same SET either way — that part of the contract does hold.
    std::vector<u32> sweepSorted = sweep;
    std::vector<u32> fallbackSorted = fallback;
    std::sort(sweepSorted.begin(), sweepSorted.end());
    std::sort(fallbackSorted.begin(), fallbackSorted.end());
    EXPECT_EQ(sweepSorted, fallbackSorted);

    // And each path is reproducible on its own — which is what acceptance
    // criterion 2 actually rests on.
    EXPECT_EQ(visitOrder(1.0f), sweep);
    EXPECT_EQ(visitOrder(hugeRadius), fallback);
}

TEST(FlockSpatialHashTest, FlockingCellSizingKeepsEveryQueryOffTheFallback)
{
    // The invariant FlockingSystem relies on: it sizes each grid's cells from
    // the widest radius any agent queries, so a query can only ever span
    // 3x3x3 cells and the capped steering callback can never meet the
    // differently-ordered fallback. Verified through the public API by
    // checking the capped prefix is the cell-sweep prefix at that sizing.
    const std::vector<glm::vec3> points = ScatterPoints(400, 20.0f, 77u);
    constexpr f32 kRadius = 4.0f;

    FlockSpatialHash hash;
    hash.Rebuild(points, kRadius); // cellSize == query radius, as FlockingSystem does

    for (const glm::vec3& centre : { points[0], points[137], points[399] })
    {
        std::vector<u32> full;
        hash.ForEachInRadius(centre, kRadius, [&full](u32 i, const glm::vec3&, f32)
                             { full.push_back(i); });

        constexpr u32 kCap = 3;
        std::vector<u32> capped;
        hash.ForEachInRadius(centre, kRadius,
                             [&capped](u32 i, const glm::vec3&, f32) -> bool
                             {
                                 capped.push_back(i);
                                 return capped.size() < kCap;
                             });

        const sizet expected = std::min<sizet>(kCap, full.size());
        ASSERT_EQ(capped.size(), expected);
        EXPECT_TRUE(std::equal(capped.begin(), capped.end(), full.begin()))
            << "a capped sweep did not take the uncapped sweep's prefix — the fallback was reached";
    }
}

TEST(FlockSpatialHashTest, WithinACellItemsAreVisitedInAscendingIndexOrder)
{
    // The stable counting-sort scatter is what makes "the first N neighbours"
    // (the steering kernel's cap) a reproducible set rather than an
    // allocation-order accident.
    std::vector<glm::vec3> points;
    for (u32 i = 0; i < 16; ++i)
        points.push_back({ 0.05f * static_cast<f32>(i), 0.0f, 0.0f });

    FlockSpatialHash hash;
    hash.Rebuild(points, 10.0f); // one cell holds them all

    std::vector<u32> order;
    hash.ForEachInRadius({ 0.0f, 0.0f, 0.0f }, 10.0f, [&order](u32 i, const glm::vec3&, f32)
                         { order.push_back(i); });

    ASSERT_EQ(order.size(), points.size());
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
}
