#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTextureCubemapArray.h — the VMA-backed TextureCubemapArray backend
// twin of OpenGLTextureCubemapArray (#691; split out of the single
// VulkanTransientResources.h).
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
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanTextureCubemapArray — a 6*Layers-layer 2D image with a CUBE_ARRAY
    // view type (#691).
    //
    // Brought up for the reflection-probe arrays (issue #705's radiance /
    // distance-field arrays): ReflectionProbeArray::Init creates two of these
    // eagerly, so without this class the factory's assert wedged the first
    // --rhi=vulkan editor launch during init — the same
    // backend-blind-factory shape as amendment (64). Scope matches
    // VulkanTextureCubemap's: real image, real identity (binds / barriers /
    // layout tracking all work), with the CPU upload and GPU layer-copy
    // halves warn-once no-ops until the cubemap-upload work lands.
    // -------------------------------------------------------------------------
    class VulkanTextureCubemapArray : public TextureCubemapArray
    {
      public:
        explicit VulkanTextureCubemapArray(const CubemapArraySpecification& spec);
        ~VulkanTextureCubemapArray() override;

        [[nodiscard]] const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] u32 GetWidth() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const std::string& GetPath() const override
        {
            return m_Path;
        }
        [[nodiscard]] bool IsLoaded() const override
        {
            return m_Image != VK_NULL_HANDLE;
        }
        [[nodiscard]] bool HasAlphaChannel() const override
        {
            return true;
        }
        [[nodiscard]] const CubemapArraySpecification& GetArraySpecification() const override
        {
            return m_ArraySpecification;
        }
        [[nodiscard]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetData(void* data, u32 size) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        bool SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes) override;
        bool CopyLayerFromCubemap(u32 layer, const TextureCubemap& source) override;
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

      private:
        TextureSpecification m_Specification;
        CubemapArraySpecification m_ArraySpecification;
        std::string m_Path;
        u32 m_MipLevels = 1;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
