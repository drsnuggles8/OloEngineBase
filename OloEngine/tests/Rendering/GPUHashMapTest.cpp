// OLO_TEST_LAYER: plumbing
// =============================================================================
// GPUHashMapTest.cpp — the lock-free, GPU-layout hash map of the paged-cache
// substrate (issue #704, acceptance criterion 3: "hash collision" coverage,
// headless).
//
// Every test here runs with the HostOnly backing — the probing/CAS logic is
// byte-identical to what the GPU-visible (HostMirrored) configuration runs,
// the bytes just live on the heap, so no GL context is needed and nothing
// skips. The shader-side view of the same table is covered by
// GPUCacheShaderResolveTest.cpp (shaderpipe).
//
// Ported from the VoxelEngine reference's Tests/gpu_lockfree_hash_map_tests.cpp,
// plus regression tests for the sharp edges the port fixed:
//   * a non-power-of-two capacity left the rounded-up tail of the table
//     UNINITIALISED in the reference (Create initialised [0, capacity) but
//     allocated bit_ceil(capacity) entries);
//   * an Insert whose CAS lost the race to another thread claiming the SAME
//     key missed the duplicate and inserted a second entry further down the
//     probe sequence;
//   * an update-Insert that claimed a tombstone AHEAD of the key's live entry,
//     duplicating it (one Erase then resurrected the stale copy);
//   * tombstone accumulation, answered by NeedsCompaction/CompactTombstones.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUCache/GPUHashMap.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    using Map = GPUHashMap<u64, u32>;
} // namespace

TEST(GPUHashMapTest, CreateRoundsCapacityToPowerOfTwo)
{
    Map map;
    ASSERT_TRUE(map.Create(96));
    EXPECT_EQ(map.GetCapacity(), 128u);
}

TEST(GPUHashMapTest, CreateWithZeroCapacityFails)
{
    Map map;
    EXPECT_FALSE(map.Create(0));
    EXPECT_FALSE(map.IsCreated());
}

TEST(GPUHashMapTest, InsertAndFind)
{
    Map map;
    ASSERT_TRUE(map.Create(128));

    EXPECT_TRUE(map.Insert(1, 42));

    u32 value = 0;
    EXPECT_TRUE(map.Find(1, value));
    EXPECT_EQ(value, 42u);
}

TEST(GPUHashMapTest, ContainsKey)
{
    Map map;
    ASSERT_TRUE(map.Create(128));

    EXPECT_FALSE(map.Contains(5));
    EXPECT_TRUE(map.Insert(5, 99));
    EXPECT_TRUE(map.Contains(5));
}

TEST(GPUHashMapTest, FindNonExistentKey)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    u32 value = 0;
    EXPECT_FALSE(map.Find(999, value));
}

// Regression for the reference's uninitialised-tail bug: with capacity 96 the
// table is 128 entries, and entries [96, 128) must read as EMPTY, not as
// whatever the allocation held. Nothing is inserted, so every lookup must
// miss — including keys whose probe sequence starts inside the rounded tail.
TEST(GPUHashMapTest, NonPowerOfTwoCapacityTailReadsEmpty)
{
    Map map;
    ASSERT_TRUE(map.Create(96));

    for (u64 key = 0; key < 4096; ++key)
    {
        EXPECT_FALSE(map.Contains(key)) << "phantom entry for key " << key;
    }
}

TEST(GPUHashMapTest, EraseKey)
{
    Map map;
    ASSERT_TRUE(map.Create(128));

    EXPECT_TRUE(map.Insert(7, 77));
    EXPECT_TRUE(map.Contains(7));

    EXPECT_TRUE(map.Erase(7));
    EXPECT_FALSE(map.Contains(7));

    u32 value = 0;
    EXPECT_FALSE(map.Find(7, value));
}

TEST(GPUHashMapTest, EraseNonExistentKeyReturnsFalse)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    EXPECT_FALSE(map.Erase(123));
}

TEST(GPUHashMapTest, InsertMultipleKeys)
{
    Map map;
    ASSERT_TRUE(map.Create(256));

    for (u64 i = 0; i < 100; ++i)
    {
        EXPECT_TRUE(map.Insert(i, static_cast<u32>(i * 10)));
    }
    for (u64 i = 0; i < 100; ++i)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(i, value));
        EXPECT_EQ(value, static_cast<u32>(i * 10));
    }
}

TEST(GPUHashMapTest, InsertEraseInsert)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    EXPECT_TRUE(map.Insert(1, 10));
    EXPECT_TRUE(map.Erase(1));
    EXPECT_TRUE(map.Insert(1, 20));

    u32 value = 0;
    EXPECT_TRUE(map.Find(1, value));
    EXPECT_EQ(value, 20u);
}

TEST(GPUHashMapTest, DuplicateInsertOverwrites)
{
    Map map;
    ASSERT_TRUE(map.Create(128));

    EXPECT_TRUE(map.Insert(10, 1));
    EXPECT_TRUE(map.Insert(10, 2));

    u32 value = 0;
    EXPECT_TRUE(map.Find(10, value));
    EXPECT_EQ(value, 2u);
}

// A probe that crosses a tombstone must keep going: erase a key, and every
// key inserted after/around it must still be findable. Exercised densely so
// probe sequences genuinely cross the tombstones.
TEST(GPUHashMapTest, LookupsCrossTombstones)
{
    Map map;
    ASSERT_TRUE(map.Create(16)); // small: probes collide constantly

    for (u64 i = 0; i < 12; ++i)
    {
        ASSERT_TRUE(map.Insert(i, static_cast<u32>(i)));
    }
    // Punch holes.
    for (u64 i = 0; i < 12; i += 3)
    {
        ASSERT_TRUE(map.Erase(i));
    }
    for (u64 i = 0; i < 12; ++i)
    {
        u32 value = 0;
        if (i % 3 == 0)
        {
            EXPECT_FALSE(map.Find(i, value)) << "erased key " << i << " resurfaced";
        }
        else
        {
            EXPECT_TRUE(map.Find(i, value)) << "key " << i << " lost behind a tombstone";
            EXPECT_EQ(value, static_cast<u32>(i));
        }
    }
    // Tombstoned slots are reusable.
    for (u64 i = 0; i < 12; i += 3)
    {
        EXPECT_TRUE(map.Insert(i, static_cast<u32>(i + 100)));
    }
    for (u64 i = 0; i < 12; i += 3)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(i, value));
        EXPECT_EQ(value, static_cast<u32>(i + 100));
    }
}

TEST(GPUHashMapTest, InsertIntoFullTableReturnsFalse)
{
    Map map;
    ASSERT_TRUE(map.Create(8));
    ASSERT_EQ(map.GetCapacity(), 8u);

    for (u64 i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(map.Insert(i, static_cast<u32>(i)));
    }
    EXPECT_FALSE(map.Insert(1000, 0)) << "a full table must refuse, not spin or overwrite";

    // Nothing was disturbed.
    for (u64 i = 0; i < 8; ++i)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(i, value));
        EXPECT_EQ(value, static_cast<u32>(i));
    }
}

TEST(GPUHashMapTest, DenseFillForcesCollisions)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    // 60 of 64 slots occupied: nearly every probe sequence collides multiple
    // times, so this exercises wrap-around and long probe runs.
    for (u64 i = 0; i < 60; ++i)
    {
        EXPECT_TRUE(map.Insert(i * 7919, static_cast<u32>(i)));
    }
    for (u64 i = 0; i < 60; ++i)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(i * 7919, value));
        EXPECT_EQ(value, static_cast<u32>(i));
    }
}

// NOTE for all the multi-threaded tests below: gtest assertions are NOT
// thread-safe on Windows, so worker threads only count failures into an
// atomic; the main thread asserts the count.
TEST(GPUHashMapTest, MultiThreadedInsertDistinctKeys)
{
    Map map;
    ASSERT_TRUE(map.Create(16384));

    constexpr int kThreadCount = 8;
    constexpr int kInsertsPerThread = 500;
    std::atomic<int> workerFailures{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back(
            [&map, &workerFailures, t]()
            {
                for (int i = 0; i < kInsertsPerThread; ++i)
                {
                    const auto key = static_cast<u64>(t * kInsertsPerThread + i);
                    if (!map.Insert(key, static_cast<u32>(key * 10)))
                    {
                        workerFailures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(workerFailures.load(), 0);

    for (u64 i = 0; i < static_cast<u64>(kThreadCount) * kInsertsPerThread; ++i)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(i, value));
        EXPECT_EQ(value, static_cast<u32>(i * 10));
    }
}

// All threads hammer the SAME key. The claim CAS must produce exactly one
// live entry (any thread's value) — the reference could produce a duplicate
// entry when the claiming CAS lost the race to another thread inserting the
// same key. Verified by erasing once: a duplicate would still be findable.
TEST(GPUHashMapTest, MultiThreadedDuplicateInsertLeavesOneEntry)
{
    Map map;
    ASSERT_TRUE(map.Create(512));

    constexpr int kThreadCount = 16;
    std::atomic<int> workerFailures{ 0 };
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back(
            [&map, &workerFailures, t]()
            {
                for (int i = 0; i < 100; ++i)
                {
                    if (!map.Insert(42, static_cast<u32>(t)))
                    {
                        workerFailures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(workerFailures.load(), 0);

    u32 value = 0;
    EXPECT_TRUE(map.Find(42, value));
    EXPECT_LT(value, static_cast<u32>(kThreadCount));

    EXPECT_TRUE(map.Erase(42));
    EXPECT_FALSE(map.Contains(42)) << "duplicate entry for the contended key survived one erase";
}

TEST(GPUHashMapTest, MultiThreadedStressWithErases)
{
    Map map;
    ASSERT_TRUE(map.Create(16384));

    const int threadCount = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
    constexpr int kOperations = 20000;
    constexpr int kKeySpace = 2000;
    std::atomic<int> workerFailures{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<sizet>(threadCount));
    for (int t = 0; t < threadCount; ++t)
    {
        threads.emplace_back(
            [&map, &workerFailures, t]()
            {
                std::mt19937 rng(static_cast<u32>(t));
                std::uniform_int_distribution<int> dist(0, kKeySpace);
                for (int i = 0; i < kOperations; ++i)
                {
                    const auto key = static_cast<u64>(dist(rng));
                    (void)map.Insert(key, static_cast<u32>(key * 2));
                    if (u32 value = 0; map.Find(key, value) && value != static_cast<u32>(key * 2))
                    {
                        workerFailures.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (i % 5 == 0)
                    {
                        (void)map.Erase(key);
                    }
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(workerFailures.load(), 0);

    // Final consistency: every surviving key still maps to its derived value.
    for (u64 key = 0; key <= kKeySpace; ++key)
    {
        if (u32 value = 0; map.Find(key, value))
        {
            EXPECT_EQ(value, static_cast<u32>(key * 2));
        }
    }
}

// =============================================================================
// Regressions for the tombstone-aware Insert (the reference claimed the first
// tombstone it probed WITHOUT checking whether the key already lived further
// along the chain — an update-insert then created a duplicate entry, and Erase
// removed only the first copy, resurrecting the stale one).
// =============================================================================

namespace
{
    // Two distinct keys whose probe sequences START in the same slot of a
    // `capacity`-entry table, found by brute force over the real hash — the
    // scenario needs a genuine collision, not an assumed one.
    std::pair<u64, u64> FindCollidingKeys(u32 capacity)
    {
        const u64 mask = capacity - 1;
        std::vector<u64> firstKeyForSlot(capacity, 0);
        for (u64 key = 1;; ++key)
        {
            const auto slot = static_cast<sizet>(Map::HashKey(key) & mask);
            if (firstKeyForSlot[slot] != 0)
            {
                return { firstKeyForSlot[slot], key };
            }
            firstKeyForSlot[slot] = key;
        }
    }
} // namespace

TEST(GPUHashMapTest, UpdateInsertAcrossTombstoneDoesNotDuplicate)
{
    Map map;
    ASSERT_TRUE(map.Create(8));
    const auto [k1, k2] = FindCollidingKeys(map.GetCapacity());

    ASSERT_TRUE(map.Insert(k1, 1)); // lands at the shared start slot
    ASSERT_TRUE(map.Insert(k2, 2)); // probes past k1
    ASSERT_TRUE(map.Erase(k1));     // tombstone AHEAD of k2's entry

    // The update-insert must find k2 beyond the tombstone, not claim the
    // tombstone as a second home for it.
    ASSERT_TRUE(map.Insert(k2, 22));
    u32 value = 0;
    ASSERT_TRUE(map.Find(k2, value));
    EXPECT_EQ(value, 22u);

    // One erase must remove the key entirely — a duplicate would survive it.
    ASSERT_TRUE(map.Erase(k2));
    EXPECT_FALSE(map.Contains(k2)) << "a duplicate entry resurrected the erased key";
}

TEST(GPUHashMapTest, MutateUpdatesInPlaceAndSkipsAbsentKeys)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    ASSERT_TRUE(map.Insert(7, 70));
    EXPECT_TRUE(map.Mutate(7, [](u32& value)
                           { value += 1; }));

    u32 value = 0;
    ASSERT_TRUE(map.Find(7, value));
    EXPECT_EQ(value, 71u);

    bool called = false;
    EXPECT_FALSE(map.Mutate(8, [&called](u32&)
                            { called = true; }));
    EXPECT_FALSE(called) << "Mutate must not invoke the callback for an absent key";
}

// Tombstones are a non-renewable poison for probe lengths (misses only stop at
// EMPTY slots); CompactTombstones rebuilds the table without them, preserving
// every live entry.
TEST(GPUHashMapTest, CompactTombstonesPreservesLiveEntries)
{
    Map map;
    ASSERT_TRUE(map.Create(64));

    // Persistent residents that must survive the rebuild.
    for (u64 key = 5000; key < 5010; ++key)
    {
        ASSERT_TRUE(map.Insert(key, static_cast<u32>(key)));
    }

    // Insert/erase churn mints tombstones faster than probe paths reuse them.
    bool sawNeedsCompaction = false;
    for (u64 key = 0; key < 1000; ++key)
    {
        ASSERT_TRUE(map.Insert(key, static_cast<u32>(key * 3)));
        ASSERT_TRUE(map.Erase(key));
        sawNeedsCompaction = sawNeedsCompaction || map.NeedsCompaction();
    }
    EXPECT_TRUE(sawNeedsCompaction) << "churn never tripped the compaction threshold — the test is vacuous";

    map.CompactTombstones();
    EXPECT_EQ(map.GetTombstoneCount(), 0u);

    for (u64 key = 5000; key < 5010; ++key)
    {
        u32 value = 0;
        EXPECT_TRUE(map.Find(key, value)) << "live key " << key << " lost in compaction";
        EXPECT_EQ(value, static_cast<u32>(key));
    }
    for (u64 key = 0; key < 1000; ++key)
    {
        EXPECT_FALSE(map.Contains(key)) << "erased key " << key << " resurrected by compaction";
    }
}

// The GLSL side of the lookup contract is a hand-mirrored file, and the GPU
// test that executes it needs a device — so this HEADLESS text pin is what
// keeps CI honest about drift: the include must carry the exact hash constants
// and sentinels the C++ side uses.
TEST(GPUHashMapTest, GlslResolveContractMatchesCppConstants)
{
    namespace fs = std::filesystem;
    fs::path candidate = fs::current_path();
    fs::path includePath;
    for (int depth = 0; depth < 6; ++depth)
    {
        // The GPU fixtures chdir into OloEditor/, so probe both spellings.
        for (const char* relative : { "assets/shaders/include/GPUCacheResolve.glsl",
                                      "OloEditor/assets/shaders/include/GPUCacheResolve.glsl" })
        {
            if (fs::exists(candidate / relative))
            {
                includePath = candidate / relative;
                break;
            }
        }
        if (!includePath.empty() || !candidate.has_parent_path() || candidate == candidate.parent_path())
        {
            break;
        }
        candidate = candidate.parent_path();
    }
    ASSERT_FALSE(includePath.empty()) << "GPUCacheResolve.glsl not found from " << fs::current_path();

    std::ifstream file(includePath);
    ASSERT_TRUE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    // The MurmurHash3 finalizer, exactly as Hash::Hash64 (Core/Hash.h) has it.
    EXPECT_NE(source.find("0xff51afd7ed558ccdUL"), std::string::npos);
    EXPECT_NE(source.find("0xc4ceb9fe1a85ec53UL"), std::string::npos);
    EXPECT_NE(source.find(">> 33"), std::string::npos);

    // The key sentinels and the chain terminator, as literals the GLSL uses.
    static_assert(Map::kEmptyKey == 0xFFFFFFFFFFFFFFFFull);
    static_assert(Map::kTombstoneKey == 0xFFFFFFFFFFFFFFFEull);
    EXPECT_NE(source.find("0xFFFFFFFFFFFFFFFFUL"), std::string::npos);
    EXPECT_NE(source.find("0xFFFFFFFFFFFFFFFEUL"), std::string::npos);
    EXPECT_NE(source.find("OLO_GPU_CACHE_INVALID_PAGE = 0xFFFFFFFFu"), std::string::npos);

    // And the C++ hash really is the canonical engine one.
    EXPECT_EQ(Map::HashKey(0x0123456789ABCDEFull), Hash::Hash64(0x0123456789ABCDEFull));
}
