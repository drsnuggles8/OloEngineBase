#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanDeferredReclaim.h — generation-waited destruction for the Vulkan
// backend's VMA allocations and non-VMA device objects (#691; split out of
// the single VulkanTransientResources.h in Phase 9 — no GL twin, the driver
// refcounts GL names).
//
// This header exposes Vulkan types directly — it is included only by
// Platform/Vulkan siblings and by OLO_WITH_VULKAN-guarded engine factory TUs
// (the sanctioned factory-include pattern, rhi-abstraction-boundary.md).
// =============================================================================

// VulkanDevice.h provides <volk.h> and <vk_mem_alloc.h> (with the
// VMA_STATIC/DYNAMIC_VULKAN_FUNCTIONS config that must stay in sync with
// VulkanMemoryAllocator.cpp) — do NOT include either directly here, and NEVER
// <vulkan/vulkan.h> (volk owns the function pointers, ADR 0011 amendment 41a).
#include "Platform/Vulkan/VulkanDevice.h"

#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanDeferredReclaim — deferred-destroy queue for VMA allocations.
    //
    // WHY: TransientPool::Clear()/Trim() destroy pooled GPU objects while prior
    // frames may still be executing on the GPU. On GL the driver refcounts the
    // object behind the name, so an in-flight delete is safe; on Vulkan,
    // destroying a resource a submitted command buffer still references is
    // undefined behavior. So no Vulkan resource class ever calls
    // vmaDestroyImage/vmaDestroyBuffer inline — destruction enqueues here and
    // the actual destroy happens once the GPU is provably past the frame that
    // could have referenced the object.
    //
    // The wait is counted in GENERATIONS, not clocks: the frame loop calls
    // NotifyFrameCompleted() once per completed frame, and an entry is
    // destroyed once >= kFramesInFlight (2, matching VulkanContext's
    // frames-in-flight shape) notifications have passed since it was enqueued.
    // FlushAll() is the shutdown/device-idle path: the CALLER guarantees
    // vkDeviceWaitIdle has already been done, and everything is destroyed
    // immediately.
    //
    // Thread-safety: NONE, deliberately — render thread only, like the rest of
    // the backend.
    // -------------------------------------------------------------------------
    class VulkanDeferredReclaim
    {
      public:
        // Process-wide instance, deliberately leaked (see
        // VulkanImageInfoRegistry::Get for the rationale).
        [[nodiscard]] static VulkanDeferredReclaim& Get();

        void Enqueue(VkImage image, VmaAllocation allocation);
        void Enqueue(VkBuffer buffer, VmaAllocation allocation);
        // Phase 6: non-VMA device objects share the same generation discipline.
        // A semaphore may be referenced by an in-flight submit's wait/signal
        // list; a pipeline by an in-flight command buffer (ADR 0011 §3(d) —
        // hot-reload destruction is deferred, never inline).
        void Enqueue(VkSemaphore semaphore);
        void Enqueue(VkPipeline pipeline);
        // Phase 7: attachment views (vkCmdBeginRendering references them from
        // in-flight command buffers exactly like pipelines).
        void Enqueue(VkImageView view);
        // Phase 7 Wave C: occlusion query pools. vkCmdResetQueryPool /
        // vkCmdBeginQuery reference the pool from in-flight command buffers,
        // and DeleteQueries is called from a frame that may still have the
        // previous one submitted — same generation discipline as pipelines.
        void Enqueue(VkQueryPool queryPool);

        // Called by the frame loop once per completed frame. Destroys every
        // entry enqueued >= 2 notifications ago; also unregisters images from
        // VulkanImageInfoRegistry at actual-destroy time.
        void NotifyFrameCompleted();

        // Destroy everything immediately. Caller guarantees device idle
        // (vkDeviceWaitIdle already done — shutdown, swapchain teardown).
        void FlushAll();

        // Diagnostic/test affordance.
        [[nodiscard]] sizet GetPendingCount() const
        {
            return m_Entries.size();
        }

      private:
        VulkanDeferredReclaim() = default;

        struct Entry
        {
            VkImage Image = VK_NULL_HANDLE; // exactly one of Image/Buffer/Semaphore/Pipeline/View/QueryPool is set
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkSemaphore Semaphore = VK_NULL_HANDLE;
            VkPipeline Pipeline = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            VkQueryPool QueryPool = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE; // set only for Image/Buffer entries
            u64 EnqueuedAtGeneration = 0;
        };

        // Destroys one entry through the live device's allocator. When the
        // device is already gone (shutdown teardown races) the entry is
        // dropped with a warn log — leaking at process exit beats calling
        // into a destroyed allocator.
        static void DestroyEntry(const Entry& entry);

        // Matches VulkanContextData::kFramesInFlight.
        static constexpr u64 kFramesInFlight = 2;

        std::vector<Entry> m_Entries;
        u64 m_Generation = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
