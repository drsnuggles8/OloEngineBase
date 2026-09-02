#pragma once

// VulkanSamplerHeap — the VK_EXT_descriptor_heap SAMPLER-heap buffer.
// Issue #691 (ADR 0011 §1.2a's second heap, and the sampler
// deduplication it said "has no GL counterpart").
//
// Same shape as VulkanResourceHeap (a BDA-addressable, persistently-mapped
// descriptor buffer bound with vkCmdBindSamplerHeapEXT), with one addition:
// GetOrCreateSlot deduplicates by the FULL VkSamplerCreateInfo — a handful of
// distinct sampler states serve every texture in the engine, so slots are
// write-once and never freed (the population is dozens, the capacity 256).
//
// Slot 0 is ALWAYS the default post-process read sampler (linear /
// clamp-to-edge / full mip range) — byte-identical to the embedded sampler
// every pipeline used to bake, so a binding whose sampler was never
// staged samples exactly as it did before this heap existed.
//
// Thread-safety (issue #806, ADR 0011 amendment (92) rule 8): GetOrCreateSlot
// is on the draw path (BindTexture stages a sampler index per draw) and may
// run from several recording threads at once. It, EnsureCreated and Release
// serialise on m_Mutex, which covers the hash map, the bump cursor and the
// descriptor writes into the mapped heap. CmdBind locks only its lazy
// creation (through EnsureCreated) and then reads the heap's address and
// sizes lock-free: they are written once at creation and cleared only by
// Release, a render-thread teardown path. The stride / offset getters are the
// same kind of read-after-creation and take no lock.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace OloEngine
{
    class VulkanSamplerHeap
    {
      public:
        [[nodiscard]] static VulkanSamplerHeap& Get();

        // Lazily creates the heap buffer and writes the default sampler into
        // slot 0 (requires a live VulkanDevice). False when no device is up
        // or creation failed. Takes m_Mutex.
        [[nodiscard]] bool EnsureCreated();

        // Get-or-create the slot whose descriptor realises `info`. Dedup is
        // by every field vkCreateSampler consumes. Every failure arm —
        // absent heap, exhausted slots, a failed WRITE for a new state —
        // returns DefaultSlot (draws then sample linear/clamp — the pre-heap
        // behaviour), so callers can store the result directly and never
        // leak InvalidSlot into root data. Takes m_Mutex for its whole body.
        [[nodiscard]] u32 GetOrCreateSlot(const VkSamplerCreateInfo& info);

        static constexpr u32 DefaultSlot = 0u;
        static constexpr u32 InvalidSlot = 0xFFFFFFFFu;

        // The default post-process read sampler (slot 0's state): linear,
        // clamp-to-edge, full mip range. The one source of that description —
        // VulkanPipelineBuilder's old DefaultEmbeddedSampler duplicated it
        // until the embedded-sampler path retired (#691).
        [[nodiscard]] static VkSamplerCreateInfo DefaultSamplerInfo();

        // Record the heap bind. Must run before any draw whose pipeline
        // sources sampler descriptors from the heap; re-recorded per command
        // buffer (binds are command-buffer state).
        void CmdBind(VkCommandBuffer cmd);

        // The mapping inputs (VkDescriptorMappingSourceIndirectIndexEXT's
        // samplerHeapOffset / samplerHeapIndexStride).
        [[nodiscard]] VkDeviceSize GetDescriptorStride() const
        {
            return m_DescriptorStride;
        }
        [[nodiscard]] VkDeviceSize GetSlotRegionOffset() const
        {
            return m_SlotRegionOffset;
        }

        // Diagnostic/test affordance.
        [[nodiscard]] sizet GetLiveSlotCount() const
        {
            std::shared_lock lock(m_Mutex);
            return m_SlotByHash.size();
        }

        // Teardown (device idle): enqueue the buffer for reclaim, forget
        // state; lazily re-creatable. Takes m_Mutex.
        void Release();

      private:
        VulkanSamplerHeap() = default;

        // The bodies behind EnsureCreated / Release, for callers that already
        // hold m_Mutex (GetOrCreateSlot, and the default-sampler failure arm
        // inside creation itself).
        [[nodiscard]] bool EnsureCreatedLocked();
        void ReleaseLocked();

        // Caller holds m_Mutex.
        [[nodiscard]] bool WriteSampler(u32 slot, const VkSamplerCreateInfo& info);

        // Distinct sampler states in the engine number in the dozens (§4f's
        // six-row table times filter variants); 256 is deep headroom and
        // ~8 KiB of heap.
        static constexpr u32 kSlotCapacity = 256;

        mutable std::shared_mutex m_Mutex; ///< See the thread-safety note at the top of this header.
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr;
        VkDeviceAddress m_BaseAddress = 0;
        VkDeviceSize m_DescriptorStride = 0;
        VkDeviceSize m_SlotRegionOffset = 0;
        VkDeviceSize m_ReservedRangeSize = 0;
        VkDeviceSize m_TotalSize = 0;
        bool m_NeedsFlush = false; ///< Allocation lacks HOST_COHERENT (the VulkanFrameArena rule).
        u32 m_NextSlot = 0;
        VkDevice m_OwningDevice = VK_NULL_HANDLE;
        std::unordered_map<u64, u32> m_SlotByHash;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
