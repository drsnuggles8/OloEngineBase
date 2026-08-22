#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTexture.h — VulkanTexture2D, the VMA-backed Texture2D backend twin of
// OpenGLTexture (#691; split out of the single VulkanTransientResources.h).
// Real uploads (staged into the frame command buffer, one-shot
// outside a bracket), mip generation, readbacks on the ReadTextureSubImage
// spine, sampler-state registry metadata.
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

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanTexture2D — attribute-only VMA image for the TransientPool.
    //
    // Fully implemented: allocation, identity, metadata, Resize. Upload /
    // bind / readback virtuals are warn-once no-ops for now.
    // -------------------------------------------------------------------------
    class VulkanTexture2D : public Texture2D
    {
      public:
        explicit VulkanTexture2D(const TextureSpecification& specification);
        // File load (#691): stbi with the SAME thread-local vertical
        // flip the GL twin uses — asset bytes must be identical across
        // backends, since UV sampling is convention-free.
        VulkanTexture2D(const std::string& path, bool srgb, const std::string& identityPath = "");
        ~VulkanTexture2D() override;

        const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        [[nodiscard("Store this!")]] u32 GetWidth() const override
        {
            return m_Width;
        }
        [[nodiscard("Store this!")]] u32 GetHeight() const override
        {
            return m_Height;
        }
        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard("Store this!")]] const std::string& GetPath() const override
        {
            return m_Path;
        }

        // Real upload/readback paths (#691): one-shot staged copies,
        // mip generation via a blit chain, final layout SHADER_READ_ONLY
        // recorded through VulkanImageInfoRegistry::SetInitialLayout.
        void SetData(void* data, u32 size) override;
        void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        // Bind is meaningless on this backend (heap-bindless): warn-once no-op.
        void Bind(u32 slot) const override;
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

        [[nodiscard("Store this!")]] bool IsLoaded() const override
        {
            return m_IsLoaded;
        }

        [[nodiscard("Use for transparency")]] bool HasAlphaChannel() const override
        {
            return m_Specification.Format == ImageFormat::RGBA8 ||
                   m_Specification.Format == ImageFormat::RGBA16F ||
                   m_Specification.Format == ImageFormat::RGBA32F ||
                   m_Specification.Format == ImageFormat::BC7;
        }

        [[nodiscard("Store this!")]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }

        // Recreates the VMA image at the new size (old image goes through
        // VulkanDeferredReclaim). Identity is PRESERVED via m_RHIHandle.Sync,
        // matching the GL twin's recreate-in-place semantics.
        void Resize(u32 width, u32 height) override;

        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        // Lazily-created whole-image VkImageView for dynamic-rendering
        // attachment use (the ONE place view objects still exist on this
        // backend — sampled use goes through descriptor-heap view
        // DESCRIPTIONS). Cached; released with the image (Resize mints a new
        // one). VK_NULL_HANDLE on failure.
        [[nodiscard]] VkImageView GetOrCreateAttachmentView();

      private:
        void CreateImage();
        void ReleaseImage();
        // Full-image base-level upload + optional blit-chain mip generation,
        // leaving every mip in SHADER_READ_ONLY_OPTIMAL. `data` is tightly
        // packed rows in the spec's format.
        bool UploadPixels(const void* data, u64 sizeBytes);

        TextureSpecification m_Specification;
        std::string m_Path; // set by the file ctor; empty for transient/spec textures
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_MipLevels = 1;
        bool m_IsLoaded = false;

        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        VkImageView m_AttachmentView = VK_NULL_HANDLE; ///< See GetOrCreateAttachmentView.
        // Generation-checked identity for m_Image, kept in lockstep by
        // m_RHIHandle.Sync at every site that assigns the native handle —
        // same pattern as the GL twin (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
