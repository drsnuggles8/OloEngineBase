#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanImageInfoRegistry.h — the image-info/layout-seed registry the barrier
// translator reads: VkImage → creation attributes, sampler state and initial
// layout (#691; split out of the single VulkanTransientResources.h in
// No GL twin, the handle-side metadata GL keeps on the object).
//
// This header exposes Vulkan types directly — it is included only by
// Platform/Vulkan siblings and by OLO_WITH_VULKAN-guarded engine factory TUs
// (the sanctioned factory-include pattern, rhi-abstraction-boundary.md).
// =============================================================================

// VulkanDevice.h provides <volk.h> and <vk_mem_alloc.h> (with the
// VMA_STATIC/DYNAMIC_VULKAN_FUNCTIONS config that must stay in sync with
// VulkanMemoryAllocator.cpp) — do NOT include either directly here, and NEVER
// <vulkan/vulkan.h> (volk owns the function pointers, ADR 0011 amendment 41a).
#include "Platform/Vulkan/VulkanDevice.h"

#include <unordered_map>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanImageInfoRegistry — backend-internal VkImage metadata side table.
    //
    // Maps a live VkImage to the creation attributes a barrier emitter cannot
    // recover from the handle alone. CONSUMER: the orchestrator's
    // VulkanRendererAPI reads this to derive VkImageAspectFlags for barriers
    // (color vs depth|stencil) and to seed its image-layout tracking; keep the
    // API to Register/Lookup/Unregister — it is a lookup table, not a manager.
    //
    // Thread-safety: NONE, deliberately. Registration happens in resource
    // constructors and unregistration inside VulkanDeferredReclaim's destroy
    // pass — all render-thread work, matching the rest of the Vulkan backend.
    // -------------------------------------------------------------------------
    struct VulkanImageInfo
    {
        VkFormat Format = VK_FORMAT_UNDEFINED;
        // Mip-0 extent (#691). GetTextureDimensions is the consumer:
        // GL answers it from glGetTextureLevelParameteriv, and the handle
        // alone cannot recover an extent here. 0 = "registered before this
        // field existed / extent unknown" and the facade reports the query
        // unanswerable rather than guessing.
        u32 Width = 0;
        u32 Height = 0;
        u32 MipLevels = 1;
        u32 ArrayLayers = 1;
        // For a 3D image this is the mip-0 DEPTH (slice count), not a layer
        // count — VkImageCreateInfo puts the two in different fields and the
        // copy/readback paths address them differently. `ViewType` below is
        // what tells them apart.
        //
        // Samples > 1 marks a multisample image. Recorded because a consumer
        // that reproduces this storage has to match it (VkImageCreateInfo's
        // samples participates in copy compatibility) and because the handle
        // alone cannot recover it — the same reason Format and the extent are
        // here. GL's twin reads GL_TEXTURE_SAMPLES off the object.
        u32 Samples = 1;
        bool HasDepth = false;
        bool HasStencil = false;
        // The default whole-image sampled-view dimensionality. 2D for every
        // texture/attachment; 3D volumes (Texture3D — froxel fog, noise
        // fields) register VK_IMAGE_VIEW_TYPE_3D so the bind paths build the
        // right view instead of hardcoding 2D (issue #691).
        VkImageViewType ViewType = VK_IMAGE_VIEW_TYPE_2D;

        // Stamped by Register() from a process-wide monotonic counter. A
        // destroyed VkImage's handle VALUE can be recycled by the driver for
        // a later image with identical extents; layout trackers key on the
        // handle and would otherwise inherit the dead image's layouts (a
        // wrong oldLayout, i.e. a validation error at best). Comparing this
        // stamp is how a tracker detects "same handle value, different
        // image" — the same recycled-name lesson as RHI handle generations.
        u64 RegistrationId = 0;

        // The layout the image sits in BEFORE the graph's layout tracker
        // first sees it. UNDEFINED for attachment/storage images (first use
        // discards); SHADER_READ_ONLY_OPTIMAL for content uploaded by a
        // load-time one-shot (#691) — without this, the tracker's
        // first barrier would use oldLayout = UNDEFINED and LEGALLY discard
        // the uploaded pixels. Updated via SetInitialLayout, which does NOT
        // bump RegistrationId (the image is the same image).
        VkImageLayout InitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // The image's OWN sampler state (#691) — GL keeps filter and
        // wrap on the texture object, so the "inherit" half of the sampler
        // contract (rhi-abstraction-boundary.md §4f: no stated intent means
        // the object's state, parity by construction) needs somewhere
        // API-neutral callers can't see to carry it. Registered with the
        // per-class GL defaults (§4f's table: Texture2D/attachments REPEAT,
        // arrays and cubes CLAMP_TO_EDGE, depth arrays CLAMP_TO_BORDER
        // opaque-white) and mutated by SetTextureFilter / SetTextureWrap.
        // BindTexture derives the effective VkSamplerCreateInfo from these at
        // bind time; integer formats are forced to NEAREST there, not here.
        VkFilter MinFilter = VK_FILTER_LINEAR;
        VkFilter MagFilter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode MipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode AddressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkBorderColor BorderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    };

    class VulkanImageInfoRegistry
    {
      public:
        // Process-wide instance. Deliberately leaked (never destroyed), same
        // rationale as RHI::ResourceRegistry: a Ref<Texture> released during
        // static destruction must still find a live registry.
        [[nodiscard]] static VulkanImageInfoRegistry& Get();

        void Register(VkImage image, const VulkanImageInfo& info);
        // Returns nullptr when the image was never registered (or already
        // unregistered). The pointer is invalidated by the next Register or
        // Unregister — copy out, don't hold.
        [[nodiscard]] const VulkanImageInfo* Lookup(VkImage image) const;
        void Unregister(VkImage image);

        // Record the layout an upload left the image in (see
        // VulkanImageInfo::InitialLayout). No-op for an unregistered image.
        // Only affects trackers that have NOT yet registered the image —
        // a tracker already following it keeps its own (fresher) state.
        void SetInitialLayout(VkImage image, VkImageLayout layout);

        // #691: the SetTextureFilter / SetTextureWrap facade entries
        // mutate the recorded per-image sampler state (see the sampler-state
        // fields above). No-op for an unregistered image; takes effect at the
        // next BindTexture (the sampler slot is derived at bind time).
        void SetSamplerFilter(VkImage image, VkFilter minFilter, VkFilter magFilter);
        void SetSamplerAddressMode(VkImage image, VkSamplerAddressMode mode);

      private:
        VulkanImageInfoRegistry() = default;

        std::unordered_map<VkImage, VulkanImageInfo> m_Infos;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
