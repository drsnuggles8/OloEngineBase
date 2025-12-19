#pragma once

// @file LockFreeList.h
// @brief Lock-free linked list implementations for OloEngine
//
// This is a complete port of UE5's indexed pointer-based lock-free lists.
// The key to ABA safety is using indexed pointers with a 38-bit ABA counter
// instead of raw pointers.
//
// Key components:
// - FIndexedPointer: 64-bit value combining 26-bit index + 38-bit ABA counter
// - TLockFreeAllocOnceIndexedAllocator: Pre-allocated link pool (never frees)
// - LockFreeLinkAllocator_TLSCache: TLS caching for high-performance allocation
// - FLockFreePointerListLIFORoot/Base: LIFO stack using indexed pointers
// - FLockFreePointerFIFOBase: FIFO queue using indexed pointers
// - FStallingTaskQueue: Priority-based task queue with thread stalling
// - TClosableLockFreePointerListUnorderedSingleConsumer: Closable list for dependencies
//
// Ported from Unreal Engine's LockFreeList.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Templates/FunctionRef.h"

#include <atomic>
#include <type_traits>

namespace OloEngine
{
// ========================================================================
// Configuration Constants
// ========================================================================

// Maximum number of lock-free links (2^26 = ~67 million)
#define MAX_LOCK_FREE_LINKS_AS_BITS (26)
#define MAX_LOCK_FREE_LINKS (1 << MAX_LOCK_FREE_LINKS_AS_BITS)

// Maximum value for the ABA counter (38 bits)
#define MAX_TAG_BITS_VALUE (u64(1) << (64 - MAX_LOCK_FREE_LINKS_AS_BITS))

    // ========================================================================
    // Debug/Test Helpers
    // ========================================================================

#if OLO_DEBUG
#define checkLockFreePointerList(x) OLO_CORE_ASSERT(x, "LockFreeList check failed")
#else
#define checkLockFreePointerList(x) ((void)0)
#endif

    // Test critical stalls - for finding livelocks in lock-free code
    // When enabled, randomly sleeps at critical points to encourage race conditions
#if !OLO_DIST
    void DoTestCriticalStall();
    extern i32 GTestCriticalStalls;

    inline void TestCriticalStall()
    {
        if (GTestCriticalStalls)
        {
            DoTestCriticalStall();
        }
    }
#else
    OLO_FINLINE void TestCriticalStall() {}
#endif

    // ========================================================================
    // Error Handlers (defined in LockFreeList.cpp)
    // ========================================================================

    void LockFreeTagCounterHasOverflowed();
    void LockFreeLinksExhausted(u32 TotalNum);
    void* LockFreeAllocLinks(sizet AllocSize);
    void LockFreeFreeLinks(sizet AllocSize, void* Ptr);

    // ========================================================================
    // Forward Declarations
    // ========================================================================

    struct FIndexedLockFreeLink;
    struct FLockFreeLinkPolicy;

    // ========================================================================
    // TLockFreeAllocOnceIndexedAllocator
    // ========================================================================

    // @class TLockFreeAllocOnceIndexedAllocator
    // @brief Pre-allocated pool of items accessed by index
    //
    // This allocator never frees memory - it only recycles indices.
    // This is critical for ABA safety because the same index always
    // points to the same memory location.
    //
    // @tparam T Type of items to allocate
    // @tparam MaxTotalItems Maximum total items
    // @tparam ItemsPerPage Items per allocation page
    template<class T, unsigned int MaxTotalItems, unsigned int ItemsPerPage>
    class TLockFreeAllocOnceIndexedAllocator
    {
        static constexpr unsigned int MaxBlocks = (MaxTotalItems + ItemsPerPage - 1) / ItemsPerPage;

      public:
        [[nodiscard]] TLockFreeAllocOnceIndexedAllocator()
        {
            ++m_NextIndex; // skip index 0 (null pointer)
            for (u32 Index = 0; Index < MaxBlocks; Index++)
            {
                m_Pages[Index] = nullptr;
            }
        }

        // @brief Allocate one or more consecutive items
        // @param Count Number of items to allocate
        // @return Index of first allocated item
        inline u32 Alloc(u32 Count = 1)
        {
            u32 FirstItem = m_NextIndex.fetch_add(Count, std::memory_order_relaxed);
            if (FirstItem + Count > MaxTotalItems)
            {
                LockFreeLinksExhausted(MaxTotalItems);
            }
            for (u32 CurrentItem = FirstItem; CurrentItem < FirstItem + Count; CurrentItem++)
            {
                ::new (GetRawItem(CurrentItem)) T();
            }
            return FirstItem;
        }

        // @brief Get item by index
        // @param Index Index of item (0 returns nullptr)
        // @return Pointer to item or nullptr
        [[nodiscard]] inline T* GetItem(u32 Index)
        {
            if (!Index)
            {
                return nullptr;
            }
            u32 BlockIndex = Index / ItemsPerPage;
            u32 SubIndex = Index % ItemsPerPage;
            checkLockFreePointerList(Index < m_NextIndex.load(std::memory_order_relaxed) &&
                                     Index < MaxTotalItems &&
                                     BlockIndex < MaxBlocks &&
                                     m_Pages[BlockIndex]);
            return m_Pages[BlockIndex] + SubIndex;
        }

      private:
        [[nodiscard]] void* GetRawItem(u32 Index)
        {
            u32 BlockIndex = Index / ItemsPerPage;
            u32 SubIndex = Index % ItemsPerPage;
            checkLockFreePointerList(Index &&
                                     Index < m_NextIndex.load(std::memory_order_relaxed) &&
                                     Index < MaxTotalItems &&
                                     BlockIndex < MaxBlocks);
            if (!m_Pages[BlockIndex])
            {
                T* NewBlock = static_cast<T*>(LockFreeAllocLinks(ItemsPerPage * sizeof(T)));
                checkLockFreePointerList(IsAligned(NewBlock, alignof(T)));

                // Atomic compare-exchange to set the page
                T* Expected = nullptr;
                if (!std::atomic_ref(m_Pages[BlockIndex]).compare_exchange_strong(Expected, NewBlock, std::memory_order_release, std::memory_order_relaxed))
                {
                    // Another thread beat us - discard our block
                    checkLockFreePointerList(m_Pages[BlockIndex] && m_Pages[BlockIndex] != NewBlock);
                    LockFreeFreeLinks(ItemsPerPage * sizeof(T), NewBlock);
                }
                else
                {
                    checkLockFreePointerList(m_Pages[BlockIndex]);
                }
            }
            return static_cast<void*>(m_Pages[BlockIndex] + SubIndex);
        }

        alignas(OLO_PLATFORM_CACHE_LINE_SIZE) std::atomic<u32> m_NextIndex{ 0 };
        alignas(OLO_PLATFORM_CACHE_LINE_SIZE) T* m_Pages[MaxBlocks];
    };

    // ========================================================================
    // FIndexedPointer
    // ========================================================================

    // @struct FIndexedPointer
    // @brief 64-bit atomic pointer combining index and ABA counter
    //
    // Layout:
    // - Bits 0-25: Index (26 bits, supports up to 67 million links)
    // - Bits 26-63: ABA counter/state (38 bits)
    //
    // The ABA counter prevents the ABA problem by ensuring that even if
    // an index is recycled, the counter will be different.
    struct alignas(8) FIndexedPointer
    {
        // No constructor - we need to preserve the ABA counter

        // @brief Initialize to zero (only for non-recycled pointers)
        void Init()
        {
            static_assert(((MAX_LOCK_FREE_LINKS - 1) & MAX_LOCK_FREE_LINKS) == 0,
                          "MAX_LOCK_FREE_LINKS must be a power of two");
            m_Ptrs.store(0, std::memory_order_relaxed);
        }

        // @brief Set both pointer and counter/state
        inline void SetAll(u32 Ptr, u64 CounterAndState)
        {
            checkLockFreePointerList(Ptr < MAX_LOCK_FREE_LINKS &&
                                     CounterAndState < MAX_TAG_BITS_VALUE);
            m_Ptrs.store(u64(Ptr) | (CounterAndState << MAX_LOCK_FREE_LINKS_AS_BITS),
                         std::memory_order_relaxed);
        }

        // @brief Get the index portion
        [[nodiscard]] OLO_FINLINE u32 GetPtr() const
        {
            return u32(m_Ptrs.load(std::memory_order_relaxed) & (MAX_LOCK_FREE_LINKS - 1));
        }

        // @brief Set only the index portion
        OLO_FINLINE void SetPtr(u32 To)
        {
            SetAll(To, GetCounterAndState());
        }

        // @brief Get the counter/state portion
        [[nodiscard]] OLO_FINLINE u64 GetCounterAndState() const
        {
            return (m_Ptrs.load(std::memory_order_relaxed) >> MAX_LOCK_FREE_LINKS_AS_BITS);
        }

        // @brief Set only the counter/state portion
        OLO_FINLINE void SetCounterAndState(u64 To)
        {
            SetAll(GetPtr(), To);
        }

        // @brief Advance counter from another pointer's state
        // @param From Source pointer to get counter from
        // @param TABAInc Amount to increment counter
        inline void AdvanceCounterAndState(const FIndexedPointer& From, u64 TABAInc)
        {
            SetCounterAndState(From.GetCounterAndState() + TABAInc);
            if (OLO_UNLIKELY(GetCounterAndState() < From.GetCounterAndState()))
            {
                // Counter overflow - extremely rare, just log and continue
                LockFreeTagCounterHasOverflowed();
            }
        }

        // @brief Get state bits (lower bits of counter based on TABAInc)
        template<u64 TABAInc>
        [[nodiscard]] OLO_FINLINE u64 GetState() const
        {
            return GetCounterAndState() & (TABAInc - 1);
        }

        // @brief Set state bits
        template<u64 TABAInc>
        inline void SetState(u64 Value)
        {
            checkLockFreePointerList(Value < TABAInc);
            SetCounterAndState((GetCounterAndState() & ~(TABAInc - 1)) | Value);
        }

        // @brief Atomically read from another pointer
        inline void AtomicRead(const FIndexedPointer& Other)
        {
            checkLockFreePointerList(IsAligned(&m_Ptrs, 8) && IsAligned(&Other.m_Ptrs, 8));
            m_Ptrs.store(Other.m_Ptrs.load(std::memory_order_acquire), std::memory_order_relaxed);
            TestCriticalStall();
        }

        // @brief Atomic compare-exchange
        // @param Exchange Value to set if comparison succeeds
        // @param Comparand Expected current value
        // @return True if exchange succeeded
        inline bool InterlockedCompareExchange(const FIndexedPointer& Exchange,
                                               const FIndexedPointer& Comparand)
        {
            TestCriticalStall();
            u64 Expected = Comparand.m_Ptrs.load(std::memory_order_relaxed);
            return m_Ptrs.compare_exchange_strong(
                Expected,
                Exchange.m_Ptrs.load(std::memory_order_relaxed),
                std::memory_order_acq_rel,
                std::memory_order_relaxed);
        }

        [[nodiscard]] OLO_FINLINE bool operator==(const FIndexedPointer& Other) const
        {
            return m_Ptrs.load(std::memory_order_relaxed) ==
                   Other.m_Ptrs.load(std::memory_order_relaxed);
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const FIndexedPointer& Other) const
        {
            return m_Ptrs.load(std::memory_order_relaxed) !=
                   Other.m_Ptrs.load(std::memory_order_relaxed);
        }

      private:
        std::atomic<u64> m_Ptrs;
    };

    // ========================================================================
    // FIndexedLockFreeLink
    // ========================================================================

    // @struct FIndexedLockFreeLink
    // @brief Link node for lock-free lists
    //
    // Contains:
    // - DoubleNext: For FIFO queues (needs ABA counter)
    // - Payload: The actual data pointer
    // - SingleNext: For LIFO stacks (index only, no ABA counter needed)
    struct FIndexedLockFreeLink
    {
        FIndexedPointer DoubleNext;
        std::atomic<void*> Payload;
        std::atomic<u32> SingleNext;
    };

    // ========================================================================
    // FLockFreeLinkPolicy
    // ========================================================================

    // @struct FLockFreeLinkPolicy
    // @brief Policy class for lock-free link allocation
    //
    // Provides the allocator and helper functions for working with indexed links.
    struct FLockFreeLinkPolicy
    {
        static constexpr int MAX_BITS_IN_TLinkPtr = MAX_LOCK_FREE_LINKS_AS_BITS;

        using TDoublePtr = FIndexedPointer;
        using TLink = FIndexedLockFreeLink;
        using TLinkPtr = u32;
        using TAllocator = TLockFreeAllocOnceIndexedAllocator<FIndexedLockFreeLink, MAX_LOCK_FREE_LINKS, 16384>;

        [[nodiscard]] static OLO_FINLINE FIndexedLockFreeLink* DerefLink(u32 Ptr)
        {
            return s_LinkAllocator.GetItem(Ptr);
        }

        [[nodiscard]] static OLO_FINLINE FIndexedLockFreeLink* IndexToLink(u32 Index)
        {
            return s_LinkAllocator.GetItem(Index);
        }

        [[nodiscard]] static OLO_FINLINE u32 IndexToPtr(u32 Index)
        {
            return Index;
        }

        static u32 AllocLockFreeLink();
        static void FreeLockFreeLink(u32 Item);

        static TAllocator s_LinkAllocator;
    };

    // ========================================================================
    // FLockFreePointerListLIFORoot
    // ========================================================================

    // @class FLockFreePointerListLIFORoot
    // @brief Low-level LIFO stack root using indexed pointers
    //
    // This is the core implementation that handles the atomic operations.
    // Higher-level classes wrap this to provide payload handling.
    //
    // @tparam TPaddingForCacheContention Cache line padding
    // @tparam TABAInc ABA counter increment (use > 1 for state bits)
    template<int TPaddingForCacheContention, u64 TABAInc = 1>
    class FLockFreePointerListLIFORoot
    {
        using TDoublePtr = FLockFreeLinkPolicy::TDoublePtr;
        using TLink = FLockFreeLinkPolicy::TLink;
        using TLinkPtr = FLockFreeLinkPolicy::TLinkPtr;

      public:
        FLockFreePointerListLIFORoot(const FLockFreePointerListLIFORoot&) = delete;
        FLockFreePointerListLIFORoot& operator=(const FLockFreePointerListLIFORoot&) = delete;

        [[nodiscard]] inline FLockFreePointerListLIFORoot()
        {
            // Ensure we have enough counter bits to avoid ABA problem
            static_assert(MAX_TAG_BITS_VALUE / TABAInc >= (1 << 23),
                          "Risk of ABA problem - need more counter bits");
            static_assert((TABAInc & (TABAInc - 1)) == 0,
                          "TABAInc must be power of two");
            Reset();
        }

        void Reset()
        {
            m_Head.Init();
        }

        // @brief Push a link onto the stack
        // @param Item Index of link to push
        void Push(TLinkPtr Item)
        {
            while (true)
            {
                TDoublePtr LocalHead;
                LocalHead.AtomicRead(m_Head);
                TDoublePtr NewHead;
                NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                NewHead.SetPtr(Item);
                FLockFreeLinkPolicy::DerefLink(Item)->SingleNext.store(
                    LocalHead.GetPtr(), std::memory_order_relaxed);
                if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                {
                    break;
                }
            }
        }

        // @brief Conditionally push based on current state
        // @param AllocateIfOkToPush Function that returns link to push or 0 if not ok
        // @return True if push succeeded
        bool PushIf(TFunctionRef<TLinkPtr(u64)> AllocateIfOkToPush)
        {
            static_assert(TABAInc > 1, "PushIf should not be used for lists without state");
            while (true)
            {
                TDoublePtr LocalHead;
                LocalHead.AtomicRead(m_Head);
                u64 LocalState = LocalHead.template GetState<TABAInc>();
                TLinkPtr Item = AllocateIfOkToPush(LocalState);
                if (!Item)
                {
                    return false;
                }

                TDoublePtr NewHead;
                NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                FLockFreeLinkPolicy::DerefLink(Item)->SingleNext.store(
                    LocalHead.GetPtr(), std::memory_order_relaxed);
                NewHead.SetPtr(Item);
                if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                {
                    break;
                }
            }
            return true;
        }

        // @brief Pop a link from the stack
        // @return Index of popped link, or 0 if empty
        TLinkPtr Pop()
        {
            TLinkPtr Item = 0;
            while (true)
            {
                TDoublePtr LocalHead;
                LocalHead.AtomicRead(m_Head);
                Item = LocalHead.GetPtr();
                if (!Item)
                {
                    break;
                }
                TDoublePtr NewHead;
                NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                TLink* ItemP = FLockFreeLinkPolicy::DerefLink(Item);
                NewHead.SetPtr(ItemP->SingleNext.load(std::memory_order_relaxed));
                if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                {
                    ItemP->SingleNext.store(0, std::memory_order_relaxed);
                    break;
                }
            }
            return Item;
        }

        // @brief Pop all links atomically
        // @return Index of first link in chain, or 0 if empty
        TLinkPtr PopAll()
        {
            TLinkPtr Item = 0;
            while (true)
            {
                TDoublePtr LocalHead;
                LocalHead.AtomicRead(m_Head);
                Item = LocalHead.GetPtr();
                if (!Item)
                {
                    break;
                }
                TDoublePtr NewHead;
                NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                NewHead.SetPtr(0);
                if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                {
                    break;
                }
            }
            return Item;
        }

        // @brief Pop all links and change state atomically
        // @param StateChange Function to transform state
        // @return Index of first link in chain
        TLinkPtr PopAllAndChangeState(TFunctionRef<u64(u64)> StateChange)
        {
            static_assert(TABAInc > 1, "PopAllAndChangeState should not be used for lists without state");
            TLinkPtr Item = 0;
            while (true)
            {
                TDoublePtr LocalHead;
                LocalHead.AtomicRead(m_Head);
                Item = LocalHead.GetPtr();
                TDoublePtr NewHead;
                NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                NewHead.template SetState<TABAInc>(StateChange(LocalHead.template GetState<TABAInc>()));
                NewHead.SetPtr(0);
                if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                {
                    break;
                }
            }
            return Item;
        }

        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return !m_Head.GetPtr();
        }

        [[nodiscard]] inline u64 GetState() const
        {
            TDoublePtr LocalHead;
            LocalHead.AtomicRead(m_Head);
            return LocalHead.template GetState<TABAInc>();
        }

      private:
        alignas(TPaddingForCacheContention > 0 ? TPaddingForCacheContention : 1) TDoublePtr m_Head;
    };

    // ========================================================================
    // FLockFreePointerListLIFOBase
    // ========================================================================

    // @class FLockFreePointerListLIFOBase
    // @brief LIFO stack with payload handling
    //
    // Wraps FLockFreePointerListLIFORoot to provide:
    // - Automatic link allocation/deallocation
    // - Payload storage and retrieval
    // - PopAll and PopAllAndApply helpers
    template<class T, int TPaddingForCacheContention, u64 TABAInc = 1>
    class FLockFreePointerListLIFOBase
    {
        using TDoublePtr = FLockFreeLinkPolicy::TDoublePtr;
        using TLink = FLockFreeLinkPolicy::TLink;
        using TLinkPtr = FLockFreeLinkPolicy::TLinkPtr;

      public:
        FLockFreePointerListLIFOBase(const FLockFreePointerListLIFOBase&) = delete;
        FLockFreePointerListLIFOBase& operator=(const FLockFreePointerListLIFOBase&) = delete;

        [[nodiscard]] FLockFreePointerListLIFOBase() = default;

        ~FLockFreePointerListLIFOBase()
        {
            while (Pop())
            {
            }
        }

        void Reset()
        {
            while (Pop())
            {
            }
            m_RootList.Reset();
        }

        // @brief Push a payload onto the stack
        // @param InPayload Payload to push (cannot be nullptr)
        void Push(T* InPayload)
        {
            TLinkPtr Item = FLockFreeLinkPolicy::AllocLockFreeLink();
            FLockFreeLinkPolicy::DerefLink(Item)->Payload.store(InPayload, std::memory_order_relaxed);
            m_RootList.Push(Item);
        }

        // @brief Conditionally push based on state
        // @param InPayload Payload to push
        // @param OkToPush Function that returns true if push should proceed
        // @return True if push succeeded
        bool PushIf(T* InPayload, TFunctionRef<bool(u64)> OkToPush)
        {
            TLinkPtr Item = 0;

            auto AllocateIfOkToPush = [&OkToPush, InPayload, &Item](u64 State) -> TLinkPtr
            {
                if (OkToPush(State))
                {
                    if (!Item)
                    {
                        Item = FLockFreeLinkPolicy::AllocLockFreeLink();
                        FLockFreeLinkPolicy::DerefLink(Item)->Payload.store(
                            InPayload, std::memory_order_relaxed);
                    }
                    return Item;
                }
                return 0;
            };

            if (!m_RootList.PushIf(AllocateIfOkToPush))
            {
                if (Item)
                {
                    // We allocated a link but the list was closed
                    FLockFreeLinkPolicy::FreeLockFreeLink(Item);
                }
                return false;
            }
            return true;
        }

        // @brief Pop a payload from the stack
        // @return Popped payload, or nullptr if empty
        [[nodiscard]] T* Pop()
        {
            TLinkPtr Item = m_RootList.Pop();
            T* Result = nullptr;
            if (Item)
            {
                Result = static_cast<T*>(FLockFreeLinkPolicy::DerefLink(Item)->Payload.load(
                    std::memory_order_relaxed));
                FLockFreeLinkPolicy::FreeLockFreeLink(Item);
            }
            return Result;
        }

        // @brief Pop all items into a container
        // @tparam ContainerType Container with Add() method
        // @param OutContainer Container to receive items
        template<typename ContainerType>
        void PopAll(ContainerType& OutContainer)
        {
            TLinkPtr Links = m_RootList.PopAll();
            while (Links)
            {
                TLink* LinksP = FLockFreeLinkPolicy::DerefLink(Links);
                OutContainer.Add(static_cast<T*>(LinksP->Payload.load(std::memory_order_relaxed)));
                TLinkPtr Del = Links;
                Links = LinksP->SingleNext.load(std::memory_order_relaxed);
                FLockFreeLinkPolicy::FreeLockFreeLink(Del);
            }
        }

        // @brief Pop all items and apply a functor
        // @tparam FunctorType Functor type
        // @param InFunctor Functor to apply to each item
        template<typename FunctorType>
        void PopAllAndApply(FunctorType InFunctor)
        {
            TLinkPtr Links = m_RootList.PopAll();
            while (Links)
            {
                TLink* LinksP = FLockFreeLinkPolicy::DerefLink(Links);
                InFunctor(static_cast<T*>(LinksP->Payload.load(std::memory_order_relaxed)));
                TLinkPtr Del = Links;
                Links = LinksP->SingleNext.load(std::memory_order_relaxed);
                FLockFreeLinkPolicy::FreeLockFreeLink(Del);
            }
        }

        // @brief Pop all items and change state atomically
        // @tparam ContainerType Container with Add() method
        // @param OutContainer Container to receive items
        // @param StateChange Function to transform state
        template<typename ContainerType>
        void PopAllAndChangeState(ContainerType& OutContainer, TFunctionRef<u64(u64)> StateChange)
        {
            TLinkPtr Links = m_RootList.PopAllAndChangeState(StateChange);
            while (Links)
            {
                TLink* LinksP = FLockFreeLinkPolicy::DerefLink(Links);
                OutContainer.Add(static_cast<T*>(LinksP->Payload.load(std::memory_order_relaxed)));
                TLinkPtr Del = Links;
                Links = LinksP->SingleNext.load(std::memory_order_relaxed);
                FLockFreeLinkPolicy::FreeLockFreeLink(Del);
            }
        }

        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return m_RootList.IsEmpty();
        }

        [[nodiscard]] OLO_FINLINE u64 GetState() const
        {
            return m_RootList.GetState();
        }

      private:
        FLockFreePointerListLIFORoot<TPaddingForCacheContention, TABAInc> m_RootList;
    };

    // ========================================================================
    // FLockFreePointerFIFOBase
    // ========================================================================

    // @class FLockFreePointerFIFOBase
    // @brief FIFO queue using indexed pointers
    //
    // Uses the Michael & Scott algorithm with indexed pointers for ABA safety.
    // Items come out in the order they were pushed.
    template<class T, int TPaddingForCacheContention, u64 TABAInc = 1>
    class FLockFreePointerFIFOBase
    {
        using TDoublePtr = FLockFreeLinkPolicy::TDoublePtr;
        using TLink = FLockFreeLinkPolicy::TLink;
        using TLinkPtr = FLockFreeLinkPolicy::TLinkPtr;

      public:
        FLockFreePointerFIFOBase(const FLockFreePointerFIFOBase&) = delete;
        FLockFreePointerFIFOBase& operator=(const FLockFreePointerFIFOBase&) = delete;

        [[nodiscard]] inline FLockFreePointerFIFOBase()
        {
            static_assert(TABAInc <= 65536, "Risk of ABA problem");
            static_assert((TABAInc & (TABAInc - 1)) == 0, "TABAInc must be power of two");

            m_Head.Init();
            m_Tail.Init();
            TLinkPtr Stub = FLockFreeLinkPolicy::AllocLockFreeLink();
            m_Head.SetPtr(Stub);
            m_Tail.SetPtr(Stub);
        }

        ~FLockFreePointerFIFOBase()
        {
            while (Pop())
            {
            }
            FLockFreeLinkPolicy::FreeLockFreeLink(m_Head.GetPtr());
        }

        // @brief Push a payload onto the tail of the queue
        // @param InPayload Payload to push (cannot be nullptr)
        void Push(T* InPayload)
        {
            TLinkPtr Item = FLockFreeLinkPolicy::AllocLockFreeLink();
            FLockFreeLinkPolicy::DerefLink(Item)->Payload.store(InPayload, std::memory_order_relaxed);
            TDoublePtr LocalTail;

            while (true)
            {
                LocalTail.AtomicRead(m_Tail);
                TLink* LocalTailP = FLockFreeLinkPolicy::DerefLink(LocalTail.GetPtr());
                TDoublePtr LocalNext;
                LocalNext.AtomicRead(LocalTailP->DoubleNext);
                TDoublePtr TestLocalTail;
                TestLocalTail.AtomicRead(m_Tail);

                if (TestLocalTail == LocalTail)
                {
                    if (LocalNext.GetPtr())
                    {
                        // Tail is lagging, help advance it
                        TestCriticalStall();
                        TDoublePtr NewTail;
                        NewTail.AdvanceCounterAndState(LocalTail, TABAInc);
                        NewTail.SetPtr(LocalNext.GetPtr());
                        m_Tail.InterlockedCompareExchange(NewTail, LocalTail);
                    }
                    else
                    {
                        // Try to link new node at end
                        TestCriticalStall();
                        TDoublePtr NewNext;
                        NewNext.AdvanceCounterAndState(LocalNext, TABAInc);
                        NewNext.SetPtr(Item);
                        if (LocalTailP->DoubleNext.InterlockedCompareExchange(NewNext, LocalNext))
                        {
                            break;
                        }
                    }
                }
            }

            // Try to swing tail to new node
            TestCriticalStall();
            TDoublePtr NewTail;
            NewTail.AdvanceCounterAndState(LocalTail, TABAInc);
            NewTail.SetPtr(Item);
            m_Tail.InterlockedCompareExchange(NewTail, LocalTail);
        }

        // @brief Pop a payload from the head of the queue
        // @return Popped payload, or nullptr if empty
        [[nodiscard]] T* Pop()
        {
            T* Result = nullptr;
            TDoublePtr LocalHead;

            while (true)
            {
                LocalHead.AtomicRead(m_Head);
                TDoublePtr LocalTail;
                LocalTail.AtomicRead(m_Tail);
                TDoublePtr LocalNext;
                LocalNext.AtomicRead(FLockFreeLinkPolicy::DerefLink(LocalHead.GetPtr())->DoubleNext);
                TDoublePtr LocalHeadTest;
                LocalHeadTest.AtomicRead(m_Head);

                if (LocalHead == LocalHeadTest)
                {
                    if (LocalHead.GetPtr() == LocalTail.GetPtr())
                    {
                        if (!LocalNext.GetPtr())
                        {
                            return nullptr; // Queue is empty
                        }
                        // Tail is lagging, help advance it
                        TestCriticalStall();
                        TDoublePtr NewTail;
                        NewTail.AdvanceCounterAndState(LocalTail, TABAInc);
                        NewTail.SetPtr(LocalNext.GetPtr());
                        m_Tail.InterlockedCompareExchange(NewTail, LocalTail);
                    }
                    else
                    {
                        // Read payload before CAS
                        TestCriticalStall();
                        Result = static_cast<T*>(
                            FLockFreeLinkPolicy::DerefLink(LocalNext.GetPtr())->Payload.load(std::memory_order_relaxed));
                        TDoublePtr NewHead;
                        NewHead.AdvanceCounterAndState(LocalHead, TABAInc);
                        NewHead.SetPtr(LocalNext.GetPtr());
                        if (m_Head.InterlockedCompareExchange(NewHead, LocalHead))
                        {
                            break;
                        }
                    }
                }
            }

            FLockFreeLinkPolicy::FreeLockFreeLink(LocalHead.GetPtr());
            return Result;
        }

        // @brief Pop all items into a container
        // @tparam ContainerType Container with Add() method
        // @param OutContainer Container to receive items
        template<typename ContainerType>
        void PopAll(ContainerType& OutContainer)
        {
            while (T* Item = Pop())
            {
                OutContainer.Add(Item);
            }
        }

        [[nodiscard]] inline bool IsEmpty() const
        {
            TDoublePtr LocalHead;
            LocalHead.AtomicRead(m_Head);
            TDoublePtr LocalNext;
            LocalNext.AtomicRead(FLockFreeLinkPolicy::DerefLink(LocalHead.GetPtr())->DoubleNext);
            return !LocalNext.GetPtr();
        }

      private:
        alignas(TPaddingForCacheContention > 0 ? TPaddingForCacheContention : 1) TDoublePtr m_Head;
        alignas(TPaddingForCacheContention > 0 ? TPaddingForCacheContention : 1) TDoublePtr m_Tail;
    };

    // ========================================================================
    // FStallingTaskQueue
    // ========================================================================

    // @class FStallingTaskQueue
    // @brief Priority-based task queue with thread stalling support
    //
    // Used by the task system for scheduling tasks with different priorities.
    // Threads can register as "stalled" when no work is available, and will
    // be woken up when new work arrives.
    template<class T, int TPaddingForCacheContention, int NumPriorities>
    class FStallingTaskQueue
    {
        using TDoublePtr = FLockFreeLinkPolicy::TDoublePtr;
        using TLink = FLockFreeLinkPolicy::TLink;
        using TLinkPtr = FLockFreeLinkPolicy::TLinkPtr;

      public:
        FStallingTaskQueue(const FStallingTaskQueue&) = delete;
        FStallingTaskQueue& operator=(const FStallingTaskQueue&) = delete;

        [[nodiscard]] FStallingTaskQueue()
        {
            m_MasterState.Init();
        }

        // @brief Push a task with a given priority
        // @param InPayload Task to push
        // @param Priority Priority level (0 = highest)
        // @return Thread index to wake, or -1 if none
        i32 Push(T* InPayload, u32 Priority)
        {
            checkLockFreePointerList(Priority < static_cast<u32>(NumPriorities));
            TDoublePtr LocalMasterState;
            LocalMasterState.AtomicRead(m_MasterState);
            m_PriorityQueues[Priority].Push(InPayload);
            TDoublePtr NewMasterState;
            NewMasterState.AdvanceCounterAndState(LocalMasterState, 1);
            i32 ThreadToWake = FindThreadToWake(LocalMasterState.GetPtr());

            if (ThreadToWake >= 0)
            {
                NewMasterState.SetPtr(TurnOffBit(LocalMasterState.GetPtr(), ThreadToWake));
            }
            else
            {
                NewMasterState.SetPtr(LocalMasterState.GetPtr());
            }

            while (!m_MasterState.InterlockedCompareExchange(NewMasterState, LocalMasterState))
            {
                LocalMasterState.AtomicRead(m_MasterState);
                NewMasterState.AdvanceCounterAndState(LocalMasterState, 1);
                ThreadToWake = FindThreadToWake(LocalMasterState.GetPtr());

                if (ThreadToWake >= 0)
                {
                    NewMasterState.SetPtr(TurnOffBit(LocalMasterState.GetPtr(), ThreadToWake));
                }
                else
                {
                    NewMasterState.SetPtr(LocalMasterState.GetPtr());
                }
            }
            return ThreadToWake;
        }

        // @brief Pop a task for a thread
        // @param MyThread Thread index (for stalling)
        // @param bAllowStall If true, register as stalled when no work
        // @return Task or nullptr
        [[nodiscard]] T* Pop(i32 MyThread, bool bAllowStall)
        {
            OLO_CORE_ASSERT(MyThread >= 0 && MyThread < FLockFreeLinkPolicy::MAX_BITS_IN_TLinkPtr,
                            "Invalid thread index");

            while (true)
            {
                TDoublePtr LocalMasterState;
                LocalMasterState.AtomicRead(m_MasterState);

                // Try each priority queue
                for (i32 Index = 0; Index < NumPriorities; Index++)
                {
                    T* Result = m_PriorityQueues[Index].Pop();
                    if (Result)
                    {
                        while (true)
                        {
                            TDoublePtr NewMasterState;
                            NewMasterState.AdvanceCounterAndState(LocalMasterState, 1);
                            NewMasterState.SetPtr(LocalMasterState.GetPtr());
                            if (m_MasterState.InterlockedCompareExchange(NewMasterState, LocalMasterState))
                            {
                                return Result;
                            }
                            LocalMasterState.AtomicRead(m_MasterState);
                            checkLockFreePointerList(!TestBit(LocalMasterState.GetPtr(), MyThread));
                        }
                    }
                }

                if (!bAllowStall)
                {
                    break; // Not stalling, queues are empty
                }

                // Register as stalled
                TDoublePtr NewMasterState;
                NewMasterState.AdvanceCounterAndState(LocalMasterState, 1);
                NewMasterState.SetPtr(TurnOnBit(LocalMasterState.GetPtr(), MyThread));
                if (m_MasterState.InterlockedCompareExchange(NewMasterState, LocalMasterState))
                {
                    break;
                }
            }
            return nullptr;
        }

      private:
        [[nodiscard]] static i32 FindThreadToWake(TLinkPtr Ptr)
        {
            i32 Result = -1;
            uptr Test = uptr(Ptr);
            if (Test)
            {
                Result = 0;
                while (!(Test & 1))
                {
                    Test >>= 1;
                    Result++;
                }
            }
            return Result;
        }

        [[nodiscard]] static TLinkPtr TurnOffBit(TLinkPtr Ptr, i32 BitToTurnOff)
        {
            return static_cast<TLinkPtr>(uptr(Ptr) & ~(uptr(1) << BitToTurnOff));
        }

        [[nodiscard]] static TLinkPtr TurnOnBit(TLinkPtr Ptr, i32 BitToTurnOn)
        {
            return static_cast<TLinkPtr>(uptr(Ptr) | (uptr(1) << BitToTurnOn));
        }

        [[nodiscard]] static bool TestBit(TLinkPtr Ptr, i32 BitToTest)
        {
            return !!(uptr(Ptr) & (uptr(1) << BitToTest));
        }

        FLockFreePointerFIFOBase<T, TPaddingForCacheContention> m_PriorityQueues[NumPriorities];
        alignas(TPaddingForCacheContention > 0 ? TPaddingForCacheContention : 1) TDoublePtr m_MasterState;
    };

    // ========================================================================
    // Public API Classes
    // ========================================================================

    // @class TLockFreePointerListLIFOPad
    // @brief LIFO stack with cache line padding
    template<class T, int TPaddingForCacheContention>
    class TLockFreePointerListLIFOPad : private FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>
    {
      public:
        void Push(T* NewItem)
        {
            FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>::Push(NewItem);
        }

        [[nodiscard]] T* Pop()
        {
            return FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>::Pop();
        }

        template<typename ContainerType>
        void PopAll(ContainerType& Output)
        {
            FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>::PopAll(Output);
        }

        template<typename FunctorType>
        void PopAllAndApply(FunctorType InFunctor)
        {
            FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>::PopAllAndApply(InFunctor);
        }

        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return FLockFreePointerListLIFOBase<T, TPaddingForCacheContention>::IsEmpty();
        }
    };

    // @class TLockFreePointerListLIFO
    // @brief LIFO stack without cache line padding
    template<class T>
    class TLockFreePointerListLIFO : public TLockFreePointerListLIFOPad<T, 0>
    {
    };

    // @class TLockFreePointerListUnordered
    // @brief Unordered lock-free list (LIFO behavior)
    template<class T, int TPaddingForCacheContention>
    class TLockFreePointerListUnordered : public TLockFreePointerListLIFOPad<T, TPaddingForCacheContention>
    {
    };

    // @class TLockFreePointerListFIFO
    // @brief FIFO queue with cache line padding
    template<class T, int TPaddingForCacheContention>
    class TLockFreePointerListFIFO : private FLockFreePointerFIFOBase<T, TPaddingForCacheContention>
    {
      public:
        void Push(T* NewItem)
        {
            FLockFreePointerFIFOBase<T, TPaddingForCacheContention>::Push(NewItem);
        }

        [[nodiscard]] T* Pop()
        {
            return FLockFreePointerFIFOBase<T, TPaddingForCacheContention>::Pop();
        }

        template<typename ContainerType>
        void PopAll(ContainerType& Output)
        {
            FLockFreePointerFIFOBase<T, TPaddingForCacheContention>::PopAll(Output);
        }

        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return FLockFreePointerFIFOBase<T, TPaddingForCacheContention>::IsEmpty();
        }
    };

    // @class TClosableLockFreePointerListUnorderedSingleConsumer
    // @brief Closable list for task dependencies (single consumer)
    //
    // Can be atomically closed to prevent further pushes.
    // Used for task dependency tracking.
    template<class T, int TPaddingForCacheContention>
    class TClosableLockFreePointerListUnorderedSingleConsumer
        : private FLockFreePointerListLIFOBase<T, TPaddingForCacheContention, 2>
    {
      public:
        void Reset()
        {
            FLockFreePointerListLIFOBase<T, TPaddingForCacheContention, 2>::Reset();
        }

        // @brief Push if the list is not closed
        // @param NewItem Item to push
        // @return True if pushed, false if list was closed
        bool PushIfNotClosed(T* NewItem)
        {
            return FLockFreePointerListLIFOBase<T, TPaddingForCacheContention, 2>::PushIf(
                NewItem,
                [](u64 State) -> bool
                { return !(State & 1); });
        }

        // @brief Pop all items and atomically close the list
        // @tparam ContainerType Container with Add() method
        // @param Output Container to receive items
        template<typename ContainerType>
        void PopAllAndClose(ContainerType& Output)
        {
            auto CheckOpenAndClose = [](u64 State) -> u64
            {
                checkLockFreePointerList(!(State & 1));
                return State | 1;
            };
            FLockFreePointerListLIFOBase<T, TPaddingForCacheContention, 2>::PopAllAndChangeState(
                Output, CheckOpenAndClose);
        }

        // @brief Check if the list is closed
        // @return True if closed
        [[nodiscard]] bool IsClosed() const
        {
            return !!(FLockFreePointerListLIFOBase<T, TPaddingForCacheContention, 2>::GetState() & 1);
        }
    };

} // namespace OloEngine
