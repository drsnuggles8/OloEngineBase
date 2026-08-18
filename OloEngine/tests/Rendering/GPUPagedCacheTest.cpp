// OLO_TEST_LAYER: plumbing
// =============================================================================
// GPUPagedCacheTest.cpp — the paged-cache substrate's allocation machinery
// (issue #704, acceptance criterion 3: allocate / evict / split-chain,
// headless).
//
// Everything here runs with the HostOnly backing: the page-chain, bitset and
// eviction logic is byte-identical to the GPU-visible configuration — the
// bytes just live on the heap — so nothing needs a GL context and nothing
// skips. The GPU-visible side (device arena writes, shader-side lookup) is
// covered by GPUCacheShaderResolveTest.cpp (shaderpipe).
//
// Ported from the VoxelEngine reference's Tests/gpu_paged_cache_tests.cpp,
// plus regression tests for behaviour the port deliberately changed:
//   * allocation FAILS (with rollback) instead of hanging when the policy has
//     no evictable victim — the reference looped forever;
//   * a partial page reservation is either completed by eviction or rolled
//     back — the reference leaked the partially-claimed pages;
//   * PushBackToObject after ClearObject writes to the chain page the element
//     count says, not blindly to the end page;
//   * GetObjectBufferRanges reports content-exact spans (a partially-used
//     page ends its range) — asserted on VALUES, not just range counts,
//     because a count-only comparison passes on the very bugs it should catch
//     (docs/agent-rules/gpu-scan-compaction.md §2).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "GPUCacheInspector.h"
#include "OloEngine/Renderer/GPUCache/GPUCachePolicy.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedBuffer.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedCache.h"

#include <array>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    using Cache = GPUPagedCache<u32, int, FIFOPolicy>;
    using LruCache = GPUPagedCache<u32, int, LRUPolicy>;
    using ClockCache = GPUPagedCache<u32, int, ClockPolicy>;
    using Inspector = GPUPagedCacheInspector<Cache>;

    constexpr sizet kPageSize = 5;
    constexpr u32 kPageCount = 10;
} // namespace

// ---------------------------------------------------------------- lifecycle

TEST(GPUPagedCacheTest, CreateCache)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));
    EXPECT_TRUE(cache.IsCreated());
}

TEST(GPUPagedCacheTest, CreateWithZeroPageCountFails)
{
    Cache cache;
    ASSERT_FALSE(cache.Create(kPageSize, 0));
    EXPECT_FALSE(cache.IsCreated());
}

TEST(GPUPagedCacheTest, CreateTwiceFailsAndKeepsOriginal)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));
    ASSERT_FALSE(cache.Create(kPageSize, kPageCount));
    EXPECT_TRUE(cache.IsCreated());
    EXPECT_EQ(cache.GetPageCount(), kPageCount);
}

TEST(GPUPagedCacheTest, DestroyEmptyCache)
{
    Cache cache;
    ASSERT_NO_THROW(cache.Destroy());
    EXPECT_FALSE(cache.IsCreated());
}

TEST(GPUPagedCacheTest, DestroyPopulatedCache)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const int data[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data, std::size(data)));
    ASSERT_TRUE(cache.Has(1));

    ASSERT_NO_THROW(cache.Destroy());
    EXPECT_FALSE(cache.IsCreated());
}

// ------------------------------------------------------------ AllocatePages

TEST(GPUPagedCacheTest, AllocatePagesSinglePage)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 32));

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 1, alloc));

    EXPECT_EQ(alloc.m_StartPage, alloc.m_EndPage);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 31u);
}

TEST(GPUPagedCacheTest, AllocatePagesExactRemainingCapacity)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 4, alloc));

    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 0u);
    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc), 4u);
}

TEST(GPUPagedCacheTest, AllocatePagesSingleEviction)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 2, alloc));
    ASSERT_TRUE(cache.AllocatePages(2, 2, alloc));
    ASSERT_EQ(Inspector::GetFreePagesCount(cache), 0u);

    // This must evict exactly one object (FIFO: object 1).
    ASSERT_TRUE(cache.AllocatePages(3, 2, alloc));

    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc), 2u);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 0u);
    EXPECT_FALSE(cache.Has(1));
    EXPECT_TRUE(cache.Has(2));
}

TEST(GPUPagedCacheTest, AllocatePagesMultipleEvictions)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    Cache::ObjectAllocation alloc;
    for (u32 obj = 1; obj <= 4; ++obj)
    {
        ASSERT_TRUE(cache.AllocatePages(obj, 1, alloc));
    }
    ASSERT_EQ(Inspector::GetFreePagesCount(cache), 0u);

    // Needs 3 pages -> must evict 3 single-page objects.
    ASSERT_TRUE(cache.AllocatePages(5, 3, alloc));

    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc), 3u);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 0u);
}

TEST(GPUPagedCacheTest, AllocatePagesFragmentedAllocation)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 8));

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 2, alloc));
    ASSERT_TRUE(cache.AllocatePages(2, 2, alloc));
    ASSERT_TRUE(cache.AllocatePages(3, 2, alloc));
    ASSERT_EQ(Inspector::GetFreePagesCount(cache), 2u);

    // Free the middle allocation -> fragmentation.
    cache.DeallocateObject(2);
    ASSERT_EQ(Inspector::GetFreePagesCount(cache), 4u);

    ASSERT_TRUE(cache.AllocatePages(4, 3, alloc));
    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc), 3u);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 1u);
}

TEST(GPUPagedCacheTest, AllocatePagesReusesFreedPages)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 8));

    Cache::ObjectAllocation alloc1;
    ASSERT_TRUE(cache.AllocatePages(1, 2, alloc1));
    const u32 firstStart = alloc1.m_StartPage;

    cache.DeallocateObject(1);
    ASSERT_EQ(Inspector::GetFreePagesCount(cache), 8u);

    Cache::ObjectAllocation alloc2;
    ASSERT_TRUE(cache.AllocatePages(2, 2, alloc2));
    EXPECT_EQ(alloc2.m_StartPage, firstStart); // lowest-first: must reuse the same pages
}

TEST(GPUPagedCacheTest, AllocatePagesAppendsToExistingAllocation)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 8));

    Cache::ObjectAllocation alloc1;
    ASSERT_TRUE(cache.AllocatePages(1, 1, alloc1));
    const u32 first = alloc1.m_StartPage;

    Cache::ObjectAllocation alloc2;
    ASSERT_TRUE(cache.AllocatePages(1, 2, alloc2)); // append 2 more pages

    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc2), 3u);
    EXPECT_EQ(alloc2.m_StartPage, first);
}

// -------------------------------------------------- failure instead of hang

// The reference's eviction loop had no exit: with nothing evictable it spun
// forever on the render thread. The port must FAIL the allocation and leave
// the cache untouched.
TEST(GPUPagedCacheTest, AllocationBeyondCapacityFailsCleanly)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    Cache::ObjectAllocation alloc;
    ASSERT_FALSE(cache.AllocatePages(1, 5, alloc)) << "5 pages can never fit in a 4-page cache";

    EXPECT_FALSE(cache.Has(1));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 4u) << "partially-reserved pages leaked on failure";
}

// The only evictable candidate is the requester itself: the policy must skip
// it (evicting it would free the very chain being grown) and the allocation
// must fail rather than hang or self-cannibalise.
TEST(GPUPagedCacheTest, RequesterIsNeverItsOwnVictim)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 4, alloc));

    ASSERT_FALSE(cache.AllocatePages(1, 1, alloc)) << "no victim exists besides the requester";
    EXPECT_TRUE(cache.Has(1));

    Cache::ObjectAllocation surviving;
    ASSERT_TRUE(cache.Find(1, surviving));
    EXPECT_EQ(Inspector::CountAllocatedPages(cache, surviving), 4u);
}

// A failed grow of an EXISTING object must restore its exact prior chain and
// contents — and the evictions performed on its behalf stand (documented
// contract; they cannot be undone).
TEST(GPUPagedCacheTest, FailedGrowRollsBackToPriorAllocation)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(2, 4));

    const int data[] = { 10, 20, 30, 40 }; // 2 pages
    ASSERT_TRUE(cache.AllocateObject(1, data, std::size(data)));

    Cache::ObjectAllocation alloc;
    ASSERT_FALSE(cache.AllocatePages(1, 3, alloc)) << "2 existing + 3 more exceeds the 4-page capacity";

    ASSERT_TRUE(cache.Has(1));
    Cache::ObjectAllocation after;
    ASSERT_TRUE(cache.Find(1, after));
    EXPECT_EQ(Inspector::CountAllocatedPages(cache, after), 2u) << "appended pages were not rolled back";
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), std::vector<int>(data, data + std::size(data)));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 2u);
}

TEST(GPUPagedCacheTest, EvictionListenerReportsPolicyVictimsOnly)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 4));

    std::vector<u32> evicted;
    cache.SetEvictionListener([&evicted](const u32& victim)
                              { evicted.push_back(victim); });

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.AllocatePages(1, 2, alloc));
    ASSERT_TRUE(cache.AllocatePages(2, 2, alloc));

    cache.DeallocateObject(2); // explicit — must NOT notify
    EXPECT_TRUE(evicted.empty());

    ASSERT_TRUE(cache.AllocatePages(3, 2, alloc)); // free pages exist — no eviction
    EXPECT_TRUE(evicted.empty());

    ASSERT_TRUE(cache.AllocatePages(4, 2, alloc)); // pressure — FIFO evicts object 1
    EXPECT_EQ(evicted, std::vector<u32>{ 1u });
}

// ---------------------------------------------------------- DeallocateObject

TEST(GPUPagedCacheTest, DeallocateObjectBasic)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    ASSERT_TRUE(cache.Has(1));
    const u32 freePagesBefore = Inspector::GetFreePagesCount(cache);

    cache.DeallocateObject(1);
    EXPECT_FALSE(cache.Has(1));
    EXPECT_GT(Inspector::GetFreePagesCount(cache), freePagesBefore);
    EXPECT_TRUE(Inspector::ReadObjectData(cache, 1).empty());
}

TEST(GPUPagedCacheTest, DeallocateObjectNonExisting)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const u32 freePagesBefore = Inspector::GetFreePagesCount(cache);
    EXPECT_NO_THROW(cache.DeallocateObject(9999));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), freePagesBefore);
}

TEST(GPUPagedCacheTest, DeallocateObjectEmptyObject)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    ASSERT_TRUE(cache.AllocateObject(7, nullptr, 0));
    ASSERT_TRUE(cache.Has(7));
    const u32 freePagesBefore = Inspector::GetFreePagesCount(cache);

    EXPECT_NO_THROW(cache.DeallocateObject(7));
    EXPECT_FALSE(cache.Has(7));
    EXPECT_GT(Inspector::GetFreePagesCount(cache), freePagesBefore);
}

TEST(GPUPagedCacheTest, DeallocateObjectTwiceIsIdempotent)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data = { 10, 20, 30, 40, 50 };
    ASSERT_TRUE(cache.AllocateObject(42, data.data(), data.size()));
    cache.DeallocateObject(42);
    EXPECT_FALSE(cache.Has(42));
    const u32 freePagesAfterFirst = Inspector::GetFreePagesCount(cache);

    EXPECT_NO_THROW(cache.DeallocateObject(42));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), freePagesAfterFirst);
}

TEST(GPUPagedCacheTest, DeallocateObjectFragmentedAllocation)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> dataA = { 1, 2, 3, 4, 5 };
    const std::vector<int> dataB = { 10, 11, 12, 13 };
    const std::vector<int> dataC = { 20, 21, 22, 23 };
    ASSERT_TRUE(cache.AllocateObject(1, dataA.data(), dataA.size()));
    ASSERT_TRUE(cache.AllocateObject(2, dataB.data(), dataB.size()));
    ASSERT_TRUE(cache.AllocateObject(3, dataC.data(), dataC.size()));

    cache.DeallocateObject(2);
    EXPECT_FALSE(cache.Has(2));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), dataA);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 3), dataC);

    // The freed hole is reusable by a larger, now-fragmented object.
    const std::vector<int> dataD = { 100, 101, 102, 103, 104, 105 };
    ASSERT_TRUE(cache.AllocateObject(4, dataD.data(), dataD.size()));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 4), dataD);

    cache.DeallocateObject(4);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 8u);
}

// ------------------------------------------------------------ AllocateObject

TEST(GPUPagedCacheTest, AllocateObjectSinglePage)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data = { 1, 2, 3 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    ASSERT_TRUE(cache.Has(1));

    const auto ranges = cache.GetObjectBufferRanges(1);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].m_AtomCount, 3u);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);
}

TEST(GPUPagedCacheTest, AllocateObjectTwiceReplacesContents)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    std::vector<int> data = { 1, 2, 3 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);

    data.push_back(4);
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);
}

TEST(GPUPagedCacheTest, AllocateObjectExactPageBoundary)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(2, data.data(), data.size()));

    const auto ranges = cache.GetObjectBufferRanges(2);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].m_AtomCount, 5u);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 2), data);
}

TEST(GPUPagedCacheTest, AllocateObjectOverMultiplePagesContiguous)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data(12, 42); // 3 pages
    ASSERT_TRUE(cache.AllocateObject(3, data.data(), data.size()));

    const auto ranges = cache.GetObjectBufferRanges(3);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].m_AtomCount, 12u);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 3), data);
}

TEST(GPUPagedCacheTest, AllocateObjectOverMultiplePagesFragmented)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 6));

    const std::array<int, 5> pageData{ 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData.data(), pageData.size()));
    ASSERT_TRUE(cache.AllocateObject(2, pageData.data(), pageData.size()));
    ASSERT_TRUE(cache.AllocateObject(3, pageData.data(), pageData.size()));
    cache.DeallocateObject(2); // hole at page 1

    const std::vector<int> largeData(7, 7); // 2 pages: the hole + a tail page
    ASSERT_TRUE(cache.AllocateObject(4, largeData.data(), largeData.size()));

    const auto ranges = cache.GetObjectBufferRanges(4);
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 4), largeData);
}

TEST(GPUPagedCacheTest, AllocateObjectWithNoElements)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    ASSERT_TRUE(cache.AllocateObject(5, nullptr, 0));
    EXPECT_TRUE(cache.Has(5));
    EXPECT_TRUE(cache.GetObjectBufferRanges(5).empty());
}

TEST(GPUPagedCacheTest, AllocateObjectCausesSingleEviction)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));

    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5)); // requires eviction
    ASSERT_TRUE(cache.Has(3));

    const bool obj1Alive = cache.Has(1);
    const bool obj2Alive = cache.Has(2);
    EXPECT_NE(obj1Alive, obj2Alive) << "exactly one prior object should have been evicted";
}

TEST(GPUPagedCacheTest, AllocateObjectCausesMultipleEvictions)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 3));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5));

    const std::vector<int> largeData(10, 9); // 2 pages
    ASSERT_TRUE(cache.AllocateObject(4, largeData.data(), largeData.size()));
    ASSERT_TRUE(cache.Has(4));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 4), largeData);

    const int survivors = (cache.Has(1) ? 1 : 0) + (cache.Has(2) ? 1 : 0) + (cache.Has(3) ? 1 : 0);
    EXPECT_EQ(survivors, 1) << "only one old object should remain";
}

TEST(GPUPagedCacheTest, AllocateObjectAfterDeallocationReusesFreedPages)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));

    cache.DeallocateObject(1);

    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5)); // reuses the freed page, no eviction
    EXPECT_TRUE(cache.Has(2));
    EXPECT_TRUE(cache.Has(3));
}

// ---------------------------------------------------------------- ClearObject

TEST(GPUPagedCacheTest, ClearObjectKeepsObjectAlive)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const int data[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data, 5));

    cache.ClearObject(1);
    EXPECT_TRUE(cache.Has(1));
    EXPECT_TRUE(Inspector::ReadObjectData(cache, 1).empty());

    // Clearing an already-empty object is safe.
    cache.ClearObject(1);
    EXPECT_TRUE(cache.Has(1));
}

TEST(GPUPagedCacheTest, ClearObjectNonExisting)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    EXPECT_NO_THROW(cache.ClearObject(999));
    EXPECT_FALSE(cache.Has(999));
}

TEST(GPUPagedCacheTest, AllocateObjectAfterClearObject)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const int initialData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, initialData, 5));
    cache.ClearObject(1);

    const int newData[] = { 9, 8, 7 };
    ASSERT_TRUE(cache.AllocateObject(1, newData, 3));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), std::vector<int>({ 9, 8, 7 }));
    ASSERT_EQ(cache.GetObjectBufferRanges(1).size(), 1u);
}

// ------------------------------------------------------------- PushBackToObject

TEST(GPUPagedCacheTest, PushBackCreatesMissingObject)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    ASSERT_TRUE(cache.PushBackToObject(1, 11));
    ASSERT_TRUE(cache.Has(1));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), std::vector<int>({ 11 }));
}

TEST(GPUPagedCacheTest, PushBackGrowsAcrossPageBoundary)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(2, 4)); // tiny pages: boundary every 2 atoms

    std::vector<int> expected;
    for (int i = 0; i < 7; ++i) // 4 pages worth minus one atom
    {
        ASSERT_TRUE(cache.PushBackToObject(1, i));
        expected.push_back(i);
    }
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), expected);

    Cache::ObjectAllocation alloc;
    ASSERT_TRUE(cache.Find(1, alloc));
    EXPECT_EQ(Inspector::CountAllocatedPages(cache, alloc), 4u);
}

// Regression: after ClearObject the element count rewinds but the chain keeps
// its pages, so the next PushBack must land in the FIRST chain page. The
// reference wrote to the end page — the appended element vanished from
// ReadObjectData (it read chain-ordered) while `Has` stayed true.
TEST(GPUPagedCacheTest, PushBackAfterClearWritesChainStart)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, kPageCount));

    const std::vector<int> data(7, 3); // 2 pages
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    cache.ClearObject(1);

    ASSERT_TRUE(cache.PushBackToObject(1, 77));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), std::vector<int>({ 77 }));
}

// ---------------------------------------------------------------- Move / Swap

TEST(GPUPagedCacheTest, MoveObjectBasic)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));

    cache.MoveObject(1, 2);
    EXPECT_FALSE(cache.Has(1));
    ASSERT_TRUE(cache.Has(2));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 2), data);
}

TEST(GPUPagedCacheTest, MoveObjectNonExistingSource)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    cache.MoveObject(123, 456);
    EXPECT_FALSE(cache.Has(123));
    EXPECT_FALSE(cache.Has(456));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 2u);
}

TEST(GPUPagedCacheTest, MoveObjectOntoExistingDestination)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 3));

    const std::vector<int> srcData = { 1, 2, 3, 4, 5 };
    const std::vector<int> dstData = { 9, 8, 7 };
    ASSERT_TRUE(cache.AllocateObject(1, srcData.data(), srcData.size()));
    ASSERT_TRUE(cache.AllocateObject(2, dstData.data(), dstData.size()));

    cache.MoveObject(1, 2);
    EXPECT_FALSE(cache.Has(1));
    ASSERT_TRUE(cache.Has(2));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 2), srcData);
    // The overwritten destination's page was freed.
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 2u);
}

// A moved object must stay evictable under FIFO. The reference kept the
// source's policy handle, whose queue entry named the OLD id — after the move
// nothing in the queue named the survivor, so it could never be evicted.
TEST(GPUPagedCacheTest, MovedObjectRemainsEvictable)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    cache.MoveObject(1, 2);
    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5));

    // Pressure: someone must be evicted, and object 2 is the only candidate
    // besides the requester.
    ASSERT_TRUE(cache.AllocateObject(4, pageData, 5));
    EXPECT_FALSE(cache.Has(2)) << "the moved object was invisible to the eviction policy";
    EXPECT_TRUE(cache.Has(4));
}

TEST(GPUPagedCacheTest, SwapSinglePageObjects)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const std::vector<int> data1 = { 1, 2, 3, 4, 5 };
    const std::vector<int> data2 = { 9, 8, 7 };
    ASSERT_TRUE(cache.AllocateObject(1, data1.data(), data1.size()));
    ASSERT_TRUE(cache.AllocateObject(2, data2.data(), data2.size()));

    cache.Swap(1, 2);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data2);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 2), data1);
}

TEST(GPUPagedCacheTest, SwapMultiPageObjects)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 6));

    const std::vector<int> data1(10, 1); // 2 pages
    const std::vector<int> data2(15, 2); // 3 pages
    ASSERT_TRUE(cache.AllocateObject(1, data1.data(), data1.size()));
    ASSERT_TRUE(cache.AllocateObject(2, data2.data(), data2.size()));

    cache.Swap(1, 2);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data2);
    EXPECT_EQ(Inspector::ReadObjectData(cache, 2), data1);
}

TEST(GPUPagedCacheTest, SwapNonExistingObject)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 3));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));

    cache.Swap(1, 999);
    ASSERT_TRUE(cache.Has(1));
    EXPECT_FALSE(cache.Has(999));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 2u);
}

TEST(GPUPagedCacheTest, SwapBothNonExistingObjects)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    cache.Swap(111, 222);
    EXPECT_FALSE(cache.Has(111));
    EXPECT_FALSE(cache.Has(222));
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), 2u);
}

TEST(GPUPagedCacheTest, SwapObjectWithItself)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));

    cache.Swap(1, 1);
    ASSERT_TRUE(cache.Has(1));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);
}

// -------------------------------------------------------- GetObjectBufferRanges

// Content-exact spans, not just counts: chain [1, 2, 0] where page 0 holds the
// PARTIAL data tail. Pages 0..2 are address-adjacent, so a naive contiguous
// merge reports one 5-atom span [0, 5) — but atoms 1..2 of page 0 are garbage
// (the tail wrote only one atom there). The partial page must END its span.
TEST(GPUPagedCacheTest, BufferRangesSplitAtThePartialPage)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(2, 3)); // pageSize 2, 3 pages

    // Object 1 takes page 0; object 2 then needs 3 pages: reserves {1, 2} and
    // evicts object 1 to take page 0 — chain [1, 2, 0].
    const int seed[] = { 7, 7 };
    ASSERT_TRUE(cache.AllocateObject(1, seed, 2));
    const std::vector<int> data = { 10, 11, 12, 13, 14 }; // 5 atoms = 2 full pages + 1
    ASSERT_TRUE(cache.AllocateObject(2, data.data(), data.size()));
    ASSERT_FALSE(cache.Has(1));
    ASSERT_EQ(Inspector::ChainPages(cache, 2), std::vector<u32>({ 1, 2, 0 }));

    using Range = Cache::BufferRange;
    const auto ranges = cache.GetObjectBufferRanges(2);
    const std::vector<Range> expected = {
        Range{ 0, 1 }, // page 0: the 1-atom data tail
        Range{ 2, 4 }, // pages 1-2: full, address-adjacent
    };
    EXPECT_EQ(ranges, expected);
}

TEST(GPUPagedCacheTest, BufferRangesForNonExistingObjectAreEmpty)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));
    EXPECT_TRUE(cache.GetObjectBufferRanges(404).empty());
}

// ------------------------------------------------------------- other policies

TEST(GPUPagedCacheTest, LruPolicyEvictsTheLeastRecentlyUsed)
{
    LruCache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 3));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5));

    // Touch 1 (the current LRU tail) so 2 becomes the coldest.
    (void)cache.GetObjectBufferRanges(1);

    ASSERT_TRUE(cache.AllocateObject(4, pageData, 5));
    EXPECT_TRUE(cache.Has(1));
    EXPECT_FALSE(cache.Has(2)) << "LRU must evict the coldest object";
    EXPECT_TRUE(cache.Has(3));
}

TEST(GPUPagedCacheTest, ClockPolicyEvictsAnUnreferencedObject)
{
    ClockCache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 3));

    const int pageData[] = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5));

    ASSERT_TRUE(cache.AllocateObject(4, pageData, 5));
    ASSERT_TRUE(cache.Has(4));

    const int survivors = (cache.Has(1) ? 1 : 0) + (cache.Has(2) ? 1 : 0) + (cache.Has(3) ? 1 : 0);
    EXPECT_EQ(survivors, 2) << "clock must evict exactly one object";
}

// -------------------------------------------------------------- stress tests

TEST(GPUPagedCacheTest, FullCacheChurnStressTest)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(4, 32));

    std::mt19937 rng(555);
    for (int cycle = 0; cycle < 100; ++cycle)
    {
        for (u32 i = 0; i < 64; ++i)
        {
            std::vector<int> data((rng() % 8) + 1);
            for (int& v : data)
            {
                v = static_cast<int>(rng());
            }
            ASSERT_TRUE(cache.AllocateObject(i, data.data(), data.size()));
            ASSERT_TRUE(cache.Has(i));
        }
        for (u32 i = 0; i < 64; ++i)
        {
            cache.DeallocateObject(i);
            ASSERT_FALSE(cache.Has(i));
        }
        ASSERT_EQ(Inspector::GetFreePagesCount(cache), 32u);
    }
}

TEST(GPUPagedCacheTest, RepeatedFragmentationReuseStressTest)
{
    Cache cache;
    constexpr sizet kStressPageSize = 8;
    constexpr u32 kStressPageCount = 64;
    ASSERT_TRUE(cache.Create(kStressPageSize, kStressPageCount));

    std::mt19937 rng(777);
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        const u32 objectID = rng() % 128;
        if ((rng() % 3) == 0)
        {
            cache.DeallocateObject(objectID);
            ASSERT_FALSE(cache.Has(objectID));
        }
        else
        {
            std::vector<int> data((rng() % (kStressPageSize * 5)) + 1);
            for (int& v : data)
            {
                v = static_cast<int>(rng());
            }
            ASSERT_TRUE(cache.AllocateObject(objectID, data.data(), data.size()));
            ASSERT_TRUE(cache.Has(objectID));
            ASSERT_EQ(Inspector::ReadObjectData(cache, objectID), data);
        }
    }
    ASSERT_LE(Inspector::GetFreePagesCount(cache), kStressPageCount);
}

TEST(GPUPagedCacheTest, LargeObjectChurnStressTest)
{
    Cache cache;
    constexpr sizet kStressPageSize = 64;
    ASSERT_TRUE(cache.Create(kStressPageSize, 256));

    std::mt19937 rng(9001);
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        const u32 objectID = static_cast<u32>(iteration % 32);
        std::vector<int> data(kStressPageSize * ((rng() % 16) + 1));
        for (int& v : data)
        {
            v = static_cast<int>(rng());
        }
        ASSERT_TRUE(cache.AllocateObject(objectID, data.data(), data.size()));
        ASSERT_EQ(Inspector::ReadObjectData(cache, objectID), data);

        if ((iteration % 3) == 0)
        {
            cache.DeallocateObject(objectID);
            ASSERT_FALSE(cache.Has(objectID));
        }
    }
}

// The dst-exists branch of MoveObject frees the destination's chain — without
// a self-move guard that chain is the object's OWN live chain, and the policy
// node gets removed twice.
TEST(GPUPagedCacheTest, MoveObjectOntoItselfIsANoOp)
{
    Cache cache;
    ASSERT_TRUE(cache.Create(kPageSize, 2));

    const std::vector<int> data = { 1, 2, 3, 4, 5 };
    ASSERT_TRUE(cache.AllocateObject(1, data.data(), data.size()));
    const u32 freeBefore = Inspector::GetFreePagesCount(cache);

    cache.MoveObject(1, 1);

    ASSERT_TRUE(cache.Has(1));
    EXPECT_EQ(Inspector::ReadObjectData(cache, 1), data);
    EXPECT_EQ(Inspector::GetFreePagesCount(cache), freeBefore);

    // The policy must still see the object: pressure evicts it, not a hang.
    const int pageData[] = { 9, 9, 9, 9, 9 };
    ASSERT_TRUE(cache.AllocateObject(2, pageData, 5));
    ASSERT_TRUE(cache.AllocateObject(3, pageData, 5));
    EXPECT_FALSE(cache.Has(1)) << "self-move corrupted the eviction policy's view of the object";
}

// Lowest-index-first is a DOCUMENTED guarantee of ReserveUpToPages (consumers
// derive deterministic layout from it — VirtualMeshRegistry's slot order), so
// it gets pinned here against scan "optimisations" that would reorder it.
TEST(GPUPagedCacheTest, ReserveUpToPagesClaimsLowestIndicesFirst)
{
    GPUPagedBuffer<int> buffer;
    ASSERT_TRUE(buffer.Create(1, 8, GPUCacheBacking::HostOnly));

    std::vector<u32> pages;
    ASSERT_EQ(buffer.ReserveUpToPages(5, pages), 5u);
    EXPECT_EQ(pages, std::vector<u32>({ 0, 1, 2, 3, 4 }));

    buffer.FreePage(3);
    buffer.FreePage(1);

    pages.clear();
    ASSERT_EQ(buffer.ReserveUpToPages(2, pages), 2u);
    EXPECT_EQ(pages, std::vector<u32>({ 1, 3 })) << "freed pages must be re-claimed in ascending order";

    // A short claim reports how much it got and keeps the claim.
    pages.clear();
    EXPECT_EQ(buffer.ReserveUpToPages(9, pages), 3u); // only 5..7 remain
    EXPECT_EQ(pages, std::vector<u32>({ 5, 6, 7 }));
}
