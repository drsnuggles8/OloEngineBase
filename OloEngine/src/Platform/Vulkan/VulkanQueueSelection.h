#pragma once

// =============================================================================
// VulkanQueueSelection — which queue family carries async compute, and why not
// (issue #808).
//
// The backend has always created ONE queue: a family with graphics and, when a
// surface exists, present (ADR 0010's queue-topology row). This module adds the
// SECOND, OPTIONAL pick: a family that can dispatch compute but cannot draw, so
// compute work with no ordering relationship to the raster work around it can
// execute on a queue of its own.
//
// It is a pure function over the family properties the driver reports, with no
// device and no allocation, for the same reason VulkanBarrierLowering is pure:
// the policy — including every way it says NO — is then pinned in plain CI by
// VulkanAsyncComputeTest against fabricated family tables, on machines with no
// Vulkan device at all. That matters more here than usual, because the
// unavailable answer is the one CI hardware actually takes.
//
// WHAT COUNTS AS AN ASYNC COMPUTE FAMILY. Compute-capable, NOT graphics-capable,
// with at least one queue. The graphics family always reports COMPUTE too
// (Vulkan requires it), so "compute-capable" alone would select the family the
// engine already uses and buy nothing; the absence of VK_QUEUE_GRAPHICS_BIT is
// what makes a family a genuinely separate hardware pipe. Among several, the
// one with the FEWEST capability bits wins (the most dedicated one — a
// compute+transfer family is preferred over compute+transfer+video), ties going
// to the lowest index so the pick is deterministic across runs and captures.
//
// TIMESTAMPS ARE PART OF THE PICK, not an afterthought. `timestampValidBits`
// is a PER-FAMILY property, and vkCmdWriteTimestamp2 on a family reporting 0 is
// invalid usage (VUID-vkCmdWriteTimestamp2-timestampValidBits-03863). The
// engine stamps every render-graph pass through GPUPassTimerPool, so a family
// that cannot carry those stamps would either produce validation errors or
// force a second, untested "no timers here" recording path. It is refused
// instead — and refused with its own reason, so the log says which of the two
// things the device lacks. Every desktop async-compute family on the ADR 0010
// hardware floor reports 64.
//
// NOT AN ADR 0010 CONTRACT ROW. A device with no such family still satisfies
// the contract: the renderer keeps every compute pass on the graphics queue and
// says so, loudly and countably (CLAUDE.md — no silent fallbacks). That degrade
// path is the one CI runs, so it is the one the tests cover first.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <span>
#include <string_view>

namespace OloEngine::VulkanQueueSelection
{
    // Why a device has no async compute queue. `None` means it has one.
    // Stored as an enum rather than a string so the log line, the MCP
    // telemetry and the test assertion cannot drift from each other.
    enum class AsyncComputeUnavailableReason : u8
    {
        None = 0,                  ///< An async compute family was selected.
        NoDeviceQueueFamilies,     ///< The driver reported no families at all (a broken ICD).
        NoComputeOnlyFamily,       ///< Every compute-capable family also does graphics.
        FamilyHasNoQueues,         ///< A compute-only family exists but reports queueCount == 0.
        FamilyHasNoTimestamps,     ///< A compute-only family exists but cannot write GPU timestamps.
        CommandPoolCreationFailed, ///< The queue exists; its command pool could not be created.
        DisabledByLever,           ///< OLO_VK_ASYNC_COMPUTE=0 — the deliberate A/B arm.
        NoDevice,                  ///< Asked before (or after) the device existed.
    };

    [[nodiscard]] std::string_view Describe(AsyncComputeUnavailableReason reason);

    struct AsyncComputeSelection
    {
        bool Found = false;
        u32 FamilyIndex = 0;
        // The family's own timestampValidBits. A GPU timer written on this
        // queue is meaningless when it is 0, and the value is PER FAMILY —
        // the graphics family's 64 says nothing about this one (the
        // timestamp-period finding in #801 is the same class of mistake).
        u32 TimestampValidBits = 0;
        AsyncComputeUnavailableReason Reason = AsyncComputeUnavailableReason::NoComputeOnlyFamily;
    };

    // `families` is vkGetPhysicalDeviceQueueFamilyProperties' output verbatim;
    // `graphicsFamily` is the already-picked graphics(+present) family, which
    // is never a candidate no matter what its flags say.
    [[nodiscard]] AsyncComputeSelection SelectAsyncComputeFamily(std::span<const VkQueueFamilyProperties> families,
                                                                 u32 graphicsFamily);

    // --- Sharing mode for buffers both queues can reach ---------------------
    //
    // WHY BUFFERS ARE CONCURRENT AND IMAGES ARE NOT. An ownership transfer can
    // only be emitted for a resource somebody NAMES, and on this backend the
    // set of buffers a compute dispatch reads is not statically known: root
    // data reaches a shader as a device ADDRESS out of the frame arena, and
    // every texture and sampler is reached by index through the descriptor and
    // sampler heaps (ADR 0011 §4, ADR 0010's heap-bindless row). The render
    // graph declares the pass's own inputs and outputs; it does not — and
    // cannot — declare the heap, the arena or the root-data storage the
    // binding model dereferences on the way there.
    //
    // Left EXCLUSIVE, those buffers' contents are undefined the moment a second
    // queue family reads them, and their contents are POINTERS. That is not a
    // wrong pixel: it is a garbage address, and #808's first live run took a
    // VK_ERROR_DEVICE_LOST with a "READ of invalid address" device-fault report
    // for exactly this reason, with the validation layers silent throughout —
    // undefined contents are not invalid usage, so there is nothing for them to
    // report.
    //
    // Images keep EXCLUSIVE sharing and the release/acquire pairs, because a
    // compute pass reaches an image through a declared graph edge (that edge is
    // what plans the barrier the pair is split from), and CONCURRENT costs
    // image compression on hardware that has it.
    //
    // No async compute queue -> every resource stays EXCLUSIVE and nothing
    // about the single-queue backend changes.
    class CrossQueueBufferSharing
    {
      public:
        // Reads the live VulkanDevice. Cheap; construct one per buffer
        // creation and keep it alive until the create call returns — the
        // family array must outlive it.
        CrossQueueBufferSharing();
        void ApplyTo(VkBufferCreateInfo& info) const;
        [[nodiscard]] bool IsConcurrent() const
        {
            return m_Count > 1u;
        }

      private:
        u32 m_Families[2]{};
        u32 m_Count = 0;
    };
} // namespace OloEngine::VulkanQueueSelection

#endif // OLO_WITH_VULKAN
