#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanRayTracingBackend — IRayTracingBackend over VK_KHR_acceleration_structure.
// Issue #978.
//
// This is the device half of the RT scene: it owns VkAccelerationStructureKHR
// handles and their backing buffers, the pooled build scratch, the
// compacted-size query pool, and the barriers. The policy that decides WHAT to
// build lives in the neutral RayTracingScene.
//
// FOUR RULES THIS FILE EXISTS TO HOLD:
//
//  1. **Builds ride the FRAME command buffer, never a one-shot.** A one-shot
//     submit is ordered BEFORE the still-recording frame (ADR 0011 amendment
//     (72)), so a BLAS built that way would consume vertex data the frame has
//     not uploaded yet — silently, with the wrong geometry and no error.
//
//  2. **No vkDeviceWaitIdle on routine scene mutation.** Compaction is
//     therefore a two-stage, multi-frame flow: build and stamp the size query
//     in one frame, poll the query WITHOUT waiting in a later one, and only
//     then record the compacting copy. A blocking read of that query is what
//     an idle would be hiding.
//
//  3. **Nothing is destroyed inline.** Every retired acceleration structure
//     and buffer goes to VulkanDeferredReclaim, and the AS handle is enqueued
//     BEFORE the buffer that backs it — the reclaim queue destroys in
//     insertion order within a generation, and destroying the memory first is
//     use-after-free inside the driver.
//
//  4. **Scratch is device-local and explicitly aligned.** The frame arena is
//     the wrong home for it: it is host-visible write-combined memory whose
//     base address carries no alignment guarantee, and AS builds hammer
//     scratch with GPU writes.
// =============================================================================

#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "Platform/Vulkan/VulkanDevice.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace OloEngine::RayTracing
{
    // Constructed only through this factory, and only from the neutral
    // CreateRayTracingBackend() switch — a Platform/ TU that installs itself
    // unconditionally is the §9a factory leak.
    [[nodiscard]] std::unique_ptr<IRayTracingBackend> CreateVulkanRayTracingBackend();
} // namespace OloEngine::RayTracing

#endif // OLO_WITH_VULKAN
