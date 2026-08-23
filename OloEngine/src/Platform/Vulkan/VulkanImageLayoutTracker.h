#pragma once

// =============================================================================
// VulkanImageLayoutTracker — per-subresource image-layout state
// (issue #691).
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
        // Trackers self-register so the deferred-reclaim destroy pass can
        // retire an image from all of them (see ForgetImageEverywhere); the
        // registered `this` is why copy/move are deleted.
        VulkanImageLayoutTracker();
        ~VulkanImageLayoutTracker();
        VulkanImageLayoutTracker(const VulkanImageLayoutTracker&) = delete;
        VulkanImageLayoutTracker& operator=(const VulkanImageLayoutTracker&) = delete;
        VulkanImageLayoutTracker(VulkanImageLayoutTracker&&) = delete;
        VulkanImageLayoutTracker& operator=(VulkanImageLayoutTracker&&) = delete;

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
        // load-time one-shot uploaded (VulkanImageInfo::InitialLayout, #691)
        // Transitioning those from UNDEFINED would legally discard
        // the uploaded pixels.
        void RegisterImage(VkImage image, u32 mipCount, u32 layerCount, u64 registrationId = 0,
                           VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

        // Drop all state for an image (call at destroy — a recycled handle
        // value must not inherit the dead image's layouts).
        void ForgetImage(VkImage image);

        // Drop `image` from EVERY live tracker. This is the call the reclaim
        // queue's destroy pass makes: it is the only place that knows an image
        // is really gone, and it cannot name a particular tracker (the API
        // owns one per instance and a test fixture may own its own). Without
        // it the map only ever grows — one row per VkImage ever seen, for the
        // life of the process. Correctness does not depend on it (a recycled
        // handle value carries a fresh RegistrationId, which resets the row),
        // so this is a leak fix, not a hazard fix.
        static void ForgetImageEverywhere(VkImage image);

        void Reset();

        // ---- Recorded vs EXECUTED layout (issue #800) --------------------
        // SetLayout below advances the RECORDED layout: what the command
        // buffer currently being recorded will have left the image in, once it
        // is submitted. That is the right answer for a barrier recorded into
        // that same command buffer, and the WRONG answer for work that will
        // execute BEFORE it.
        //
        // VulkanOneShot::Submit is exactly that work: it submits its own
        // command buffer immediately, so it runs ahead of the frame command
        // buffer still being recorded (ADR 0011 amendment 72). A one-shot that
        // barriers an image with oldLayout = CurrentLayout(...) is therefore
        // naming a layout the queue has not reached yet. On a steady frame the
        // two agree by construction — the graph ends every frame in the layouts
        // it started in — and nothing goes wrong, which is why this only ever
        // fired on the ONE frame where an attachment is brand new: a window
        // resize. That is issue #800.
        //
        // So the tracker keeps a second layout per subresource: the layout
        // after all SUBMITTED work. CommitRecordedToExecuted() advances it when
        // the recording bracket closes; writes made while an immediate-
        // execution scope is open advance BOTH, since that work reaches the
        // queue as it is recorded.
        void CommitRecordedToExecuted();
        [[nodiscard]] VkImageLayout CurrentExecutedLayout(VkImage image, const VkImageSubresourceRange& range) const;

        // RAII, opened by VulkanOneShot::Submit around its record callback.
        // Nesting is counted: ClearTextureFloat's load-time fallback records a
        // one-shot from inside another one's callback.
        class ImmediateExecutionScope
        {
          public:
            ImmediateExecutionScope();
            ~ImmediateExecutionScope();
            ImmediateExecutionScope(const ImmediateExecutionScope&) = delete;
            ImmediateExecutionScope& operator=(const ImmediateExecutionScope&) = delete;
            ImmediateExecutionScope(ImmediateExecutionScope&&) = delete;
            ImmediateExecutionScope& operator=(ImmediateExecutionScope&&) = delete;
        };
        [[nodiscard]] static bool InImmediateExecutionScope();

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
            // Same indexing; the layout after all SUBMITTED work (see
            // CommitRecordedToExecuted).
            std::vector<VkImageLayout> Executed;
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
