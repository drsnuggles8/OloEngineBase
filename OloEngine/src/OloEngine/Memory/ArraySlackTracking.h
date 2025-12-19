#pragma once

// @file ArraySlackTracking.h
// @brief Array slack tracking for memory debugging
//
// Array slack tracking is a debug feature to track unused space in heap allocated TArray
// (and similar) structures. This feature increases heap memory usage and has performance cost,
// so it is usually disabled by default.
//
// When enabled, it adds a header to each heap allocation tracking:
// - Peak usage
// - Reallocation count
// - Stack traces (when available)
// - Current slack (wasted space)
//
// Ported from Unreal Engine's Containers/ContainerAllocationPolicies.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/UnrealMemory.h"

namespace OloEngine
{
// ============================================================================
// Slack Tracking Configuration
// ============================================================================

// Array slack tracking is disabled by default. Enable for debugging memory waste.
// Note: This has performance and memory overhead.
#ifndef OLO_ENABLE_ARRAY_SLACK_TRACKING
#ifdef OLO_DEBUG
#define OLO_ENABLE_ARRAY_SLACK_TRACKING 0 // Set to 1 to enable in debug builds
#else
#define OLO_ENABLE_ARRAY_SLACK_TRACKING 0
#endif
#endif

#if OLO_ENABLE_ARRAY_SLACK_TRACKING

// Maximum stack frames to capture
#ifndef OLO_SLACK_TRACKING_STACK_FRAMES
#define OLO_SLACK_TRACKING_STACK_FRAMES 9
#endif

    // @struct FArraySlackTrackingHeader
    // @brief Header prepended to array heap allocations for tracking slack waste
    //
    // For detailed tracking of array slack waste, we add a header to heap allocations.
    // It's impossible to track the TArray structure itself since it can be inside other
    // structures and moved around, while the heap allocation is invariant.
    struct FArraySlackTrackingHeader
    {
        FArraySlackTrackingHeader* Next; // Linked list of tracked items
        FArraySlackTrackingHeader** Prev;
        u16 AllocOffset;     // Offset below header to actual allocation (for alignment)
        u8 Tag;              // LLM-style tag for categorization
        i8 NumStackFrames;   // Number of stack frames captured
        u32 FirstAllocFrame; // Frame number of first allocation
        u32 ReallocCount;    // Number of reallocs
        u32 ArrayPeak;       // Peak observed ArrayNum
        u64 ElemSize;        // Size of each element

        // Note: ArrayNum initially set to INDEX_NONE since we don't know if it's
        // actually an array until UpdateNumUsed is called
        i64 ArrayNum;
        i64 ArrayMax;
        u64 StackFrames[OLO_SLACK_TRACKING_STACK_FRAMES];

        // Add this allocation to the tracking list
        void AddAllocation();

        // Remove this allocation from the tracking list
        void RemoveAllocation();

        // Update the number of used elements
        // @param NewNumUsed The new number of used elements
        void UpdateNumUsed(i64 NewNumUsed);

        // Reallocate with tracking
        // @param Ptr Existing pointer (or nullptr)
        // @param Count Number of elements
        // @param ElemSize Size of each element
        // @param Alignment Required alignment
        // @return New allocation pointer
        OLO_NOINLINE static void* Realloc(void* Ptr, i64 Count, u64 ElemSize, i32 Alignment);

        // Free a tracked allocation
        // @param Ptr Pointer to free
        static void Free(void* Ptr)
        {
            if (Ptr)
            {
                FArraySlackTrackingHeader* TrackingHeader =
                    reinterpret_cast<FArraySlackTrackingHeader*>(
                        reinterpret_cast<u8*>(Ptr) - sizeof(FArraySlackTrackingHeader));
                TrackingHeader->RemoveAllocation();

                void* ActualPtr = reinterpret_cast<u8*>(TrackingHeader) - TrackingHeader->AllocOffset;
                FMemory::Free(ActualPtr);
            }
        }

        // Update used count for a tracked allocation
        // @param Ptr The allocation pointer
        // @param NewNumUsed New number of used elements
        OLO_FINLINE static void UpdateNumUsed(void* Ptr, i64 NewNumUsed)
        {
            if (Ptr)
            {
                FArraySlackTrackingHeader* TrackingHeader =
                    reinterpret_cast<FArraySlackTrackingHeader*>(
                        reinterpret_cast<u8*>(Ptr) - sizeof(FArraySlackTrackingHeader));
                TrackingHeader->UpdateNumUsed(NewNumUsed);
            }
        }

        // Disable tracking on an allocation
        // @param Ptr The allocation pointer
        OLO_FINLINE static void DisableTracking(void* Ptr)
        {
            if (Ptr)
            {
                FArraySlackTrackingHeader* TrackingHeader =
                    reinterpret_cast<FArraySlackTrackingHeader*>(
                        reinterpret_cast<u8*>(Ptr) - sizeof(FArraySlackTrackingHeader));
                TrackingHeader->RemoveAllocation();
                // Reset ArrayNum as it's used as a flag for tracking state
                TrackingHeader->ArrayNum = INDEX_NONE;
            }
        }

        // Calculate current slack in bytes
        // @return Slack size in bytes
        [[nodiscard]] OLO_FINLINE i64 SlackSizeInBytes() const
        {
            return (ArrayMax - ArrayNum) * static_cast<i64>(ElemSize);
        }
    };

    // Initialize the slack tracking system
    void ArraySlackTrackInit();

    // Generate a report of array slack
    // @param Cmd Command line options
    void ArraySlackTrackGenerateReport(const char* Cmd);

    // Get current LLM-style tag (for categorization)
    // @return Current tag value
    u8 LlmGetActiveTag();

#endif // OLO_ENABLE_ARRAY_SLACK_TRACKING

    // ============================================================================
    // Allocator Slack Tracking Helpers
    // ============================================================================

    // @struct TSupportsSlackTracking
    // @brief Helper to check if an allocator supports slack tracking at compile time
    template<typename AllocatorType>
    struct TSupportsSlackTracking
    {
        static constexpr bool Value = false;
    };

    // @brief Call SlackTrackerLogNum on an allocator if supported
    // @tparam AllocatorType The allocator type
    // @param Allocator The allocator instance
    // @param NewNumUsed The new number of used elements
    template<typename AllocatorType, typename SizeType>
    OLO_FINLINE void SlackTrackerLogNumIfSupported(
        [[maybe_unused]] AllocatorType& Allocator,
        [[maybe_unused]] SizeType NewNumUsed)
    {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
        if constexpr (TSupportsSlackTracking<AllocatorType>::Value)
        {
            Allocator.SlackTrackerLogNum(NewNumUsed);
        }
#endif
    }

} // namespace OloEngine
