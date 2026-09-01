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
    // Fully implemented: allocation, identity, metadata, Resize, staged and
    // host-copy uploads, blit-chain mip generation, readback and bind. No
    // no-op virtuals remain on this class.
    // -------------------------------------------------------------------------
    class VulkanTexture2D : public Texture2D
    {
      public:
        // `renderTargetOnly` marks a texture created purely as a framebuffer
        // attachment — content arrives from the GPU, `SetData` is never
        // called. Backend-internal, so it stays a constructor argument rather
        // than a TextureSpecification field: the neutral spec is shared with
        // the GL arm, which has no use for the distinction. Its only effect
        // is to keep VK_IMAGE_USAGE_HOST_TRANSFER_BIT off images that can
        // never take the host-upload route (#809 — see CreateImage).
        explicit VulkanTexture2D(const TextureSpecification& specification, bool renderTargetOnly = false);
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
        void RegenerateMips() override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        // Forwards to the facade's BindTexture, so ShaderResourceRegistry-
        // routed binds resolve on this backend too (they were silently dropped
        // while this was a stub).
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

        // #809: how many uploads have completed through the host-image-copy
        // route since process start. Diagnostic only — but it is the ONLY
        // thing that distinguishes the host route from the staging one from
        // outside, because both produce byte-identical pixels in the same
        // final layout. A capability flag says the route is PERMITTED; this
        // says it was TAKEN, which is what the tests need to assert on.
        [[nodiscard]] static u64 GetHostImageCopyUploadCount();

      private:
        void CreateImage();
        void ReleaseImage();
        // Full-image base-level upload + optional blit-chain mip generation,
        // leaving every mip in SHADER_READ_ONLY_OPTIMAL. `data` is tightly
        // packed rows in the spec's format.
        bool UploadPixels(const void* data, u64 sizeBytes);
        // #809: the host-image-copy arm of UploadPixels. Writes mip 0 straight
        // from `data` with vkCopyMemoryToImage — no staging buffer — and then
        // either finishes host-side (no mip chain: no queue submit at all) or
        // hands an image whose mip 0 already holds the pixels to the shared
        // blit chain below. Returns false when the host route could not be
        // taken or failed, and the caller falls back to staging; a false
        // return leaves the image in a discardable state, never a
        // half-described one.
        bool UploadPixelsFromHost(const void* data, u64 sizeBytes, VkFilter blitFilter);
        // Records the mip-generation blit chain into `cmd`. PRECONDITION:
        // mip 0 holds the base image in TRANSFER_SRC_OPTIMAL and mips
        // 1..N-1 are in TRANSFER_DST_OPTIMAL (contents irrelevant).
        // POSTCONDITION: every mip is in SHADER_READ_ONLY_OPTIMAL. Shared by
        // the staging and host paths so the two cannot drift in their barrier
        // scopes — the half of the upload the host route still needs a queue for.
        void RecordMipChain(VkCommandBuffer cmd, VkFilter blitFilter) const;

        TextureSpecification m_Specification;
        std::string m_Path; // set by the file ctor; empty for transient/spec textures
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_MipLevels = 1;
        bool m_IsLoaded = false;
        // #809: set by CreateImage when the image was actually created with
        // VK_IMAGE_USAGE_HOST_TRANSFER_BIT. Not the same question as
        // VulkanDevice::IsHostImageCopyEnabled — the usage bit is also gated
        // per format and per image kind — and it must be read off the IMAGE
        // rather than re-derived, because a host copy into an image that
        // lacks the usage bit is invalid usage, not a slow path.
        bool m_HostTransferUsage = false;
        // See the constructor: true for framebuffer attachments, which never
        // take a client-data upload and so must not carry the usage bit.
        bool m_RenderTargetOnly = false;

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
