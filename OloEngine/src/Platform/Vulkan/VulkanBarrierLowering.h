#pragma once

// =============================================================================
// VulkanBarrierLowering — the pure RHI::Access → Vulkan synchronization2
// lowering (issue #691, ADR 0011 §1.5).
//
// This is the Vulkan half of the two-currency barrier contract: the render
// graph's planner derives (FromAccess, ToAccess, range, lanes) as the neutral
// truth, the GL backend executes the MemoryBarrierFlags bitmask, and THIS
// module lowers the neutral pair to (stage mask, access mask, image layout).
//
// Everything here is a pure function — no device, no state, no allocation —
// so the whole table is pinned headlessly by VulkanBarrierLoweringTest in
// plain CI, which is the cheap layer that proves the formula
// (CLAUDE.md's rendering-verification rule, step 1).
//
// Layout is resolved from (Access, aspect, read-while-attached) — ADR 0011
// §1.5 explicitly rejects a bare Access → layout table: the depth/stencil
// aspect and the read-while-attached flag are the two inputs the common-case
// mapping cannot do without (the PCSS blocker search samples raw depth of a
// resource that is simultaneously in play as an attachment).
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <volk.h>

namespace OloEngine::VulkanBarrierLowering
{
    struct StageAccess
    {
        VkPipelineStageFlags2 StageMask = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 AccessMask = VK_ACCESS_2_NONE;
    };

    // Lower one side of a transition to (stage mask, access mask).
    //
    // `queue` narrows the shader-stage union: a Compute-lane access waits on
    // COMPUTE_SHADER only; a Graphics-lane access uses the full graphics
    // union PLUS compute, because this engine's Graphics-work-type passes
    // legitimately dispatch compute mid-pass (bloom, AO). Over-inclusion is
    // correctness-safe (extra sync); under-inclusion is a race.
    //
    // `aspect` matters only for the two clear/attachment accesses whose stage
    // differs between color and depth (ClearAsLoadOp).
    [[nodiscard]] StageAccess LowerAccess(RHI::Access access, RHI::QueueType queue, RHI::TextureAspect aspect);

    // The layout-resolution function of ADR 0011 §1.5:
    // (Access, aspect, read-while-attached) → VkImageLayout.
    //
    //  - Storage accesses → GENERAL (heap-bindless storage images live there).
    //  - A read-while-attached depth sample → DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    //    never SHADER_READ_ONLY_OPTIMAL — the attachment half of the feedback
    //    is still live.
    //  - A read-while-attached color sample / input attachment → GENERAL.
    //    ATTACHMENT_FEEDBACK_LOOP_OPTIMAL would be tighter but requires
    //    VK_EXT_attachment_feedback_loop_layout, which the ADR 0010 contract
    //    does not carry; GENERAL is always legal.
    //  - Undefined → UNDEFINED (discardable — the external/first-use state).
    //
    // Buffers have no layout; callers only consult this for image barriers.
    [[nodiscard]] VkImageLayout LayoutFor(RHI::Access access, RHI::TextureAspect aspect, bool readWhileAttached);

    [[nodiscard]] VkImageAspectFlags AspectMaskFor(RHI::TextureAspect aspect);

    // RHI::Format -> VkFormat, honouring the image allocator's widening rules
    // (3-channel formats widen to RGBA, D24S8 lowers to D32_S8 — the
    // VulkanTransientResources choices, mirrored so a view reinterpretation
    // names the format the image actually has). No default: -Wswitch makes a
    // new RHI::Format member a compile error here (#691).
    [[nodiscard]] VkFormat ToVkFormat(RHI::Format format);

    // Assemble a full VkImageMemoryBarrier2 from a neutral barrier plus the
    // backend-known image identity.
    //
    // `aspect` comes from the backend's own knowledge of the image's format —
    // authoritative, where the neutral Range.Aspect is only a request for an
    // aspect-split transition (which no pass declares today).
    //
    // `trackedOldLayout` comes from the VulkanImageLayoutTracker and is
    // AUTHORITATIVE for oldLayout: the neutral FromAccess describes the
    // producer's access (source stage/access masks), but the image's actual
    // layout can differ — a pooled transient re-acquired this frame carries
    // whatever its previous tenant left, and first use is UNDEFINED. When the
    // tracker answers UNDEFINED the source masks are forced to NONE too:
    // contents are discarded, so there is nothing to make available.
    //
    // Queue-family indices come out IGNORED. A barrier that must also move a
    // resource between queue families is built here first and then split by
    // SplitImageOwnershipTransfer below — one source of truth for the masks,
    // the layouts and the range, so the two halves cannot disagree.
    [[nodiscard]] VkImageMemoryBarrier2 BuildImageBarrier(const RHI::Barrier& barrier,
                                                          VkImage image,
                                                          RHI::TextureAspect aspect,
                                                          VkImageLayout trackedOldLayout,
                                                          u32 imageMipCount,
                                                          u32 imageLayerCount);

    [[nodiscard]] VkBufferMemoryBarrier2 BuildBufferBarrier(const RHI::Barrier& barrier, VkBuffer buffer);

    // --- Queue-family ownership transfer (issue #808) ---------------------
    //
    // A `VK_SHARING_MODE_EXCLUSIVE` resource whose CONTENTS must survive a move
    // between two queue families needs a matched pair of barriers: a RELEASE
    // recorded into the source family's command buffer and an ACQUIRE into the
    // destination family's, submitted in that order with a semaphore between
    // them (Vulkan 1.4 §7.7.4). A semaphore alone orders the two queues and
    // leaves the contents UNDEFINED.
    //
    // The two halves are asymmetric, and the asymmetry is load-bearing twice
    // over:
    //   - the release carries the source scope and NO destination scope; the
    //     acquire carries the destination scope and NO source scope — which is
    //     what the spec says is read from each;
    //   - and it is also what keeps the pair LEGAL. A compute-only queue may
    //     not name a graphics stage in a barrier
    //     (VUID-vkCmdPipelineBarrier2-srcStageMask-03849), and a compute pass's
    //     producer is usually a raster pass. Because the graphics stages live
    //     only in the source scope, they appear only on the half recorded into
    //     the graphics command buffer.
    //
    // Everything else — `oldLayout`, `newLayout`, the image/buffer and the
    // subresource range — must be IDENTICAL on both halves; the layout
    // transition happens once. Splitting one already-built barrier is how that
    // is guaranteed rather than reviewed.
    //
    // `srcFamily` and `dstFamily` are FAMILY indices, never queue indices, and
    // must differ (equal indices mean there is nothing to transfer).
    struct ImageOwnershipTransfer
    {
        VkImageMemoryBarrier2 Release{};
        VkImageMemoryBarrier2 Acquire{};
    };
    [[nodiscard]] ImageOwnershipTransfer SplitImageOwnershipTransfer(const VkImageMemoryBarrier2& barrier,
                                                                     u32 srcFamily, u32 dstFamily);

    struct BufferOwnershipTransfer
    {
        VkBufferMemoryBarrier2 Release{};
        VkBufferMemoryBarrier2 Acquire{};
    };
    [[nodiscard]] BufferOwnershipTransfer SplitBufferOwnershipTransfer(const VkBufferMemoryBarrier2& barrier,
                                                                       u32 srcFamily, u32 dstFamily);

    // The pipeline stages a COMPUTE-ONLY queue family supports. Anything
    // outside this set is invalid usage in a command buffer allocated from
    // such a family's pool, whatever the barrier means. Used by
    // VulkanRecordingContext::RecordBarrier to clamp scopes that reach the
    // compute queue from a code path with no idea which queue it is on.
    [[nodiscard]] VkPipelineStageFlags2 ComputeQueueStageMask();
} // namespace OloEngine::VulkanBarrierLowering

#endif // OLO_WITH_VULKAN
