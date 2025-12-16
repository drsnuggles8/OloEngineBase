// MallocVerifyProxy.h - Allocation verification proxy for debugging
// Ported from UE5.7 HAL/MallocVerify.h

#pragma once

/**
 * @file MallocVerifyProxy.h
 * @brief FMalloc proxy that verifies allocation validity
 * 
 * Maintains a set of all allocated pointers and verifies:
 * - Free() is called with a valid allocated pointer
 * - Double-free detection
 * - Realloc() is called with valid pointers
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <unordered_set>

namespace OloEngine
{

/** Enable/disable malloc verification at compile time */
#if !defined(OLO_MALLOC_VERIFY)
    #if defined(OLO_DEBUG)
        #define OLO_MALLOC_VERIFY 1
    #else
        #define OLO_MALLOC_VERIFY 0
    #endif
#endif

#if OLO_MALLOC_VERIFY

/**
 * @class FMallocVerify
 * @brief Maintains a list of all pointers to currently allocated memory
 */
class FMallocVerify
{
    /** List of all currently allocated pointers */
    std::unordered_set<void*> m_AllocatedPointers;

public:
    /** Handles new allocated pointer */
    void Malloc(void* Ptr)
    {
        if (Ptr)
        {
            auto Result = m_AllocatedPointers.insert(Ptr);
            OLO_CORE_ASSERT(Result.second, "FMallocVerify: Malloc returned pointer {} that was already allocated!", Ptr);
        }
    }

    /** Handles reallocation */
    void Realloc(void* OldPtr, void* NewPtr)
    {
        if (OldPtr)
        {
            sizet Erased = m_AllocatedPointers.erase(OldPtr);
            OLO_CORE_ASSERT(Erased == 1, "FMallocVerify: Realloc called with invalid pointer {}!", OldPtr);
        }
        if (NewPtr)
        {
            auto Result = m_AllocatedPointers.insert(NewPtr);
            OLO_CORE_ASSERT(Result.second, "FMallocVerify: Realloc returned pointer {} that was already allocated!", NewPtr);
        }
    }

    /** Removes allocated pointer from list */
    void Free(void* Ptr)
    {
        if (Ptr)
        {
            sizet Erased = m_AllocatedPointers.erase(Ptr);
            OLO_CORE_ASSERT(Erased == 1, "FMallocVerify: Free called with invalid or already-freed pointer {}!", Ptr);
        }
    }
};

/**
 * @class FMallocVerifyProxy
 * @brief A verifying proxy malloc that checks that the caller is passing valid pointers
 */
class FMallocVerifyProxy : public FMalloc
{
private:
    /** Malloc we're based on, aka using under the hood */
    FMalloc* m_UsedMalloc;

    /** Verifier object */
    FMallocVerify m_Verify;

    /** Malloc critical section */
    mutable FMutex m_VerifyMutex;

public:
    explicit FMallocVerifyProxy(FMalloc* InMalloc)
        : m_UsedMalloc(InMalloc)
    {
        OLO_CORE_ASSERT(m_UsedMalloc, "FMallocVerifyProxy is used without a valid malloc!");
    }

    virtual void* Malloc(sizet Size, u32 Alignment) override
    {
        TUniqueLock<FMutex> Lock(m_VerifyMutex);
        void* Result = m_UsedMalloc->Malloc(Size, Alignment);
        m_Verify.Malloc(Result);
        return Result;
    }

    virtual void* TryMalloc(sizet Size, u32 Alignment) override
    {
        TUniqueLock<FMutex> Lock(m_VerifyMutex);
        void* Result = m_UsedMalloc->TryMalloc(Size, Alignment);
        m_Verify.Malloc(Result);
        return Result;
    }

    virtual void* Realloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        TUniqueLock<FMutex> Lock(m_VerifyMutex);
        void* Result = m_UsedMalloc->Realloc(Ptr, NewSize, Alignment);
        m_Verify.Realloc(Ptr, Result);
        return Result;
    }

    virtual void* TryRealloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        TUniqueLock<FMutex> Lock(m_VerifyMutex);
        void* Result = m_UsedMalloc->TryRealloc(Ptr, NewSize, Alignment);
        if (Result)
        {
            m_Verify.Realloc(Ptr, Result);
        }
        return Result;
    }

    virtual void Free(void* Ptr) override
    {
        if (Ptr)
        {
            TUniqueLock<FMutex> Lock(m_VerifyMutex);
            m_Verify.Free(Ptr);
            m_UsedMalloc->Free(Ptr);
        }
    }

    virtual void InitializeStatsMetadata() override
    {
        m_UsedMalloc->InitializeStatsMetadata();
    }

    virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override
    {
        m_UsedMalloc->GetAllocatorStats(OutStats);
    }

    virtual void DumpAllocatorStats(FOutputDevice& Ar) override
    {
        m_UsedMalloc->DumpAllocatorStats(Ar);
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

    virtual bool GetAllocationSize(void* Original, sizet& OutSize) override
    {
        return m_UsedMalloc->GetAllocationSize(Original, OutSize);
    }

    virtual sizet QuantizeSize(sizet Count, u32 Alignment) override
    {
        return m_UsedMalloc->QuantizeSize(Count, Alignment);
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

    virtual const char* GetDescriptiveName() override
    {
        return "VerifyProxy";
    }

    virtual bool IsInternallyThreadSafe() const override
    {
        // We add our own locking, so we're thread-safe
        return true;
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

#endif // OLO_MALLOC_VERIFY

} // namespace OloEngine
