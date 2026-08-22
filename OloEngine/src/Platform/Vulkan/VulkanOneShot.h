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
        // Returns false (with an error log naming `what`) when no live device
        // exists or any Vulkan call fails; the callback is then never invoked
        // or its recording is discarded. Never throws.
        bool Submit(const char* what, const std::function<void(VkCommandBuffer)>& record);

        // Staged upload into a non-mappable buffer: host staging → one-shot
        // vkCmdCopyBuffer → an availability barrier covering every later
        // submission's reads (a fence wait alone does not make transfer
        // writes visible across submissions) → staging destroyed. Blocking,
        // like Submit.
        bool UploadToBuffer(VkBuffer dst, u64 dstOffset, const void* data, u64 sizeBytes, const char* what);
    } // namespace VulkanOneShot
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
