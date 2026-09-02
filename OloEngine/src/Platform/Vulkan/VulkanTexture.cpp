#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTexture.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <glm/gtc/packing.hpp>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // #809, see VulkanTexture2D::GetHostImageCopyUploadCount. Atomic
        // because texture creation is not guaranteed to stay on one thread.
        std::atomic<u64> s_HostImageCopyUploadCount{ 0 };

        [[nodiscard]] bool IsDepthImageFormat(ImageFormat format)
        {
            return format == ImageFormat::DEPTH24STENCIL8;
        }

        [[nodiscard]] bool IsSrgbVkFormat(VkFormat format)
        {
            return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_BC7_SRGB_BLOCK;
        }

        [[nodiscard]] VkSampleCountFlagBits SampleCountFromU32(u32 samples)
        {
            switch (samples)
            {
                case 0:
                case 1:
                    return VK_SAMPLE_COUNT_1_BIT;
                case 2:
                    return VK_SAMPLE_COUNT_2_BIT;
                case 4:
                    return VK_SAMPLE_COUNT_4_BIT;
                case 8:
                    return VK_SAMPLE_COUNT_8_BIT;
                case 16:
                    return VK_SAMPLE_COUNT_16_BIT;
                case 32:
                    return VK_SAMPLE_COUNT_32_BIT;
                case 64:
                    return VK_SAMPLE_COUNT_64_BIT;
                default:
                    OLO_CORE_WARN("VulkanTexture2D: unsupported sample count {} — falling back to 1", samples);
                    return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        // Mirrors OpenGLTexture2D::CalculateFullMipCount.
        [[nodiscard]] u32 CalculateFullMipCount(u32 width, u32 height)
        {
            return static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height))))) + 1;
        }

        // Mirrors the GL twin's mip derivation, including the spec mutations:
        // multisampling forces a single level, an explicit MipLevels wins,
        // GenerateMips selects the full chain, otherwise 1.
        [[nodiscard]] u32 DeriveMipLevels(TextureSpecification& spec, u32 width, u32 height)
        {
            if (spec.Samples > 1u)
            {
                spec.GenerateMips = false;
                spec.MipLevels = 1u;
                return 1u;
            }
            if (spec.MipLevels > 0u)
            {
                // Clamp an authored level count to the full chain the extents
                // support — Vulkan refuses mipLevels beyond
                // floor(log2(max(w,h)))+1 (VUID-VkImageCreateInfo-mipLevels-02255),
                // where GL silently tolerated the over-ask. Resize re-derives
                // through this same path, so a persisted over-large spec stays
                // bounded there too.
                return std::min(spec.MipLevels, CalculateFullMipCount(width, height));
            }
            if (spec.GenerateMips)
            {
                return CalculateFullMipCount(width, height);
            }
            return 1u;
        }

        // Bytes per pixel of the resolved VkFormat's texel (the widened
        // 4-channel form for the 3-channel engine formats).
        [[nodiscard]] u32 VkFormatTexelBytes(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::RGB8:
                    return 4; // widened to RGBA8
                case ImageFormat::RGB32F:
                    return 16; // widened to RGBA32F
                default:
                    return VulkanUpload::EngineFormatClientBpp(format);
            }
        }

        // The GL facade's SetData/SubImage contract for the half-float
        // formats hands the backend f32 PER CHANNEL and lets the driver
        // convert down (OpenGLTexture.cpp: "Upload as GL_FLOAT ... OpenGL
        // converts to half-float"). vkCmdCopyBufferToImage copies bytes
        // verbatim, so the conversion happens here instead — the call sites
        // are backend-neutral and ship f32 by that contract.
        // SlugFontProcessor's RGBA16F curve texture was the first to hit the
        // mismatch: the whole-texture size assert fired the moment a scene
        // with a UI text component played under Vulkan (#691).
        [[nodiscard]] bool EngineFormatClientIsF32ToHalf(ImageFormat format)
        {
            return format == ImageFormat::RGBA16F || format == ImageFormat::RG16F;
        }

        [[nodiscard]] std::vector<u16> PackF32ClientToHalf(const void* data, u64 floatCount)
        {
            std::vector<u16> halves(floatCount);
            const auto* src = static_cast<const f32*>(data);
            for (u64 i = 0; i < floatCount; ++i)
            {
                halves[i] = glm::packHalf1x16(src[i]);
            }
            return halves;
        }
    } // namespace

    VulkanTexture2D::VulkanTexture2D(const TextureSpecification& specification, bool renderTargetOnly)
        // Vulkan refuses a zero extent outright (GL merely misbehaved), so
        // clamp defensively — pre-Resize 0-sized framebuffer specs reach here.
        : m_Specification(specification),
          m_Width(std::max(specification.Width, 1u)),
          m_Height(std::max(specification.Height, 1u)),
          m_RenderTargetOnly(renderTargetOnly)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanTexture2D requires a live VulkanDevice");

        // Mirror the GL twin: block-compressed formats have no population path
        // through the spec ctor — they MUST come via the CompressedTextureImage
        // overload (not yet implemented for Vulkan). Refuse loudly but non-fatally.
        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("VulkanTexture2D: block-compressed format {} cannot be created from a "
                           "TextureSpecification — the CompressedTextureImage overload is not implemented",
                           static_cast<u32>(m_Specification.Format));
            m_IsLoaded = false;
            return;
        }

        m_Specification.Samples = std::max(m_Specification.Samples, 1u);
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        CreateImage();
        m_IsLoaded = true;
        VulkanUpload::TrackLive(this, "VulkanTexture2D(spec)");
    }

    VulkanTexture2D::VulkanTexture2D(const std::string& path, bool srgb, const std::string& identityPath)
    {
        VulkanUpload::TrackLive(this, "VulkanTexture2D(path)");
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanTexture2D requires a live VulkanDevice");

        int width = 0;
        int height = 0;
        int channels = 0;
        // Thread-local flip, exactly like the GL twin: asset BYTES must be
        // identical across backends (UV sampling is convention-free, so the
        // same bytes give the same result).
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - VulkanTexture2D::VulkanTexture2D(const std::string&)");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }
        ::stbi_set_flip_vertically_on_load_thread(0);

        if (data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTexture2D: failed to load image '{}'", path);
            m_Width = 1;
            m_Height = 1;
            m_IsLoaded = false;
            return;
        }

        m_Specification.SRGB = srgb;
        Invalidate(identityPath.empty() ? path : identityPath, static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(channels));
        ::stbi_image_free(data);
    }

    VulkanTexture2D::~VulkanTexture2D()
    {
        VulkanUpload::UntrackLive(this);
        // Retire the identity first (outstanding handles go stale), then hand
        // the native object to the deferred queue — NEVER vmaDestroyImage
        // inline, prior frames may still be executing. Destructors must not
        // let an exception escape (the reclaim enqueue can allocate): a
        // failed enqueue leaks one image until process exit, which beats
        // std::terminate.
        try
        {
            // The engine heap's views of this texture retire FIRST — the
            // amendment (22) correction: destruction is RetireResource
            // (poison + generation advance), never InvalidateResource (whose
            // re-acquire is for storage that was replaced, not destroyed).
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            ReleaseImage();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanTexture2D: release failed ({}) — leaking the image until process exit", e.what());
        }
        catch (...)
        {
            // See ~VulkanStorageBuffer: `catch (const std::exception&)` alone
            // still lets a non-std throw escape and terminate. One policy
            // across every Vulkan resource destructor (#803).
            OLO_CORE_ERROR("~VulkanTexture2D: release failed (unknown exception) — leaking the image until process "
                           "exit");
        }
    }

    void VulkanTexture2D::CreateImage()
    {
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture2D::CreateImage requires a live VulkanDevice");
        if (device == nullptr)
        {
            // The assert compiles out in Release; the factory arm guards this
            // path, but a Resize on a device that has since shut down must
            // fail loudly rather than dereference null.
            throw std::runtime_error("VulkanTexture2D::CreateImage: no live VulkanDevice");
        }

        const VkFormat format = VulkanUpload::ImageFormatToVkFormat(m_Specification.Format, m_Specification.SRGB);
        const bool isDepth = IsDepthImageFormat(m_Specification.Format);

        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (isDepth)
        {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        else
        {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            // STORAGE only where the format-feature contract allows it without
            // a per-format query: sRGB image views don't support
            // storage writes, and multisampled storage images need an opt-in
            // device feature.
            if (!IsSrgbVkFormat(format) && m_Specification.Samples == 1u)
            {
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
        }

        // #809: VK_IMAGE_USAGE_HOST_TRANSFER_BIT is what lets UploadPixels
        // write this image straight from host memory (SubImage deliberately
        // stays on the staging path — it is a partial update with a mid-frame
        // arm of its own, and #809 scopes the host route to the load-time
        // upload). Three
        // independent gates, all required
        // (VUID-VkImageCreateInfo-usage-10245): the device enabled
        // hostImageCopy, THIS format advertises the host-transfer feature at
        // optimal tiling, and the image is a single-sampled colour image
        // (there is no client-data upload path for depth, and a multisampled
        // image cannot be host-copied). The bit is harmless when the host
        // path is never taken.
        //
        // A fourth gate is m_RenderTargetOnly, and it is not cosmetic: the
        // driver may pick different memory type requirements for an image
        // carrying this bit (NVIDIA reports identicalMemoryTypeRequirements =
        // VK_FALSE, measured on an RTX 4090), so putting it on every
        // framebuffer attachment would charge every render target — on every
        // resize — for a route an attachment can never take. Gating on the
        // driver property instead was tried and is wrong: it disables the
        // whole feature on exactly the hardware it was built for.
        m_HostTransferUsage = !isDepth && !m_RenderTargetOnly && m_Specification.Samples == 1u &&
                              device->SupportsHostImageCopyForFormat(format);
        if (m_HostTransferUsage)
        {
            usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { m_Width, m_Height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 1u;
        imageInfo.samples = SampleCountFromU32(m_Specification.Samples);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VulkanUpload::VkCheck(vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr),
                              "vmaCreateImage (VulkanTexture2D)");
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTexture2D");

        // HasStencil follows the RESOLVED VkFormat: the engine's combined
        // depth format lowers to D32_SFLOAT_S8_UINT, which carries a stencil
        // aspect — the orchestrator derives barrier aspect masks from this.
        VulkanImageInfoRegistry::Get().Register(m_Image, VulkanImageInfo{
                                                             .Format = format,
                                                             .Width = m_Width,
                                                             .Height = m_Height,
                                                             .MipLevels = m_MipLevels,
                                                             .ArrayLayers = 1u,
                                                             // The one texture class that can be multisampled.
                                                             .Samples = std::max(m_Specification.Samples, 1u),
                                                             .HasDepth = isDepth,
                                                             .HasStencil = isDepth,
                                                         });

        m_RHIHandle.Sync(RHI::ResourceKind::Texture, VulkanUpload::VkHandleToU64(m_Image), RHI::Backend::Vulkan);
    }

    void VulkanTexture2D::ReleaseImage()
    {
        // Resource destruction runs on the render thread with no region open
        // (amendment (92) rule 7), so no GetOrCreateAttachmentView can be
        // racing this; the exchange just keeps the handle's accesses atomic.
        if (const VkImageView view = m_AttachmentView.exchange(VK_NULL_HANDLE, std::memory_order_acq_rel);
            view != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(view);
        }
        if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            // The info-registry entry retires inside the reclaim queue at
            // actual-destroy time, not here.
            VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    VkImageView VulkanTexture2D::GetOrCreateAttachmentView()
    {
        // Double-checked (#806, amendment (92) rule 8): every draw against
        // this texture asks for the view, possibly from several recording
        // threads at once, and after the first call the answer is a cached
        // handle — so the fast path is one acquire load, and only the
        // creating call takes the lock and re-checks under it.
        if (const VkImageView cached = m_AttachmentView.load(std::memory_order_acquire); cached != VK_NULL_HANDLE)
        {
            return cached;
        }
        std::lock_guard<std::mutex> lock(m_AttachmentViewMutex);
        if (const VkImageView cached = m_AttachmentView.load(std::memory_order_relaxed); cached != VK_NULL_HANDLE)
        {
            return cached; // another thread created it while this one waited
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        if (info == nullptr)
        {
            return VK_NULL_HANDLE;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info->Format;
        // An attachment view renders into mip 0 / layer 0; a combined
        // depth-stencil format carries BOTH aspects (legal for the depth
        // attachment and required if the stencil half is ever attached).
        viewInfo.subresourceRange.aspectMask =
            info->HasDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (info->HasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u))
                           : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D: attachment view creation failed");
            return VK_NULL_HANDLE;
        }
        // Release pairs with the fast path's acquire: a thread that sees the
        // handle also sees the finished view object behind it.
        m_AttachmentView.store(view, std::memory_order_release);
        return view;
    }

    void VulkanTexture2D::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u)
        {
            OLO_CORE_WARN("VulkanTexture2D::Resize: ignoring zero extent {}x{}", width, height);
            return;
        }
        if (width == m_Width && height == m_Height && m_Image != VK_NULL_HANDLE)
        {
            return;
        }

        ReleaseImage();

        m_Width = width;
        m_Height = height;
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        // Sync inside CreateImage PRESERVES the identity — same object, new
        // storage, matching the GL twin's recreate-in-place semantics.
        CreateImage();

        // Storage replaced, object lives: the engine heap's views re-describe
        // against the new image (amendment (22) — a reload must PUSH; the
        // view's generation is unchanged, so OffsetOf cannot detect this).
        RHI::DescriptorHeap::Get().InvalidateResource(m_RHIHandle.Get());
    }

    u64 VulkanTexture2D::GetHostImageCopyUploadCount()
    {
        return s_HostImageCopyUploadCount.load(std::memory_order_relaxed);
    }

    void VulkanTexture2D::RecordMipChain(VkCommandBuffer cmd, VkFilter blitFilter) const
    {
        // Classic blit chain: mip N-1 (TRANSFER_SRC) → mip N (TRANSFER_DST),
        // then N becomes the next source. Extracted from UploadPixels so the
        // staging and host-image-copy routes share ONE barrier sequence —
        // the two differ only in how mip 0 got its pixels, and a second copy
        // of this ladder is the two-mirrors shape that drifts.
        //
        // Precondition (see the header): mip 0 is already TRANSFER_SRC.
        i32 srcW = static_cast<i32>(m_Width);
        i32 srcH = static_cast<i32>(m_Height);
        for (u32 mip = 1; mip < m_MipLevels; ++mip)
        {
            const i32 dstW = std::max(srcW / 2, 1);
            const i32 dstH = std::max(srcH / 2, 1);

            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 0u, 1u };
            blit.srcOffsets[1] = { srcW, srcH, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, 1u };
            blit.dstOffsets[1] = { dstW, dstH, 1 };
            vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, blitFilter);

            // This mip is the next iteration's source. The last mip is never
            // read, so it stays in TRANSFER_DST for the final transition.
            if (mip + 1u < m_MipLevels)
            {
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, mip, 1u);
            }

            srcW = dstW;
            srcH = dstH;
        }

        // Mips [0, N-1) sit in TRANSFER_SRC, the last in TRANSFER_DST — bring
        // all to SHADER_READ_ONLY, the backend's steady state for sampled
        // content (GetData and SubImage both name it as their oldLayout).
        VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels - 1u);
        VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         VK_ACCESS_2_MEMORY_READ_BIT, m_MipLevels - 1u, 1u);
    }

    bool VulkanTexture2D::UploadPixelsFromHost(const void* data, u64 sizeBytes, VkFilter blitFilter)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr || !device->IsHostImageCopyEnabled() || m_Image == VK_NULL_HANDLE)
        {
            return false;
        }

        // vkCopyMemoryToImage reads width*height*texelBytes straight out of
        // the CALLER's allocation, so a payload short of the full extent is a
        // host heap over-read here — where the staging path would only have
        // produced an over-read of a VMA buffer and a VUID. Decline rather
        // than read. Zero means "no client-upload path for this format"
        // (depth, compressed), which also declines.
        const u32 texelBytes = VkFormatTexelBytes(m_Specification.Format);
        if (texelBytes == 0u || sizeBytes < static_cast<u64>(m_Width) * m_Height * texelBytes)
        {
            return false;
        }

        // VK_IMAGE_LAYOUT_GENERAL is the ONE layout the spec guarantees in
        // both host-copy layout lists, so it is what the copy targets. Every
        // other layout is asked for, never assumed — the lists are a driver
        // property, not a constant.
        constexpr VkImageLayout kCopyLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!device->IsHostCopyDstLayoutSupported(kCopyLayout))
        {
            return false;
        }
        // The upload MUST end in SHADER_READ_ONLY_OPTIMAL: that is the
        // backend's steady state for sampled content, and both GetData and
        // SubImage name it as their barrier's oldLayout. Leaving the image in
        // GENERAL instead would still sample correctly and would silently
        // break those two, so a driver that cannot make that host transition
        // declines the whole route rather than weakening the invariant.
        if (!device->IsHostTransitionTargetSupported(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        {
            return false;
        }

        const VkDevice vkDevice = device->GetDevice();
        const bool generateMips = m_MipLevels > 1u;

        // Whole image → GENERAL. oldLayout UNDEFINED is a full discard, which
        // is exactly right here: mip 0 is about to be completely overwritten
        // and every other mip regenerated from it.
        VkHostImageLayoutTransitionInfo toCopy{};
        toCopy.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
        toCopy.image = m_Image;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toCopy.newLayout = kCopyLayout;
        toCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
        if (vkTransitionImageLayout(vkDevice, 1u, &toCopy) != VK_SUCCESS)
        {
            OLO_CORE_WARN("VulkanTexture2D::UploadPixels: host layout transition failed — falling back to staging");
            return false;
        }

        // memoryRowLength / memoryImageHeight 0 = "tightly packed to
        // imageExtent", the same contract the staging path's VkBufferImageCopy
        // relies on. The caller has already widened 3-channel client data to
        // the image's 4-channel form, so `data` matches the VkImage's format.
        VkMemoryToImageCopy region{};
        region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
        region.pHostPointer = data;
        region.memoryRowLength = 0u;
        region.memoryImageHeight = 0u;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        region.imageExtent = { m_Width, m_Height, 1u };

        VkCopyMemoryToImageInfo copyInfo{};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
        copyInfo.dstImage = m_Image;
        copyInfo.dstImageLayout = kCopyLayout;
        copyInfo.regionCount = 1u;
        copyInfo.pRegions = &region;
        if (vkCopyMemoryToImage(vkDevice, &copyInfo) != VK_SUCCESS)
        {
            OLO_CORE_WARN("VulkanTexture2D::UploadPixels: host image copy failed — falling back to staging");
            return false;
        }

        if (!generateMips)
        {
            // The whole upload happened on the host: no staging buffer, no
            // command buffer, no submit, and therefore nothing to order
            // against the frame that may be recording around this call.
            VkHostImageLayoutTransitionInfo toSampled{};
            toSampled.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
            toSampled.image = m_Image;
            toSampled.oldLayout = kCopyLayout;
            toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toSampled.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
            if (vkTransitionImageLayout(vkDevice, 1u, &toSampled) != VK_SUCCESS)
            {
                OLO_CORE_WARN("VulkanTexture2D::UploadPixels: host transition to SHADER_READ_ONLY failed — "
                              "falling back to staging");
                return false;
            }
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            s_HostImageCopyUploadCount.fetch_add(1u, std::memory_order_relaxed);
            return true;
        }

        // Mip generation is a BLIT, which has no host form — only the base
        // level moved off the command stream. The one-shot submit stays, and
        // with it amendment (72)'s ordering caveat, for mipped textures.
        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::UploadPixels(host)",
            [&](VkCommandBuffer cmd)
            {
                // Mip 0 carries the host-written pixels and must be
                // PRESERVED: src scope is the host write, which the queue
                // submit's implicit host-write ordering already makes
                // available — naming it explicitly is what keeps sync
                // validation able to see the dependency.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, kCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0u, 1u);
                // Mips 1..N-1 hold nothing worth keeping — UNDEFINED discards
                // whatever the GENERAL transition left there.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                 VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 1u, m_MipLevels - 1u);
                RecordMipChain(cmd, blitFilter);
            });

        if (ok)
        {
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            s_HostImageCopyUploadCount.fetch_add(1u, std::memory_order_relaxed);
        }
        return ok;
    }

    bool VulkanTexture2D::UploadPixels(const void* data, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || sizeBytes == 0)
        {
            return false;
        }
        if (m_Specification.Samples > 1u)
        {
            OLO_CORE_ERROR("VulkanTexture2D::UploadPixels: multisampled textures have no upload path");
            return false;
        }

        // The VkImage may be the widened 4-channel form of a 3-channel engine
        // format — expand CPU-side when so.
        const void* uploadData = data;
        u64 uploadSize = sizeBytes;
        std::vector<u8> expanded;
        if (m_Specification.Format == ImageFormat::RGB8 || m_Specification.Format == ImageFormat::RGB32F)
        {
            expanded = VulkanUpload::ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(m_Width) * m_Height);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        const bool generateMips = m_MipLevels > 1u;
        const VkFilter blitFilter = IsIntegerFormat(m_Specification.Format) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

        // #809: the host-image-copy route first. It writes mip 0 with no
        // staging buffer at all, and for a texture with no mip chain it
        // finishes the upload without ever touching the command stream —
        // which is what takes an asset load out of the one-shot-submit
        // ordering hazard (amendment (72): a one-shot submit is ordered
        // BEFORE the frame still recording around it) rather than managing
        // it. A false return means the route was unavailable or failed, and
        // the staging path below then runs exactly as it did before —
        // including its oldLayout UNDEFINED full discard, which is valid
        // from whatever state a partial host attempt left behind.
        //
        // "Load-time" is enforced here, not assumed, and it takes TWO checks
        // because vkTransitionImageLayout / vkCopyMemoryToImage run on the
        // CPU the moment they are called — no queue ordering, no fence:
        //
        //  - No frame may be RECORDING. The staging path's one-shot submit is
        //    at least ordered against the queue; a host write to an image the
        //    frame being recorded will sample is not ordered against anything.
        //    Same guard SubImage already applies to its mid-frame arm.
        //  - This must be the image's FIRST upload. A re-upload targets an
        //    image previously handed to the renderer, which an ALREADY
        //    SUBMITTED frame may still be reading — and "no frame is
        //    recording" says nothing about frames in flight. The registry's
        //    InitialLayout is exactly that record: UNDEFINED until an upload
        //    publishes content, so it is still UNDEFINED only for a freshly
        //    created (or freshly Resize()d) image. This is what keeps the
        //    per-frame re-uploaders — VideoTexture::UpdateFrame — on the
        //    command path where they belong. (OceanFFTField::Upload was the
        //    other one until issue #969 moved the cascade fields onto
        //    Texture2DArray::SetLayerData; the same reasoning applies there.)
        const auto* priorInfo = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const bool firstUpload = priorInfo == nullptr || priorInfo->InitialLayout == VK_IMAGE_LAYOUT_UNDEFINED;
        const bool frameRecording = VulkanUpload::TryGetRecordingVulkanAPI() != nullptr;
        if (m_HostTransferUsage && firstUpload && !frameRecording &&
            UploadPixelsFromHost(uploadData, uploadSize, blitFilter))
        {
            return true;
        }

        // Host staging buffer — destroyed right after the blocking one-shot.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D::UploadPixels: staging allocation failed ({} bytes)", uploadSize);
            return false;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::UploadPixels",
            [&](VkCommandBuffer cmd)
            {
                // Whole image (every mip) → TRANSFER_DST. oldLayout UNDEFINED
                // is deliberate: this is a FULL overwrite, discard is free.
                // dst scope includes BLIT: the mip chain's blits write mips
                // this transition covers, and a transition visible only to
                // COPY leaves those writes unordered (sync validation caught
                // exactly this — WAW between the transition and vkCmdBlitImage).
                // src scope is non-empty even though oldLayout is UNDEFINED:
                // SetData re-uploads reach this on an ALREADY-written image
                // (hot reload), and an empty src scope leaves those earlier
                // writes unordered against the transition (the same
                // WRITE_AFTER_WRITE shape sync validation caught on the
                // cubemap chain). Discard semantics are unchanged.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                region.imageExtent = { m_Width, m_Height, 1u };
                vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

                if (generateMips)
                {
                    // RecordMipChain's precondition: mip 0 in TRANSFER_SRC
                    // holding the base image, every other mip in TRANSFER_DST.
                    // Mip 0's last write was the COPY above; the rest are
                    // already TRANSFER_DST from the whole-image transition.
                    VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                     VK_ACCESS_2_TRANSFER_READ_BIT, 0u, 1u);
                    RecordMipChain(cmd, blitFilter);
                }
                else
                {
                    VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                     VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels);
                }
            });

        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);

        if (ok)
        {
            // Seed the layout tracker's first sight of this image (see
            // VulkanImageInfo::InitialLayout) — without this, the graph's
            // first barrier would transition from UNDEFINED and could
            // legally discard the pixels just uploaded.
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return ok;
    }

    void VulkanTexture2D::SetData(void* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        const u32 nativeBpp = VulkanUpload::EngineFormatClientBpp(m_Specification.Format);
        if (nativeBpp == 0)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SetData: format {} has no client-upload path",
                           static_cast<u32>(m_Specification.Format));
            return;
        }
        // Half-float formats take f32 per channel from the caller (the GL
        // facade contract — see EngineFormatClientIsF32ToHalf) — the client
        // payload is twice the native image size.
        const bool f32Client = EngineFormatClientIsF32ToHalf(m_Specification.Format);
        const u32 clientBpp = f32Client ? nativeBpp * 2u : nativeBpp;
        const u64 expected = static_cast<u64>(m_Width) * m_Height * clientBpp;
        OLO_CORE_ASSERT(size == expected, "VulkanTexture2D::SetData: size must cover the whole texture");
        if (size != expected)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SetData: got {} bytes, expected {} — dropping the upload", size,
                           expected);
            return;
        }

        if (f32Client)
        {
            const std::vector<u16> halves = PackF32ClientToHalf(data, expected / sizeof(f32));
            m_IsLoaded = UploadPixels(halves.data(), halves.size() * sizeof(u16)) || m_IsLoaded;
            return;
        }
        m_IsLoaded = UploadPixels(data, size) || m_IsLoaded;
    }

    void VulkanTexture2D::RegenerateMips()
    {
        OLO_PROFILE_FUNCTION();

        if (m_MipLevels <= 1u || m_Image == VK_NULL_HANDLE)
        {
            return;
        }

        const VkFilter blitFilter = IsIntegerFormat(m_Specification.Format) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::RegenerateMips",
            [&](VkCommandBuffer cmd)
            {
                // Unlike the upload paths, level 0's producer here is whatever the
                // CALLER just did to it — a compute imageStore or a transfer copy —
                // not a staging copy recorded in this command buffer. So the source
                // scope has to name both possibilities, and level 0's old layout
                // must be PRESERVED rather than discarded: it holds the only copy of
                // the data the whole chain is derived from. Passing UNDEFINED here,
                // as the full-overwrite paths legitimately do, would let the driver
                // discard the very texels being propagated.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_GENERAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, 0u, 1u);
                // Mips 1..N hold stale content that is about to be overwritten in
                // full, so discarding them is free and correct.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 1u, m_MipLevels - 1u);
                RecordMipChain(cmd, blitFilter);
            });

        if (ok)
        {
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        else
        {
            OLO_CORE_ERROR("VulkanTexture2D::RegenerateMips: mip chain submit failed");
        }
    }

    void VulkanTexture2D::SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        const u32 nativeBpp = VulkanUpload::EngineFormatClientBpp(m_Specification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || nativeBpp == 0)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: no upload path (device/image/format)");
            return;
        }
        // Reject the origin first, then compare sizes against the REMAINING
        // extent — `x + width > m_Width` wraps on huge x and lets an
        // out-of-range region through (review finding).
        if (x >= m_Width || y >= m_Height || width > m_Width - x || height > m_Height - y)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: region {}x{}+{}+{} exceeds {}x{}", width, height, x, y,
                           m_Width, m_Height);
            return;
        }
        // Same client contract as SetData: half-float formats arrive as f32
        // per channel and convert here (before this, a 16F region upload
        // passed the too-small size check and reinterpreted the f32 bits as
        // halves — silent garbage rather than an assert).
        const bool f32Client = EngineFormatClientIsF32ToHalf(m_Specification.Format);
        const u32 clientBpp = f32Client ? nativeBpp * 2u : nativeBpp;
        const u64 expected = static_cast<u64>(width) * height * clientBpp;
        if (dataSize < expected)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: got {} bytes, region needs {}", dataSize, expected);
            return;
        }
        std::vector<u16> halfPayload;
        if (f32Client)
        {
            halfPayload = PackF32ClientToHalf(data, expected / sizeof(f32));
            data = halfPayload.data();
            dataSize = static_cast<u32>(halfPayload.size() * sizeof(u16));
        }

        // Mid-frame (#691): a region flush between two GPU uses (the
        // terrain sculpt/paint shape) must be ORDERED within the frame
        // command buffer — the one-shot below submits BEFORE the
        // still-recording frame and also diverges from the API's layout
        // tracker. Route through the facade's staged frame-CB upload, which
        // owns both. The one-shot arm below stays for load time (no
        // recording), where it is correct and the tracker learns the layout
        // through InitialLayout.
        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            // The 3-channel engine formats live in WIDENED 4-channel
            // images (there is no linear-filterable RGB8/RGB32F on the
            // Vulkan floor) — widen the payload FIRST and hand the staged
            // path the image's own 4-channel format. Passing the raw RGB
            // format matched neither the image nor any conversion pair
            // downstream, so the mid-frame region upload silently dropped
            // (review finding, #691).
            std::vector<u8> widened;
            const void* stagedData = data;
            const RHI::Format clientFormat = [&]
            {
                switch (m_Specification.Format)
                {
                    case ImageFormat::R8:
                        return RHI::Format::R8UNorm;
                    case ImageFormat::RGB8:
                        widened = VulkanUpload::ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(width) * height);
                        stagedData = widened.data();
                        return RHI::Format::RGBA8UNorm;
                    case ImageFormat::RGBA8:
                        return RHI::Format::RGBA8UNorm;
                    case ImageFormat::R32F:
                        return RHI::Format::R32Float;
                    case ImageFormat::RG32F:
                        return RHI::Format::RG32Float;
                    case ImageFormat::RGB32F:
                        widened = VulkanUpload::ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(width) * height);
                        stagedData = widened.data();
                        return RHI::Format::RGBA32Float;
                    case ImageFormat::RGBA32F:
                        return RHI::Format::RGBA32Float;
                    // The payload for these two was converted to native
                    // halves above, so the staged upload sees the image's
                    // own format — no further conversion downstream.
                    case ImageFormat::RG16F:
                        return RHI::Format::RG16Float;
                    case ImageFormat::RGBA16F:
                        return RHI::Format::RGBA16Float;
                    default:
                        return RHI::Format::Unknown;
                }
            }();
            if (clientFormat != RHI::Format::Unknown)
            {
                vk->UploadTextureSubImage2D(m_RHIHandle.Get(), static_cast<i32>(x), static_cast<i32>(y), width,
                                            height, clientFormat, stagedData);
                return;
            }
            // An unmapped format falls through to the one-shot with the
            // known previous-frame-ordering caveat — loud, not silent.
            OLO_CORE_WARN("VulkanTexture2D::SubImage: mid-frame upload of unmapped format {} takes the "
                          "one-shot path (ordered BEFORE this frame's GPU work)",
                          static_cast<u32>(m_Specification.Format));
        }

        // Native byte count for the staging copy — differs from `expected`
        // exactly when the client payload was f32-to-half converted above.
        const u64 nativeExpected = static_cast<u64>(width) * height * nativeBpp;
        const void* uploadData = data;
        u64 uploadSize = nativeExpected;
        std::vector<u8> expanded;
        if (m_Specification.Format == ImageFormat::RGB8 || m_Specification.Format == ImageFormat::RGB32F)
        {
            expanded = VulkanUpload::ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(width) * height);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: staging allocation failed ({} bytes)", uploadSize);
            return;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        // PARTIAL update: the untouched texels must survive, so oldLayout is
        // the steady-state SHADER_READ_ONLY every upload leaves the image in
        // — never UNDEFINED (a legal discard of the rest). Backend invariant:
        // sampled asset textures are not graph-written, so outside graph
        // execution they sit in SHADER_READ_ONLY.
        //
        // Read it back rather than hardcoding it: a texture created but never
        // uploaded is still UNDEFINED, and naming SHADER_READ_ONLY as the
        // oldLayout there is invalid usage (VUID-VkImageMemoryBarrier2-oldLayout-01197).
        // Discarding is harmless in that case — there are no prior texels to keep.
        const auto* imageInfo = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = imageInfo != nullptr ? imageInfo->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool ok = VulkanOneShot::Submit("VulkanTexture2D::SubImage",
                                              [&](VkCommandBuffer cmd)
                                              {
                                                  VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout,
                                                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                                                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u,
                                                                                   1u);

                                                  VkBufferImageCopy region{};
                                                  region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                                                  region.imageOffset = { static_cast<i32>(x), static_cast<i32>(y), 0 };
                                                  region.imageExtent = { width, height, 1u };
                                                  vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                         1u, &region);

                                                  VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                                                                   0u, 1u);
                                              });

        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
        if (ok)
        {
            // Only on success: recording a layout the image never reached is
            // exactly the wrong-oldLayout hazard InitialLayout exists to stop.
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    void VulkanTexture2D::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u || data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTexture2D::Invalidate: bad arguments ({}x{}, data {})", width, height,
                           static_cast<const void*>(data));
            return;
        }

        // Mirror the GL twin's channel handling: 3/4-channel data honours the
        // sRGB flag, 1/2-channel data is linear.
        ImageFormat format;
        switch (channels)
        {
            case 1:
                format = ImageFormat::R8;
                break;
            case 2:
                format = ImageFormat::RG8;
                break;
            case 3:
                format = ImageFormat::RGB8;
                break;
            case 4:
                format = ImageFormat::RGBA8;
                break;
            default:
                OLO_CORE_ERROR("VulkanTexture2D::Invalidate: unsupported channel count {}", channels);
                return;
        }

        ReleaseImage();

        m_Path = std::string(path);
        m_Width = width;
        m_Height = height;
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_Specification.Format = format;
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        // Sync inside CreateImage PRESERVES identity — in-place reload, the
        // amendment (12) contract.
        CreateImage();

        const u64 sizeBytes = static_cast<u64>(width) * height * channels;
        m_IsLoaded = UploadPixels(data, sizeBytes);

        // Same contract as Resize: storage replaced, identity preserved —
        // push the re-describe (amendment (22)).
        RHI::DescriptorHeap::Get().InvalidateResource(m_RHIHandle.Get());
    }

    void VulkanTexture2D::Bind(u32 slot) const
    {
        // #691: forward like the 3D/array/cube classes always did —
        // this stub was the one dead end in the family, silently dropping
        // every ShaderResourceRegistry-routed bind (shader-graph materials,
        // ShadowMap's raw views, wind/snow fields, video textures).
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    bool VulkanTexture2D::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        OLO_PROFILE_FUNCTION();

        outData.clear();
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || mipLevel >= m_MipLevels)
        {
            return false;
        }
        const u32 texelBytes = VkFormatTexelBytes(m_Specification.Format);
        if (texelBytes == 0 || m_Specification.Samples > 1u)
        {
            return false;
        }

        const u32 mipW = std::max(m_Width >> mipLevel, 1u);
        const u32 mipH = std::max(m_Height >> mipLevel, 1u);
        const u64 sizeBytes = static_cast<u64>(mipW) * mipH * texelBytes;

        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = sizeBytes;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            return false;
        }

        // The image's ACTUAL prior layout, not the steady-state assumption.
        // Sampled asset content usually does sit in SHADER_READ_ONLY between
        // graph executions, but two cases break the assumption and both are
        // invalid usage (VUID-VkImageMemoryBarrier2-oldLayout-01197) that also
        // leaves the copied texels undefined: a texture created and never
        // uploaded is still UNDEFINED, and an ATTACHMENT sits in whatever
        // layout the graph left it in.
        //
        // Prefer the tracker's EXECUTED layout, not its recorded one — this is
        // a one-shot, so it runs BEFORE the still-recording frame command
        // buffer whose transitions the recorded layout already reflects (#800,
        // the same reasoning as VulkanRendererAPI::ReadTextureSubImage's borrow
        // mode). Deliberately NO RegisterImage first: re-registering with
        // extents or a registration id the graph did not use RESETS that
        // image's rows, and wiping the graph's own tracked layouts to learn one
        // value would be a far worse trade. An image the tracker has never seen
        // answers UNDEFINED, which is exactly when the registry's load-time
        // record is the better answer.
        const auto* imageInfo = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout registryLayout =
            imageInfo != nullptr ? imageInfo->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1u, 0u, 1u };
        auto* vk = VulkanUpload::TryGetVulkanAPI();
        VkImageLayout priorLayout = registryLayout;
        if (vk != nullptr)
        {
            if (const VkImageLayout tracked = vk->LayoutTracker().CurrentExecutedLayout(m_Image, range);
                tracked != VK_IMAGE_LAYOUT_UNDEFINED)
            {
                priorLayout = tracked;
            }
        }

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::GetData",
            [&](VkCommandBuffer cmd)
            {
                VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, mipLevel, 1u);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0u, 1u };
                region.imageExtent = { mipW, mipH, 1u };
                vkCmdCopyImageToBuffer(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);

                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_READ_BIT, mipLevel, 1u);

                // Settle recorded INSIDE the one-shot's immediate-execution
                // scope, so it advances the executed layout only once the
                // buffer really reached the queue (#800) — the same shape as
                // VulkanRendererAPI::ReadTextureSubImage's non-borrow arm.
                if (vk != nullptr)
                {
                    vk->LayoutTracker().SetLayout(m_Image, range, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            });

        if (ok)
        {
            // The chain ended in SHADER_READ_ONLY, so an image that entered
            // UNDEFINED has genuinely reached it. Record it in the registry
            // too: the tracker SetLayout above no-ops for an image the tracker
            // never registered, and leaving a stale UNDEFINED behind would let
            // the next barrier legally discard the texels. Only on success —
            // the same gate as every other layout write in this backend.
            //
            // ONLY when this read covered the whole image. InitialLayout is a
            // WHOLE-IMAGE field and the barriers above name `mipLevel` alone,
            // so stamping it after a single-mip read of a mipped texture would
            // claim mips 1..N reached SHADER_READ_ONLY when they are still
            // UNDEFINED — manufacturing the very desync this change removes.
            // The per-subresource truth is the tracker's, set inside the
            // callback above.
            if (m_MipLevels == 1u)
            {
                VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, sizeBytes);
            outData.resize(sizeBytes);
            std::memcpy(outData.data(), readbackOut.pMappedData, sizeBytes);
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
