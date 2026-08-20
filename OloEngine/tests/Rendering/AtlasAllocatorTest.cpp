// OLO_TEST_LAYER: L1
//
// Pure-math contracts for the shared power-of-two atlas allocator (issue
// #718): allocate / free / coalesce / occupancy, plus a randomised
// allocate-free fuzz sequence asserting the core invariants (no overlapping
// live regions, occupancy matches the live set, every fully-freed subtree
// coalesces back into its parent). Deterministic CPU math — no GL context.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/AtlasAllocator.h"

#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    bool RegionsOverlap(const AtlasAllocator::Region& a, const AtlasAllocator::Region& b)
    {
        return a.X < b.X + b.Size && b.X < a.X + a.Size &&
               a.Y < b.Y + b.Size && b.Y < a.Y + a.Size;
    }
} // namespace

TEST(AtlasAllocator, DefaultConstructedIsZeroCapacity)
{
    AtlasAllocator allocator;
    EXPECT_EQ(allocator.AtlasSize(), 0u);
    EXPECT_EQ(allocator.Allocate(1), AtlasAllocator::kInvalidNode);
    EXPECT_FLOAT_EQ(allocator.Occupancy(), 0.0f);
}

TEST(AtlasAllocator, InvalidConstructionArgsYieldZeroCapacity)
{
    // Non-power-of-two atlas, non-power-of-two tile, min > atlas, zero atlas —
    // none of these should assert or crash, just refuse to allocate.
    EXPECT_EQ(AtlasAllocator(1000, 32).Allocate(32), AtlasAllocator::kInvalidNode);
    EXPECT_EQ(AtlasAllocator(1024, 100).Allocate(100), AtlasAllocator::kInvalidNode);
    EXPECT_EQ(AtlasAllocator(64, 128).Allocate(64), AtlasAllocator::kInvalidNode);
    EXPECT_EQ(AtlasAllocator(0, 1).Allocate(1), AtlasAllocator::kInvalidNode);
}

TEST(AtlasAllocator, AllocateWholeAtlasOnce)
{
    AtlasAllocator allocator(1024, 32);
    const u32 node = allocator.Allocate(1024);
    ASSERT_NE(node, AtlasAllocator::kInvalidNode);

    const auto region = allocator.GetRegion(node);
    EXPECT_EQ(region.X, 0u);
    EXPECT_EQ(region.Y, 0u);
    EXPECT_EQ(region.Size, 1024u);

    // The atlas is now fully committed — nothing else fits, at any size.
    EXPECT_EQ(allocator.Allocate(1024), AtlasAllocator::kInvalidNode);
    EXPECT_EQ(allocator.Allocate(32), AtlasAllocator::kInvalidNode);
}

TEST(AtlasAllocator, FourQuadrantsExactlyFillTheAtlas)
{
    AtlasAllocator allocator(1024, 32);
    std::vector<AtlasAllocator::Region> regions;
    for (int i = 0; i < 4; ++i)
    {
        const u32 node = allocator.Allocate(512);
        ASSERT_NE(node, AtlasAllocator::kInvalidNode);
        regions.push_back(allocator.GetRegion(node));
    }

    // A 5th 512-sized request must fail: the atlas is exactly full.
    EXPECT_EQ(allocator.Allocate(512), AtlasAllocator::kInvalidNode);

    for (sizet a = 0; a < regions.size(); ++a)
    {
        EXPECT_EQ(regions[a].Size, 512u);
        EXPECT_LE(regions[a].X + regions[a].Size, 1024u);
        EXPECT_LE(regions[a].Y + regions[a].Size, 1024u);
        for (sizet b = a + 1; b < regions.size(); ++b)
            EXPECT_FALSE(RegionsOverlap(regions[a], regions[b])) << "quadrants " << a << " and " << b << " overlap";
    }
}

TEST(AtlasAllocator, CoalescesOnlyOnceEveryChildIsFreed)
{
    AtlasAllocator allocator(1024, 32);

    std::vector<u32> quadrants;
    for (int i = 0; i < 4; ++i)
    {
        const u32 node = allocator.Allocate(512);
        ASSERT_NE(node, AtlasAllocator::kInvalidNode);
        quadrants.push_back(node);
    }

    // Root-sized allocation cannot succeed while any quadrant is live.
    EXPECT_EQ(allocator.Allocate(1024), AtlasAllocator::kInvalidNode);

    // Freeing 3 of the 4 still leaves the root covered by the 4th.
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(allocator.Free(quadrants[i]));
    EXPECT_EQ(allocator.Allocate(1024), AtlasAllocator::kInvalidNode);

    // Freeing the last quadrant coalesces the whole atlas back together.
    EXPECT_TRUE(allocator.Free(quadrants[3]));
    const u32 whole = allocator.Allocate(1024);
    EXPECT_NE(whole, AtlasAllocator::kInvalidNode);
}

TEST(AtlasAllocator, FreeIsIdempotentAndTolerant)
{
    AtlasAllocator allocator(256, 32);
    const u32 node = allocator.Allocate(64);
    ASSERT_NE(node, AtlasAllocator::kInvalidNode);

    EXPECT_TRUE(allocator.Free(node));
    EXPECT_FALSE(allocator.Free(node)); // already free
    EXPECT_FALSE(allocator.Free(AtlasAllocator::kInvalidNode));
    EXPECT_FALSE(allocator.Free(999999));
}

TEST(AtlasAllocator, OutOfRangeAndNonPowerOfTwoSizesFail)
{
    AtlasAllocator allocator(1024, 32);
    EXPECT_EQ(allocator.Allocate(16), AtlasAllocator::kInvalidNode);   // below MinTileSize
    EXPECT_EQ(allocator.Allocate(2048), AtlasAllocator::kInvalidNode); // above AtlasSize
    EXPECT_EQ(allocator.Allocate(100), AtlasAllocator::kInvalidNode);  // not a power of two
    EXPECT_EQ(allocator.Allocate(0), AtlasAllocator::kInvalidNode);
}

TEST(AtlasAllocator, OccupancyTracksLiveArea)
{
    AtlasAllocator allocator(1024, 32);
    EXPECT_FLOAT_EQ(allocator.Occupancy(), 0.0f);

    const u32 a = allocator.Allocate(512);
    ASSERT_NE(a, AtlasAllocator::kInvalidNode);
    EXPECT_NEAR(allocator.Occupancy(), 0.25f, 1e-6f); // 512^2 / 1024^2

    const u32 b = allocator.Allocate(256);
    ASSERT_NE(b, AtlasAllocator::kInvalidNode);
    EXPECT_NEAR(allocator.Occupancy(), 0.25f + 1.0f / 16.0f, 1e-6f);

    EXPECT_TRUE(allocator.Free(a));
    EXPECT_NEAR(allocator.Occupancy(), 1.0f / 16.0f, 1e-6f);

    EXPECT_TRUE(allocator.Free(b));
    EXPECT_FLOAT_EQ(allocator.Occupancy(), 0.0f);
}

TEST(AtlasAllocator, LiveAllocationCountTracksAllocateAndFree)
{
    AtlasAllocator allocator(1024, 32);
    EXPECT_EQ(allocator.LiveAllocationCount(), 0u);
    const u32 a = allocator.Allocate(128);
    const u32 b = allocator.Allocate(128);
    ASSERT_NE(a, AtlasAllocator::kInvalidNode);
    ASSERT_NE(b, AtlasAllocator::kInvalidNode);
    EXPECT_EQ(allocator.LiveAllocationCount(), 2u);
    allocator.Free(a);
    EXPECT_EQ(allocator.LiveAllocationCount(), 1u);
    allocator.Free(b);
    EXPECT_EQ(allocator.LiveAllocationCount(), 0u);
}

TEST(AtlasAllocator, ResetDropsEveryLiveAllocation)
{
    AtlasAllocator allocator(1024, 32);
    std::ignore = allocator.Allocate(512);
    std::ignore = allocator.Allocate(256);
    ASSERT_GT(allocator.LiveAllocationCount(), 0u);

    allocator.Reset();
    EXPECT_EQ(allocator.LiveAllocationCount(), 0u);
    EXPECT_FLOAT_EQ(allocator.Occupancy(), 0.0f);
    // Full capacity is available again post-reset.
    EXPECT_NE(allocator.Allocate(1024), AtlasAllocator::kInvalidNode);
}

TEST(AtlasAllocator, MixedSizesNeverOverlapAndStayInBounds)
{
    AtlasAllocator allocator(2048, 32);
    const std::array<u32, 4> sizes = { 1024u, 512u, 256u, 128u };

    std::vector<AtlasAllocator::Region> regions;
    for (u32 size : sizes)
    {
        for (int i = 0; i < 3; ++i)
        {
            const u32 node = allocator.Allocate(size);
            if (node != AtlasAllocator::kInvalidNode)
                regions.push_back(allocator.GetRegion(node));
        }
    }

    ASSERT_FALSE(regions.empty());
    for (sizet a = 0; a < regions.size(); ++a)
    {
        EXPECT_LE(regions[a].X + regions[a].Size, 2048u);
        EXPECT_LE(regions[a].Y + regions[a].Size, 2048u);
        for (sizet b = a + 1; b < regions.size(); ++b)
            EXPECT_FALSE(RegionsOverlap(regions[a], regions[b])) << "regions " << a << " and " << b << " overlap";
    }
}

// Randomised allocate/free sequence. A hand-picked scenario can pass on a
// buddy-merge bug that only shows up once refcounts have been pushed up and
// down enough times to drift — this drives that arithmetic hundreds of times
// with a fixed seed (deterministic, reproducible on failure).
TEST(AtlasAllocator, FuzzedAllocateFreeSequenceHoldsInvariants)
{
    constexpr u32 kAtlasSize = 4096;
    constexpr u32 kMinTile = 32;
    AtlasAllocator allocator(kAtlasSize, kMinTile);

    const std::array<u32, 5> sizes = { 2048u, 1024u, 512u, 256u, 128u };

    std::mt19937 rng(0xA71A5u); // fixed seed — reproducible on failure
    std::uniform_int_distribution<int> sizeDist(0, static_cast<int>(sizes.size()) - 1);
    std::uniform_real_distribution<float> actionDist(0.0f, 1.0f);

    std::unordered_map<u32, AtlasAllocator::Region> live;

    for (int step = 0; step < 2000; ++step)
    {
        const bool wantAllocate = live.empty() || actionDist(rng) < 0.6f;
        if (wantAllocate)
        {
            const u32 size = sizes[static_cast<sizet>(sizeDist(rng))];
            const u32 node = allocator.Allocate(size);
            if (node == AtlasAllocator::kInvalidNode)
                continue; // atlas full at this size — not a failure

            const auto region = allocator.GetRegion(node);
            ASSERT_EQ(region.Size, size);
            ASSERT_LE(region.X + region.Size, kAtlasSize);
            ASSERT_LE(region.Y + region.Size, kAtlasSize);
            for (const auto& [otherNode, otherRegion] : live)
            {
                ASSERT_FALSE(RegionsOverlap(region, otherRegion))
                    << "node " << node << " overlaps live node " << otherNode;
            }
            live.emplace(node, region);
        }
        else
        {
            auto it = std::next(live.begin(), std::uniform_int_distribution<sizet>(0, live.size() - 1)(rng));
            EXPECT_TRUE(allocator.Free(it->first));
            live.erase(it);
        }

        EXPECT_EQ(allocator.LiveAllocationCount(), static_cast<u32>(live.size()));

        u64 liveArea = 0;
        for (const auto& [node, region] : live)
            liveArea += static_cast<u64>(region.Size) * static_cast<u64>(region.Size);
        const f32 expectedOccupancy =
            static_cast<f32>(static_cast<f64>(liveArea) / (static_cast<f64>(kAtlasSize) * static_cast<f64>(kAtlasSize)));
        EXPECT_NEAR(allocator.Occupancy(), expectedOccupancy, 1e-5f);
    }

    // Freeing everything must return the allocator to a clean, fully-coalesced
    // state — the whole atlas is allocatable as one region again.
    for (const auto& [node, region] : live)
        allocator.Free(node);
    EXPECT_EQ(allocator.LiveAllocationCount(), 0u);
    EXPECT_FLOAT_EQ(allocator.Occupancy(), 0.0f);
    EXPECT_NE(allocator.Allocate(kAtlasSize), AtlasAllocator::kInvalidNode);
}
