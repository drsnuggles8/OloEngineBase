#pragma once

// VulkanFrameArena — the per-frame GPU-visible bump allocator behind the
// root-data pointer model. Issue #691 Phase 6, ADR 0011 §4.
//
// WHAT THIS IS FOR. Every draw/dispatch packs its per-object data (transforms,
// material scalars, texture-heap indices, buffer device addresses) into one
// POD struct allocated HERE, and reaches the shader as a single 8-byte GPU
// pointer. The arena hands out byte ranges *within* one persistently-mapped
// buffer — the CPU writes root structs (and per-draw block payloads) straight
// into GPU-visible memory, no staging buffer, no copy command.
//
// WHAT THIS IS NOT. Not a TransientPool reuse (ADR 0011 §4 is explicit):
// TransientPool pools whole physical resources; this allocator suballocates
// scratch bytes. It is the Vulkan sibling of Commands/CommandAllocator (CPU
// packet memory) and Commands/FrameDataBuffer (CPU frame staging), one level
// down: the memory is GPU-visible and the reset is gated on the GPU having
// finished the frame slot.
//
// LIFETIME. kFramesInFlight (2) independent slots, matching
// VulkanContextData::kFramesInFlight and VulkanDeferredReclaim — §1.2's "do
// not invent a third lifetime class" rule. BeginFrame(slot) resets a slot's
// cursor and may only be called after the frame loop's vkWaitForFences proves
// the GPU is done with that slot (VulkanContext::SwapBuffers calls it right
// after the wait; the headless test fixture waits idle per submit). Data
// written into slot N stays valid until BeginFrame(N) comes around again —
// two full frames, the §4 contract.
//
// OVERFLOW. Allocate returns a null allocation (Cpu == nullptr) once a slot's
// capacity is exhausted, warn-once per run, counted in the stats — the
// FrameDataBuffer sentinel discipline. A dropped draw beats writing past a
// mapped range.
//
// Thread-safety: NONE, deliberately — render thread only, like the rest of
// the backend.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// VulkanDevice.h provides <volk.h> and <vk_mem_alloc.h> in the one legal
// order (volk first — see the comment there and ADR 0011 amendment 41a).
#include "Platform/Vulkan/VulkanDevice.h"

#include <array>

namespace OloEngine
{
    struct VulkanFrameArenaAllocation
    {
        void* Cpu = nullptr;     ///< Write-through mapped pointer (null on overflow).
        VkDeviceAddress Gpu = 0; ///< The address shaders dereference (0 on overflow).
        u64 Offset = 0;          ///< Byte offset within the slot's buffer.

        [[nodiscard]] bool IsValid() const
        {
            return Cpu != nullptr;
        }
    };

    class VulkanFrameArena
    {
      public:
        // Process-wide instance, deliberately leaked (see
        // VulkanImageInfoRegistry::Get for the rationale). Buffers are created
        // lazily on first Allocate (a live VulkanDevice is required then, not
        // at static init).
        [[nodiscard]] static VulkanFrameArena& Get();

        // Reset a slot's cursor. Caller guarantees the GPU is done with that
        // slot (the frame loop's fence wait, or device idle in tests).
        void BeginFrame(u32 frameSlot);

        // Bump-allocate from the current slot. Alignment must be a power of
        // two; 16 is std430-safe for any root struct the model produces.
        [[nodiscard]] VulkanFrameArenaAllocation Allocate(u64 sizeBytes, u64 alignment = 16);

        // Convenience: allocate + memcpy + return. The common "one root
        // struct per draw" shape.
        [[nodiscard]] VulkanFrameArenaAllocation Push(const void* data, u64 sizeBytes, u64 alignment = 16);

        // Enqueue every slot buffer for deferred reclaim and forget them —
        // shutdown / device-teardown path (test TearDown, context shutdown).
        // The arena becomes lazily re-creatable afterwards.
        void ReleaseBuffers();

        // --- Stats (diagnostic/test affordances) -----------------------------
        [[nodiscard]] u64 GetCurrentSlotUsedBytes() const;
        [[nodiscard]] u64 GetSlotCapacityBytes() const
        {
            return kSlotCapacityBytes;
        }
        [[nodiscard]] u64 GetAllocationCountThisFrame() const
        {
            return m_AllocationsThisFrame;
        }
        [[nodiscard]] u64 GetOverflowCount() const
        {
            return m_OverflowCount;
        }
        [[nodiscard]] u32 GetCurrentSlot() const
        {
            return m_CurrentSlot;
        }
        // The slot's backing buffer — for transfer commands that stage
        // through the arena (vkCmdCopyBufferToImage of a Push()ed payload).
        [[nodiscard]] VkBuffer GetSlotBuffer(u32 frameSlot) const
        {
            return m_Slots[frameSlot % kFramesInFlight].Buffer;
        }

        // Matches VulkanContextData::kFramesInFlight / VulkanDeferredReclaim.
        static constexpr u32 kFramesInFlight = 2;
        // Root structs are tens of bytes and per-draw block payloads hundreds;
        // 16 MiB per slot is far past any current frame's total (10k instanced
        // draws x 224 B InstanceData ~= 2.2 MiB) while costing 32 MiB of BAR —
        // generous beats a mid-frame cliff. Revisit with real Phase 7 numbers.
        static constexpr u64 kSlotCapacityBytes = 16ull * 1024 * 1024;

      private:
        VulkanFrameArena() = default;

        // Creates all slot buffers; requires a live VulkanDevice. Returns
        // false (warn-once) when none is up.
        [[nodiscard]] bool EnsureBuffers();

        struct Slot
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            void* Mapped = nullptr;
            VkDeviceAddress BaseAddress = 0;
            u64 Cursor = 0;
        };

        std::array<Slot, kFramesInFlight> m_Slots{};
        u32 m_CurrentSlot = 0;
        u64 m_AllocationsThisFrame = 0;
        u64 m_OverflowCount = 0;
        bool m_OverflowWarned = false;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
