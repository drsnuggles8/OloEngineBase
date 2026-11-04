// OLO_TEST_LAYER: unit
//
// FLockFreeLinkPolicy hands out links from a fixed index space through a
// per-thread cache of up to two 64-link bundles. A thread that exits must hand
// its cache back: a cache stranded by every exited scheduler worker is index
// space carved again by the next thread, and an exhausted index space stops
// the process (LockFreeLinksExhausted).
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/LockFreeList.h"

#include <thread>
#include <vector>

namespace
{
    using namespace OloEngine;

    constexpr i32 kPerBundle = 64;

    // The next index the link allocator would carve. Costs one link.
    [[nodiscard]] u32 HighWater()
    {
        return FLockFreeLinkPolicy::s_LinkAllocator.Alloc(1);
    }
} // namespace

TEST(LockFreeLinkThreadExit, ThreadsThatExitDoNotKeepCarvingTheIndexSpace)
{
    // Each thread takes more links than one bundle holds and frees them all, so
    // it exits holding a full bundle and a partial one.
    constexpr i32 kThreads = 50;
    constexpr i32 kLinksPerThread = 3 * kPerBundle + kPerBundle / 2;

    const u32 before = HighWater();
    for (i32 t = 0; t < kThreads; ++t)
    {
        std::thread thread(
            []
            {
                std::vector<u32> links;
                for (i32 i = 0; i < kLinksPerThread; ++i)
                {
                    links.push_back(FLockFreeLinkPolicy::AllocLockFreeLink());
                }
                for (const u32 link : links)
                {
                    FLockFreeLinkPolicy::FreeLockFreeLink(link);
                }
            });
        thread.join();
    }
    const u32 carved = HighWater() - before - 1;

    // Handed back, the first thread's links serve every later one: about four
    // bundles in all. Stranded, every thread carves its two cached bundles anew.
    EXPECT_LT(carved, static_cast<u32>(10 * kPerBundle))
        << carved << " links carved by " << kThreads << " threads that each needed at most " << kLinksPerThread;
}
