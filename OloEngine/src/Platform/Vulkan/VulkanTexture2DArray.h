#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTexture2DArray.h — the VMA-backed Texture2DArray backend twin of
// OpenGLTexture2DArray (#691; split out of the single
// VulkanTransientResources.h in Phase 9).
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

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Texture2DArray.h"

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanTexture2DArray — the Texture2DArray backend twin (issue #691
    // Phase 7 Wave B). First consumer: ShadowMap's CSM/atlas placeholder
    // (sampler2DArrayShadow), which VolumetricFogPass::Execute materialises
    // lazily — under --rhi=vulkan the old GL-only factory constructed an
    // OpenGLTexture2DArray whose glCreateTextures call went through a null
    // glad pointer in any GL-context-free process (an access violation, found
    // by VulkanPassSuiteTest's fog tenant in an isolated run). Registers
    // VK_IMAGE_VIEW_TYPE_2D_ARRAY so both bind paths build array views
    // (sampler2DArrayShadow needs the array dimensionality, not the 2D
    // default). Allocation/identity/lifetime are full; the upload/mip
    // virtuals are warn-once no-ops until the Wave C shadow work needs them.
    // -------------------------------------------------------------------------
    class VulkanTexture2DArray : public Texture2DArray
    {
      public:
        explicit VulkanTexture2DArray(const Texture2DArraySpecification& spec);
        ~VulkanTexture2DArray() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Specification.Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] u32 GetLayers() const override
        {
            return m_Specification.Layers;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture2DArraySpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetLayerData(u32 layer, const void* data, u32 width, u32 height) override;
        void GenerateMipmaps() override;

      private:
        Texture2DArraySpecification m_Specification;
        u32 m_MipLevels = 1;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
