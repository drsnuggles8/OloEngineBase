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

#include <atomic>

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
        void ClearData(u32 offset, u32 size) override;

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
        // Falls back to the persistent address when no snapshot is live.
        //
        // A `DynamicCopy` buffer NEVER has a live snapshot to fall back from:
        // PushSnapshot refuses one outright (issue #1058). The producer of a
        // GPU-written buffer is a compute dispatch, which resolves
        // GetDeviceAddress, so a CPU snapshot would hand draws a second copy
        // the dispatch can never write — which is precisely how virtual
        // geometry's mesh-shader task stage read a zeroed draw-args snapshot
        // and launched EmitMeshTasksEXT(0) every frame. The reasoning lives at
        // the guard in PushSnapshot; do not re-derive it here.
        [[nodiscard]] VkDeviceAddress GetRootDataAddress();

        // Diagnostics and contract tests: the live snapshot's extent, and a
        // read-only view of its CPU-side bytes. Both report "no snapshot"
        // (0 / nullptr) unless one is live for the arena's CURRENT frame
        // generation — the same liveness GetRootDataAddress applies, so the
        // two can never disagree. The invariant they exist to pin is #1080's:
        // a live snapshot spans the WHOLE buffer, because the address a draw
        // receives carries no length with which to bound a shorter one.
        [[nodiscard]] u32 GetLiveSnapshotBytes() const;
        [[nodiscard]] const void* GetLiveSnapshotCpuData() const;

        /// Times PushSnapshot REFUSED to stage a snapshot for a reason other
        /// than the two by-design ones (a DynamicCopy buffer, and a write
        /// outside a recording bracket) — i.e. the frame arena overflowed, or
        /// a partial write had no CPU-readable source for the bytes it does
        /// not define. Each refusal drops the affected buffer back to
        /// last-write-wins draw ordering: safe, never garbage, but no longer
        /// the command-ordered semantics #691 established. Process-wide and
        /// monotonic, alongside the warn-once, so the condition is assertable
        /// rather than only greppable (no-silent-fallbacks.md — "warn once,
        /// AND count"). A tenant that renders real geometry should assert it
        /// does not move.
        [[nodiscard]] static u64 GetSnapshotRefusedCount();

        // Drop any live snapshot because someone wrote the PERSISTENT buffer
        // behind SetData's back (issue #1052): VulkanRendererAPI's
        // UploadBufferSubData / CopyBufferSubData reach this buffer by its
        // VkBuffer, so a snapshot staged by an earlier SetData would keep
        // shadowing it and the write would land where no draw looks. Dropping
        // it returns the buffer to resolving persistent, which is where the
        // write went.
        void InvalidateSnapshotForExternalWrite()
        {
            NoteGpuWriteThisFrame();
            InvalidateSnapshot();
        }

      private:
        // Shared liveness predicate behind GetRootDataAddress and the two
        // GetLiveSnapshot* accessors — a snapshot expires with its arena frame.
        [[nodiscard]] bool HasLiveSnapshot() const;
        // Record that a GPU-SIDE write to the PERSISTENT buffer was enqueued
        // this frame — a ranged/whole ClearData inside a recording bracket, or
        // an external UploadBufferSubData / CopyBufferSubData. Those writes
        // execute at submit and never touch m_Mapped, so from here to the end
        // of the frame the mapping no longer tells the truth about this
        // buffer's contents and must not be used to fill the bytes a partial
        // write leaves undefined — doing so resurrects exactly the content the
        // clear was issued to remove.
        void NoteGpuWriteThisFrame();
        [[nodiscard]] bool GpuWroteThisFrame() const;
        void CreateBuffer();
        void ReleaseBuffer();
        // Copies the WHOLE buffer — the just-written range, with every byte
        // it does not define sourced from the live snapshot or the mapped
        // persistent buffer — into a fresh frame-arena range and points
        // GetRootDataAddress at it. Whole-buffer is the rule, not an
        // optimisation choice: the address the draw receives carries no
        // length, so a shorter snapshot turns a shader read past the written
        // range into a read of the next arena tenant (#1080). Failure (arena
        // overflow, undefined bytes with no CPU-readable source) invalidates
        // the snapshot so draws fall back to the persistent buffer — the
        // pre-snapshot semantics, never garbage.
        void PushSnapshot(const void* data, u32 size, u32 offset);
        void InvalidateSnapshot()
        {
            m_SnapshotAddress = 0;
            m_SnapshotCpu = nullptr;
            m_SnapshotBytes = 0;
            m_SnapshotArenaOffset = 0;
            m_SnapshotConsumed.store(false, std::memory_order_relaxed);
        }

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr; ///< Non-null when VMA gave a host-visible placement.
        bool m_NeedsFlush = false;
        VkDeviceAddress m_DeviceAddress = 0;
        // Command-ordered draw-read snapshot (see GetRootDataAddress).
        // Valid only while m_SnapshotFrameGeneration matches the arena's
        // current frame — arena ranges recycle after kFramesInFlight.
        std::atomic<u64> m_ParallelWriter{ 0 }; ///< (region << 32 | item) of the region's first RecordParallel writer (ClaimParallelWriter, #806).
        u64 m_SnapshotFrameGeneration = ~0ull;
        VkDeviceAddress m_SnapshotAddress = 0;
        void* m_SnapshotCpu = nullptr;
        u64 m_SnapshotArenaOffset = 0; ///< the live snapshot's offset in the slot buffer, for FlushWrite on the reuse path.
        // Whether a DRAW has already embedded the live snapshot's address
        // (GetRootDataAddress). Until one has, nothing can observe the
        // snapshot's contents, so a further SetData may overwrite it in place
        // instead of claiming a second whole-buffer arena range — which is
        // what keeps a batch of N scattered writes (GPUScene::Upload issues
        // one SetData per non-adjacent dirty range) costing ONE snapshot
        // rather than N.
        //
        // ATOMIC because GetRootDataAddress SETS it and runs on the parallel
        // draw-record path: RecordParallel pre-pushes every bound UBO before
        // the fork precisely so no item pushes a shared object, but storage
        // buffers get no such warm-up, so N worker threads can resolve the
        // same buffer at once. Relaxed is enough — the flag only ever moves
        // false -> true within a frame, the fork/join orders it against the
        // render-thread writes around it, and a racing reader that misses the
        // set merely declines the in-place reuse and claims a fresh range,
        // which is the conservative answer.
        std::atomic<bool> m_SnapshotConsumed{ false };
        u64 m_GpuWriteFrameGeneration = ~0ull; ///< see NoteGpuWriteThisFrame.
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
