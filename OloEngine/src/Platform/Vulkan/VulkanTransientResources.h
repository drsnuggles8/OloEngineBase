#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTransientResources.h — TRANSITIONAL UMBRELLA (#691 Phase 9 split).
//
// The VMA-backed resource classes that lived here as one 11-type header now
// live in per-class file pairs mirroring the OpenGL twin's layout. This
// header remains only so existing consumers keep compiling unchanged; NEW
// code should include the specific header it needs:
//
//   VulkanImageInfoRegistry.h     — VulkanImageInfo + VulkanImageInfoRegistry
//   VulkanDeferredReclaim.h       — generation-waited destruction
//   VulkanTexture.h               — VulkanTexture2D
//   VulkanTexture3D.h             — VulkanTexture3D
//   VulkanTextureCubemap.h        — VulkanTextureCubemap
//   VulkanTextureCubemapArray.h   — VulkanTextureCubemapArray
//   VulkanTexture2DArray.h        — VulkanTexture2DArray
//   VulkanFramebuffer.h           — VulkanFramebuffer
//   VulkanStorageBuffer.h         — VulkanStorageBuffer
//   VulkanRawResourceRegistries.h — VulkanRawTextureRegistry +
//                                   VulkanRawFramebufferRegistry
//
// (The formerly shared anonymous-namespace .cpp helpers moved to the
// TU-internal VulkanTransientUpload.{h,cpp} — deliberately NOT included
// here; see that header's banner.)
// =============================================================================

#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanTexture3D.h"
#include "Platform/Vulkan/VulkanTextureCubemap.h"
#include "Platform/Vulkan/VulkanTextureCubemapArray.h"
#include "Platform/Vulkan/VulkanTexture2DArray.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"
#include "Platform/Vulkan/VulkanRawResourceRegistries.h"

namespace OloEngine
{
    // Teardown forensics (#691 Phase 8): logs every VulkanTexture2D /
    // VulkanStorageBuffer object still alive, with its Debug-captured
    // creation stack — the texture/storage twin of
    // VulkanRootObjectRegistry::LogSurvivingVertexArrays. No-op in
    // non-Debug builds.
    // Declared here (not via the TU-internal VulkanTransientUpload.h, which
    // consumers must not include); defined in VulkanTransientUpload.cpp
    // alongside the TrackLive/UntrackLive machinery it walks.
    void VulkanLogSurvivingTransients();

} // namespace OloEngine

#endif // OLO_WITH_VULKAN
