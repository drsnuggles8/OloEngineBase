#pragma once

/**
 * @file HazardPointer.h
 * @brief Hazard Pointer based safe memory reclamation for lock-free data structures
 * 
 * Implements the Hazard Pointer algorithm for safe memory reclamation in lock-free
 * data structures. Based on the paper "Hazard Pointers: Safe Memory Reclamation 
 * for Lock-Free objects" by Maged Michael.
 * 
 * Key concepts:
 * - Hazard records protect pointers from being deleted while in use
 * - Type-erased deleters ensure correct destructor calls
 * - Thread-local reclamation lists amortize deletion costs
 * 
 * Ported from Unreal Engine's Experimental/Containers/HazardPointer.h
 * 
 * @see http://web.cecs.pdx.edu/~walpole/class/cs510/papers/11.pdf
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/CriticalSection.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Algo/Sort.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Templates/Sorting.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <atomic>
#include <chrono>

namespace OloEngine
{

// Forward declaration
class FHazardPointerCollection;

namespace HazardPointer_Impl
{
    /**
     * @class FHazardDeleter
     * @brief Type-erased base class for calling the correct destructor
     * 
     * Used to erase the type of a class so that we can call the correct destructor
     * when reclaiming memory.
     */
    class FHazardDeleter
    {
        friend class ::OloEngine::FHazardPointerCollection;

    protected:
        void* Pointer;
        FHazardDeleter(void* InPointer) : Pointer(InPointer) {}

    public:
        virtual void Delete() 
        {
            OLO_CORE_ASSERT(false, "FHazardDeleter::Delete called on base class");
        }

    public:
        FHazardDeleter(const FHazardDeleter& Other)
        {
            // Deliberately stomp over the vtable pointer so that this FHazardDeleter becomes a THazardDeleter
            FMemory::Memcpy(this, &Other, sizeof(FHazardDeleter)); //-V598
        }

        FHazardDeleter& operator= (const FHazardDeleter& Other)
        {
            // Deliberately stomp over the vtable pointer so that this FHazardDeleter becomes a THazardDeleter
            FMemory::Memcpy(this, &Other, sizeof(FHazardDeleter)); //-V598
            return *this;
        }

        bool operator== (const FHazardDeleter& Other) const
        {
            return Other.Pointer == Pointer;
        }
    };
} // namespace HazardPointer_Impl

// Forward declaration
template<typename, bool>
class THazardPointer;

/**
 * @class FHazardPointerCollection
 * @brief Collection of hazard pointers for safe memory reclamation
 * 
 * Manages hazard pointer slots that threads can acquire to protect pointers
 * from being deleted while in use. Provides safe deletion via deferred reclamation.
 */
class FHazardPointerCollection
{
    template<typename, bool>
    friend class THazardPointer;

    /**
     * @struct FTlsData
     * @brief Thread-local data for reclamation list
     */
    struct FTlsData
    {
        TArray<HazardPointer_Impl::FHazardDeleter> ReclamationList;
        double TimeOfLastCollection = 0.0;

        ~FTlsData()
        {
            for (HazardPointer_Impl::FHazardDeleter& Deleter : ReclamationList)
            {
                Deleter.Delete();
            }
            ReclamationList.Empty();
        }
    };

    /**
     * @class FHazardRecord
     * @brief A single hazard pointer slot
     */
    class alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) FHazardRecord
    {
        friend class FHazardPointerCollection;

        template<typename, bool>
        friend class THazardPointer;

        static constexpr uptr FreeHazardEntry = ~uptr(0);

        std::atomic<uptr> Hazard{ FreeHazardEntry };

        FHazardRecord() = default;

        inline void* GetHazard() const
        {
            return reinterpret_cast<void*>(Hazard.load(std::memory_order_acquire));
        }

        // Assign hazard pointer once acquired
        [[nodiscard]] inline void* SetHazard(void* InHazard)
        {
            Hazard.store(reinterpret_cast<uptr>(InHazard), std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return reinterpret_cast<void*>(Hazard.load(std::memory_order_acquire));
        }

        // This thread wants to re-use the slot but does not want to hold onto the pointer
        inline void Retire()
        {
            Hazard.store(0, std::memory_order_release);
        }

        // Good to reuse by another thread
        inline void Release()
        {
            Hazard.store(FreeHazardEntry, std::memory_order_release);
        }
    };

    /**
     * @class THazardDeleter
     * @brief Type-specific deleter that calls the correct destructor
     */
    template<typename D>
    class THazardDeleter final : public HazardPointer_Impl::FHazardDeleter
    {
    public:
        THazardDeleter(D* InPointer) : HazardPointer_Impl::FHazardDeleter(reinterpret_cast<void*>(InPointer))
        {
            static_assert(sizeof(THazardDeleter<D>) == sizeof(FHazardDeleter), 
                         "Size mismatch: we want to store a THazardDeleter in an FHazardDeleter array");
        }

        void Delete() override
        {
            D* Ptr = reinterpret_cast<D*>(Pointer);
            delete Ptr;
        }
    };

    static constexpr u32 HazardChunkSize = 32;

    /**
     * @struct FHazardRecordChunk
     * @brief A chunk of hazard records for growing the pool
     */
    struct FHazardRecordChunk
    {
        FHazardRecord Records[HazardChunkSize] = {};
        std::atomic<FHazardRecordChunk*> Next{ nullptr };

        inline void* operator new(sizet Size)
        {
            return FMemory::Malloc(Size, 128u);
        }

        inline void operator delete(void* Ptr)
        {
            FMemory::Free(Ptr);
        }
    };

    FHazardRecordChunk m_Head;

    FCriticalSection m_AllTlsVariablesCS;
    FCriticalSection m_HazardRecordBlocksCS;
    TArray<FTlsData*> m_AllTlsVariables;
    TArray<FHazardRecordChunk*> m_HazardRecordBlocks;

    u32 m_CollectablesTlsSlot = FPlatformTLS::InvalidTlsSlot;
    std::atomic_uint m_TotalNumHazardRecords{ HazardChunkSize };

    void Collect(TArray<HazardPointer_Impl::FHazardDeleter>& Collectables);

    // Mark pointer for deletion
    void Delete(const HazardPointer_Impl::FHazardDeleter& Deleter, i32 CollectLimit);

    template<bool Cached>
    FHazardRecord* Grow();

public:
    FHazardPointerCollection()
    {
        // Allocate TLS slot for per-thread reclamation data
        m_CollectablesTlsSlot = FPlatformTLS::AllocTlsSlot();
        OLO_CORE_ASSERT(FPlatformTLS::IsValidTlsSlot(m_CollectablesTlsSlot), 
                       "Failed to allocate TLS slot for HazardPointerCollection");
    }

    ~FHazardPointerCollection()
    {
        // Clean up all TLS data
        {
            FScopeLock Lock(&m_AllTlsVariablesCS);
            for (FTlsData* TlsData : m_AllTlsVariables)
            {
                delete TlsData;
            }
            m_AllTlsVariables.Empty();
        }

        // Clean up hazard record chunks (not the head, which is embedded)
        {
            FScopeLock Lock(&m_HazardRecordBlocksCS);
            for (FHazardRecordChunk* Block : m_HazardRecordBlocks)
            {
                delete Block;
            }
            m_HazardRecordBlocks.Empty();
        }

        // Free the TLS slot
        if (FPlatformTLS::IsValidTlsSlot(m_CollectablesTlsSlot))
        {
            FPlatformTLS::FreeTlsSlot(m_CollectablesTlsSlot);
            m_CollectablesTlsSlot = FPlatformTLS::InvalidTlsSlot;
        }
    }

    /**
     * @brief Acquire a hazard pointer slot
     * 
     * Grab a hazard pointer and once hazard is set the other threads leave it alone.
     * 
     * @tparam Cached If true, starts search from second chunk (optimization)
     * @return Pointer to acquired hazard record
     */
    template<bool Cached>
    inline FHazardRecord* Acquire()
    {
        struct FPseudo
        {
            static inline u32 GetThreadId()
            {
                static std::atomic_uint counter{0};
                u32 value = counter.fetch_add(1, std::memory_order_relaxed);
                value = ((value >> 16) ^ value) * 0x45d9f3b;
                value = ((value >> 16) ^ value) * 0x45d9f3b;
                value = (value >> 22) ^ value;
                return value;
            }
        };

        static thread_local u32 StartIndex = FPseudo::GetThreadId();
    
        FHazardRecordChunk* p = &m_Head;
        if (Cached)
        {
            p = p->Next.load(std::memory_order_relaxed);
            goto TestCondition;
        }
        
        // Search HazardPointerList for an empty entry
        do
        {
            for (u64 idx = 0; idx < HazardChunkSize; idx++)
            {
                uptr Nullptr = 0;
                uptr FreeEntry = FHazardRecord::FreeHazardEntry;
                u64 i = (StartIndex + idx) % HazardChunkSize;
                if (p->Records[i].Hazard.compare_exchange_weak(FreeEntry, Nullptr, std::memory_order_relaxed))
                {
                    OLO_CORE_ASSERT(p->Records[i].GetHazard() == nullptr, "Hazard should be null after acquire");
                    return &p->Records[i];
                }
            }
            p = p->Next.load(std::memory_order_relaxed);
            TestCondition:;
        } while (p);

        return Grow<Cached>();
    }

    /**
     * @brief Mark a pointer for deletion with type-specific destructor
     * 
     * If we own the pointer, schedule it for deletion when safe.
     * 
     * @tparam D Type of object to delete
     * @param Pointer Pointer to delete (can be nullptr)
     * @param CollectLimit Threshold for triggering collection (-1 for default)
     */
    template<typename D>
    inline void Delete(D* Pointer, i32 CollectLimit = -1)
    {
        if (Pointer)
        {
            Delete(THazardDeleter<D>(Pointer), CollectLimit);
        }
    }
};

// Implementation of template methods that need full class definition

template<bool Cached>
FHazardPointerCollection::FHazardRecord* FHazardPointerCollection::Grow()
{
    // Check total before taking lock to enable re-check optimization
    u32 TotalNumBeforeLock = m_TotalNumHazardRecords.load(std::memory_order_relaxed);
    
    // No empty Entry found: create new under lock
    FScopeLock Lock(&m_HazardRecordBlocksCS);
    
    // Re-check: another thread may have grown the pool while we waited for the lock
    if (m_TotalNumHazardRecords.load(std::memory_order_relaxed) != TotalNumBeforeLock)
    {
        return Acquire<Cached>();
    }
    
    // Find the end of the list
    FHazardRecordChunk* lst = &m_Head;
    while (lst->Next.load(std::memory_order_relaxed) != nullptr)
    {
        lst = lst->Next.load(std::memory_order_relaxed);
    }
    
    FHazardRecordChunk* NewChunk = new FHazardRecordChunk();
    NewChunk->Records[0].Retire();  // Mark first slot as acquired (value = 0)
    
    OLO_CORE_ASSERT(lst->Next.load(std::memory_order_acquire) == nullptr, "List end should be null");
    lst->Next.store(NewChunk, std::memory_order_release);
    
    // Keep count of HazardPointers, there should not be too many (maybe 2 per thread)
    m_TotalNumHazardRecords.fetch_add(HazardChunkSize, std::memory_order_relaxed);
    
    m_HazardRecordBlocks.Add(NewChunk);
    
    OLO_CORE_ASSERT(NewChunk->Records[0].GetHazard() == nullptr, "First record should be null after Retire");
    return &NewChunk->Records[0];
}

inline void FHazardPointerCollection::Delete(const HazardPointer_Impl::FHazardDeleter& Deleter, i32 CollectLimit)
{
    // Use FPlatformTLS for thread-local storage (matches UE5.7)
    FTlsData* TlsData = static_cast<FTlsData*>(FPlatformTLS::GetTlsValue(m_CollectablesTlsSlot));
    
    if (TlsData == nullptr)
    {
        FScopeLock Lock(&m_AllTlsVariablesCS);
        TlsData = new FTlsData();
        m_AllTlsVariables.Add(TlsData);
        FPlatformTLS::SetTlsValue(m_CollectablesTlsSlot, TlsData);
    }
    
    OLO_CORE_ASSERT(!TlsData->ReclamationList.Contains(Deleter), "Deleter already in reclamation list");
    
    // Add to-be-deleted pointer to thread local list
    TlsData->ReclamationList.Add(Deleter);
    
    // Maybe scan the list - use time and count based triggers (matches UE5.7)
    // Note: Using a simple time approximation since we don't have FApp::GetGameTime()
    static thread_local double s_LastCollectionTime = 0.0;
    const double CurrentTime = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    const bool bTimeLimit = (CurrentTime - TlsData->TimeOfLastCollection) > 1.0;
    const i32 DeleteMetric = CollectLimit <= 0 ? ((5 * static_cast<i32>(m_TotalNumHazardRecords.load(std::memory_order_relaxed))) / 4) : CollectLimit;
    
    if (bTimeLimit || TlsData->ReclamationList.Num() >= DeleteMetric)
    {
        TlsData->TimeOfLastCollection = CurrentTime;
        Collect(TlsData->ReclamationList);
    }
}

/**
 * @brief Binary search for a pointer in a sorted array of hazard pointers
 * @param Hazards Sorted array of hazard pointers
 * @param Pointer Pointer to search for
 * @return Index of the pointer if found, ~0u otherwise
 */
static inline u32 BinarySearchHazard(const TArray<void*, TInlineAllocator<64>>& Hazards, void* Pointer)
{
    i32 Index = 0;
    i32 Size = Hazards.Num();

    // Start with binary search for larger lists
    while (Size > 32)
    {
        const i32 LeftoverSize = Size % 2;
        Size = Size / 2;

        const i32 CheckIndex = Index + Size;
        const i32 IndexIfLess = CheckIndex + LeftoverSize;

        Index = Hazards[CheckIndex] < Pointer ? IndexIfLess : Index;
    }

    // Small size array optimization - linear search for remainder
    i32 ArrayEnd = (Index + Size + 1 < Hazards.Num()) ? Index + Size + 1 : Hazards.Num();
    while (Index < ArrayEnd)
    {
        if (Hazards[Index] == Pointer)
        {
            return static_cast<u32>(Index);
        }
        Index++;
    }

    return ~0u;
}

inline void FHazardPointerCollection::Collect(TArray<HazardPointer_Impl::FHazardDeleter>& Collectables)
{
    // Collect all global Hazards in the system using inline allocator for performance
    TArray<void*, TInlineAllocator<64>> Hazards;
    
    FHazardRecordChunk* p = &m_Head;
    do
    {
        for (u32 i = 0; i < HazardChunkSize; i++)
        {
            void* h = p->Records[i].GetHazard();
            if (h && h != reinterpret_cast<void*>(FHazardRecord::FreeHazardEntry))
            {
                Hazards.Add(h);
            }
        }
        p = p->Next.load(std::memory_order_acquire);
    } while (p);
    
    // Sort the pointers for binary search
    if (Hazards.Num() > 1)
    {
        TArrayRange<void*> ArrayRange(&Hazards[0], Hazards.Num());
        Algo::Sort(ArrayRange, TLess<void*>());
    }
    
    // Use inline allocator for deleted collectables too
    TArray<HazardPointer_Impl::FHazardDeleter, TInlineAllocator<64>> DeletedCollectables;
    
    // Check all thread local to-be-deleted pointers if they are NOT in the hazard list
    for (i32 c = 0; c < Collectables.Num(); c++)
    {
        const HazardPointer_Impl::FHazardDeleter& Collectable = Collectables[c];
        u32 Index = BinarySearchHazard(Hazards, Collectable.Pointer);

        // Potentially delete and remove item from thread local list
        if (Index == ~0u)
        {
            DeletedCollectables.Add(Collectable);
            Collectables[c] = Collectables.Last();
            Collectables.Pop(EAllowShrinking::No);
            c--;
        }
    }

    // Actually delete the collectables that are safe to delete
    for (HazardPointer_Impl::FHazardDeleter& DeletedCollectable : DeletedCollectables)
    {
        DeletedCollectable.Delete();
    }
}

/**
 * @class THazardPointer
 * @brief RAII wrapper for a hazard pointer slot
 * 
 * Used to keep an allocation alive until all threads that referenced it 
 * finished their access.
 * 
 * @tparam H Type of object being protected
 * @tparam Cached If true, uses cached acquisition (optimization for frequent use)
 */
template<typename H, bool Cached = false>
class THazardPointer
{
    THazardPointer(const THazardPointer&) = delete;
    THazardPointer& operator=(const THazardPointer&) = delete;
    
    std::atomic<H*>* m_Hazard = nullptr;
    FHazardPointerCollection::FHazardRecord* m_Record = nullptr;

public:
    THazardPointer(THazardPointer&& Other) 
        : m_Hazard(Other.m_Hazard)
        , m_Record(Other.m_Record)
    {
        if (m_Record)
        {
            m_Record->Release();
        }
        Other.m_Hazard = nullptr;
        Other.m_Record = nullptr;
    }

    THazardPointer& operator=(THazardPointer&& Other)
    {
        if (m_Record)
        {
            m_Record->Release();
        }
        m_Hazard = Other.m_Hazard;
        m_Record = Other.m_Record;
        Other.m_Hazard = nullptr;
        Other.m_Record = nullptr;
        return *this;
    }

public:
    THazardPointer() = default;

    inline THazardPointer(std::atomic<H*>& InHazard, FHazardPointerCollection& Collection)
        : m_Hazard(&InHazard)
        , m_Record(Collection.Acquire<Cached>())
    {
        OLO_CORE_ASSERT(m_Record->GetHazard() == nullptr, "Record should be cleared after acquire");
    }

    inline ~THazardPointer()
    {
        if (m_Record)
        {
            m_Record->Release();
        }
    }

    /**
     * @brief Retire the hazard pointer without releasing the slot
     * 
     * Can be used to release the hazard pointer without reacquiring a new
     * hazard slot in the collection.
     */
    inline void Retire()
    {
        OLO_CORE_ASSERT(m_Record, "Retire called on null record");
        m_Record->Retire();
    }

    /**
     * @brief Destroy the hazard pointer, releasing the slot
     * 
     * Use with care, because the hazard pointer will not protect anymore 
     * and needs to be recreated.
     */
    inline void Destroy()
    {
        OLO_CORE_ASSERT(m_Record, "Destroy called on null record");
        m_Record->Release();
        m_Record = nullptr;
        m_Hazard = nullptr;
    }

    /**
     * @brief Get the protected pointer value
     * 
     * Loads the atomic pointer and sets it as the hazard, ensuring it won't
     * be deleted while we hold the hazard pointer.
     * 
     * @return The protected pointer value
     */
    inline H* Get() const
    {
        OLO_CORE_ASSERT(m_Record, "Get called on null record");
        H* HazardPointer;
        do
        {
            HazardPointer = reinterpret_cast<H*>(m_Record->SetHazard(m_Hazard->load(std::memory_order_acquire)));
        } while (HazardPointer != m_Hazard->load(std::memory_order_acquire));
        return HazardPointer;
    }

    /**
     * @brief Check if the hazard pointer is valid
     * @return true if both hazard and record are set
     */
    inline bool IsValid()
    {
        return m_Hazard != nullptr && m_Record != nullptr;
    }
};

/**
 * @brief Helper function to create a non-cached hazard pointer
 * @tparam H Type of object being protected
 * @param InHazard Atomic pointer to protect
 * @param Collection Hazard pointer collection
 * @return A hazard pointer protecting the given atomic
 */
template<typename H>
THazardPointer<H, false> MakeHazardPointer(std::atomic<H*>& InHazard, FHazardPointerCollection& Collection)
{
    return {InHazard, Collection};
}

} // namespace OloEngine
