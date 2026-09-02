#pragma once

// =============================================================================
// VulkanSecondaryCommandPools — the command pools behind RecordParallel
// (issue #806, ADR 0011 amendment (92) rule 9).
//
// A VkCommandPool is externally synchronised: recording into a buffer touches
// its pool, so two threads may never record into buffers of one pool at once.
// There is therefore one pool per (frame slot, ITEM). Keying on the item, not
// on the worker, is what lets the render thread acquire and begin every
// item's secondary BEFORE the fork: whichever worker later records an item is
// the only thread that ever touches that item's pool, and a failure to
// acquire is discovered while the region can still fall back to the one
// inline path (no post-join "record it now" arm with different semantics).
//
// The frame owner is the frame arena: VulkanFrameArena::BeginFrame(slot)
// runs only after the frame fence proved the slot's submissions retired, so
// "the arena's generation advanced" is exactly the moment the slot's pools
// may be reset. SyncToFrame reads that generation on the render thread at
// every fork; a generation of 0 means no frame has begun and the region
// records inline instead. Keying on the arena rather than adding a second
// BeginFrame call site keeps one frame clock for every per-slot cache
// (VulkanFrameArena.h, GetFrameGeneration).
//
// Thread-safety: every method runs on the render thread outside a region.
// The pools' buffers are recorded by workers inside the region, one thread
// per item.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanFrameArena.h"

#include <array>
#include <vector>

namespace OloEngine
{
    class VulkanSecondaryCommandPools
    {
      public:
        // Process-wide, deliberately leaked (the VulkanFrameArena::Get shape).
        [[nodiscard]] static VulkanSecondaryCommandPools& Get();

        static constexpr u32 kFramesInFlight = VulkanFrameArena::kFramesInFlight;

        // Render thread, at a fork. Resets the current frame slot's used pools
        // when the arena's generation advanced since the last sync. False
        // when no frame has begun (the caller must record inline) or no
        // device is up.
        [[nodiscard]] bool SyncToFrame();

        // Render thread, at a fork, once per item. A secondary command buffer
        // from the (current slot, item) pool, already begun with
        // ONE_TIME_SUBMIT and an empty inheritance block (it executes outside
        // any render pass instance, so it opens its own dynamic-rendering
        // scopes). VK_NULL_HANDLE when the pool or buffer could not be
        // created or begun; the caller then records the whole region inline.
        [[nodiscard]] VkCommandBuffer AcquireBegun(u32 itemIndex);

        // Device teardown (VulkanDevice::Shutdown): destroy every pool. The
        // object becomes lazily re-creatable afterwards.
        void ReleaseAll();

        // --- Diagnostics -------------------------------------------------
        [[nodiscard]] u32 GetLivePoolCount() const;
        [[nodiscard]] u64 GetSyncedGeneration() const
        {
            return m_SyncedGeneration;
        }
        [[nodiscard]] u32 GetActiveSlot() const
        {
            return m_ActiveSlot;
        }

      private:
        VulkanSecondaryCommandPools() = default;

        struct Pool
        {
            VkCommandPool Handle = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer> Buffers;
            u32 Cursor = 0; ///< Buffers[0, Cursor) are in use this frame.
        };

        [[nodiscard]] Pool* EnsurePool(u32 slot, u32 itemIndex);

        std::array<std::vector<Pool>, kFramesInFlight> m_Pools{};
        u64 m_SyncedGeneration = 0;
        u32 m_ActiveSlot = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
