// ParkingLot.cpp - Global hash table of wait queues keyed by memory address
// Ported 1:1 from UE5.7 UE::ParkingLot

#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Templates/Sorting.h"
#include <atomic>
#include <new>
#include <cstdlib>
#include <cstring>
#include <limits>

// Platform cache line size
#ifndef PLATFORM_CACHE_LINE_SIZE
#define PLATFORM_CACHE_LINE_SIZE 64
#endif

namespace OloEngine::ParkingLot
{
    //////////////////////////////////////////////////////////////////////////
    // Forward declarations
    class FWordMutex;

    //////////////////////////////////////////////////////////////////////////
    // FWordMutexQueueNode - Node for the intrusive queue in FWordMutex
    // Each waiting thread allocates this on its stack

    struct FWordMutexQueueNode
    {
        // Points to the next node in the tail-to-head direction. Only null for the current tail.
        FWordMutexQueueNode* Prev = nullptr;
        // Points to the next node in the head-to-tail direction. The tail points to the head.
        // Null until UnlockSlow() has traversed from the tail to fill in next pointers.
        FWordMutexQueueNode* Next = nullptr;

        FPlatformManualResetEvent Event;
    };

    //////////////////////////////////////////////////////////////////////////
    // FWordMutex - Pointer-sized intrusive queue-based mutex
    // The lower 2 bits are flags, the rest is a pointer to the queue tail

    class FWordMutex final
    {
      public:
        constexpr FWordMutex() = default;

        FWordMutex(const FWordMutex&) = delete;
        FWordMutex& operator=(const FWordMutex&) = delete;

        [[nodiscard]] bool TryLock()
        {
            uintptr_t Expected = 0;
            return m_State.compare_exchange_strong(Expected, IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed);
        }

        void Lock()
        {
            uintptr_t Expected = 0;
            if (OLO_LIKELY(m_State.compare_exchange_weak(Expected, IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }
            LockSlow();
        }

        void Unlock()
        {
            // Unlock immediately to allow other threads to acquire the lock while this thread looks for a thread to wake.
            uintptr_t CurrentState = m_State.fetch_sub(IsLockedFlag, std::memory_order_release);

            // An empty queue indicates that there are no threads to wake.
            const bool bQueueEmpty = !(CurrentState & QueueMask);
            // A locked queue indicates that another thread is looking for a thread to wake.
            const bool bQueueLocked = (CurrentState & IsQueueLockedFlag);

            if (OLO_LIKELY(bQueueEmpty || bQueueLocked))
            {
                return;
            }

            UnlockSlow(CurrentState);
        }

      private:
        void LockSlow()
        {
            static_assert((alignof(FWordMutexQueueNode) & QueueMask) == alignof(FWordMutexQueueNode),
                          "Alignment of FWordMutexQueueNode is insufficient to pack flags into the lower bits.");

            constexpr i32 SpinLimit = 40;
            i32 SpinCount = 0;
            for (;;)
            {
                uintptr_t CurrentState = m_State.load(std::memory_order_relaxed);

                // Try to acquire the lock if it was unlocked, even if there is a queue.
                if (!(CurrentState & IsLockedFlag))
                {
                    if (m_State.compare_exchange_weak(CurrentState, CurrentState | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed))
                    {
                        return;
                    }
                    continue;
                }

                // Spin up to the spin limit while there is no queue.
                if (!(CurrentState & QueueMask) && SpinCount < SpinLimit)
                {
                    FPlatformProcess::Yield();
                    ++SpinCount;
                    continue;
                }

                // Create the node that will be used to add this thread to the queue.
                FWordMutexQueueNode Self;
                Self.Event.Reset();

                // The state points to the tail of the queue, and each node points to the previous node.
                if (FWordMutexQueueNode* Tail = reinterpret_cast<FWordMutexQueueNode*>(CurrentState & QueueMask))
                {
                    Self.Prev = Tail;
                }
                else
                {
                    Self.Next = &Self;
                }

                // Swap this thread in as the tail, which makes it visible to any other thread that acquires the queue lock.
                if (!m_State.compare_exchange_weak(CurrentState, (CurrentState & ~QueueMask) | reinterpret_cast<uintptr_t>(&Self), std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    continue;
                }

                // Do not enter oversubscription during a wait on a mutex since the wait is generally too short
                LowLevelTasks::Private::FOversubscriptionAllowedScope _(false);

                // Wait until another thread wakes this thread
                Self.Event.Wait();

                // Loop back and try to acquire the lock.
                SpinCount = 0;
            }
        }

        void UnlockSlow(uintptr_t CurrentState)
        {
            // IsLockedFlag was cleared by Unlock().
            CurrentState &= ~IsLockedFlag;

            for (;;)
            {
                // Try to lock the queue.
                if (m_State.compare_exchange_weak(CurrentState, CurrentState | IsQueueLockedFlag, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    CurrentState |= IsQueueLockedFlag;
                    break;
                }

                // A locked queue indicates that another thread is looking for a thread to wake.
                if ((CurrentState & IsQueueLockedFlag) || !(CurrentState & QueueMask))
                {
                    return;
                }
            }

            for (;;)
            {
                // This thread now holds the queue lock. Neither the queue nor State will change while the queue is locked.
                FWordMutexQueueNode* Tail = reinterpret_cast<FWordMutexQueueNode*>(CurrentState & QueueMask);

                // Traverse from the tail to find the head and set next pointers for any nodes added since the last unlock.
                for (FWordMutexQueueNode* Node = Tail; !Tail->Next;)
                {
                    FWordMutexQueueNode* Prev = Node->Prev;
                    Tail->Next = Prev->Next;
                    Prev->Next = Node;
                    Node = Prev;
                }

                // Another thread may acquire the lock while this thread has been finding a thread to unlock.
                if (CurrentState & IsLockedFlag)
                {
                    if (m_State.compare_exchange_weak(CurrentState, CurrentState & ~IsQueueLockedFlag, std::memory_order_release, std::memory_order_acquire))
                    {
                        return;
                    }
                    continue;
                }

                // The next node from the tail is the head.
                FWordMutexQueueNode* Head = Tail->Next;

                // Remove the head from the queue and unlock the queue.
                if (FWordMutexQueueNode* NewHead = Head->Next; NewHead == Head)
                {
                    // Unlock and clear the queue.
                    if (!m_State.compare_exchange_strong(CurrentState, CurrentState & IsLockedFlag, std::memory_order_release, std::memory_order_acquire))
                    {
                        continue;
                    }
                }
                else
                {
                    // Clear pointers to the head node being removed.
                    NewHead->Prev = nullptr;
                    Tail->Next = NewHead;

                    // Unlock the queue regardless of whether new nodes have been added in the meantime.
                    m_State.fetch_and(~IsQueueLockedFlag, std::memory_order_release);
                }

                // Wake the thread that was at the head of the queue.
                Head->Event.Notify();
                break;
            }
        }

        static constexpr uintptr_t IsLockedFlag = 1 << 0;
        static constexpr uintptr_t IsQueueLockedFlag = 1 << 1;
        static constexpr uintptr_t QueueMask = ~(IsLockedFlag | IsQueueLockedFlag);

        std::atomic<uintptr_t> m_State{ 0 };
    };
    //////////////////////////////////////////////////////////////////////////
    // FThread - Per-thread wait state

    struct alignas(PLATFORM_CACHE_LINE_SIZE) FThread
    {
        FThread* Next = nullptr;
        std::atomic<const void*> WaitAddress{ nullptr };
        u64 WakeToken = 0;
        FPlatformManualResetEvent Event;
        std::atomic<u32> ReferenceCount{ 0 };

        void AddRef()
        {
            ReferenceCount.fetch_add(1, std::memory_order_relaxed);
        }

        void Release()
        {
            if (ReferenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                delete this;
            }
        }
    };

    //////////////////////////////////////////////////////////////////////////
    // TRefCountPtr - Simple ref-counted pointer

    template<typename T>
    class TRefCountPtr
    {
      public:
        TRefCountPtr() : m_Ptr(nullptr) {}

        explicit TRefCountPtr(T* InPtr) : m_Ptr(InPtr)
        {
            if (m_Ptr)
            {
                m_Ptr->AddRef();
            }
        }

        TRefCountPtr(const TRefCountPtr& Other) : m_Ptr(Other.m_Ptr)
        {
            if (m_Ptr)
            {
                m_Ptr->AddRef();
            }
        }

        TRefCountPtr(TRefCountPtr&& Other) noexcept : m_Ptr(Other.m_Ptr)
        {
            Other.m_Ptr = nullptr;
        }

        ~TRefCountPtr()
        {
            if (m_Ptr)
            {
                m_Ptr->Release();
            }
        }

        TRefCountPtr& operator=(const TRefCountPtr& Other)
        {
            if (this != &Other)
            {
                if (m_Ptr)
                {
                    m_Ptr->Release();
                }
                m_Ptr = Other.m_Ptr;
                if (m_Ptr)
                {
                    m_Ptr->AddRef();
                }
            }
            return *this;
        }

        TRefCountPtr& operator=(TRefCountPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                if (m_Ptr)
                {
                    m_Ptr->Release();
                }
                m_Ptr = Other.m_Ptr;
                Other.m_Ptr = nullptr;
            }
            return *this;
        }

        TRefCountPtr& operator=(T* InPtr)
        {
            if (m_Ptr != InPtr)
            {
                if (m_Ptr)
                {
                    m_Ptr->Release();
                }
                m_Ptr = InPtr;
                if (m_Ptr)
                {
                    m_Ptr->AddRef();
                }
            }
            return *this;
        }

        T* operator->() const
        {
            return m_Ptr;
        }
        T& operator*() const
        {
            return *m_Ptr;
        }
        T* Get() const
        {
            return m_Ptr;
        }
        explicit operator bool() const
        {
            return m_Ptr != nullptr;
        }

      private:
        T* m_Ptr;
    };

    //////////////////////////////////////////////////////////////////////////
    // Forward declaration for FTable
    class FTable;

    //////////////////////////////////////////////////////////////////////////
    // FThreadLocalData - Thread-local storage for FThread

    class FThreadLocalData
    {
      public:
        static TRefCountPtr<FThread> Get()
        {
            static thread_local FThreadLocalData s_ThreadLocalData;

            if (OLO_LIKELY(s_ThreadLocalData.m_Thread.Get()))
            {
                return s_ThreadLocalData.m_Thread;
            }

            if (s_ThreadLocalData.m_bDestroyed)
            {
                // During thread destruction, create a temporary FThread
                return TRefCountPtr<FThread>(new FThread);
            }

            s_ThreadLocalData.m_Thread = TRefCountPtr<FThread>(new FThread);
            return s_ThreadLocalData.m_Thread;
        }

      private:
        FThreadLocalData();
        ~FThreadLocalData();

        inline static std::atomic<u32> s_ThreadCount{ 0 };
        TRefCountPtr<FThread> m_Thread;
        bool m_bDestroyed = false;
    };

    //////////////////////////////////////////////////////////////////////////
    // EQueueAction - Visitor action enum

    enum class EQueueAction
    {
        Stop,
        Continue,
        RemoveAndStop,
        RemoveAndContinue,
    };

    //////////////////////////////////////////////////////////////////////////
    // TDynamicUniqueLock - RAII lock wrapper

    template<typename MutexType>
    class TDynamicUniqueLock
    {
      public:
        TDynamicUniqueLock() : m_Mutex(nullptr) {}

        explicit TDynamicUniqueLock(MutexType& InMutex) : m_Mutex(&InMutex)
        {
            m_Mutex->Lock();
        }

        TDynamicUniqueLock(const TDynamicUniqueLock&) = delete;
        TDynamicUniqueLock& operator=(const TDynamicUniqueLock&) = delete;

        TDynamicUniqueLock(TDynamicUniqueLock&& Other) noexcept : m_Mutex(Other.m_Mutex)
        {
            Other.m_Mutex = nullptr;
        }

        TDynamicUniqueLock& operator=(TDynamicUniqueLock&& Other) noexcept
        {
            if (m_Mutex)
            {
                m_Mutex->Unlock();
            }
            m_Mutex = Other.m_Mutex;
            Other.m_Mutex = nullptr;
            return *this;
        }

        ~TDynamicUniqueLock()
        {
            if (m_Mutex)
            {
                m_Mutex->Unlock();
            }
        }

        void Unlock()
        {
            if (m_Mutex)
            {
                m_Mutex->Unlock();
                m_Mutex = nullptr;
            }
        }

        explicit operator bool() const
        {
            return m_Mutex != nullptr;
        }

      private:
        MutexType* m_Mutex;
    };

    //////////////////////////////////////////////////////////////////////////
    // FBucket - Cache-line aligned wait queue

    class alignas(PLATFORM_CACHE_LINE_SIZE) FBucket final
    {
      public:
        TDynamicUniqueLock<FWordMutex> LockDynamic()
        {
            return TDynamicUniqueLock<FWordMutex>(m_Mutex);
        }

        void Lock()
        {
            m_Mutex.Lock();
        }
        void Unlock()
        {
            m_Mutex.Unlock();
        }

        bool IsEmpty() const
        {
            return m_Head == nullptr;
        }

        void Enqueue(FThread* Thread)
        {
            Thread->Next = nullptr;
            if (m_Tail)
            {
                m_Tail->Next = Thread;
                m_Tail = Thread;
            }
            else
            {
                m_Head = Thread;
                m_Tail = Thread;
            }
        }

        FThread* Dequeue()
        {
            FThread* Thread = m_Head;
            if (Thread)
            {
                m_Head = Thread->Next;
                Thread->Next = nullptr;
                if (m_Tail == Thread)
                {
                    m_Tail = nullptr;
                }
            }
            return Thread;
        }

        template<typename VisitorType>
        void DequeueIf(VisitorType&& Visitor)
        {
            // Double-pointer iteration pattern for in-place removal
            FThread** Next = &m_Head;
            FThread* Prev = nullptr;
            while (FThread* Thread = *Next)
            {
                switch (Visitor(Thread))
                {
                    case EQueueAction::Stop:
                        return;
                    case EQueueAction::Continue:
                        Prev = Thread;
                        Next = &Thread->Next;
                        break;
                    case EQueueAction::RemoveAndStop:
                        if (m_Tail == Thread)
                        {
                            m_Tail = Prev;
                        }
                        *Next = Thread->Next;
                        Thread->Next = nullptr;
                        return;
                    case EQueueAction::RemoveAndContinue:
                        if (m_Tail == Thread)
                        {
                            m_Tail = Prev;
                        }
                        *Next = Thread->Next;
                        Thread->Next = nullptr;
                        break;
                }
            }
        }

      private:
        FWordMutex m_Mutex;
        FThread* m_Head = nullptr;
        FThread* m_Tail = nullptr;
    };

    //////////////////////////////////////////////////////////////////////////
    // FTable - Hash table of buckets

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200) // nonstandard extension: zero-sized array - intentional flexible array member
#endif
    class FTable final
    {
        static constexpr u32 MinSize = 32;

      public:
        static FBucket& FindOrCreateBucket(const void* Address, TDynamicUniqueLock<FWordMutex>& OutLock)
        {
            const u32 Hash = HashAddress(Address);
            for (;;)
            {
                FTable& Table = CreateOrGet();
                u32 Index = Hash % Table.m_BucketCount;
                FBucket& Bucket = Table.FindOrCreateBucketAtIndex(Index, []
                                                                  { return new FBucket(); });

                OutLock = Bucket.LockDynamic();
                // Check if table was resized while we were waiting
                if (&Table == s_GlobalTable.load(std::memory_order_acquire)) [[likely]]
                {
                    return Bucket;
                }
                // Table was resized, unlock and retry
                OutLock.Unlock();
            }
        }

        static FBucket* FindBucket(const void* Address, TDynamicUniqueLock<FWordMutex>& OutLock)
        {
            const u32 Hash = HashAddress(Address);
            for (;;)
            {
                FTable* Table = s_GlobalTable.load(std::memory_order_acquire);
                if (!Table)
                {
                    return nullptr;
                }

                u32 Index = Hash % Table->m_BucketCount;
                FBucket* Bucket = Table->m_Buckets[Index].load(std::memory_order_acquire);
                if (!Bucket)
                {
                    return nullptr;
                }

                OutLock = Bucket->LockDynamic();
                // Check if table was resized while we were waiting
                if (Table == s_GlobalTable.load(std::memory_order_acquire)) [[likely]]
                {
                    return Bucket;
                }
                // Table was resized, unlock and retry
                OutLock.Unlock();
            }
        }

        static void Reserve(u32 ThreadCount)
        {
            const u32 TargetBucketCount = RoundUpToPowerOfTwo(ThreadCount);
            TArray<FBucket*> ExistingBuckets;

            for (;;)
            {
                FTable& ExistingTable = CreateOrGet();

                if (OLO_LIKELY(ExistingTable.m_BucketCount >= TargetBucketCount))
                {
                    // Reserve is called every time a thread is created and has amortized constant time
                    // because of its power-of-two table growth. Most calls return here without locking.
                    return;
                }

                if (!TryLock(ExistingTable, ExistingBuckets))
                {
                    continue;
                }

                // Gather waiting threads to be redistributed into the buckets of the new table.
                // Threads with the same address remain in the same relative order as they were queued.
                TArray<FThread*> Threads;
                for (FBucket* Bucket : ExistingBuckets)
                {
                    while (FThread* Thread = Bucket->Dequeue())
                    {
                        Threads.Add(Thread);
                    }
                }

                FTable& NewTable = Create(TargetBucketCount);

                // Reuse existing now-empty buckets when populating the new table.
                TArray<FBucket*> AvailableBuckets = ExistingBuckets;
                const auto AllocateBucket = [&AvailableBuckets]() -> FBucket*
                {
                    if (AvailableBuckets.Num() > 0)
                    {
                        FBucket* Bucket = AvailableBuckets.Last();
                        AvailableBuckets.Pop();
                        return Bucket;
                    }
                    return new FBucket();
                };

                // Add waiting threads to the new table.
                for (FThread* Thread : Threads)
                {
                    const u32 Hash = HashAddress(Thread->WaitAddress.load(std::memory_order_relaxed));
                    const u32 Index = Hash % NewTable.m_BucketCount;
                    FBucket& Bucket = NewTable.FindOrCreateBucketAtIndex(Index, AllocateBucket);
                    Bucket.Enqueue(Thread);
                }

                // Assign any available buckets to the table to avoid having to free them.
                for (u32 Index = 0; AvailableBuckets.Num() > 0 && Index < NewTable.m_BucketCount; ++Index)
                {
                    NewTable.FindOrCreateBucketAtIndex(Index, AllocateBucket);
                }

                // Make the new table visible to other threads.
                FTable* CompareTable = s_GlobalTable.exchange(&NewTable, std::memory_order_release);
                OLO_CORE_ASSERT(CompareTable == &ExistingTable);

                // Unlock buckets that came from the existing table now that the new table is visible.
                UnlockBuckets(ExistingBuckets);
                return;
            }
        }

      private:
        static FTable& CreateOrGet()
        {
            FTable* Table = s_GlobalTable.load(std::memory_order_acquire);
            if (OLO_LIKELY(Table))
            {
                return *Table;
            }

            FTable& NewTable = Create(MinSize);
            FTable* Expected = nullptr;
            if (OLO_LIKELY(s_GlobalTable.compare_exchange_strong(Expected, &NewTable, std::memory_order_release, std::memory_order_acquire)))
            {
                return NewTable;
            }

            Destroy(NewTable);
            OLO_CORE_ASSERT(Expected);
            return *Expected;
        }

        static FTable& Create(u32 Size)
        {
            // Round up to power of 2
            u32 BucketCount = RoundUpToPowerOfTwo(Size);
            if (BucketCount < MinSize)
            {
                BucketCount = MinSize;
            }

            // Allocate table with flexible array
            sizet TableSize = sizeof(FTable) + BucketCount * sizeof(std::atomic<FBucket*>);
            void* Memory = std::malloc(TableSize);
            std::memset(Memory, 0, TableSize);
            FTable* NewTable = new (Memory) FTable();
            NewTable->m_BucketCount = BucketCount;
            return *NewTable;
        }

        static void Destroy(FTable& Table)
        {
            Table.~FTable();
            std::free(&Table);
        }

        template<typename AllocatorType>
        FBucket& FindOrCreateBucketAtIndex(u32 Index, AllocatorType&& BucketAllocator)
        {
            std::atomic<FBucket*>& BucketPtr = m_Buckets[Index];
            FBucket* Bucket = BucketPtr.load(std::memory_order_acquire);
            if (OLO_UNLIKELY(!Bucket))
            {
                FBucket* NewBucket = BucketAllocator();
                if (BucketPtr.compare_exchange_strong(Bucket, NewBucket, std::memory_order_release, std::memory_order_acquire))
                {
                    Bucket = NewBucket;
                }
                else
                {
                    delete NewBucket;
                }
                OLO_CORE_ASSERT(Bucket);
            }
            return *Bucket;
        }

        static bool TryLock(FTable& Table, TArray<FBucket*>& OutBuckets)
        {
            OutBuckets.Reset();
            OutBuckets.Reserve(Table.m_BucketCount);

            // Gather buckets from the table, creating them as needed because the lock is on the bucket.
            for (u32 Index = 0; Index < Table.m_BucketCount; ++Index)
            {
                OutBuckets.Add(&Table.FindOrCreateBucketAtIndex(Index, []
                                                                { return new FBucket(); }));
            }

            // Lock the buckets in order by address to ensure consistent ordering regardless of the table being locked.
            Algo::Sort(OutBuckets);
            for (FBucket* Bucket : OutBuckets)
            {
                Bucket->Lock();
            }

            // Table is locked if the global table pointer still points to it, otherwise it has grown.
            if (&Table == s_GlobalTable.load(std::memory_order_acquire))
            {
                return true;
            }

            // Unlock and return that the table could not be locked.
            UnlockBuckets(OutBuckets);
            OutBuckets.Reset();
            return false;
        }

        static void UnlockBuckets(const TArray<FBucket*>& LockedBuckets)
        {
            for (FBucket* Bucket : LockedBuckets)
            {
                Bucket->Unlock();
            }
        }

        static u32 RoundUpToPowerOfTwo(u32 Value)
        {
            if (Value == 0)
            {
                return 1;
            }
            Value--;
            Value |= Value >> 1;
            Value |= Value >> 2;
            Value |= Value >> 4;
            Value |= Value >> 8;
            Value |= Value >> 16;
            return Value + 1;
        }

        static u32 HashAddress(const void* Address)
        {
            // High-quality hash function from UE5.7
            constexpr u64 A = 0xdc2b17dc9d2fbc29ULL;
            constexpr u64 B = 0xcb1014192cb2c5fcULL;
            constexpr u64 C = 0x5b12db9242bd7ce7ULL;
            const uintptr_t Value = reinterpret_cast<uintptr_t>(Address);
            return static_cast<u32>(((A * (Value >> 32)) + (B * (Value & 0xffffffff)) + C) >> 32);
        }

        inline static std::atomic<FTable*> s_GlobalTable{ nullptr };
        u32 m_BucketCount = 0;
        std::atomic<FBucket*> m_Buckets[0]; // Flexible array member
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    //////////////////////////////////////////////////////////////////////////
    // FThreadLocalData implementation (after FTable is complete)

    FThreadLocalData::FThreadLocalData()
    {
        FTable::Reserve(s_ThreadCount.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    FThreadLocalData::~FThreadLocalData()
    {
        s_ThreadCount.fetch_sub(1, std::memory_order_relaxed);
        m_Thread = TRefCountPtr<FThread>(); // Release thread
        m_bDestroyed = true;
    }

    //////////////////////////////////////////////////////////////////////////
    // Private namespace - raw function pointer implementations

    namespace Private
    {

        FWaitState Wait(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext)
        {
            TRefCountPtr<FThread> Self = FThreadLocalData::Get();
            FWaitState State;

            // Enqueue the thread if CanWait returns true while the bucket is locked
            {
                TDynamicUniqueLock<FWordMutex> BucketLock;
                FBucket& Bucket = FTable::FindOrCreateBucket(Address, BucketLock);
                State.bDidWait = !CanWait || CanWait(CanWaitContext);
                if (!State.bDidWait)
                {
                    return State;
                }
                Self->WaitAddress.store(Address, std::memory_order_relaxed);
                Self->Event.Reset();
                Bucket.Enqueue(Self.Get());
            }

            // BeforeWait must be invoked after the bucket is unlocked
            if (BeforeWait)
            {
                BeforeWait(BeforeWaitContext);
            }

            // Wait until the thread has been dequeued
            Self->Event.Wait();

            State.bDidWake = true;
            State.WakeToken = Self->WakeToken;
            Self->WakeToken = 0;
            return State;
        }

        //////////////////////////////////////////////////////////////////////////
        // TimedWait - Internal helper for timed waits

        static FWaitState TimedWait(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext,
            void (*WaitOnEvent)(void*, FPlatformManualResetEvent&),
            void* WaitOnEventContext)
        {
            TRefCountPtr<FThread> Self = FThreadLocalData::Get();
            FWaitState State;

            // Enqueue the thread if CanWait returns true while the bucket is locked
            {
                TDynamicUniqueLock<FWordMutex> BucketLock;
                FBucket& Bucket = FTable::FindOrCreateBucket(Address, BucketLock);
                State.bDidWait = !CanWait || CanWait(CanWaitContext);
                if (!State.bDidWait)
                {
                    return State;
                }
                Self->WaitAddress.store(Address, std::memory_order_relaxed);
                Self->Event.Reset();
                Bucket.Enqueue(Self.Get());
            }

            // BeforeWait must be invoked after the bucket is unlocked
            if (BeforeWait)
            {
                BeforeWait(BeforeWaitContext);
            }

            // Wait until the timeout or until the thread has been dequeued
            WaitOnEvent(WaitOnEventContext, Self->Event);

            // WaitAddress is reset when the thread is dequeued
            if (!Self->WaitAddress.load(std::memory_order_relaxed))
            {
                State.bDidWake = true;
                State.WakeToken = Self->WakeToken;
                Self->WakeToken = 0;
                return State;
            }

            // The timeout was reached and the thread needs to dequeue itself.
            // This can race with a call to wake a thread, which means Self is unsafe to access outside of the lock.
            bool bDequeued = false;
            if (TDynamicUniqueLock<FWordMutex> BucketLock; FBucket* Bucket = FTable::FindBucket(Address, BucketLock))
            {
                Bucket->DequeueIf([SelfPtr = Self.Get(), &bDequeued](FThread* Thread) -> EQueueAction
                                  {
				if (Thread == SelfPtr)
				{
					bDequeued = true;
					Thread->WaitAddress.store(nullptr, std::memory_order_relaxed);
					return EQueueAction::RemoveAndStop;
				}
				return EQueueAction::Continue; });
            }

            // The thread did not dequeue itself, which means that we need to wait until the other thread
            // has finished waking this thread by setting its wait address to null.
            if (!bDequeued)
            {
                Self->Event.Wait();
                State.bDidWake = true;
                State.WakeToken = Self->WakeToken;
                Self->WakeToken = 0;
            }

            return State;
        }

        static void WaitForTime(void* WaitTime, FPlatformManualResetEvent& Event)
        {
            Event.WaitFor(*static_cast<FMonotonicTimeSpan*>(WaitTime));
        }

        static void WaitUntilTime(void* WaitTime, FPlatformManualResetEvent& Event)
        {
            Event.WaitUntil(*static_cast<FMonotonicTimePoint*>(WaitTime));
        }

        FWaitState WaitFor(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext,
            FMonotonicTimeSpan WaitTime)
        {
            OLO_CORE_ASSERT(!WaitTime.IsNaN());
            return TimedWait(Address, CanWait, CanWaitContext, BeforeWait, BeforeWaitContext, WaitForTime, &WaitTime);
        }

        FWaitState WaitUntil(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext,
            FMonotonicTimePoint WaitTime)
        {
            OLO_CORE_ASSERT(!WaitTime.IsNaN());
            return TimedWait(Address, CanWait, CanWaitContext, BeforeWait, BeforeWaitContext, WaitUntilTime, &WaitTime);
        }

        void WakeOne(
            const void* Address,
            u64 (*OnWakeState)(void* Context, FWakeState State),
            void* OnWakeStateContext)
        {
            TRefCountPtr<FThread> WakeThread;
            u64 WakeToken = 0;

            {
                TDynamicUniqueLock<FWordMutex> BucketLock;
                FBucket& Bucket = FTable::FindOrCreateBucket(Address, BucketLock);
                Bucket.DequeueIf([Address, &WakeThread](FThread* Thread) -> EQueueAction
                                 {
				if (Thread->WaitAddress.load(std::memory_order_relaxed) == Address)
				{
					WakeThread = TRefCountPtr<FThread>(Thread);
					return EQueueAction::RemoveAndStop;
				}
				return EQueueAction::Continue; });

                FWakeState WakeState;
                WakeState.bDidWake = !!WakeThread;
                WakeState.bHasWaitingThreads = !Bucket.IsEmpty();
                WakeToken = OnWakeState ? OnWakeState(OnWakeStateContext, WakeState) : 0;
            }

            if (WakeThread)
            {
                WakeThread->WakeToken = WakeToken;
                WakeThread->WaitAddress.store(nullptr, std::memory_order_relaxed);
                WakeThread->Event.Notify();
            }
        }

    } // namespace Private

    //////////////////////////////////////////////////////////////////////////
    // Public API Implementation (non-inline functions)

    FWakeState WakeOne(const void* Address)
    {
        FWakeState Result;
        Private::WakeOne(Address, [](void* Context, FWakeState State) -> u64
                         {
			*static_cast<FWakeState*>(Context) = State;
			return 0; }, &Result);
        return Result;
    }

    u32 WakeMultiple(const void* Address, u32 WakeCount)
    {
        TArray<TRefCountPtr<FThread>> WakeThreads;
        WakeThreads.Reserve(FMath::Min(WakeCount, 128u));

        {
            TDynamicUniqueLock<FWordMutex> BucketLock;
            FBucket* Bucket = FTable::FindBucket(Address, BucketLock);
            if (Bucket)
            {
                Bucket->DequeueIf([Address, &WakeThreads, WakeCount](FThread* Thread) -> EQueueAction
                                  {
					if (Thread->WaitAddress.load(std::memory_order_relaxed) == Address)
					{
						WakeThreads.Add(TRefCountPtr<FThread>(Thread));
						return static_cast<u32>(WakeThreads.Num()) == WakeCount
							? EQueueAction::RemoveAndStop
							: EQueueAction::RemoveAndContinue;
					}
					return EQueueAction::Continue; });
            }
        }

        for (auto& WakeThread : WakeThreads)
        {
            WakeThread->WaitAddress.store(nullptr, std::memory_order_relaxed);
            WakeThread->Event.Notify();
        }

        return static_cast<u32>(WakeThreads.Num());
    }

    u32 WakeAll(const void* Address)
    {
        return WakeMultiple(Address, std::numeric_limits<u32>::max());
    }

    void Reserve(u32 ThreadCount)
    {
        FTable::Reserve(ThreadCount);
    }

} // namespace OloEngine::ParkingLot
