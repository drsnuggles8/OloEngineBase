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
    }

    i32 GTestCriticalStalls = 0;
#endif

    // ========================================================================
    // Memory Statistics Tracking
    // ========================================================================

    namespace
    {
        // Memory tracking is disabled during lock-free operations in UE5.7
        // because updating stats is not safe in the middle of an atomic operation.
        // We keep this for reference but disabled by default.
#if 0
        std::atomic<i64> s_LockFreeListMem{0};
        
        void ChangeMem(i64 Delta)
        {
            s_LockFreeListMem.fetch_add(Delta, std::memory_order_relaxed);
            // Note: Cannot safely update external stats systems here
            // as we may be in the middle of a lock-free operation
        }
#endif
    } // namespace

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
            // TODO: Add IsInGameThread() check here once OloEngine has a formal threading system.
            // UE5.7 uses: check(IsInGameThread());
            // This ensures TLS slot allocation happens on the main thread for deterministic
            // initialization order before worker threads start using the allocator.
            m_TlsSlot = FPlatformTLS::AllocTlsSlot();
            bool isValid = FPlatformTLS::IsValidTlsSlot(m_TlsSlot);
            // Skipping checkLockFreePointerList and OLO_CORE_TRACE for now
            // checkLockFreePointerList(FPlatformTLS::IsValidTlsSlot(m_TlsSlot));
            // OLO_CORE_TRACE("LockFreeLinkAllocator_TLSCache: Initialized with TLS slot {0}", m_TlsSlot);
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
                        for (i32 Index = 0; Index < NUM_PER_BUNDLE; Index++)
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
            TLS.NumPartial--;

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
            TLS.NumPartial++;
        }

        // @brief Get singleton instance
        //
        // Uses placement new into static storage to ensure the allocator is NEVER destructed.
        // This is critical because lock-free lists may still be in use during static destruction
        // (e.g., other static objects freeing links in their destructors).
        // Matches UE5.7's approach: "make memory that will not go away"
        static LockFreeLinkAllocator_TLSCache& Get()
        {
            // Make memory that will not go away, a replacement for TLazySingleton
            // C++11 guarantees thread-safe initialization of static local variables
            alignas(LockFreeLinkAllocator_TLSCache) static u8 Data[sizeof(LockFreeLinkAllocator_TLSCache)];
            static bool bIsInitialized = false;
            static LockFreeLinkAllocator_TLSCache* Instance = nullptr;

            // Use a simple double-checked locking pattern
            if (!bIsInitialized)
            {
                static std::atomic_flag s_InitLock = ATOMIC_FLAG_INIT;
                while (s_InitLock.test_and_set(std::memory_order_acquire))
                {
                    // Spin
                }

                if (!bIsInitialized)
                {
                    ::new (static_cast<void*>(Data)) LockFreeLinkAllocator_TLSCache();
                    Instance = reinterpret_cast<LockFreeLinkAllocator_TLSCache*>(Data);
                    bIsInitialized = true;
                }

                s_InitLock.clear(std::memory_order_release);
            }

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
