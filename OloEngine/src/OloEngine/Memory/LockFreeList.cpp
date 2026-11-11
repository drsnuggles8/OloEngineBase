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

#include <mutex>
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
    // DIVERGENCE FROM UE 5.7: a thread hands its cache back when it exits
    // (ReclaimExitingThreadCache, called by a thread_local hook below). UE keeps
    // its threads; the scheduler here restarts its workers, and every exited
    // worker stranded two bundles of a fixed index space that LockFreeLinksExhausted
    // stops the process on. The allocator is never destroyed, so a thread exiting
    // during static destruction can still hand back to it.
    //
    // thread_local destructors run in reverse order of construction, so another
    // thread_local's destructor can push to a lock-free list after this cache has
    // been handed back. Such a call gets no cache (GetTLS returns null) and takes
    // one link at a time from the orphans under m_OrphanMutex. A cache created
    // there would have no one left to hand it back.
    namespace
    {
        // Set by the exit hook below. Trivially destructible, so readable after
        // the hook is destroyed.
        thread_local bool t_LinkCacheThreadExiting = false;
    } // namespace

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
            FThreadLocalCache* const Cache = GetTLS();
            if (!Cache)
            {
                return PopWithoutCache();
            }
            FThreadLocalCache& TLS = *Cache;

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
                        TLS.PartialBundle = CarveBundle();
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
            FThreadLocalCache* const Cache = GetTLS();
            if (!Cache)
            {
                PushWithoutCache(Item);
                return;
            }
            FThreadLocalCache& TLS = *Cache;

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

        // @brief Hand the calling thread's cache back; called as the thread exits.
        //
        // The full bundle goes to the global list. The partial bundle's links join
        // the other exited threads' until they make a whole bundle.
        void ReclaimExitingThreadCache()
        {
            FThreadLocalCache* TLS = static_cast<FThreadLocalCache*>(FPlatformTLS::GetTlsValue(m_TlsSlot));
            if (!TLS)
            {
                return;
            }
            FPlatformTLS::SetTlsValue(m_TlsSlot, nullptr);
            if (TLS->FullBundle)
            {
                m_GlobalFreeListBundles.Push(TLS->FullBundle);
            }
            {
                const std::lock_guard<std::mutex> Lock(m_OrphanMutex);
                TLinkPtr Link = TLS->PartialBundle;
                while (Link)
                {
                    const TLinkPtr Next = static_cast<TLinkPtr>(reinterpret_cast<uptr>(
                        FLockFreeLinkPolicy::DerefLink(Link)->Payload.load(std::memory_order_relaxed)));
                    AdoptOrphan(Link);
                    Link = Next;
                }
            }
            delete TLS;
        }

      private:
        // @struct FThreadLocalCache
        // @brief Per-thread cache of lock-free links
        //
        // Matches UE5.7's FThreadLocalCache structure. Freed by
        // ReclaimExitingThreadCache when its thread exits.
        struct FThreadLocalCache
        {
            TLinkPtr FullBundle;
            TLinkPtr PartialBundle;
            i32 NumPartial;

            FThreadLocalCache()
                : FullBundle(0), PartialBundle(0), NumPartial(0)
            {
            }
        };

        // Null on a thread that has already handed its cache back.
        FThreadLocalCache* GetTLS();

        // A bundle of NUM_PER_BUNDLE links from the main allocator, chained by
        // the Payload field (matching UE5.7).
        [[nodiscard]] static TLinkPtr CarveBundle()
        {
            TLinkPtr Bundle = 0;
            const i32 FirstIndex = FLockFreeLinkPolicy::s_LinkAllocator.Alloc(NUM_PER_BUNDLE);
            for (i32 Index = 0; Index < NUM_PER_BUNDLE; ++Index)
            {
                TLink* Event = FLockFreeLinkPolicy::IndexToLink(FirstIndex + Index);
                Event->DoubleNext.Init();
                Event->SingleNext.store(0, std::memory_order_relaxed);
                Event->Payload.store(reinterpret_cast<void*>(static_cast<uptr>(Bundle)), std::memory_order_relaxed);
                Bundle = FLockFreeLinkPolicy::IndexToPtr(FirstIndex + Index);
            }
            return Bundle;
        }

        // Adds one free link to the orphans; a whole bundle of them goes to the
        // global list. Call with m_OrphanMutex held.
        void AdoptOrphan(TLinkPtr Link)
        {
            FLockFreeLinkPolicy::DerefLink(Link)->Payload.store(
                reinterpret_cast<void*>(static_cast<uptr>(m_OrphanBundle)), std::memory_order_relaxed);
            m_OrphanBundle = Link;
            if (++m_NumOrphans == NUM_PER_BUNDLE)
            {
                m_GlobalFreeListBundles.Push(m_OrphanBundle);
                m_OrphanBundle = 0;
                m_NumOrphans = 0;
            }
        }

        // Pop for a thread past its exit hook: one link from the orphans.
        [[nodiscard]] TLinkPtr PopWithoutCache()
        {
            const std::lock_guard<std::mutex> Lock(m_OrphanMutex);
            if (!m_OrphanBundle)
            {
                m_OrphanBundle = m_GlobalFreeListBundles.Pop();
                if (!m_OrphanBundle)
                {
                    m_OrphanBundle = CarveBundle();
                }
                m_NumOrphans = NUM_PER_BUNDLE;
            }
            const TLinkPtr Result = m_OrphanBundle;
            TLink* ResultP = FLockFreeLinkPolicy::DerefLink(Result);
            m_OrphanBundle = static_cast<TLinkPtr>(
                reinterpret_cast<uptr>(ResultP->Payload.load(std::memory_order_relaxed)));
            --m_NumOrphans;
            ResultP->Payload.store(nullptr, std::memory_order_relaxed);
            checkLockFreePointerList(!ResultP->DoubleNext.GetPtr() &&
                                     !ResultP->SingleNext.load(std::memory_order_relaxed));
            return Result;
        }

        // Push for a thread past its exit hook: the link joins the orphans.
        void PushWithoutCache(TLinkPtr Item)
        {
            TLink* ItemP = FLockFreeLinkPolicy::DerefLink(Item);
            ItemP->DoubleNext.SetPtr(0);
            ItemP->SingleNext.store(0, std::memory_order_relaxed);
            const std::lock_guard<std::mutex> Lock(m_OrphanMutex);
            AdoptOrphan(Item);
        }

        // Free links no thread's cache holds, linked by Payload, fewer than
        // NUM_PER_BUNDLE: exited threads' partial bundles, and what threads past
        // their exit hook pushed.
        std::mutex m_OrphanMutex;
        TLinkPtr m_OrphanBundle = 0;
        i32 m_NumOrphans = 0;

        // Slot for TLS struct
        u32 m_TlsSlot;

        // Lock free list of free memory blocks, these are all linked into a bundle of NUM_PER_BUNDLE
        FLockFreePointerListLIFORoot<OLO_PLATFORM_CACHE_LINE_SIZE> m_GlobalFreeListBundles;
    };

    namespace
    {
        // Hands the thread's link cache back when the thread exits.
        struct FLinkCacheExitHook
        {
            bool bHasCache = false;
            bool bRegistered = false;

            ~FLinkCacheExitHook()
            {
                t_LinkCacheThreadExiting = true;
                if (bHasCache)
                {
                    LockFreeLinkAllocator_TLSCache::Get().ReclaimExitingThreadCache();
                }
            }
        };
        thread_local FLinkCacheExitHook t_LinkCacheExitHook;
    } // namespace

    LockFreeLinkAllocator_TLSCache::FThreadLocalCache* LockFreeLinkAllocator_TLSCache::GetTLS()
    {
        checkLockFreePointerList(FPlatformTLS::IsValidTlsSlot(m_TlsSlot));
        FThreadLocalCache* TLS = static_cast<FThreadLocalCache*>(FPlatformTLS::GetTlsValue(m_TlsSlot));
        if (!TLS)
        {
            // Past the hook's destructor (another thread_local's destructor using
            // a list), nothing would hand a new cache back.
            if (t_LinkCacheThreadExiting)
            {
                return nullptr;
            }
            TLS = new FThreadLocalCache();
            FPlatformTLS::SetTlsValue(m_TlsSlot, TLS);
            t_LinkCacheExitHook.bHasCache = true;
        }
        return TLS;
    }

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

    void FLockFreeLinkPolicy::RegisterThreadExitHook()
    {
        if (!t_LinkCacheThreadExiting)
        {
            // The write is what constructs the hook where thread_locals are lazy.
            t_LinkCacheExitHook.bRegistered = true;
        }
    }

} // namespace OloEngine
