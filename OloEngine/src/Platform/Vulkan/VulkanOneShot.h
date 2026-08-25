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
        enum class Outcome
        {
            NotSubmitted, // never reached the queue — nothing recorded will run
            Submitted,    // accepted by the queue, but the fence wait did not succeed
            Completed,    // accepted and retired; the work is done
        };

        // Returns false (with an error log naming `what`) when no live device
        // exists or any Vulkan call fails; the callback is then never invoked
        // or its recording is discarded. `outOutcome`, when given, separates
        // the two failure kinds above. Never throws.
        bool Submit(const char* what, const std::function<void(VkCommandBuffer)>& record,
                    Outcome* outOutcome = nullptr);

        // Staged upload into a non-mappable buffer: host staging → one-shot
        // vkCmdCopyBuffer → an availability barrier covering every later
        // submission's reads (a fence wait alone does not make transfer
        // writes visible across submissions) → staging destroyed. Blocking,
        // like Submit.
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
        // fail — the VK_TIMEOUT shape. Outcome is `Submitted`, so a caller
        // MUST still advance any state the queued work establishes. Without
        // this the timeout path has no test, and it is the half that looks
        // identical to a hard failure at the call site.
        void SetFailNextFenceWaitsForTesting(u32 count);
        [[nodiscard]] u32 GetPendingFailNextFenceWaitsForTesting();
#endif
    } // namespace VulkanOneShot
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
