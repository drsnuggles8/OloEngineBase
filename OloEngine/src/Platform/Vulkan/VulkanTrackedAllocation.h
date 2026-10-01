#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"

// Every VmaAllocation the engine makes goes through these (issue #1342).
//
// Same signatures as the vma* functions they wrap, plus RendererMemoryTracker
// bookkeeping keyed on the VmaAllocation handle, with the size VMA itself reports
// (VmaAllocationInfo::size — COMMITTED bytes, alignment included). That is what lets
// the memory report reconcile the tracker against vmaGetHeapBudgets to the byte:
// both sides count the same allocations at the same size.
//
// A raw vmaCreate*/vmaDestroy* anywhere else is an allocation the report cannot see —
// RendererMemoryReportTest.VulkanAllocationsGoThroughTheTrackedWrappers counts them.
namespace OloEngine
{
    [[nodiscard]] VkResult TrackedVmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bufferCreateInfo,
                                                  const VmaAllocationCreateInfo* allocationCreateInfo, VkBuffer* buffer,
                                                  VmaAllocation* allocation, VmaAllocationInfo* allocationInfo);

    [[nodiscard]] VkResult TrackedVmaCreateBufferWithAlignment(VmaAllocator allocator, const VkBufferCreateInfo* bufferCreateInfo,
                                                               const VmaAllocationCreateInfo* allocationCreateInfo,
                                                               VkDeviceSize minAlignment, VkBuffer* buffer,
                                                               VmaAllocation* allocation, VmaAllocationInfo* allocationInfo);

    [[nodiscard]] VkResult TrackedVmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo* imageCreateInfo,
                                                 const VmaAllocationCreateInfo* allocationCreateInfo, VkImage* image,
                                                 VmaAllocation* allocation, VmaAllocationInfo* allocationInfo);

    // Frees now. For an allocation that went through VulkanDeferredReclaim, use the
    // *Retired* variants with the ticket the reclaim queue got from
    // RetireTrackedVmaAllocation.
    void TrackedVmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
    void TrackedVmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);

    // Names the allocation in VMA's own debug output AND in the memory report.
    void TrackedVmaSetAllocationName(VmaAllocator allocator, VmaAllocation allocation, const char* name);

    // The owner released `allocation`; its memory is freed only after the frames that may
    // still use it complete. Returns the retire ticket (0 when the allocation is untracked).
    [[nodiscard]] u64 RetireTrackedVmaAllocation(VmaAllocation allocation);
    void TrackedVmaDestroyRetiredBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, u64 retireTicket);
    void TrackedVmaDestroyRetiredImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation, u64 retireTicket);

    // The allocator's own figures for the memory report: per-heap VMA statistics and
    // budgets. `budgetExtensionEnabled` says whether those budgets are the OS's
    // (VK_EXT_memory_budget) or VMA's fallback estimate.
    [[nodiscard]] bool ObserveVmaMemory(VmaAllocator allocator, bool budgetExtensionEnabled, BackendMemoryObservation& out);
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
