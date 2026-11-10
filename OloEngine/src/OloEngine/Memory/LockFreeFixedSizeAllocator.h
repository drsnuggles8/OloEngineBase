#pragma once
// @file LockFreeFixedSizeAllocator.h
// @brief Lock-free fixed-size block allocator for OloEngine
//
// Provides efficient thread-safe allocation of fixed-size memory blocks:
// - TLockFreeFixedSizeAllocator: Simple lock-free allocator with free list
// - TLockFreeFixedSizeAllocator_TLSCacheBase: TLS-cached version for better performance
// - TLockFreeClassAllocator: Type-safe wrapper for class instances
//
// Ported from Unreal Engine's LockFreeFixedSizeAllocator.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Memory/LockFreeList.h"
#include "OloEngine/Memory/NoopCounter.h"

#include <atomic>
#include <mutex>
#include <new>

// Debug mode flag - useful for finding who really leaked
#ifndef USE_NAIVE_TLockFreeFixedSizeAllocator_TLSCacheBase
#define USE_NAIVE_TLockFreeFixedSizeAllocator_TLSCacheBase (0)
#endif

namespace OloEngine
{
    // ========================================================================
    // TLS-Cached Fixed Size Allocator Base
    // ========================================================================

    // @class TLockFreeFixedSizeAllocator_TLSCacheBase
    // @brief Thread safe, lock free pooling allocator with TLS caching
    //
    // Never returns free space, even at shutdown.
    // Alignment isn't handled; assumes FMemory::Malloc will work.
    //
    // @tparam SIZE Size of each allocation block
    // @tparam TBundleRecycler The type used for recycling bundles
    // @tparam TTrackingCounter Counter type for tracking (FNoopCounter for release)
    template<i32 SIZE, typename TBundleRecycler, typename TTrackingCounter = FNoopCounter>
    class TLockFreeFixedSizeAllocator_TLSCacheBase
    {
        enum
        {
            SIZE_PER_BUNDLE = 65536,
            NUM_PER_BUNDLE = SIZE_PER_BUNDLE / SIZE,
            // Largest power-of-2 factor of SIZE, used as bundle allocation alignment.
            // Guarantees every chunk at stride SIZE within the bundle shares this alignment.
            BLOCK_ALIGNMENT = SIZE & (-SIZE)
        };

        // A SIZE larger than SIZE_PER_BUNDLE produces NUM_PER_BUNDLE == 0,
        // which in turn breaks every invariant downstream (TLS.NumPartial
        // underflow, bundle-never-full logic, empty-bundle recycling). Catch
        // the misuse at compile time rather than at silent runtime corruption.
        static_assert(NUM_PER_BUNDLE > 0,
                      "TLockFreeFixedSizeAllocator: SIZE must be <= SIZE_PER_BUNDLE (65536) so each bundle holds at least one block.");

      public:
        TLockFreeFixedSizeAllocator_TLSCacheBase()
        {
            static_assert(SIZE >= static_cast<i32>(sizeof(void*)) && SIZE % sizeof(void*) == 0,
                          "Blocks in TLockFreeFixedSizeAllocator must be at least the size of a pointer.");
        }

        // Destructor, leaks all of the memory
        ~TLockFreeFixedSizeAllocator_TLSCacheBase() = default;

        // Non-copyable
        TLockFreeFixedSizeAllocator_TLSCacheBase(const TLockFreeFixedSizeAllocator_TLSCacheBase&) = delete;
        TLockFreeFixedSizeAllocator_TLSCacheBase& operator=(const TLockFreeFixedSizeAllocator_TLSCacheBase&) = delete;

        // Allocates a memory block of size SIZE.
        // @return Pointer to the allocated memory.
        void* Allocate()
        {
#if USE_NAIVE_TLockFreeFixedSizeAllocator_TLSCacheBase
            return FMemory::Malloc(SIZE, BLOCK_ALIGNMENT);
#else
            FThreadLocalCache& TLS = GetTLS();

            if (!TLS.PartialBundle)
            {
                if (TLS.FullBundle)
                {
                    TLS.PartialBundle = TLS.FullBundle;
                    TLS.FullBundle = nullptr;
                }
                else
                {
                    TLS.PartialBundle = static_cast<void**>(m_GlobalFreeListBundles.Pop());
                    if (!TLS.PartialBundle)
                    {
                        TLS.PartialBundle = static_cast<void**>(Orphanage().Bundles.Pop());
                    }
                    if (!TLS.PartialBundle)
                    {
                        TLS.PartialBundle = static_cast<void**>(FMemory::Malloc(SIZE_PER_BUNDLE, BLOCK_ALIGNMENT));
                        void** Next = TLS.PartialBundle;
                        for (i32 Index = 0; Index < NUM_PER_BUNDLE - 1; ++Index)
                        {
                            void* NextNext = reinterpret_cast<void*>(reinterpret_cast<u8*>(Next) + SIZE);
                            *Next = NextNext;
                            Next = static_cast<void**>(NextNext);
                        }
                        *Next = nullptr;
                        m_NumFree.Add(NUM_PER_BUNDLE);
                    }
                }
                TLS.NumPartial = NUM_PER_BUNDLE;
            }
            m_NumUsed.Increment();
            m_NumFree.Decrement();
            void* Result = static_cast<void*>(TLS.PartialBundle);
            TLS.PartialBundle = static_cast<void**>(*TLS.PartialBundle);
            --TLS.NumPartial;
            OLO_CORE_ASSERT(TLS.NumPartial >= 0 && ((!!TLS.NumPartial) == (!!TLS.PartialBundle)));
            if (TLS.bHandedBack) [[unlikely]]
            {
                HandBack(TLS);
            }
            return Result;
#endif
        }

        // Puts a memory block previously obtained from Allocate() back on the free list.
        // @param Item The item to free.
        void Free(void* Item)
        {
#if USE_NAIVE_TLockFreeFixedSizeAllocator_TLSCacheBase
            FMemory::Free(Item);
            return;
#else
            m_NumUsed.Decrement();
            m_NumFree.Increment();
            FThreadLocalCache& TLS = GetTLS();
            if (TLS.NumPartial >= NUM_PER_BUNDLE)
            {
                if (TLS.FullBundle)
                {
                    m_GlobalFreeListBundles.Push(TLS.FullBundle);
                }
                TLS.FullBundle = TLS.PartialBundle;
                TLS.PartialBundle = nullptr;
                TLS.NumPartial = 0;
            }
            *static_cast<void**>(Item) = static_cast<void*>(TLS.PartialBundle);
            TLS.PartialBundle = static_cast<void**>(Item);
            ++TLS.NumPartial;
            if (TLS.bHandedBack) [[unlikely]]
            {
                HandBack(TLS);
            }
#endif
        }

        // Gets the number of allocated memory blocks that are currently in use.
        // @return Reference to the counter tracking used blocks.
        const TTrackingCounter& GetNumUsed() const
        {
            return m_NumUsed;
        }

        // Gets the number of allocated memory blocks that are currently unused.
        // @return Reference to the counter tracking free blocks.
        const TTrackingCounter& GetNumFree() const
        {
            return m_NumFree;
        }

      private:
        // Blocks cached by threads that have exited. Without this every exiting
        // thread stranded its cache, up to two bundles, which Allocate then
        // replaced with fresh ones: WorkerRestartTest, which restarts the
        // scheduler's workers a thousand times, grew past 6 GB when CPU-starved.
        // Never destroyed, because a worker can exit during static destruction
        // (~FScheduler joins its workers) after the allocator is gone.
        struct FOrphanage
        {
            TBundleRecycler Bundles; // complete bundles, as m_GlobalFreeListBundles holds them
            std::mutex Mutex;
            void** Building = nullptr; // partial bundles' blocks, linked until NUM_PER_BUNDLE
            i32 NumBuilding = 0;

            void Adopt(void** Blocks)
            {
                const std::lock_guard<std::mutex> Lock(Mutex);
                while (Blocks)
                {
                    void** Next = static_cast<void**>(*Blocks);
                    *Blocks = static_cast<void*>(Building);
                    Building = Blocks;
                    if (++NumBuilding == NUM_PER_BUNDLE)
                    {
                        Bundles.Push(Building);
                        Building = nullptr;
                        NumBuilding = 0;
                    }
                    Blocks = Next;
                }
            }
        };

        static FOrphanage& Orphanage()
        {
            static FOrphanage* s_Orphanage = new FOrphanage();
            return *s_Orphanage;
        }

        // struct for the TLS cache. Trivially destructible, so its storage stays
        // valid for the whole thread, past the hook below. thread_local destructors
        // run in reverse order of construction, so one constructed before the hook
        // can still allocate or free here after the cache was handed back; from
        // then on (bHandedBack) every call hands back what it leaves in the cache.
        struct FThreadLocalCache
        {
            void** FullBundle = nullptr;
            void** PartialBundle = nullptr;
            i32 NumPartial = 0;
            bool bExitHookRegistered = false;
            bool bHandedBack = false;
        };

        // Moves everything the cache holds to the orphanage.
        static void HandBack(FThreadLocalCache& Cache)
        {
            if (Cache.FullBundle)
            {
                Orphanage().Bundles.Push(Cache.FullBundle);
            }
            Orphanage().Adopt(Cache.PartialBundle);
            Cache.FullBundle = nullptr;
            Cache.PartialBundle = nullptr;
            Cache.NumPartial = 0;
        }

        // Hands the thread's cache to the orphanage when the thread exits.
        struct FThreadExitHook
        {
            FThreadLocalCache* Cache = nullptr;

            ~FThreadExitHook()
            {
                if (Cache)
                {
                    HandBack(*Cache);
                    Cache->bHandedBack = true;
                }
            }
        };

        FThreadLocalCache& GetTLS()
        {
            thread_local FThreadLocalCache TLS;
            // Registered once, so nothing touches the hook after it has run.
            if (!TLS.bExitHookRegistered)
            {
                TLS.bExitHookRegistered = true;
                // HandBack pushes to a lock-free list, so the thread's link cache
                // has to outlive the hook.
                FLockFreeLinkPolicy::RegisterThreadExitHook();
                thread_local FThreadExitHook Hook;
                Hook.Cache = &TLS;
            }
            return TLS;
        }

        // Lock free list of free memory blocks, these are all linked into a bundle of NUM_PER_BUNDLE.
        TBundleRecycler m_GlobalFreeListBundles;

        // Total number of blocks outstanding and not in the free list.
        TTrackingCounter m_NumUsed;

        // Total number of blocks in the free list.
        TTrackingCounter m_NumFree;
    };

    // ========================================================================
    // Lock-Free Fixed Size Allocator
    // ========================================================================

    // @class TLockFreeFixedSizeAllocator
    // @brief Thread safe, lock free pooling allocator of fixed size blocks
    //
    // Only returns free space when the allocator is destroyed or Trim() is called.
    // Alignment isn't handled; assumes FMemory::Malloc will work.
    //
    // @tparam SIZE Size of each allocation block
    // @tparam TPaddingForCacheContention Cache line padding size (use PLATFORM_CACHE_LINE_SIZE)
    // @tparam TTrackingCounter Counter type for tracking (FNoopCounter for release)
    template<i32 SIZE, i32 TPaddingForCacheContention, typename TTrackingCounter = FNoopCounter>
    class TLockFreeFixedSizeAllocator
    {
      public:
        TLockFreeFixedSizeAllocator() = default;

        // Destructor, returns all memory via Memory::Free
        ~TLockFreeFixedSizeAllocator()
        {
            OLO_CORE_ASSERT(GetNumUsed() == 0);
            Trim();
        }

        // Non-copyable
        TLockFreeFixedSizeAllocator(const TLockFreeFixedSizeAllocator&) = delete;
        TLockFreeFixedSizeAllocator& operator=(const TLockFreeFixedSizeAllocator&) = delete;

        // Allocates a memory block of size SIZE.
        // @param Alignment Alignment requirement (default: MIN_ALIGNMENT)
        // @return Pointer to the allocated memory.
        void* Allocate(i32 Alignment = OLO_DEFAULT_ALIGNMENT)
        {
            void* MemoryBlock = nullptr;

            m_NumUsed.Increment();
            if (Alignment <= 4096)
            {
                // Pop from a free list only if Alignment is not larger than a memory page size
                MemoryBlock = m_FreeList.Pop();
                if (MemoryBlock)
                {
                    m_NumFree.Decrement();
                }
            }
            if (!MemoryBlock)
            {
                MemoryBlock = FMemory::Malloc(SIZE, Alignment);
            }

            return MemoryBlock;
        }

        // Puts a memory block previously obtained from Allocate() back on the free list.
        // @param Item The item to free.
        void Free(void* Item)
        {
            m_NumUsed.Decrement();
            m_FreeList.Push(Item);
            m_NumFree.Increment();
        }

        // Returns all free memory to the heap.
        void Trim()
        {
            while (void* Mem = m_FreeList.Pop())
            {
                FMemory::Free(Mem);
                m_NumFree.Decrement();
            }
        }

        // Gets the number of allocated memory blocks that are currently in use.
        // @return Number of used memory blocks.
        typename TTrackingCounter::IntegerType GetNumUsed() const
        {
            return m_NumUsed.GetValue();
        }

        // Gets the number of allocated memory blocks that are currently unused.
        // @return Number of unused memory blocks.
        typename TTrackingCounter::IntegerType GetNumFree() const
        {
            return m_NumFree.GetValue();
        }

      private:
        // Lock free list of free memory blocks.
        TLockFreePointerListUnordered<void, TPaddingForCacheContention> m_FreeList;

        // Total number of blocks outstanding and not in the free list.
        TTrackingCounter m_NumUsed;

        // Total number of blocks in the free list.
        TTrackingCounter m_NumFree;
    };

    // ========================================================================
    // TLS-Cached Version (using bundle recycler)
    // ========================================================================

    // @class TLockFreeFixedSizeAllocator_TLSCache
    // @brief Thread safe, lock free pooling allocator with TLS caching
    //
    // Never returns free space, even at shutdown.
    // Alignment isn't handled, assumes FMemory::Malloc will work.
    template<i32 SIZE, i32 TPaddingForCacheContention, typename TTrackingCounter = FNoopCounter>
    class TLockFreeFixedSizeAllocator_TLSCache
        : public TLockFreeFixedSizeAllocator_TLSCacheBase<
              SIZE,
              TLockFreePointerListUnordered<void*, TPaddingForCacheContention>,
              TTrackingCounter>
    {
    };

    // ========================================================================
    // Type-Safe Class Allocator
    // ========================================================================

    // @class TLockFreeClassAllocator
    // @brief Thread safe, lock free pooling allocator of memory for instances of T
    //
    // Never returns free space until program shutdown.
    //
    // @tparam T The type to allocate
    // @tparam TPaddingForCacheContention Cache line padding size
    template<class T, i32 TPaddingForCacheContention>
    class TLockFreeClassAllocator : private TLockFreeFixedSizeAllocator<sizeof(T), TPaddingForCacheContention, FNoopCounter>
    {
      public:
        // Returns a memory block of size sizeof(T).
        // @return Pointer to the allocated memory.
        void* Allocate()
        {
            return TLockFreeFixedSizeAllocator<sizeof(T), TPaddingForCacheContention, FNoopCounter>::Allocate();
        }

        // Returns a new T using the default constructor.
        // @return Pointer to the new object.
        T* New()
        {
            return ::new (Allocate()) T();
        }

        // Calls a destructor on Item and returns the memory to the free list.
        // @param Item The item whose memory to free.
        void Free(T* Item)
        {
            Item->~T();
            TLockFreeFixedSizeAllocator<sizeof(T), TPaddingForCacheContention, FNoopCounter>::Free(Item);
        }
    };

    // @class TLockFreeClassAllocator_TLSCache
    // @brief Thread safe, lock free pooling allocator for class T with TLS caching
    //
    // Never returns free space until program shutdown.
    template<class T, i32 TPaddingForCacheContention>
    class TLockFreeClassAllocator_TLSCache : private TLockFreeFixedSizeAllocator_TLSCache<sizeof(T), TPaddingForCacheContention, FNoopCounter>
    {
      public:
        // Returns a memory block of size sizeof(T).
        // @return Pointer to the allocated memory.
        void* Allocate()
        {
            return TLockFreeFixedSizeAllocator_TLSCache<sizeof(T), TPaddingForCacheContention, FNoopCounter>::Allocate();
        }

        // Returns a new T using the default constructor.
        // @return Pointer to the new object.
        T* New()
        {
            return ::new (Allocate()) T();
        }

        // Calls a destructor on Item and returns the memory to the free list.
        // @param Item The item whose memory to free.
        void Free(T* Item)
        {
            Item->~T();
            TLockFreeFixedSizeAllocator_TLSCache<sizeof(T), TPaddingForCacheContention, FNoopCounter>::Free(Item);
        }
    };

} // namespace OloEngine
