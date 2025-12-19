// MallocPoisonProxy.h - Memory poisoning proxy for debugging
// Ported from UE5.7 HAL/MallocPoisonProxy.h

#pragma once

// @file MallocPoisonProxy.h
// @brief FMalloc proxy that poisons new and freed allocations
// 
// Helps catch code that relies on uninitialized or freed memory by filling:
// - New allocations with OLO_DEBUG_FILL_NEW (0xCD)
// - Freed allocations with OLO_DEBUG_FILL_FREED (0xDD)
// 
// When you see 0xCDCDCDCD in memory, it means uninitialized.
// When you see 0xDDDDDDDD in memory, it means use-after-free.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/UnrealMemory.h"

namespace OloEngine
{

// Governs when malloc that poisons the allocations is enabled.
#if !defined(OLO_USE_MALLOC_FILL_BYTES)
    #if defined(OLO_DEBUG) || defined(OLO_RELEASE)
        #define OLO_USE_MALLOC_FILL_BYTES 1
    #else
        #define OLO_USE_MALLOC_FILL_BYTES 0
    #endif
#endif

// Value that a freed memory block will be filled with.
#define OLO_DEBUG_FILL_FREED (0xdd)

// Value that a new memory block will be filled with.
#define OLO_DEBUG_FILL_NEW (0xcd)

// @class FMallocPoisonProxy
// @brief FMalloc proxy that poisons new and freed allocations
// 
// This helps catch:
// - Use of uninitialized memory (will contain 0xCDCDCDCD pattern)
// - Use-after-free bugs (will contain 0xDDDDDDDD pattern)
class FMallocPoisonProxy : public FMalloc
{
private:
    // Malloc we're based on, aka using under the hood
    FMalloc* m_UsedMalloc;

public:
    explicit FMallocPoisonProxy(FMalloc* InMalloc)
        : m_UsedMalloc(InMalloc)
    {
        OLO_CORE_ASSERT(m_UsedMalloc, "FMallocPoisonProxy is used without a valid malloc!");
    }

    virtual void InitializeStatsMetadata() override
    {
        m_UsedMalloc->InitializeStatsMetadata();
    }

    virtual void* Malloc(sizet Size, u32 Alignment) override
    {
        void* Result = m_UsedMalloc->Malloc(Size, Alignment);
        if (OLO_LIKELY(Result != nullptr && Size > 0))
        {
            std::memset(Result, OLO_DEBUG_FILL_NEW, Size);
        }
        return Result;
    }

    virtual void* TryMalloc(sizet Size, u32 Alignment) override
    {
        void* Result = m_UsedMalloc->TryMalloc(Size, Alignment);
        if (OLO_LIKELY(Result != nullptr && Size > 0))
        {
            std::memset(Result, OLO_DEBUG_FILL_NEW, Size);
        }
        return Result;
    }

    virtual void* MallocZeroed(sizet Size, u32 Alignment) override
    {
        void* Result = m_UsedMalloc->Malloc(Size, Alignment);
        if (OLO_LIKELY(Result != nullptr && Size > 0))
        {
            std::memset(Result, 0, Size);
        }
        return Result;
    }

    virtual void* Realloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        // NOTE: case when Realloc returns completely new pointer is not handled properly
        // (we would like to have previous memory poisoned completely).
        // This can be done by avoiding calling Realloc() of the nested allocator and
        // Malloc()/Free()'ing directly, but this is deemed unacceptable from a
        // performance/fragmentation standpoint.
        sizet OldSize = 0;
        if (Ptr != nullptr && GetAllocationSize(Ptr, OldSize) && OldSize > 0 && OldSize > NewSize)
        {
            // Poison the tail that's being freed
            std::memset(static_cast<u8*>(Ptr) + NewSize, OLO_DEBUG_FILL_FREED, OldSize - NewSize);
        }

        void* Result = m_UsedMalloc->Realloc(Ptr, NewSize, Alignment);

        if (Result != nullptr && OldSize > 0 && OldSize < NewSize)
        {
            // Poison the new tail that was allocated
            std::memset(static_cast<u8*>(Result) + OldSize, OLO_DEBUG_FILL_NEW, NewSize - OldSize);
        }

        return Result;
    }

    virtual void* TryRealloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        sizet OldSize = 0;
        if (Ptr != nullptr && GetAllocationSize(Ptr, OldSize) && OldSize > 0 && OldSize > NewSize)
        {
            std::memset(static_cast<u8*>(Ptr) + NewSize, OLO_DEBUG_FILL_FREED, OldSize - NewSize);
        }

        void* Result = m_UsedMalloc->TryRealloc(Ptr, NewSize, Alignment);

        if (Result != nullptr && OldSize > 0 && OldSize < NewSize)
        {
            std::memset(static_cast<u8*>(Result) + OldSize, OLO_DEBUG_FILL_NEW, NewSize - OldSize);
        }

        return Result;
    }

    virtual void Free(void* Ptr) override
    {
        if (OLO_LIKELY(Ptr))
        {
            sizet AllocSize;
            if (OLO_LIKELY(GetAllocationSize(Ptr, AllocSize) && AllocSize > 0))
            {
                std::memset(Ptr, OLO_DEBUG_FILL_FREED, AllocSize);
            }
            m_UsedMalloc->Free(Ptr);
        }
    }

    virtual sizet QuantizeSize(sizet Count, u32 Alignment) override
    {
        return m_UsedMalloc->QuantizeSize(Count, Alignment);
    }

    virtual void UpdateStats() override
    {
        m_UsedMalloc->UpdateStats();
    }

    virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override
    {
        m_UsedMalloc->GetAllocatorStats(OutStats);
    }

    virtual void DumpAllocatorStats(FOutputDevice& Ar) override
    {
        m_UsedMalloc->DumpAllocatorStats(Ar);
    }

    virtual bool IsInternallyThreadSafe() const override
    {
        return m_UsedMalloc->IsInternallyThreadSafe();
    }

    virtual bool ValidateHeap() override
    {
        return m_UsedMalloc->ValidateHeap();
    }

#if OLO_ALLOW_EXEC_COMMANDS
    virtual bool Exec(const char* Cmd, FOutputDevice& Ar) override
    {
        return m_UsedMalloc->Exec(Cmd, Ar);
    }
#endif

    virtual bool GetAllocationSize(void* Original, sizet& SizeOut) override
    {
        return m_UsedMalloc->GetAllocationSize(Original, SizeOut);
    }

    virtual const char* GetDescriptiveName() override
    {
        return "PoisonProxy";
    }

    virtual void Trim(bool bTrimThreadCaches) override
    {
        m_UsedMalloc->Trim(bTrimThreadCaches);
    }

    virtual void SetupTLSCachesOnCurrentThread() override
    {
        m_UsedMalloc->SetupTLSCachesOnCurrentThread();
    }

    virtual void MarkTLSCachesAsUsedOnCurrentThread() override
    {
        m_UsedMalloc->MarkTLSCachesAsUsedOnCurrentThread();
    }

    virtual void MarkTLSCachesAsUnusedOnCurrentThread() override
    {
        m_UsedMalloc->MarkTLSCachesAsUnusedOnCurrentThread();
    }

    virtual void ClearAndDisableTLSCachesOnCurrentThread() override
    {
        m_UsedMalloc->ClearAndDisableTLSCachesOnCurrentThread();
    }

    virtual void OnMallocInitialized() override
    {
        m_UsedMalloc->OnMallocInitialized();
    }

    virtual void OnPreFork() override
    {
        m_UsedMalloc->OnPreFork();
    }

    virtual void OnPostFork() override
    {
        m_UsedMalloc->OnPostFork();
    }
};

} // namespace OloEngine
