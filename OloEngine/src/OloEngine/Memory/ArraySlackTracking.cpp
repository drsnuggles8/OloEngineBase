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
    static std::atomic<u32> g_TrackedAllocationCount{0};
    static std::atomic<u8> g_ActiveTag{0};
    static bool g_ArraySlackInit = false;

    // ============================================================================
    // Implementation
    // ============================================================================

    void ArraySlackTrackInit()
    {
        // Any array allocations before this is called won't have array slack tracking,
        // although subsequent reallocations of existing arrays will gain tracking if that occurs.
        // The goal is to filter out startup constructors which run before Main.
        g_ArraySlackInit = true;
    }

    void ArraySlackTrackGenerateReport(const char* Cmd)
    {
        std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
        
        OLO_CORE_INFO("=== Array Slack Tracking Report ===");
        OLO_CORE_INFO("Tracked allocations: {}", g_TrackedAllocationCount.load());
        
        // Calculate total slack on-demand
        u64 totalSlack = 0;
        u32 count = 0;
        FArraySlackTrackingHeader* Current = g_TrackingListHead;
        
        while (Current)
        {
            if (Current->ArrayNum != INDEX_NONE)
            {
                i64 slack = Current->SlackSizeInBytes();
                if (slack > 0)
                {
                    totalSlack += static_cast<u64>(slack);
                }
                count++;
            }
            Current = Current->Next;
        }
        
        OLO_CORE_INFO("Total slack bytes: {}", totalSlack);
        
        // Walk the list and report per-allocation details if verbose
        if (Cmd && std::string(Cmd).find("verbose") != std::string::npos)
        {
            Current = g_TrackingListHead;
            u32 itemNum = 0;
            
            while (Current)
            {
                if (Current->ArrayNum != INDEX_NONE)
                {
                    i64 slack = Current->SlackSizeInBytes();
                    
                    OLO_CORE_INFO("  Allocation #{}: Num={}, Max={}, ElemSize={}, Slack={} bytes, Reallocs={}",
                        itemNum, Current->ArrayNum, Current->ArrayMax, Current->ElemSize,
                        slack, Current->ReallocCount);
                    itemNum++;
                }
                Current = Current->Next;
            }
        }
        
        OLO_CORE_INFO("=================================");
    }

    u8 LlmGetActiveTag()
    {
        return g_ActiveTag.load();
    }

    void FArraySlackTrackingHeader::AddAllocation()
    {
        // This code is only reached for reallocations if ArrayNum is set,
        // since during the initial allocation, ArrayNum won't have been set yet.
        if (ArrayNum != INDEX_NONE)
        {
            ReallocCount++;
        }
        
        // Add to linked list if tracking is enabled
        if (g_ArraySlackInit)
        {
            std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
            
            if (g_TrackingListHead)
            {
                g_TrackingListHead->Prev = &Next;
            }
            Next = g_TrackingListHead;
            Prev = &g_TrackingListHead;
            g_TrackingListHead = this;
            
            g_TrackedAllocationCount.fetch_add(1);
        }
    }

    void FArraySlackTrackingHeader::RemoveAllocation()
    {
        // Only remove from list if Prev is set (meaning it was added to the list)
        if (Prev)
        {
            std::lock_guard<std::mutex> Lock(g_SlackTrackingMutex);
            
            if (Next)
            {
                Next->Prev = Prev;
            }
            *Prev = Next;
            
            Next = nullptr;
            Prev = nullptr;
            
            g_TrackedAllocationCount.fetch_sub(1);
        }
    }

    void FArraySlackTrackingHeader::UpdateNumUsed(i64 NewNumUsed)
    {
        OLO_CORE_ASSERT(NewNumUsed <= ArrayMax, "NewNumUsed exceeds ArrayMax");
        
        // Track the allocation in our totals when ArrayNum is first set to something other than INDEX_NONE.
        // This allows us to factor out container allocations that aren't arrays (mainly hash tables),
        // which won't ever call "UpdateNumUsed".
        if (ArrayNum == INDEX_NONE)
        {
            // On first update, initialize ArrayNum to 0
            ArrayNum = 0;
            FirstAllocFrame = 0; // Frame counter not implemented yet
        }
        
        // Update ArrayNum
        ArrayNum = NewNumUsed;
        
        // Update peak - clamp to UINT32_MAX to avoid truncation
        u32 ClampedNewNum = static_cast<u32>(NewNumUsed < 0xFFFFFFFFLL ? NewNumUsed : 0xFFFFFFFFLL);
        if (ClampedNewNum > ArrayPeak)
        {
            ArrayPeak = ClampedNewNum;
        }
    }

    OLO_NOINLINE void* FArraySlackTrackingHeader::Realloc(void* Ptr, i64 Count, u64 ElemSize, i32 Alignment)
    {
        // Figure out how much padding we need under the allocation
        // Round up header size to next power of two for alignment
        i32 HeaderAlign = 16; // sizeof(FArraySlackTrackingHeader) is typically 96, round to 16
        i32 temp = static_cast<i32>(sizeof(FArraySlackTrackingHeader));
        HeaderAlign = 1;
        while (HeaderAlign < temp)
        {
            HeaderAlign <<= 1;
        }
        i32 PaddingRequired = HeaderAlign > Alignment ? HeaderAlign : Alignment;
        
        // Get the base pointer of the original allocation, and remove tracking for it
        if (Ptr)
        {
            FArraySlackTrackingHeader* TrackingHeader = reinterpret_cast<FArraySlackTrackingHeader*>(
                reinterpret_cast<u8*>(Ptr) - sizeof(FArraySlackTrackingHeader));
            TrackingHeader->RemoveAllocation();
            
            Ptr = reinterpret_cast<u8*>(TrackingHeader) - TrackingHeader->AllocOffset;
        }
        
        u8* ResultPtr = nullptr;
        if (Count)
        {
            ResultPtr = reinterpret_cast<u8*>(FMemory::Realloc(Ptr, Count * ElemSize + PaddingRequired, Alignment));
            ResultPtr += PaddingRequired;
            FArraySlackTrackingHeader* TrackingHeader = reinterpret_cast<FArraySlackTrackingHeader*>(
                ResultPtr - sizeof(FArraySlackTrackingHeader));
            
            // Set the tag and other default information in the allocation if it's newly created
            if (!Ptr)
            {
                // Note that we initially set the slack tracking ArrayNum to INDEX_NONE. The container
                // allocator is used by both arrays and other containers (Set / Map / Hash), and we
                // don't know it's actually an array until "UpdateNumUsed" is called on it.
                OLO_CORE_ASSERT(PaddingRequired <= 65536, "Padding too large");
                TrackingHeader->Next = nullptr;
                TrackingHeader->Prev = nullptr;
                TrackingHeader->AllocOffset = static_cast<u16>(PaddingRequired - sizeof(FArraySlackTrackingHeader));
                TrackingHeader->Tag = LlmGetActiveTag();
                TrackingHeader->NumStackFrames = 0;
                TrackingHeader->FirstAllocFrame = 0;
                TrackingHeader->ReallocCount = 0;
                TrackingHeader->ArrayPeak = 0;
                TrackingHeader->ElemSize = ElemSize;
                TrackingHeader->ArrayNum = INDEX_NONE;
            }
            
            // Update ArrayMax and re-register the allocation
            TrackingHeader->ArrayMax = Count;
            TrackingHeader->AddAllocation();
        }
        else
        {
            FMemory::Free(Ptr);
        }
        
        return ResultPtr;
    }

} // namespace OloEngine

#endif // OLO_ENABLE_ARRAY_SLACK_TRACKING

