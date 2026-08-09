#pragma once

// =============================================================================
// VulkanImageLayoutTracker — per-subresource image-layout state
// (issue #691 Phase 5).
//
// GL has no image layouts, so this state machine is NEW with the Vulkan
// backend, and it is the piece a wrong guess turns into a validation error
// (or a silent GPU hazard) rather than a wrong pixel. The rules:
//
//  - The TRACKER is authoritative for a barrier's oldLayout, never the
//    transition's FromAccess: a pooled transient re-acquired this frame is in
//    whatever layout its previous tenant left, and a first use is UNDEFINED.
//    FromAccess only contributes the source stage/access masks.
//  - Granularity is per (mip, layer). Images register their extents up front
//    (RegisterImage), so VK_REMAINING_*-style open ranges expand exactly and
//    a query over a range whose subresources DISAGREE can answer per run
//    instead of guessing one layout for all of them.
//  - Unknown image / unregistered subresource → UNDEFINED, which is always a
//    legal oldLayout (it discards). Conservative in the direction that can
//    only lose contents, never corrupt sync.
//
// Pure CPU container — no device calls — so it is pinned headlessly by
// VulkanBarrierLoweringTest with fabricated non-dispatchable handles.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <functional>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class VulkanImageLayoutTracker
    {
      public:
        // Declare an image's extents. Idempotent; re-registering with
        // different extents — or a different `registrationId`
        // (VulkanImageInfo::RegistrationId: the stamp that distinguishes a
        // driver-recycled handle VALUE from the image previously tracked,
        // even at identical extents) — resets that image's state. Callers
        // without a registry stamp may pass 0.
        //
        // `initialLayout` is the layout every subresource starts in when the
        // state is (re)created — UNDEFINED for ordinary attachments/storage
        // (first use discards), SHADER_READ_ONLY_OPTIMAL for images a
        // load-time one-shot uploaded (VulkanImageInfo::InitialLayout, #691
        // Phase 7): transitioning those from UNDEFINED would legally discard
        // the uploaded pixels.
        void RegisterImage(VkImage image, u32 mipCount, u32 layerCount, u64 registrationId = 0,
                           VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

        // Drop all state for an image (call at destroy — a recycled handle
        // value must not inherit the dead image's layouts).
        void ForgetImage(VkImage image);

        void Reset();

        // Record that `range` now sits in `layout` (call AFTER recording the
        // barrier that performs the transition).
        void SetLayout(VkImage image, const VkImageSubresourceRange& range, VkImageLayout layout);

        // Visit maximal runs of equal layout inside `range`, in subresource
        // order. The visitor receives a sub-range (same aspect mask as the
        // query) and that run's layout. This is how a barrier batch stays
        // exact when one logical range spans mixed layouts: one
        // VkImageMemoryBarrier2 per run, never one guessed layout for all.
        void ForEachLayoutRun(VkImage image,
                              const VkImageSubresourceRange& range,
                              const std::function<void(const VkImageSubresourceRange&, VkImageLayout)>& visitor) const;

        // Convenience for whole-range queries: the single layout when every
        // covered subresource agrees, VK_IMAGE_LAYOUT_UNDEFINED otherwise
        // (mixed or unknown).
        [[nodiscard]] VkImageLayout CurrentLayout(VkImage image, const VkImageSubresourceRange& range) const;

      private:
        struct ImageState
        {
            u32 MipCount = 1;
            u32 LayerCount = 1;
            u64 RegistrationId = 0;
            // Indexed [layer * MipCount + mip].
            std::vector<VkImageLayout> Layouts;
        };

        // Clamp an open (VK_REMAINING_*) range against the image's extents.
        struct ResolvedRange
        {
            u32 BaseMip = 0;
            u32 MipCount = 0;
            u32 BaseLayer = 0;
            u32 LayerCount = 0;
        };
        [[nodiscard]] static ResolvedRange Resolve(const ImageState& state, const VkImageSubresourceRange& range);

        std::unordered_map<VkImage, ImageState> m_Images;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
