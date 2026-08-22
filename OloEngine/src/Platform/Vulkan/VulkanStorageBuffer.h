#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanStorageBuffer.h — the VMA-backed StorageBuffer backend twin of
// OpenGLStorageBuffer (#691; split out of the single VulkanTransientResources.h)
// .
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

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/StorageBuffer.h"

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanStorageBuffer — attribute-only VMA buffer for the TransientPool.
    // -------------------------------------------------------------------------
    class VulkanStorageBuffer : public StorageBuffer
    {
      public:
        VulkanStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage = StorageBufferUsage::DynamicDraw);
        ~VulkanStorageBuffer() override;

        // Bind is meaningless on this backend (buffers travel as device
        // addresses in root data): silent no-ops.
        void Bind() const override;
        void Unbind() const override;
        // Real paths (#691): mapped write-through (BAR/UMA) or a
        // staged one-shot copy; readback via a one-shot copy to host memory.
        void SetData(const void* data, u32 size, u32 offset = 0) override;
        void GetData(void* outData, u32 size, u32 offset = 0) const override;
        void ClearData() override;

        // Recreates the VMA buffer at the new size (old buffer goes through
        // VulkanDeferredReclaim). Identity preserved via m_RHIHandle.Sync.
        void Resize(u32 newSize) override;

        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] u32 GetSize() const override
        {
            return m_Size;
        }
        [[nodiscard]] u32 GetBinding() const override
        {
            return m_Binding;
        }

        [[nodiscard]] VkBuffer GetVkBuffer() const
        {
            return m_Buffer;
        }
        // The persistent buffer's address. Stable for the buffer's life;
        // Resize mints a new one. GPU-write participants (compute dispatch
        // root data, indirect-args resolution, copies) use THIS address —
        // their writes must land in the one buffer every later consumer
        // resolves.
        [[nodiscard]] VkDeviceAddress GetDeviceAddress() const
        {
            return m_DeviceAddress;
        }
        // The address a DRAW's root-data writer embeds (ADR 0011 §4) — the
        // storage twin of VulkanUniformBuffer::GetRootDataAddress. A CPU
        // SetData mid-frame snapshots the written range into the frame arena,
        // and draws recorded AFTER the write embed the snapshot's address
        // while earlier draws keep the one they recorded — GL's command-
        // ordered glNamedBufferSubData semantics. Without this, every draw
        // in the frame reads the LAST SetData at execute time: the exact
        // failure that emptied the auto-batched instanced draws (all batches
        // sampling the final ModelInstanceBuffer upload — #691).
        // Falls back to the persistent address when no snapshot is live
        // (GPU-written buffers never SetData mid-frame, so they always
        // resolve persistent).
        [[nodiscard]] VkDeviceAddress GetRootDataAddress();

      private:
        void CreateBuffer();
        void ReleaseBuffer();
        // Copies the just-written range (plus any live snapshot content it
        // does not cover) into a fresh frame-arena range and points
        // GetRootDataAddress at it. Failure (arena overflow, unreadable
        // prefix) invalidates the snapshot so draws fall back to the
        // persistent buffer — today's pre-fix semantics, never garbage.
        void PushSnapshot(const void* data, u32 size, u32 offset);
        void InvalidateSnapshot()
        {
            m_SnapshotAddress = 0;
            m_SnapshotCpu = nullptr;
            m_SnapshotBytes = 0;
        }

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr; ///< Non-null when VMA gave a host-visible placement.
        bool m_NeedsFlush = false;
        VkDeviceAddress m_DeviceAddress = 0;
        // Command-ordered draw-read snapshot (see GetRootDataAddress).
        // Valid only while m_SnapshotFrameGeneration matches the arena's
        // current frame — arena ranges recycle after kFramesInFlight.
        u64 m_SnapshotFrameGeneration = ~0ull;
        VkDeviceAddress m_SnapshotAddress = 0;
        void* m_SnapshotCpu = nullptr;
        u32 m_SnapshotBytes = 0;
        // Generation-checked identity for m_Buffer, kept in lockstep by
        // m_RHIHandle.Sync — same pattern as the GL twin (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Size = 0;
        u32 m_Binding = 0;
        StorageBufferUsage m_Usage = StorageBufferUsage::DynamicDraw;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
