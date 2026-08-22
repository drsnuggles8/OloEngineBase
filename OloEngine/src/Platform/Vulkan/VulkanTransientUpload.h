#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTransientUpload.h — the staging/upload/readback plumbing shared by the
// per-class transient-resource TUs (#691: these helpers were the
// anonymous-namespace members that kept the old VulkanTransientResources.cpp
// one TU; the split promoted the genuinely shared ones here, and each class
// keeps its single-consumer helpers in its own .cpp's anonymous namespace).
//
// INTERNAL — include this ONLY from the Platform/Vulkan transient-resource
// .cpp files. Several sibling TUs (VulkanDevice.cpp, VulkanContext.cpp,
// VulkanBufferResources.cpp, VulkanImGuiBackend.cpp, VulkanRendererAPI.cpp)
// keep their own anonymous-namespace copies of some of these names (VkCheck,
// VkHandleToU64, ImageFormatToVkFormat, ExpandRgbToRgba). That is why these
// live in a NESTED namespace and are called qualified (VulkanUpload::VkCheck):
// at OloEngine scope they collided with those copies by name, so build
// correctness rested on a hand-maintained unity-exclusion list — the
// two-mirrors-drift shape this repo has a whole doc genre about (review
// finding, #691). The nesting makes the collision unrepresentable;
// no exclusion list is load-bearing any more.
// =============================================================================

// VulkanDevice.h provides <volk.h> and <vk_mem_alloc.h> (with the
// VMA_STATIC/DYNAMIC_VULKAN_FUNCTIONS config that must stay in sync with
// VulkanMemoryAllocator.cpp) — do NOT include either directly here, and NEVER
// <vulkan/vulkan.h> (volk owns the function pointers, ADR 0011 amendment 41a).
#include "Platform/Vulkan/VulkanDevice.h"

#include "OloEngine/Renderer/Texture.h" // ImageFormat

#include <cstdint>
#include <type_traits>
#include <vector>

namespace OloEngine
{
    // Forward-declared at OloEngine scope ON PURPOSE: inside the nested
    // namespace below it would declare a DISTINCT (never-defined)
    // VulkanUpload::VulkanRendererAPI, and every use would be an
    // incomplete type. Unqualified uses inside VulkanUpload resolve here
    // by ordinary enclosing-scope lookup.
    class VulkanRendererAPI;
} // namespace OloEngine

namespace OloEngine::VulkanUpload
{

    // Teardown forensics for object textures / storage buffers (#691
    // The close-button VMA abort): a Ref surviving the full
    // renderer teardown keeps its VMA allocation alive into
    // vmaDestroyAllocator. The VAO twin lives in VulkanBufferResources
    // (LogSurvivingVertexArrays); this covers the classes owned by the
    // transient-resource TUs. No-ops in non-Debug builds.
    // VulkanLogSurvivingTransients walks the tracked set, but it is NOT part
    // of this namespace: it is a cross-module teardown entry point called
    // unqualified from VulkanContext.cpp through the VulkanTransientResources.h
    // umbrella, and it collides with nothing, so it stays at OloEngine scope
    // (declared in the umbrella, defined next to the tracking state it reads).
    void TrackLive(const void* object, const char* what);
    void UntrackLive(const void* object);

    // Kept in sync with VulkanDevice.cpp's / VulkanContext.cpp's
    // anonymous-namespace copies (trivially small).
    void VkCheck(VkResult result, const char* what);

    // Non-dispatchable Vulkan handles are pointers on 64-bit builds and
    // u64 on 32-bit ones — normalise either shape into the registry's u64.
    template<typename T>
    [[nodiscard]] u64 VkHandleToU64(T handle)
    {
        if constexpr (std::is_pointer_v<T>)
        {
            return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
        }
        else
        {
            return static_cast<u64>(handle);
        }
    }

    // ImageFormat -> VkFormat. Every ImageFormat member is covered; the
    // -Wswitch-with-no-default convention (ADR 0011) makes a new member a
    // compile warning here rather than a silent VK_FORMAT_UNDEFINED.
    //
    // Widening choices (documented, deliberate):
    //  - RGB8 / RGB32F widen to their RGBA siblings: 3-channel formats
    //    have no mandated optimal-tiling sampled/attachment support.
    //  - DEPTH24STENCIL8 maps to D32_SFLOAT_S8_UINT, not D24_UNORM_S8_UINT:
    //    Vulkan mandates support for at least one of the two and AMD ships
    //    only the D32 variant, so this is the portable pick (more depth
    //    precision, same aspects). Format-feature querying is not implemented.
    //  - `srgb` is honoured only where the engine defines it (8-bit color
    //    and BC7), matching TextureSpecification::SRGB's contract.
    [[nodiscard]] VkFormat ImageFormatToVkFormat(ImageFormat format, bool srgb);

    // Bytes per pixel of the ENGINE format's client data — what a caller
    // hands SetData (matches the GL twin's upload contract, so RGB8 is 3,
    // not the widened image's 4). 0 = no client-upload path (depth,
    // compressed, unknown).
    [[nodiscard]] u32 EngineFormatClientBpp(ImageFormat format);

    // Expand tightly-packed 3-channel rows into the widened 4-channel
    // layout the VkImage actually has (opaque alpha).
    [[nodiscard]] std::vector<u8> ExpandRgbToRgba(ImageFormat format, const void* data, u64 pixelCount);

    // One VkImageMemoryBarrier2 over a mip/layer range of a color image —
    // upload-path plumbing (the graph's barriers go through
    // VulkanBarrierLowering, not this). The layer pair defaults to the
    // single-layer shape every 2D upload uses; the cubemap face paths
    // (#691) pass a face index.
    void RecordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, u32 baseMip, u32 mipCount,
                            u32 baseLayer = 0u, u32 layerCount = 1u);

    // "Is a Vulkan frame recording?" probe (#691, extracted from ten
    // per-site copies — PR #794 review). Returns the live VulkanRendererAPI
    // exactly when BOTH hold: (1) the process RendererAPI is the Vulkan one,
    // and (2) a frame command buffer is currently recording
    // (CurrentCommandBuffer() != VK_NULL_HANDLE); nullptr otherwise.
    // Probed off the LIVE object, not RendererAPI::GetAPI(): a fixture can
    // set the static flag without recreating the process API (amendment
    // (39)'s construction-order gap), and a static_cast through the wrong
    // object was an access violation the first time a device-gated test
    // ran this path.
    [[nodiscard]] VulkanRendererAPI* TryGetRecordingVulkanAPI();

    // The recording-agnostic half of the probe: the live VulkanRendererAPI
    // whenever the process RendererAPI is the Vulkan one (recording or not),
    // else nullptr — for callers that must reach API-side state outside any
    // frame bracket (~VulkanFramebuffer's NotifyFramebufferDestroyed).
    [[nodiscard]] VulkanRendererAPI* TryGetVulkanAPI();
} // namespace OloEngine::VulkanUpload

#endif // OLO_WITH_VULKAN
