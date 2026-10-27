#pragma once

// =============================================================================
// VulkanOneShot — record-submit-wait helper for load-time GPU work
// (issue #691).
//
// Asset uploads (texture pixels, mesh vertex/index data) happen at LOAD time,
// outside any frame's command buffer. On GL the driver hides the transfer; on
// Vulkan someone must own a command buffer, a submission and a fence. This
// helper is that someone: allocate from the device's pool, record via the
// callback, submit, WAIT the fence, free. Blocking by design — it mirrors the
// synchronous semantics of the GL upload calls the resource classes replace,
// so load-time behavior stays identical across backends.
//
// Interleaving contract: a one-shot submit while the frame loop is RECORDING
// (not yet submitted) is legal and ordered BEFORE that frame's submission —
// queue submissions execute in submit order. Do not call from inside a
// vkCmdBeginRendering scope on the frame command buffer; the one-shot uses
// its OWN command buffer, so the only hazard would be a caller assuming the
// upload lands after already-recorded-but-unsubmitted frame commands.
//
// Deliberately does NOT drain VulkanGpuFence's staged queue ops — those
// belong to the frame submit (VulkanContext::SwapBuffers), and hijacking
// them here would attach frame-pacing semantics to an asset upload.
//
// Thread-safety: NONE, deliberately — render thread only, like the rest of
// the backend.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"

#include <functional>

namespace OloEngine
{
    namespace VulkanOneShot
    {
        // Why a failure needs two answers, not one.
        //
        // `Submit` returns false for two situations a caller must NOT treat
        // alike. If it failed before vkQueueSubmit2 — no device, allocation,
        // begin/end, fence creation, or the submit call itself — nothing was
        // queued, so nothing the callback recorded will ever execute. But if
        // the submit was ACCEPTED and only the fence wait timed out, the
        // command buffer is on the queue and its transitions WILL execute; a
        // caller that then keeps the old layout has a stale record, which is
        // the same wrong-oldLayout desync as recording the new one too early
        // (#803, and see rhi-abstraction-boundary.md §16a).
        //
        // So: state a resource reaches through queued work is gated on
        // `Submitted`, not on the bool. The bool stays "fully completed and
        // waited", which is what every existing caller means by it.
        //
        // Either way, Submit does not return while its submission can still
        // be executing: after a failed wait it waits again, without a
        // timeout, until the work retires or the device is lost. So a caller
        // may release what the recording read (#1294). If that second wait
        // errors, Submit waits for the queue to go idle instead. Only when
        // that fails too does it return `DrainFailed`: the fence and command
        // buffer are leaked rather than released in flight, and a caller must
        // keep (leak) anything the recording reads, as UploadToBuffer does.
        //
        // A second wait that RETIRES the work makes the result `Completed`
        // and true, with a warning: the 10 s first wait only separates a slow
        // GPU from a hung one, and a readback whose copy finished in the
        // drain holds valid data (#1556). `Submitted` is left for a device
        // lost before anything proved the work retired.
        enum class Outcome
        {
            NotSubmitted, // never reached the queue — nothing recorded will run
            Submitted,    // accepted by the queue; the device was lost before the work was seen to retire
            DrainFailed,  // accepted, and nothing proves it retired: keep what it reads
            Completed,    // accepted and retired; the work is done
        };

        // Returns false (with an error log naming `what`) when no live device
        // exists or any Vulkan call fails; the callback is then never invoked
        // or its recording is discarded. `outOutcome`, when given, separates
        // the two failure kinds above. Never throws.
        bool Submit(const char* what, const std::function<void(VkCommandBuffer)>& record,
                    Outcome* outOutcome = nullptr);

        // Staged upload into a non-mappable buffer: host staging → one-shot
        // vkCmdCopyMemoryKHR → an availability barrier covering every later
        // submission's reads (a fence wait alone does not make transfer
        // writes visible across submissions) → staging destroyed. Blocking,
        // like Submit.
        //
        // PRECONDITION (#1179): `dst` must have been created with
        // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT as well as
        // VK_BUFFER_USAGE_TRANSFER_DST_BIT. The copy is address-based now, so
        // the destination's address is queried here — and
        // vkGetBufferDeviceAddress on a buffer without the bit is invalid
        // usage. Every engine buffer family sets it (vertex, index, storage,
        // raw, ray-tracing), so this holds for every caller today; it is
        // written down because it is the one requirement the address form
        // ADDED and nothing in the signature says so.
        bool UploadToBuffer(VkBuffer dst, u64 dstOffset, const void* data, u64 sizeBytes, const char* what);

#ifndef OLO_DIST
        // Fault injection for the layout-desync tenants (#803). The next
        // `count` Submit() calls return false without recording or submitting
        // anything, which is the shape a caller must survive: the image stays
        // in its OLD layout, so a caller that records the NEW one into
        // VulkanImageInfoRegistry has created the wrong-oldLayout desync
        // (VUID-VkImageMemoryBarrier2-oldLayout-01197 / the VUID-09600 family)
        // that InitialLayout exists to prevent.
        //
        // Compiled out of Dist. Callers are tests only — a shipping code path
        // that reaches for this is a bug.
        void SetFailNextSubmitsForTesting(u32 count);
        [[nodiscard]] u32 GetPendingFailNextSubmitsForTesting();

        // The OTHER half of the fault injection: the next `count` submits are
        // recorded and queued for real, then their fence wait is forced to
        // fail — the VK_TIMEOUT shape. The drain that follows waits for real,
        // so on a live device the work retires and the call reports
        // `Completed` and true: a caller must still advance any state the
        // queued work establishes, and a readback must return its data.
        // Without this the timeout path has no test.
        void SetFailNextFenceWaitsForTesting(u32 count);
        [[nodiscard]] u32 GetPendingFailNextFenceWaitsForTesting();
#endif
    } // namespace VulkanOneShot
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
