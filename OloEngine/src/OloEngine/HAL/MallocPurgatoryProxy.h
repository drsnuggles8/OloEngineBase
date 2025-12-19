// MallocPurgatoryProxy.h - Use-after-free detection proxy
// Ported from UE5.7 HAL/UnrealMemory.cpp (FMallocPurgatoryProxy)

#pragma once

// @file MallocPurgatoryProxy.h
// @brief FMalloc proxy that detects use-after-free bugs
// 
// When memory is freed, instead of immediately returning it to the allocator:
// 1. Fill the memory with a canary byte pattern (0xDC)
// 2. Add to a "purgatory" queue for several frames
// 3. After N frames, verify the canary bytes are unchanged
// 4. If modified, someone wrote to freed memory -> log error
// 5. Actually free the memory
// 
// This detects use-after-free bugs that occur within a few frames of the free.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/LockFreeList.h"

#include <atomic>

namespace OloEngine
{

// Enable/disable purgatory proxy at compile time
#if !defined(OLO_MALLOC_PURGATORY)
    #if defined(OLO_DEBUG) || defined(OLO_RELEASE)
        #define OLO_MALLOC_PURGATORY 1
    #else
        #define OLO_MALLOC_PURGATORY 0
    #endif
#endif

#if OLO_MALLOC_PURGATORY

/**
 * @class FMallocPurgatoryProxy
 * @brief FMalloc proxy that keeps freed memory in purgatory to detect use-after-free
 */
class FMallocPurgatoryProxy : public FMalloc
{
    // Malloc we're based on, aka using under the hood
    FMalloc* m_UsedMalloc;
    
    enum
    {
        // Number of frames to keep freed memory in purgatory
        PURGATORY_FRAMES = 4,
        
        // Maximum total memory to keep in purgatory (100 MB)
        PURGATORY_MAX_MEM = 100 * 1024 * 1024,
        
        // Canary byte used to detect writes to freed memory
        PURGATORY_CANARY_BYTE = 0xdc,
    };
    
    // Frame number when we last checked/freed from purgatory
    u32 m_LastCheckFrame;
    
    // Outstanding size in KB in purgatory
    std::atomic<i32> m_OutstandingSizeInKB{0};
    
    // Counter for oversize clearing
    std::atomic<i32> m_NextOversizeClear{0};
    
    // Per-frame purgatory queues (lock-free LIFO stacks)
    TLockFreePointerListUnordered<void, OLO_PLATFORM_CACHE_LINE_SIZE> m_Purgatory[PURGATORY_FRAMES];

    // Global frame counter - should be updated by engine each frame
    static inline std::atomic<u32> s_FrameNumber{0};

public:
    /**
     * @brief Increment the frame counter (call once per frame from main thread)
     */
    static void IncrementFrameNumber()
    {
        s_FrameNumber.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Get the current frame number
     */
    static u32 GetFrameNumber()
    {
        return s_FrameNumber.load(std::memory_order_relaxed);
    }

    explicit FMallocPurgatoryProxy(FMalloc* InMalloc)
        : m_UsedMalloc(InMalloc)
        , m_LastCheckFrame(0)
    {
        OLO_CORE_ASSERT(m_UsedMalloc, "FMallocPurgatoryProxy is used without a valid malloc!");
    }

    virtual void InitializeStatsMetadata() override
    {
        m_UsedMalloc->InitializeStatsMetadata();
    }

    virtual void* Malloc(sizet Size, u32 Alignment) override
    {
        return m_UsedMalloc->Malloc(Size, Alignment);
    }

    virtual void* TryMalloc(sizet Size, u32 Alignment) override
    {
        return m_UsedMalloc->TryMalloc(Size, Alignment);
    }

    virtual void* Realloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        return m_UsedMalloc->Realloc(Ptr, NewSize, Alignment);
    }

    virtual void* TryRealloc(void* Ptr, sizet NewSize, u32 Alignment) override
    {
        return m_UsedMalloc->TryRealloc(Ptr, NewSize, Alignment);
    }

    virtual void Free(void* Ptr) override
    {
        if (!Ptr)
        {
            return;
        }

        // Get the allocation size
        sizet Size = 0;
        if (!GetAllocationSize(Ptr, Size) || Size == 0)
        {
            // Can't get size, just free directly
            m_UsedMalloc->Free(Ptr);
            return;
        }

        // Fill with canary bytes
        std::memset(Ptr, static_cast<u8>(PURGATORY_CANARY_BYTE), Size);

        // Add to purgatory for the current frame
        u32 CurrentFrame = GetFrameNumber();
        m_Purgatory[CurrentFrame % PURGATORY_FRAMES].Push(Ptr);
        m_OutstandingSizeInKB.fetch_add(static_cast<i32>((Size + 1023) / 1024), std::memory_order_relaxed);

        // Memory barrier to ensure visibility
        std::atomic_thread_fence(std::memory_order_release);

        u32 LocalLastCheckFrame = m_LastCheckFrame;
        u32 LocalGFrameNumber = CurrentFrame;

        bool bFlushAnyway = m_OutstandingSizeInKB.load(std::memory_order_relaxed) > (PURGATORY_MAX_MEM / 1024);

        if (bFlushAnyway || LocalLastCheckFrame != LocalGFrameNumber)
        {
            // Try to claim the work of flushing purgatory
            if (bFlushAnyway || 
                std::atomic_ref(m_LastCheckFrame).compare_exchange_strong(LocalLastCheckFrame, LocalGFrameNumber))
            {
                // Determine which frame's purgatory to flush
                i32 FrameToPop = (bFlushAnyway 
                    ? m_NextOversizeClear.fetch_add(1, std::memory_order_relaxed) 
                    : static_cast<i32>(LocalGFrameNumber)) + PURGATORY_FRAMES - 1;
                FrameToPop = FrameToPop % PURGATORY_FRAMES;

                FlushPurgatory(FrameToPop);
            }
        }
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

    virtual bool GetAllocationSize(void* Original, sizet& SizeOut) override
    {
        return m_UsedMalloc->GetAllocationSize(Original, SizeOut);
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
        return "PurgatoryProxy";
    }

    virtual bool IsInternallyThreadSafe() const override
    {
        // The lock-free lists are thread-safe
        return m_UsedMalloc->IsInternallyThreadSafe();
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

private:
    void FlushPurgatory(i32 FrameIndex)
    {
        while (true)
        {
            void* Pop = m_Purgatory[FrameIndex].Pop();
            if (!Pop)
            {
                break;
            }

            sizet Size = 0;
            if (!GetAllocationSize(Pop, Size) || Size == 0)
            {
                // Can't verify, just free
                m_UsedMalloc->Free(Pop);
                continue;
            }

            // Verify canary bytes
            u8* Bytes = static_cast<u8*>(Pop);
            for (sizet At = 0; At < Size; At++)
            {
                if (Bytes[At] != PURGATORY_CANARY_BYTE)
                {
                    OLO_CORE_ERROR("Use-after-free detected! Freed memory at {} + {} == 0x{:02X} (should be 0x{:02X})",
                        Pop, At, static_cast<i32>(Bytes[At]), static_cast<i32>(PURGATORY_CANARY_BYTE));
                    OLO_CORE_ASSERT(false, "Use-after-free detected!");
                }
            }

            // Actually free the memory
            m_UsedMalloc->Free(Pop);
            m_OutstandingSizeInKB.fetch_sub(static_cast<i32>((Size + 1023) / 1024), std::memory_order_relaxed);
        }
    }
};

#endif // OLO_MALLOC_PURGATORY

} // namespace OloEngine
