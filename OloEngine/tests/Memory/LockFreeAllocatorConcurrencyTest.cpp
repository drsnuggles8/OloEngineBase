/**
 * @file LockFreeAllocatorConcurrencyTest.cpp
 * @brief Concurrency regression test for TLockFreeAllocOnceIndexedAllocator (issue #350)
 *
 * Guards the memory-order hardening in LockFreeList.h: the page pointers in
 * TLockFreeAllocOnceIndexedAllocator are published with a release CAS
 * (GetRawItem) but were historically *consumed* with memory_order_relaxed loads
 * whose result is dereferenced. That is a formal C++ memory-model data race —
 * benign on x86-64 (TSO), but UB by the standard, and ThreadSanitizer flags it
 * intermittently, reddening the gating tsan-linux job in asan.yml. The fix
 * promotes the two dereferenced loads (GetItem return + GetRawItem return) to
 * memory_order_acquire so they pair with the publishing CAS.
 *
 * The test does not assert the absence of a race directly — TSan does that in
 * CI. It exists so the race is *exercised* under the sanitizer: many threads
 * hammer the allocator so several first-touch the same page at once. One wins
 * the publishing CAS; the losers take the consume load in GetRawItem and
 * construct into the page. That is exactly the reported race — TSan saw a page
 * malloc on one thread (GetRawItem, before the publish CAS) racing a
 * placement-new on another (Alloc, after a relaxed consume). With the acquire
 * fix the malloc happens-before the construct, so TSan is clean; weaken it back
 * to relaxed and TSan reddens here again. On x86 the test also verifies the
 * allocator's functional contract (disjoint indices, valid, dereferenceable
 * items).
 *
 * Why there is no separate cross-thread *GetItem* test: GetItem's acquire load
 * pairs with the publishing CAS, so it orders only what happened *before* the
 * page was published. The link *contents* a consumer would read (the constructed
 * item, a stored Payload) are written *after* the publish, so reading them
 * across threads needs its own happens-before edge — a relaxed handoff of the
 * index is itself UB (TSan flags the construct-vs-read, not anything the
 * allocator's ordering controls), and a release/acquire handoff would mask
 * GetItem's ordering entirely. GetItem's L147 load is the identical pattern to
 * GetRawItem's L177 load and is fixed for symmetry/completeness (see the issue);
 * the reproducible race lives on the Alloc/GetRawItem path covered below.
 *
 * Intentionally drives the real FLockFreeLinkPolicy::s_LinkAllocator singleton
 * rather than a local allocator instance: TLockFreeAllocOnceIndexedAllocator
 * never frees its pages (alloc-once, by design — matches UE5.8), so a local
 * instance would leak its pages on destruction and trip LeakSanitizer in the
 * asan-lsan-linux job. The singleton's pages stay reachable via its static
 * m_Pages array at exit, so LSan does not report them — and this is the actual
 * production allocation path.
 */

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/LockFreeList.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace OloEngine;

namespace
{
    // Number of worker threads. Oversubscription (more than physical cores)
    // increases the odds of two threads being mid-GetRawItem on the same fresh
    // page simultaneously, which is the window the fix protects.
    u32 WorkerThreadCount()
    {
        u32 Hw = std::thread::hardware_concurrency();
        if (Hw < 4u)
        {
            Hw = 4u;
        }
        return std::min(Hw, 8u);
    }

    // Start gate: every worker blocks until all of them have arrived, then they
    // are released together. Maximises the burst of concurrent first-touch page
    // allocations (page 0 first, then 1, 2, ... as the shared index advances).
    class StartGate
    {
      public:
        explicit StartGate(u32 Expected)
            : m_Expected(Expected)
        {
        }

        // Called by each worker: register arrival, then spin until released.
        void Wait()
        {
            m_Arrived.fetch_add(1, std::memory_order_acq_rel);
            while (!m_Go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }

        // Called by the orchestrating thread once all workers exist.
        void ReleaseWhenAllArrived()
        {
            while (m_Arrived.load(std::memory_order_acquire) < m_Expected)
            {
                std::this_thread::yield();
            }
            m_Go.store(true, std::memory_order_release);
        }

      private:
        const u32 m_Expected;
        std::atomic<u32> m_Arrived{ 0 };
        std::atomic<bool> m_Go{ false };
    };
} // namespace

class LockFreeAllocatorConcurrencyTest : public ::testing::Test
{
};

// Many threads allocate from the shared indexed allocator at the same instant.
// FLockFreeLinkPolicy::s_LinkAllocator uses ItemsPerPage = 16384, so the first
// ~16k allocations land in page 0: with a synchronized start, several threads
// are inside GetRawItem first-touching page 0 concurrently — one publishes via
// CAS, the rest consume the published pointer (the load the fix makes acquire)
// and construct into the page. As the index advances, pages 1, 2, ... get
// first-touched the same way while other threads are still in flight.
//
// Under TSan with relaxed consume loads this reddens; with acquire it is clean.
// On x86 it asserts the allocator's contract: every index is unique (disjoint
// fetch_add ranges), non-zero, and dereferences to a distinct valid item.
TEST_F(LockFreeAllocatorConcurrencyTest, ConcurrentAllocFirstTouchesPagesWithoutRace)
{
    const u32 NumThreads = WorkerThreadCount();
    // ~10 pages of 16384 across all threads, so first-touch races recur well
    // past the initial page-0 burst without making the test slow under TSan.
    constexpr u32 AllocsPerThread = 20000;

    StartGate Gate(NumThreads);
    std::vector<std::vector<u32>> PerThreadIndices(NumThreads);
    std::vector<std::thread> Threads;
    Threads.reserve(NumThreads);

    for (u32 T = 0; T < NumThreads; ++T)
    {
        std::vector<u32>& MyIndices = PerThreadIndices[T];
        MyIndices.reserve(AllocsPerThread);
        Threads.emplace_back(
            [&Gate, &MyIndices, T]
            {
                Gate.Wait();
                for (u32 I = 0; I < AllocsPerThread; ++I)
                {
                    u32 Index = FLockFreeLinkPolicy::s_LinkAllocator.Alloc(1);
                    // Dereference via GetItem (the load promoted to acquire at
                    // line 147) and touch the page so the consume actually
                    // accesses page memory, not just the pointer.
                    FIndexedLockFreeLink* Link = FLockFreeLinkPolicy::s_LinkAllocator.GetItem(Index);
                    ASSERT_NE(Link, nullptr);
                    Link->Payload.store(reinterpret_cast<void*>(static_cast<uptr>(Index)),
                                        std::memory_order_relaxed);
                    MyIndices.push_back(Index);
                }
            });
    }

    Gate.ReleaseWhenAllArrived();
    for (std::thread& Th : Threads)
    {
        Th.join();
    }

    // Functional contract (x86): all indices distinct, non-zero, and the
    // sentinel we wrote round-trips through a fresh GetItem.
    std::vector<u32> AllIndices;
    AllIndices.reserve(static_cast<sizet>(NumThreads) * AllocsPerThread);
    for (const std::vector<u32>& Indices : PerThreadIndices)
    {
        EXPECT_EQ(Indices.size(), AllocsPerThread);
        for (u32 Index : Indices)
        {
            EXPECT_NE(Index, 0u);
            FIndexedLockFreeLink* Link = FLockFreeLinkPolicy::s_LinkAllocator.GetItem(Index);
            ASSERT_NE(Link, nullptr);
            EXPECT_EQ(reinterpret_cast<uptr>(Link->Payload.load(std::memory_order_relaxed)),
                      static_cast<uptr>(Index));
        }
        AllIndices.insert(AllIndices.end(), Indices.begin(), Indices.end());
    }

    std::sort(AllIndices.begin(), AllIndices.end());
    const auto Duplicate = std::adjacent_find(AllIndices.begin(), AllIndices.end());
    EXPECT_EQ(Duplicate, AllIndices.end()) << "fetch_add handed the same index to two threads";
    EXPECT_EQ(AllIndices.size(), static_cast<sizet>(NumThreads) * AllocsPerThread);
}
