// =============================================================================
// UUIDTest.cpp
//
// Guards Core/UUID.{h,cpp}: `UUID()` is called from more than one thread, so it
// must be safe to call concurrently and must never hand two callers the same
// value (issue #1420).
//
// The old generator was an unsynchronised mt19937_64. Parallel mesh submission
// default-constructed UUIDs on every worker, and TSan caught the race in
// PR #1419. `ConcurrentDrawsAreUniqueAndNeverZero` is the case the Linux TSan
// job runs; on a build without TSan it also checks the returned values for
// duplicates, although a data race itself requires TSan to detect reliably.
// =============================================================================

// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"

#include "OloEngine/Core/UUID.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <latch>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // A UUID constant must stay constant-initialisable (issue #763). This is a
    // compile error, not a runtime check, if UUID(u64) stops being constexpr.
    constinit const UUID s_ConstantUUID{ 0x1420u };
} // namespace

TEST(UUIDGenerator, ValueConstructorDoesNotDraw)
{
    const u64 before = UUID::GetDrawCountOnThisThread();
    const UUID fromValue{ 42u };
    const UUID copied = fromValue;

    EXPECT_EQ(UUID::GetDrawCountOnThisThread(), before);
    EXPECT_EQ(static_cast<u64>(copied), 42u);
    EXPECT_EQ(static_cast<u64>(s_ConstantUUID), 0x1420u);
}

TEST(UUIDGenerator, EachDefaultConstructionDrawsOnTheCallingThread)
{
    const u64 before = UUID::GetDrawCountOnThisThread();
    const UUID a;
    const UUID b;

    // At least 2, not exactly: a draw that lands on 0 is redrawn.
    EXPECT_GE(UUID::GetDrawCountOnThisThread() - before, 2u);
    EXPECT_NE(static_cast<u64>(a), static_cast<u64>(b));
}

TEST(UUIDGenerator, ConcurrentDrawsAreUniqueAndNeverZero)
{
    constexpr u32 threadCount = 8;
    constexpr u32 drawsPerThread = 100'000;

    std::vector<std::vector<u64>> perThread(threadCount);
    std::vector<u64> drawCounts(threadCount, 0);
    std::latch start(threadCount);

    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (u32 t = 0; t < threadCount; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                std::vector<u64>& out = perThread[t];
                out.reserve(drawsPerThread);
                // Every thread starts drawing at once, so the draws interleave
                // instead of running one thread after another.
                start.arrive_and_wait();
                const u64 before = UUID::GetDrawCountOnThisThread();
                for (u32 i = 0; i < drawsPerThread; ++i)
                {
                    out.push_back(static_cast<u64>(UUID()));
                }
                drawCounts[t] = UUID::GetDrawCountOnThisThread() - before;
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    std::vector<u64> all;
    all.reserve(static_cast<sizet>(threadCount) * drawsPerThread);
    for (u32 t = 0; t < threadCount; ++t)
    {
        EXPECT_GE(drawCounts[t], drawsPerThread) << "thread " << t << " did not count its own draws";
        all.insert(all.end(), perThread[t].begin(), perThread[t].end());
    }
    ASSERT_EQ(all.size(), static_cast<sizet>(threadCount) * drawsPerThread);

    EXPECT_EQ(std::ranges::count(all, u64{ 0 }), 0) << "a fresh UUID must never be the 0 'no handle' value";

    std::ranges::sort(all);
    sizet repeats = 0;
    for (sizet i = 1; i < all.size(); ++i)
    {
        repeats += all[i] == all[i - 1] ? 1u : 0u;
    }
    const auto firstRepeat = std::ranges::adjacent_find(all);
    EXPECT_EQ(repeats, 0u) << "concurrent UUID() calls returned a value more than once; first repeated value 0x" << std::hex
                           << (firstRepeat != all.end() ? *firstRepeat : 0u);
}
