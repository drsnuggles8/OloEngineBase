// @file LockFreeList.cpp
// @brief Implementation of lock-free list utilities
//
// Contains:
// - Error handlers for lock-free list edge cases
// - TLS-cached link allocator for high-performance allocation
// - Static member definitions for FLockFreeLinkPolicy
// - Critical stall testing for livelock detection
// - Memory statistics tracking
//
// Ported from Unreal Engine's LockFreeList.cpp

#include "OloEnginePCH.h"
#include "OloEngine/Memory/LockFreeList.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/PlatformTLS.h"

#include <random>
#include <thread>

namespace OloEngine
{
    // ========================================================================
    // Critical Stall Testing
    // ========================================================================

#if !OLO_DIST
    void DoTestCriticalStall()
    {
        // Thread-local random generator for stall testing
        thread_local std::mt19937 rng(static_cast<unsigned int>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);

        float Test = dist(rng);
        if (Test < 0.001f)
        {
            // Very rare: 0.1% chance of 1ms sleep
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else if (Test < 0.01f)
        {
            // Rare: 1% chance of yield (0ms sleep)
            std::this_thread::yield();
        }
        else
        {
            // No additional handling required.
        }
    }

    i32 GTestCriticalStalls = 0;
#endif

    // ========================================================================
    // Error Handlers
    // ========================================================================

    void LockFreeTagCounterHasOverflowed()
    {
        // This is not expected to be a problem and it is not expected to happen very often.
        // When it does happen, we will sleep as an extra precaution.
        OLO_CORE_INFO("LockFreeList: Tag counter has overflowed (not a problem)");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void LockFreeLinksExhausted(u32 TotalNum)
    {
        OLO_CORE_FATAL("LockFreeList: Consumed {0} lock-free links; there are no more.", TotalNum);
    }

    void* LockFreeAllocLinks(sizet AllocSize)
    {
        // ChangeMem(static_cast<i64>(AllocSize)); // Disabled - not safe during lock-free ops
        void* Ptr = FMemory::Malloc(AllocSize, OLO_PLATFORM_CACHE_LINE_SIZE);
        checkLockFreePointerList(Ptr != nullptr);
        return Ptr;
    }

    void LockFreeFreeLinks(sizet AllocSize, void* Ptr)
    {
        // ChangeMem(-static_cast<i64>(AllocSize)); // Disabled - not safe during lock-free ops
        (void)AllocSize;
        FMemory::Free(Ptr);
    }

    // ========================================================================
    // TLS-Based Link Allocator Cache
    // ========================================================================

    // @class LockFreeLinkAllocator_TLSCache
    // @brief Thread-local caching layer for lock-free link allocation
    //
    // Each thread maintains a bundle of 64 links to reduce contention
    // on the global free list.
    //
    // Uses manual TLS slots instead of C++ thread_local to:
    // 1. Avoid destructor ordering issues during thread exit
    // 2. Avoid DLL boundary problems on Windows
    // 3. Match UE5.7's implementation pattern
    //
    // IMPORTANT: Thread caches are intentionally NOT cleaned up on thread exit.
    // This matches UE5.7 and avoids potential issues during thread teardown.
    class LockFreeLinkAllocator_TLSCache
    {
        enum
        {
            NUM_PER_BUNDLE = 64,
        };

        using TLink = FLockFreeLinkPolicy::TLink;
        using TLinkPtr = FLockFreeLinkPolicy::TLinkPtr;

      public:
        LockFreeLinkAllocator_TLSCache()
        {
            // Deliberately NOT asserting IsInGameThread() here (issue #597 investigation):
            // UE5.7 uses check(IsInGameThread()) because it first-touches this singleton on
            // the game thread during single-threaded startup. OloEngine's first touch is a
            // scheduler WORKER thread instead (freeing a Jolt job) — see
            // docs/adr/0004-lock-free-allocator-singleton-init.md. Asserting here would fire
            // on every run. More generally, every LockFreeList entry point actually used in
            // this engine (task scheduler queues, MallocPurgatoryProxy, EventPool) is
            // intentionally cross-thread by design, so no operation in this file is
            // contractually game-thread-only. A future genuinely game-thread-only caller
            // should assert IsInGameThread() at its own call site, not inside this generic
            // allocator.
            m_TlsSlot = FPlatformTLS::AllocTlsSlot();
            OLO_CORE_ASSERT(FPlatformTLS::IsValidTlsSlot(m_TlsSlot), "LockFreeLinkAllocator_TLSCache: invalid TLS slot after allocation");
        }

        // Destructor intentionally leaks memory - matches UE5.7
        // TLS caches are never cleaned up to avoid issues during static destruction
        ~LockFreeLinkAllocator_TLSCache()
        {
            FPlatformTLS::FreeTlsSlot(m_TlsSlot);
            m_TlsSlot = FPlatformTLS::InvalidTlsSlot;
        }

        // @brief Allocate a lock-free link (Pop from cache)
        // @return Index of allocated link
        //
        // Named "Pop" to match UE5.7 naming convention.
        // Uses Payload field to chain free links.
        [[nodiscard]] TLinkPtr Pop()
        {
            FThreadLocalCache& TLS = GetTLS();

            if (!TLS.PartialBundle)
            {
                // Need to get more links
                if (TLS.FullBundle)
                {
                    // Use a full bundle we have cached
                    TLS.PartialBundle = TLS.FullBundle;
                    TLS.FullBundle = 0;
                }
                else
                {
                    // Try to pop a bundle from the global free list
                    TLS.PartialBundle = m_GlobalFreeListBundles.Pop();

                    if (!TLS.PartialBundle)
                    {
                        // Allocate new links from the main allocator
                        // Chain them using Payload field (matching UE5.7)
                        i32 FirstIndex = FLockFreeLinkPolicy::s_LinkAllocator.Alloc(NUM_PER_BUNDLE);
                        for (i32 Index = 0; Index < NUM_PER_BUNDLE; ++Index)
                        {
                            TLink* Event = FLockFreeLinkPolicy::IndexToLink(FirstIndex + Index);
                            Event->DoubleNext.Init();
                            Event->SingleNext.store(0, std::memory_order_relaxed);
                            Event->Payload.store(reinterpret_cast<void*>(static_cast<uptr>(TLS.PartialBundle)),
                                                 std::memory_order_relaxed);
                            TLS.PartialBundle = FLockFreeLinkPolicy::IndexToPtr(FirstIndex + Index);
                        }
                    }
                }
                TLS.NumPartial = NUM_PER_BUNDLE;
            }

            // Pop from partial bundle
            TLinkPtr Result = TLS.PartialBundle;
            TLink* ResultP = FLockFreeLinkPolicy::DerefLink(TLS.PartialBundle);
            TLS.PartialBundle = static_cast<TLinkPtr>(
                reinterpret_cast<uptr>(ResultP->Payload.load(std::memory_order_relaxed)));
            --TLS.NumPartial;

            // Clear payload and verify link is clean
            ResultP->Payload.store(nullptr, std::memory_order_relaxed);
            checkLockFreePointerList(!ResultP->DoubleNext.GetPtr() &&
                                     !ResultP->SingleNext.load(std::memory_order_relaxed));
            return Result;
        }

        // @brief Free a lock-free link (Push to cache)
        // @param Item Index of link to free
        //
        // Named "Push" to match UE5.7 naming convention.
        void Push(TLinkPtr Item)
        {
            FThreadLocalCache& TLS = GetTLS();

            if (TLS.NumPartial >= NUM_PER_BUNDLE)
            {
                // Current partial bundle is full, move it to full bundle
                if (TLS.FullBundle)
                {
                    // Already have a full bundle, return it to global
                    m_GlobalFreeListBundles.Push(TLS.FullBundle);
                }
                TLS.FullBundle = TLS.PartialBundle;
                TLS.PartialBundle = 0;
                TLS.NumPartial = 0;
            }

            TLink* ItemP = FLockFreeLinkPolicy::DerefLink(Item);

            // Clear the link
            ItemP->DoubleNext.SetPtr(0);
            ItemP->SingleNext.store(0, std::memory_order_relaxed);
            // Store current partial bundle as next in chain
            ItemP->Payload.store(reinterpret_cast<void*>(static_cast<uptr>(TLS.PartialBundle)),
                                 std::memory_order_relaxed);
            TLS.PartialBundle = Item;
            ++TLS.NumPartial;
        }

        // @brief Get the never-destructed singleton instance.
        //
        // DELIBERATE DIVERGENCE FROM UE 5.7 — read
        // docs/adr/0004-lock-free-allocator-singleton-init.md before changing this.
        //
        // The allocator is placement-new'd into a static buffer and its destructor
        // is NEVER run: other statics may still free links during static
        // destruction at exit. That is also why this is NOT a TLazySingleton —
        // TLazySingleton (ours and UE's) runs the instance destructor at exit,
        // which would be a shutdown use-after-free here. UE avoids it for the same
        // reason ("a replacement for TLazySingleton, which will still get destructed").
        //
        // UE first-inits this on the game thread during single-threaded startup and
        // so gets away with a plain non-atomic `if (!bIsInitialized)`. OloEngine
        // first touches it from a SCHEDULER WORKER (freeing a Jolt job), so that
        // form races on the flag (TSan-reported). We instead rely on C++11's
        // guaranteed-once, thread-safe function-local static initialization: the
        // `Instance` initializer runs exactly once under a compiler guard whose hot
        // path is a single guarded load (~UE's bool check). Do NOT revert to UE's
        // plain-bool form or re-add a hand-rolled double-checked lock — the previous
        // version hand-rolled a DCLP whose outer check read the non-atomic flag
        // outside the lock, which is the race that motivated this comment.
        static LockFreeLinkAllocator_TLSCache& Get()
        {
            alignas(LockFreeLinkAllocator_TLSCache) static u8 Data[sizeof(LockFreeLinkAllocator_TLSCache)];
            static LockFreeLinkAllocator_TLSCache* Instance =
                ::new (static_cast<void*>(Data)) LockFreeLinkAllocator_TLSCache();
            return *Instance;
        }

      private:
        // @struct FThreadLocalCache
        // @brief Per-thread cache of lock-free links
        //
        // Matches UE5.7's FThreadLocalCache structure.
        // Note: NO destructor - we intentionally leak on thread exit.
        struct FThreadLocalCache
        {
            TLinkPtr FullBundle;
            TLinkPtr PartialBundle;
            i32 NumPartial;

            FThreadLocalCache()
                : FullBundle(0), PartialBundle(0), NumPartial(0)
            {
            }

            // NO destructor - intentionally leak on thread exit
            // This matches UE5.7 and avoids issues during thread teardown
        };

        FThreadLocalCache& GetTLS()
        {
            checkLockFreePointerList(FPlatformTLS::IsValidTlsSlot(m_TlsSlot));
            FThreadLocalCache* TLS = static_cast<FThreadLocalCache*>(FPlatformTLS::GetTlsValue(m_TlsSlot));
            if (!TLS)
            {
                TLS = new FThreadLocalCache();
                FPlatformTLS::SetTlsValue(m_TlsSlot, TLS);
            }
            return *TLS;
        }

        // Slot for TLS struct
        u32 m_TlsSlot;

        // Lock free list of free memory blocks, these are all linked into a bundle of NUM_PER_BUNDLE
        FLockFreePointerListLIFORoot<OLO_PLATFORM_CACHE_LINE_SIZE> m_GlobalFreeListBundles;
    };

    // ========================================================================
    // Helper Function
    // ========================================================================

    static LockFreeLinkAllocator_TLSCache& GetLockFreeAllocator()
    {
        auto& result = LockFreeLinkAllocator_TLSCache::Get();
        return result;
    }

    // ========================================================================
    // FLockFreeLinkPolicy Static Members
    // ========================================================================

    FLockFreeLinkPolicy::TAllocator FLockFreeLinkPolicy::s_LinkAllocator;

    u32 FLockFreeLinkPolicy::AllocLockFreeLink()
    {
        auto& allocator = GetLockFreeAllocator();
        TLinkPtr Result = allocator.Pop();
        // This can only really be a mem stomp
        checkLockFreePointerList(Result &&
                                 !DerefLink(Result)->DoubleNext.GetPtr() &&
                                 !DerefLink(Result)->Payload.load(std::memory_order_relaxed) &&
                                 !DerefLink(Result)->SingleNext.load(std::memory_order_relaxed));
        return Result;
    }

    void FLockFreeLinkPolicy::FreeLockFreeLink(u32 Item)
    {
        GetLockFreeAllocator().Push(Item);
    }

} // namespace OloEngine
