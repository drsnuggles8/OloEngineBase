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
//
// OVERLAYS (issue #806, ADR 0011 amendment (92) rule 5). A RecordParallel
// item records on its own context with its own tracker, which is an
// OVERLAY over the render thread's: reads fall through to the base for any
// image the item has not written, the first write copies that image's row
// in, and every write is remembered per subresource. At the join the
// overlays merge into the base in item order, subresource by subresource,
// writing only what each item wrote. The base is frozen for the whole
// region (the render thread is inside the join), so concurrent reads of it
// from several overlays are safe. Two items may write the same subresource
// only when every such write is an identity transition (the layout the row
// was copied with == the layout written) — otherwise the later item's
// barrier named an oldLayout the earlier item already changed, and the
// merge reports a conflict. Pinned by VulkanParallelRecordingTest.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // One RecordParallel region's record-time claims (#806): the first item
    // that transitions a subresource NON-identically claims it; a second item
    // doing the same is amendment (92) rule 5's conflict, and this names both
    // items at the offending SetLayout instead of leaving it to the merge to
    // count. Rare path (a few claims per item), so a mutex is fine.
    class VulkanLayoutClaimTable
    {
      public:
        static constexpr u32 kNone = 0xFFFFFFFFu;
        void Reset();
        // Returns kNone when `item` now holds (or already held) the claim, or
        // the index of the OTHER item that holds it.
        [[nodiscard]] u32 Claim(VkImage image, sizet subresource, u32 item);

      private:
        struct Key
        {
            VkImage Image = VK_NULL_HANDLE;
            sizet Subresource = 0;
            [[nodiscard]] auto operator==(const Key&) const -> bool = default;
        };
        struct KeyHash
        {
            [[nodiscard]] sizet operator()(const Key& key) const noexcept
            {
                sizet hash = std::hash<VkImage>{}(key.Image);
                hash ^= std::hash<sizet>{}(key.Subresource) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
        std::mutex m_Mutex;
        std::unordered_map<Key, u32, KeyHash> m_Claims;
    };

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

        // ---- Overlays (issue #806) ----------------------------------------
        // Turn this tracker into an overlay over `base` (nullptr restores a
        // plain tracker). The base must outlive the overlay and must not be
        // mutated while the overlay is live (the fork/join contract). With a
        // claim table, every non-identity write is claimed for `itemIndex` as
        // it happens (see VulkanLayoutClaimTable).
        void SetReadThroughBase(const VulkanImageLayoutTracker* base, VulkanLayoutClaimTable* claims = nullptr,
                                u32 itemIndex = 0);
        [[nodiscard]] bool IsOverlay() const
        {
            return m_Base != nullptr;
        }
        // One join's worth of merge bookkeeping: which subresources earlier
        // items wrote, and whether identically. Create one per join, pass it
        // to every MergeOverlayInto in item order, read Conflicts afterwards.
        struct MergeBatch
        {
            // Per image, per subresource index: 0 = untouched, 1 = written as
            // an identity transition, 2 = written as a real transition.
            std::unordered_map<VkImage, std::vector<u8>> Written;
            u32 Conflicts = 0;
            u32 SubresourcesMerged = 0;
        };
        // Copy every subresource this overlay wrote into `base` (its own
        // base, normally), report overlaps through `batch`, and drop the
        // overlay's local rows. Executed layouts are untouched — the base's
        // CommitRecordedToExecuted at EndRecording covers the items' work
        // exactly as it covers the render thread's.
        void MergeOverlayInto(VulkanImageLayoutTracker& base, MergeBatch& batch);
        // Drop the overlay's local rows without merging (an item that never
        // recorded, or a declined fork).
        void DiscardOverlay();
        [[nodiscard]] sizet GetOverlayRowCount() const
        {
            return m_Images.size();
        }

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
        // the recording bracket closes; an immediate-execution scope advances
        // it for the work recorded inside, but only once that work has really
        // reached the queue (see ImmediateExecutionScope::MarkSubmitted).
        void CommitRecordedToExecuted();
        [[nodiscard]] VkImageLayout CurrentExecutedLayout(VkImage image, const VkImageSubresourceRange& range) const;

        // RAII, opened by VulkanOneShot::Submit. It must span the WHOLE
        // submission — record, submit, fence wait — not just the record
        // callback, because a layout written while recording is speculative
        // until that command buffer is actually on the queue. Inside the
        // scope, SetLayout advances the recorded layout immediately (a later
        // barrier in the same buffer must transition from it) and QUEUES the
        // executed-layout write; MarkSubmitted() is what promotes the queued
        // writes, and the destructor discards them if it was never called.
        //
        // Two things that buys, both of which the eager version got wrong: a
        // one-shot that fails before reaching the queue no longer claims its
        // transitions happened, and a nested one-shot — should one ever be
        // recorded from inside another's callback — cannot barrier from the
        // OUTER submission's not-yet-executed layout. Scopes nest as a stack,
        // each owning its own queued writes.
        class ImmediateExecutionScope
        {
          public:
            ImmediateExecutionScope();
            ~ImmediateExecutionScope();
            // Call once the recorded work has reached the queue and retired.
            void MarkSubmitted();
            ImmediateExecutionScope(const ImmediateExecutionScope&) = delete;
            ImmediateExecutionScope& operator=(const ImmediateExecutionScope&) = delete;
            ImmediateExecutionScope(ImmediateExecutionScope&&) = delete;
            ImmediateExecutionScope& operator=(ImmediateExecutionScope&&) = delete;

          private:
            bool m_Submitted = false;
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
            // Overlay rows only (#806): the layouts this row was copied from
            // the base with, and which subresources this overlay wrote:
            // 0 untouched, 1 identity writes only, 2 at least one real
            // transition (what the merge's identity test reads).
            std::vector<VkImageLayout> Original;
            std::vector<u8> Written;
        };

        // Overlay support: copy `image`'s row from the base into m_Images (no
        // op when the base has no such row). Returns the local row or null.
        ImageState* CopyRowFromBase(VkImage image);

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
        const VulkanImageLayoutTracker* m_Base = nullptr;
        VulkanLayoutClaimTable* m_Claims = nullptr;
        u32 m_ItemIndex = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
