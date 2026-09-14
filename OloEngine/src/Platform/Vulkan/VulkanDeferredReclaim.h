#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanDeferredReclaim.h — generation-waited destruction for the Vulkan
// backend's VMA allocations and non-VMA device objects (#691; split out of
// the single VulkanTransientResources.h — no GL twin, the driver
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
        // Matches VulkanContextData::kFramesInFlight. Public: the descriptor heap's
        // transient ring is sized by it too (one sub-ring per frame in flight, #1198).
        static constexpr u64 kFramesInFlight = 2;

        // How long a DEPTH-STENCIL image's memory outlives its last use (issue
        // #1198): one generation more than everything else.
        //
        // This is a driver workaround, and the evidence that it is not an engine
        // reference is what justifies it. On a live forward -> forward+ switch
        // the scene target's depth attachment is destroyed and replaced by a
        // same-spec image; two generations later, the moment its memory is
        // freed, the GPU page-faults reading the OLD surface's base address
        // (Nsight Aftermath: AddressTranslationError, graphics engine, the
        // scene fragment shader active, resource destroyed=true). Every
        // reference the engine can make was audited on faulting runs and found
        // absent: no heap descriptor was ever written for the image, no
        // rendering scope, copy, blit, clear or barrier named it after the
        // switch (every image-bearing Vulkan command was interposed), no
        // command buffer submitted after the switch was recorded before it,
        // and a vkDeviceWaitIdle immediately before the free changed nothing.
        // A read that no submitted command asks for is the driver's own
        // per-surface bookkeeping for a depth target (its hierarchical-Z /
        // compression metadata is keyed by the surface base, which is exactly
        // where every fault lands), consulted for the first rendering frame
        // after the surface is replaced. Holding the memory one more
        // generation covers that frame.
        //
        // Colour attachments never faulted in any capture, so only depth-
        // stencil formats pay the extra frame of memory. Levers::
        // VulkanNoDepthReclaimHold() disables it so the workaround can be
        // re-tested against a new driver rather than kept on faith.
        static constexpr u64 kDepthStencilHoldGenerations = kFramesInFlight + 1;

        // Process-wide instance, deliberately leaked (see
        // VulkanImageInfoRegistry::Get for the rationale).
        [[nodiscard]] static VulkanDeferredReclaim& Get();

        // ALL of these are noexcept, and that is load-bearing: every caller is
        // a resource destructor, the queue is a std::vector, and a push_back
        // that throws out of a destructor terminates the process. A failed
        // enqueue drops the entry with an error log — leaking one object until
        // process exit beats std::terminate, and it stays loud rather than
        // silent. Callers therefore need no try/catch around Enqueue itself;
        // the destructors keep theirs for the OTHER teardown work they do.
        void Enqueue(VkImage image, VmaAllocation allocation) noexcept;
        void Enqueue(VkBuffer buffer, VmaAllocation allocation) noexcept;
        // Non-VMA device objects share the same generation discipline.
        // A semaphore may be referenced by an in-flight submit's wait/signal
        // list; a pipeline by an in-flight command buffer (ADR 0011 §3(d) —
        // hot-reload destruction is deferred, never inline).
        void Enqueue(VkSemaphore semaphore) noexcept;
        void Enqueue(VkPipeline pipeline) noexcept;
        // Attachment views (vkCmdBeginRendering references them from
        // in-flight command buffers exactly like pipelines).
        void Enqueue(VkImageView view) noexcept;
        // Phase 7 Wave C: occlusion query pools. vkCmdResetQueryPool /
        // vkCmdBeginQuery reference the pool from in-flight command buffers,
        // and DeleteQueries is called from a frame that may still have the
        // previous one submitted — same generation discipline as pipelines.
        void Enqueue(VkQueryPool queryPool) noexcept;
        // Acceleration structures (issue #978). An in-flight command buffer
        // can still be building into one or tracing against it, so the same
        // generation discipline applies.
        //
        // ORDERING IS LOAD-BEARING AT THE CALL SITE: an AS and the VkBuffer
        // backing it are two entries, and this queue destroys in insertion
        // order within a generation. Enqueue the AS handle FIRST — freeing the
        // memory under a live structure is a use-after-free inside the driver,
        // not a validation message.
        void Enqueue(VkAccelerationStructureKHR accelerationStructure) noexcept;

        // Called by the frame loop once per completed frame. Destroys every
        // entry enqueued >= 2 notifications ago; also unregisters images from
        // VulkanImageInfoRegistry at actual-destroy time.
        //
        // noexcept for the same reason as Enqueue, plus one of its own: both
        // drain paths run during teardown, and an entry that throws must not
        // strand the entries BEHIND it — an entry that never reaches
        // vmaDestroyAllocator is the "allocations not freed" abort. So each
        // entry's destroy is isolated and a failure is logged, not propagated.
        void NotifyFrameCompleted() noexcept;

        // Destroy everything immediately. Caller guarantees device idle
        // (vkDeviceWaitIdle already done — shutdown, swapchain teardown).
        void FlushAll() noexcept;

        // The formats that take kDepthStencilHoldGenerations.
        [[nodiscard]] static bool IsDepthStencilFormat(VkFormat format) noexcept;

        // Diagnostic/test affordance.
        [[nodiscard]] sizet GetPendingCount() const
        {
            return m_Entries.size();
        }

      private:
        VulkanDeferredReclaim() = default;

        struct Entry
        {
            VkImage Image = VK_NULL_HANDLE; // exactly one of Image/Buffer/Semaphore/Pipeline/View/QueryPool/AccelerationStructure is set
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkSemaphore Semaphore = VK_NULL_HANDLE;
            VkPipeline Pipeline = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            VkQueryPool QueryPool = VK_NULL_HANDLE;
            VkAccelerationStructureKHR AccelerationStructure = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE; // set only for Image/Buffer entries
            u64 EnqueuedAtGeneration = 0;
            /// Generations this entry is held; kFramesInFlight for everything but a
            /// depth-stencil image (see kDepthStencilHoldGenerations).
            u64 HoldGenerations = kFramesInFlight;
        };

        // Destroys one entry through the live device's allocator. When the
        // device is already gone (shutdown teardown races) the entry is
        // dropped with a warn log — leaking at process exit beats calling
        // into a destroyed allocator.
        static void DestroyEntry(const Entry& entry);

        // The one push_back every Enqueue overload funnels through, with the
        // allocation failure contained (see the Enqueue block above).
        void Push(const Entry& entry) noexcept;

        // DestroyEntry with any escape contained, so one bad entry cannot
        // strand the rest of a drain.
        static void DestroyEntryGuarded(const Entry& entry) noexcept;

        std::vector<Entry> m_Entries;
        u64 m_Generation = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
