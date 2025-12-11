#include "OloEngine/Memory/ArraySlackTracking.h"

#if OLO_ENABLE_ARRAY_SLACK_TRACKING

#include <mutex>
#include <atomic>

namespace OloEngine
{
    // ============================================================================
    // Global State
    // ============================================================================

    static std::mutex g_SlackTrackingMutex;
    static FArraySlackTrackingHeader* g_TrackingListHead = nullptr;
    static std::atomic<u64> g_TotalSlackBytes{0};
    static std::atomic<u32> g_TrackedAllocationCount{0};
    static std::atomic<u8> g_ActiveTag{0};

    // ============================================================================
    // Implementation
    // ============================================================================

    void ArraySlackTrackInit()
    {
        std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
        g_TrackingListHead = nullptr;
        g_TotalSlackBytes.store(0);
        g_TrackedAllocationCount.store(0);
    }

    void ArraySlackTrackGenerateReport(const char* Cmd)
    {
        std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
        
        OLO_CORE_INFO("=== Array Slack Tracking Report ===");
        OLO_CORE_INFO("Tracked allocations: {}", g_TrackedAllocationCount.load());
        OLO_CORE_INFO("Total slack bytes: {}", g_TotalSlackBytes.load());
        
        // Walk the list and report per-allocation details if verbose
        if (Cmd && std::string(Cmd).find("verbose") != std::string::npos)
        {
            u64 totalSlack = 0;
            u32 count = 0;
            FArraySlackTrackingHeader* Current = g_TrackingListHead;
            
            while (Current)
            {
                if (Current->ArrayNum != INDEX_NONE)
                {
                    i64 slack = Current->SlackSizeInBytes();
                    totalSlack += slack > 0 ? static_cast<u64>(slack) : 0;
                    
                    OLO_CORE_INFO("  Allocation #{}: Num={}, Max={}, ElemSize={}, Slack={} bytes, Reallocs={}",
                        count, Current->ArrayNum, Current->ArrayMax, Current->ElemSize,
                        slack, Current->ReallocCount);
                    count++;
                }
                Current = Current->Next;
            }
            
            OLO_CORE_INFO("Calculated total slack: {} bytes", totalSlack);
        }
        
        OLO_CORE_INFO("=================================");
    }

    u8 LlmGetActiveTag()
    {
        return g_ActiveTag.load();
    }

    void FArraySlackTrackingHeader::AddAllocation()
    {
        std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
        
        // Insert at head of list
        Next = g_TrackingListHead;
        Prev = &g_TrackingListHead;
        
        if (g_TrackingListHead)
        {
            g_TrackingListHead->Prev = &Next;
        }
        
        g_TrackingListHead = this;
        g_TrackedAllocationCount.fetch_add(1);
    }

    void FArraySlackTrackingHeader::RemoveAllocation()
    {
        std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
        
        // Remove from linked list
        if (Next)
        {
            Next->Prev = Prev;
        }
        *Prev = Next;
        
        g_TrackedAllocationCount.fetch_sub(1);
        
        // Update slack tracking
        if (ArrayNum != INDEX_NONE && ArrayMax > ArrayNum)
        {
            g_TotalSlackBytes.fetch_sub(static_cast<u64>((ArrayMax - ArrayNum) * ElemSize));
        }
    }

    void FArraySlackTrackingHeader::UpdateNumUsed(i64 NewNumUsed)
    {
        i64 OldSlack = (ArrayNum != INDEX_NONE && ArrayMax > ArrayNum) 
            ? (ArrayMax - ArrayNum) * static_cast<i64>(ElemSize) : 0;
        
        ArrayNum = NewNumUsed;
        
        // Update peak
        if (NewNumUsed > static_cast<i64>(ArrayPeak))
        {
            ArrayPeak = static_cast<u32>(NewNumUsed);
        }
        
        // Update global slack tracking
        i64 NewSlack = (ArrayMax > ArrayNum) 
            ? (ArrayMax - ArrayNum) * static_cast<i64>(ElemSize) : 0;
        
        i64 SlackDelta = NewSlack - OldSlack;
        if (SlackDelta > 0)
        {
            g_TotalSlackBytes.fetch_add(static_cast<u64>(SlackDelta));
        }
        else if (SlackDelta < 0)
        {
            g_TotalSlackBytes.fetch_sub(static_cast<u64>(-SlackDelta));
        }
    }

    OLO_NOINLINE void* FArraySlackTrackingHeader::Realloc(void* Ptr, i64 Count, u64 InElemSize, i32 Alignment)
    {
        // Calculate required alignment for header
        i32 HeaderAlignment = static_cast<i32>(alignof(FArraySlackTrackingHeader));
        if (Alignment < HeaderAlignment)
        {
            Alignment = HeaderAlignment;
        }
        
        FArraySlackTrackingHeader* OldHeader = nullptr;
        i64 OldArrayNum = INDEX_NONE;
        
        if (Ptr)
        {
            OldHeader = reinterpret_cast<FArraySlackTrackingHeader*>(
                reinterpret_cast<u8*>(Ptr) - sizeof(FArraySlackTrackingHeader));
            OldArrayNum = OldHeader->ArrayNum;
            OldHeader->RemoveAllocation();
        }
        
        if (Count == 0)
        {
            if (Ptr)
            {
                void* ActualPtr = reinterpret_cast<u8*>(OldHeader) - OldHeader->AllocOffset;
                FMemory::Free(ActualPtr);
            }
            return nullptr;
        }
        
        // Calculate total size needed
        sizet DataSize = static_cast<sizet>(Count) * static_cast<sizet>(InElemSize);
        sizet TotalSize = sizeof(FArraySlackTrackingHeader) + DataSize + Alignment;
        
        // Allocate new memory
        void* RawPtr = FMemory::Malloc(TotalSize, Alignment);
        if (!RawPtr)
        {
            return nullptr;
        }
        
        // Calculate aligned position for header
        uptr RawAddr = reinterpret_cast<uptr>(RawPtr);
        uptr AlignedAddr = (RawAddr + Alignment - 1) & ~(Alignment - 1);
        u16 AllocOffset = static_cast<u16>(AlignedAddr - RawAddr);
        
        // Position header
        FArraySlackTrackingHeader* NewHeader = reinterpret_cast<FArraySlackTrackingHeader*>(
            reinterpret_cast<u8*>(RawPtr) + AllocOffset);
        
        // Initialize header
        NewHeader->AllocOffset = AllocOffset;
        NewHeader->Tag = LlmGetActiveTag();
        NewHeader->NumStackFrames = 0; // Stack capture not implemented yet
        NewHeader->ElemSize = InElemSize;
        NewHeader->ArrayMax = Count;
        NewHeader->ArrayNum = OldArrayNum; // Preserve old ArrayNum
        
        if (OldHeader)
        {
            NewHeader->FirstAllocFrame = OldHeader->FirstAllocFrame;
            NewHeader->ReallocCount = OldHeader->ReallocCount + 1;
            NewHeader->ArrayPeak = OldHeader->ArrayPeak;
            
            // Copy old data
            void* OldData = reinterpret_cast<u8*>(OldHeader) + sizeof(FArraySlackTrackingHeader);
            void* NewData = reinterpret_cast<u8*>(NewHeader) + sizeof(FArraySlackTrackingHeader);
            sizet CopySize = static_cast<sizet>(OldHeader->ArrayMax) * static_cast<sizet>(OldHeader->ElemSize);
            if (CopySize > DataSize)
            {
                CopySize = DataSize;
            }
            FMemory::Memcpy(NewData, OldData, CopySize);
            
            // Free old allocation
            void* OldActualPtr = reinterpret_cast<u8*>(OldHeader) - OldHeader->AllocOffset;
            FMemory::Free(OldActualPtr);
        }
        else
        {
            NewHeader->FirstAllocFrame = 0; // Frame counter not implemented
            NewHeader->ReallocCount = 0;
            NewHeader->ArrayPeak = 0;
        }
        
        // Add to tracking list
        NewHeader->AddAllocation();
        
        // Return pointer past header
        return reinterpret_cast<u8*>(NewHeader) + sizeof(FArraySlackTrackingHeader);
    }

} // namespace OloEngine

#endif // OLO_ENABLE_ARRAY_SLACK_TRACKING

