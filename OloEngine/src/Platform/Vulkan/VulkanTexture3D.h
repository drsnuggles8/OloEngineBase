#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTexture3D.h — the VMA-backed Texture3D backend twin of OpenGLTexture3D
// (#691; split out of the single VulkanTransientResources.h).
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
#include "OloEngine/Renderer/Texture3D.h"

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanTexture3D — the Texture3D backend twin (issue #691
    // Froxel-fog volumes, 3D noise fields). Sampled (sampler3D) +
    // storage (image3D) usage in one image; registers
    // VK_IMAGE_VIEW_TYPE_3D in VulkanImageInfoRegistry so both bind paths
    // build 3D views instead of the 2D default.
    // -------------------------------------------------------------------------
    class VulkanTexture3D : public Texture3D
    {
      public:
        explicit VulkanTexture3D(const Texture3DSpecification& spec);
        ~VulkanTexture3D() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Specification.Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] u32 GetDepth() const override
        {
            return m_Specification.Depth;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture3DSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetData(const void* data, u32 size) override;

      private:
        Texture3DSpecification m_Specification;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
