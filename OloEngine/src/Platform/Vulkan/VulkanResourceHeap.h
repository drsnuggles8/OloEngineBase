#pragma once

// VulkanResourceHeap — the VK_EXT_descriptor_heap resource-heap buffer.
// Issue #691 (ADR 0011 §1.2's heap, realised on the backend it was
// designed for).
//
// The heap is a plain BDA-addressable buffer (VK_BUFFER_USAGE_DESCRIPTOR_
// HEAP_BIT_EXT), host-visible and persistently mapped: descriptors are
// produced by vkWriteResourceDescriptorsEXT straight into the mapped slot
// region, and vkCmdBindResourceHeapEXT makes the range shader-reachable. A
// slot is GetDescriptorStride() bytes (the device's image-descriptor size,
// alignment-rounded); the pipeline builder's HEAP_WITH_INDIRECT_INDEX
// mappings convert a root-struct u32 slot index into a byte offset with that
// same stride, so the two cannot drift.
//
// SCOPE (recorded deliberately): this is the BACKEND primitive, not yet the
// engine's RHI::DescriptorHeap backend. The engine-side heap singleton (slot
// lifetime, generations, poisoning, the offset-table seam) runs only where
// the GL renderer initialises it; under --rhi=vulkan the renderer is not up
// yet (amendment 49 — the frame loop owns that). A
// VulkanDescriptorHeapBackend : RHI::IDescriptorHeapBackend composes over
// this class when the render graph starts executing on Vulkan —
// the interface was audited for fit (AcquireDescriptor → write here,
// UploadSlots → memcpy into the slot region, BindHeap → CmdBind).
//
// Thread-safety: NONE, deliberately — render thread only.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"

namespace OloEngine
{
    class VulkanResourceHeap
    {
      public:
        [[nodiscard]] static VulkanResourceHeap& Get();

        // Lazily creates the heap buffer (requires a live VulkanDevice).
        // False when no device is up or creation failed.
        [[nodiscard]] bool EnsureCreated();

        // Bump-allocate the next free slot. Returns InvalidSlot when the heap
        // is full or absent. Free-listing of DYNAMIC slots lives in
        // VulkanDescriptorSlotCache; the engine RHI::DescriptorHeap manages
        // its own reserved range below.
        [[nodiscard]] u32 AllocateSlot();
        static constexpr u32 InvalidSlot = 0xFFFFFFFFu;

        // Reserve slots [0, count) for an EXTERNAL slot manager (the engine
        // RHI::DescriptorHeap, #691): bump allocation then starts at
        // `count`, so the two spaces cannot collide. Idempotent; false when
        // the heap is absent, count exceeds capacity, or dynamic allocation
        // already handed out a slot below `count` (install the engine heap
        // FIRST — a retroactive reservation would alias live slots).
        [[nodiscard]] bool ReserveSlotRange(u32 count);
        [[nodiscard]] u32 GetReservedSlots() const
        {
            return m_ReservedSlots;
        }

        // Write a SAMPLED_IMAGE descriptor into `slot` from a view
        // DESCRIPTION (no VkImageView object — the extension writes
        // descriptors from VkImageViewCreateInfo directly). `layout` is the
        // layout the image will be in when sampled. The sampler half is
        // embedded per pipeline (VulkanPipelineBuilder). Returns false on
        // failure.
        [[nodiscard]] bool WriteSampledImage(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout);
        // The STORAGE_IMAGE sibling (#691 — compute/imageStore
        // bindings). `layout` is GENERAL in every current use (the barrier
        // lowering puts storage accesses there).
        [[nodiscard]] bool WriteStorageImage(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout);

        // Record the heap bind into a command buffer. Must run before any draw
        // whose pipeline carries heap mappings; re-recorded per command buffer
        // (binds are command-buffer state).
        void CmdBind(VkCommandBuffer cmd);

        // Byte stride between consecutive slots — also the mapping's
        // heapIndexStride, by construction.
        [[nodiscard]] VkDeviceSize GetDescriptorStride() const
        {
            return m_DescriptorStride;
        }
        // Byte offset of slot 0 within the heap range (past the driver's
        // reserved range) — the mapping's heapOffset base.
        [[nodiscard]] VkDeviceSize GetSlotRegionOffset() const
        {
            return m_SlotRegionOffset;
        }

        // Teardown (device idle): enqueue the buffer for reclaim, forget
        // state; lazily re-creatable.
        void Release();

      private:
        VulkanResourceHeap() = default;

        [[nodiscard]] bool WriteImageDescriptor(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout,
                                                VkDescriptorType type);

        // 5120 engine-heap slots (kDescriptorHeapSlots — persistent 4096 +
        // transient ring 1024, the GL-parity capacities) + headroom for the
        // draw-path slot cache. 8192 x 32 B stride ≈ 256 KiB — trivial.
        static constexpr u32 kSlotCapacity = 8192;
        u32 m_ReservedSlots = 0; ///< [0, m_ReservedSlots) belongs to the engine heap.

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr;
        VkDeviceAddress m_BaseAddress = 0;
        VkDeviceSize m_DescriptorStride = 0;
        VkDeviceSize m_SlotRegionOffset = 0;
        VkDeviceSize m_ReservedRangeSize = 0;
        VkDeviceSize m_TotalSize = 0;
        u32 m_NextSlot = 0;
        VkDevice m_OwningDevice = VK_NULL_HANDLE; ///< The device the cached buffer belongs to.
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
