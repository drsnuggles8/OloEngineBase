#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanDescriptorSlotCache — get-or-create heap slots per (image, view,
// descriptor kind, layout). Issue #691.
//
// The draw path's BindTexture(slot, handle) needs "the heap slot whose
// descriptor samples this texture" — one stable slot per view for the
// texture's life, written once, recycled when the image is destroyed. This
// cache is that mapping, layered over VulkanResourceHeap's dumb primitive
// (bump allocation + descriptor writes) so the primitive stays as originally
// shipped it.
//
// The LAYOUT is part of the key on purpose: a descriptor bakes the layout
// the image will be in when accessed (SHADER_READ_ONLY for ordinary sampling,
// DEPTH_STENCIL_READ_ONLY for the read-while-attached PCSS case, GENERAL for
// storage), and one image legitimately needs different descriptors for
// different access patterns — folding them would hand a pass a descriptor
// whose baked layout disagrees with the barrier plan, which is a validation
// error at best.
//
// Slot recycling is safe WITHOUT a frames-in-flight delay because release
// happens only from VulkanDeferredReclaim's destroy pass — which already
// waited kFramesInFlight generations past the image's last possible use, so
// no in-flight frame can still index the freed slots. A freed slot's next
// tenant writes its descriptor before any draw references it.
//
// Relationship to RHI::DescriptorHeap (ADR 0011 amendment (56)): this is the
// backend-internal half of the deferred "engine DescriptorHeap backend over
// VulkanResourceHeap" item — the slot bookkeeping Waves A/B need. The full
// IDescriptorHeapBackend composition (generations, poisoning, the
// frame-transient ring) layers over the same primitive when its machinery
// buys something real on this backend; the seam is recorded, not abandoned.
//
// Thread-safety (issue #806, ADR 0011 amendment (91) rule 8): every public
// entry point holds m_Mutex for its whole body, so AcquireSlot — on every
// draw's BindTexture path — may run from several recording threads at once.
// The VulkanResourceHeap calls it makes (AllocateSlot, WriteSampledImage,
// WriteStorageImage) and the poison write on release
// (VulkanDescriptorHeapBackend::WriteNullAt) therefore run UNDER this lock:
// none of them has a lock of its own, and none may call back into this
// cache. Lock order against the backend: its null-slot memo mutex is taken
// FIRST (GetNullSampledHeapSlot -> AcquireSlot), this one second, never the
// reverse — which is why WriteNullAt takes no lock over there.
// =============================================================================

#include "Platform/Vulkan/VulkanDevice.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class VulkanDescriptorSlotCache
    {
      public:
        [[nodiscard]] static VulkanDescriptorSlotCache& Get();

        // Get-or-create the slot whose descriptor realises (image, view,
        // type, layout). `type` must be VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE or
        // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE. Returns
        // VulkanResourceHeap::InvalidSlot on failure.
        [[nodiscard]] u32 AcquireSlot(VkImage image, const VkImageViewCreateInfo& viewInfo, VkDescriptorType type,
                                      VkImageLayout layout);

        // Free every cached slot for `image` into the free list. Called from
        // VulkanDeferredReclaim's destroy pass (see the recycling note above
        // for why no extra delay is needed).
        void ReleaseSlotsForImage(VkImage image);

        // Drop all cache state (heap teardown / device loss). Slots are NOT
        // returned to the heap — the heap itself is being released.
        void Reset();

        // Diagnostic/test affordances.
        [[nodiscard]] sizet GetCachedSlotCount() const
        {
            std::shared_lock lock(m_Mutex);
            return m_SlotByKey.size();
        }
        [[nodiscard]] sizet GetFreeSlotCount() const
        {
            std::shared_lock lock(m_Mutex);
            return m_FreeSlots.size();
        }

      private:
        VulkanDescriptorSlotCache() = default;

        [[nodiscard]] static u64 HashKey(VkImage image, const VkImageViewCreateInfo& viewInfo, VkDescriptorType type,
                                         VkImageLayout layout);

        struct SlotEntry
        {
            u32 Slot = 0;
            VkDescriptorType Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        };

        mutable std::shared_mutex m_Mutex; ///< Shared on the hit path, exclusive on a miss and for release/reset. ///< Guards everything below (see the thread-safety note).
        std::unordered_map<u64, SlotEntry> m_SlotByKey;
        std::unordered_map<VkImage, std::vector<u64>> m_KeysByImage;
        std::vector<u32> m_FreeSlots;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
