#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTexture2DArray.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] VkFormat Texture2DArrayFormatToVk(const Texture2DArrayFormat format)
        {
            switch (format)
            {
                case Texture2DArrayFormat::DEPTH_COMPONENT32F:
                    return VK_FORMAT_D32_SFLOAT;
                case Texture2DArrayFormat::RGBA8:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Texture2DArrayFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Texture2DArrayFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case Texture2DArrayFormat::RGBA32UI:
                    return VK_FORMAT_R32G32B32A32_UINT;
                case Texture2DArrayFormat::BC7:
                    // Linear variant, matching the GL twin's
                    // GL_COMPRESSED_RGBA_BPTC_UNORM.
                    return VK_FORMAT_BC7_UNORM_BLOCK;
            }
            return VK_FORMAT_UNDEFINED;
        }
    } // namespace

    VulkanTexture2DArray::VulkanTexture2DArray(const Texture2DArraySpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture2DArray requires a live VulkanDevice");

        // Mirror the 2D twin (VulkanTexture2D's spec ctor): block-compressed
        // formats have no population path here — a BC image cannot take the
        // colour-attachment usage below, and its GPU-side transcode staging is
        // Not yet implemented for Vulkan. Refuse loudly but non-fatally.
        if (spec.Format == Texture2DArrayFormat::BC7)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray: block-compressed format cannot be created — the "
                           "VT tile-stage copy path is not implemented");
            return;
        }

        const VkFormat format = Texture2DArrayFormatToVk(spec.Format);
        const bool isDepth = spec.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F;
        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);
        m_MipLevels = 1u;
        if (spec.GenerateMipmaps)
        {
            m_MipLevels = 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height)))));
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = std::max(spec.Layers, 1u);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Sampled everywhere (shadow arrays feed sampler2DArrayShadow);
        // depth formats render as layered depth attachments (the CSM/atlas
        // passes), colour formats copy in/out for uploads and debug reads.
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          (isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray: image creation failed ({}x{}x{} layers)", spec.Width, spec.Height,
                           spec.Layers);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTexture2DArray");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = imageInfo.format;
        // The CLAMPED locals, not the raw spec — a zero-sized spec creates a
        // 1x1 image and readback/capture sizing reads this registration.
        registryInfo.Width = width;
        registryInfo.Height = height;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = imageInfo.arrayLayers;
        registryInfo.HasDepth = isDepth;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        // §4f sampler table: colour arrays are CLAMP_TO_EDGE on GL, depth
        // arrays CLAMP_TO_BORDER with an opaque-white border (the
        // out-of-cascade shadow lookup must read "fully lit"). A colour array
        // whose layers are PERIODIC by construction asks for REPEAT instead
        // (the FFT ocean cascades, issue #969) — and must get it on this side
        // too, or the same scene tiles on GL and smears on Vulkan, which is
        // the one-row-order-per-backend class of divergence in
        // docs/agent-rules/rhi-abstraction-boundary.md.
        registryInfo.AddressMode = isDepth                      ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                   : m_Specification.RepeatWrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                                : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTexture2DArray::~VulkanTexture2DArray()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, NOT here: releasing at enqueue
                // time hands the slot to the next texture while in-flight
                // frames still index it (VulkanDescriptorSlotCache.h's
                // recycling contract). VulkanTexture2D already does it this way.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray dtor: reclaim enqueue threw ({}); the array leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray dtor: reclaim enqueue threw; the array leaks");
        }
    }

    void VulkanTexture2DArray::Bind(u32 slot) const
    {
        // The facade's slot path — same acquire the handle form performs.
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTexture2DArray::SetLayerData(u32 layer, const void* data, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        // The GL twin's contract (OpenGLTexture2DArray::SetLayerData): full-
        // layer mip-0 upload, dimensions must match, colour formats only, and
        // — unlike Texture2D — the client data is NATIVE per format (RGBA8 =
        // u8x4, RGBA16F = halves via GL_HALF_FLOAT, RGBA32F = f32). The
        // terrain material's layer albedo/normal arrays are the production
        // caller; this was the last "deferred concern" no-op a real scene hit
        // (#691 — FoliageGenerationTest rendered black terrain).
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: no upload path (device/image/data)");
            return;
        }
        if (layer >= m_Specification.Layers)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: layer {} out of {}", layer, m_Specification.Layers);
            return;
        }
        if (width != m_Specification.Width || height != m_Specification.Height)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: {}x{} must match the array's {}x{}", width, height,
                           m_Specification.Width, m_Specification.Height);
            return;
        }
        u32 bpp = 0;
        switch (m_Specification.Format)
        {
            case Texture2DArrayFormat::RGBA8:
                bpp = 4;
                break;
            case Texture2DArrayFormat::RGBA16F:
                bpp = 8;
                break;
            case Texture2DArrayFormat::RGBA32F:
                bpp = 16;
                break;
            case Texture2DArrayFormat::RGBA32UI:
                bpp = 16;
                break;
            case Texture2DArrayFormat::BC7:
                // Same contract as the GL twin: BC7 layers are populated
                // GPU-side via CopyImageSubDataFull, never a client upload.
                OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: not supported for block-compressed formats");
                return;
            case Texture2DArrayFormat::DEPTH_COMPONENT32F:
            default:
                OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: not supported for depth formats");
                return;
        }
        const u64 uploadSize = static_cast<u64>(width) * height * bpp;

        // Mid-frame: record into the frame command buffer through the API's
        // tracker (the SetFaceDataMip discipline — a one-shot here would
        // submit BEFORE the frame and race the layout tracking).
        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            if (vk->RecordStagedImageUpload(m_Image, 0u, layer, width, height, data, uploadSize))
            {
                return;
            }
        }

        // Load time (no recording): blocking one-shot, the cubemap face shape
        // with the layer as the array index.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::SetLayerData: staging allocation failed ({} bytes)", uploadSize);
            return;
        }
        std::memcpy(stagingOut.pMappedData, data, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        const u32 layerCount = std::max(m_Specification.Layers, 1u);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2DArray::SetLayerData",
            [&](VkCommandBuffer cmd)
            {
                // Whole image through the transition (every mip, every
                // layer): partially-uploaded arrays must keep a UNIFORM
                // tracked layout — the cubemap's mixed-layout lesson.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 // Src scope stays MEMORY_WRITE even from UNDEFINED — prior
                                                 // copy-writes into other layers/mips of this image must be in
                                                 // scope or sync validation flags WRITE_AFTER_WRITE (the
                                                 // cubemap-chain lesson, #691).
                                                 VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                                                 layerCount);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, layer, 1u };
                region.imageExtent = { width, height, 1u };
                vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels, 0u, layerCount);
            });
        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
        if (ok)
        {
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    void VulkanTexture2DArray::GenerateMipmaps()
    {
        // Mip chain by blit, every layer per level in ONE blit (the
        // subresource layerCount carries the fan-out) — the cubemap's
        // GenerateMipmaps with the array's layer count. Depth arrays have no
        // colour blit path and no caller generates mips for them.
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || m_MipLevels <= 1u)
        {
            return;
        }
        if (m_Specification.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::GenerateMipmaps: not supported for depth formats");
            return;
        }
        // Compressed VT caches never mip; a BC image cannot be a blit target,
        // and a UINT format cannot take the VK_FILTER_LINEAR blit below.
        if (m_Specification.Format == Texture2DArrayFormat::BC7 ||
            m_Specification.Format == Texture2DArrayFormat::RGBA32UI)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray::GenerateMipmaps: not supported for block-compressed or "
                           "integer formats");
            return;
        }

        const u32 layerCount = std::max(m_Specification.Layers, 1u);
        const auto record = [&](VkCommandBuffer cmd, VkImageLayout priorLayout)
        {
            VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             // Same MEMORY_WRITE-from-UNDEFINED hardening as above: the
                                             // just-recorded layer copies must be in the src scope.
                                             VK_ACCESS_2_MEMORY_WRITE_BIT,
                                             VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                                             layerCount);

            i32 mipWidth = static_cast<i32>(m_Specification.Width);
            i32 mipHeight = static_cast<i32>(m_Specification.Height);
            for (u32 mip = 1; mip < m_MipLevels; ++mip)
            {
                // Source mip: TRANSFER_DST -> TRANSFER_SRC once its content
                // is final (mip 0 from the uploads, mip N from the previous
                // blit).
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, mip - 1u, 1u, 0u, layerCount);

                const i32 nextWidth = std::max(mipWidth / 2, 1);
                const i32 nextHeight = std::max(mipHeight / 2, 1);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 0u, layerCount };
                blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, layerCount };
                blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
                vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);
                mipWidth = nextWidth;
                mipHeight = nextHeight;

                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_READ_BIT, mip - 1u, 1u, 0u, layerCount);
            }
            // The last mip never became a blit source — settle it directly.
            VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             VK_ACCESS_2_MEMORY_READ_BIT, m_MipLevels - 1u, 1u, 0u, layerCount);
        };

        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            // In-frame: the tracker must agree with the chain's transitions —
            // the cubemap GenerateMipmaps discipline, with the array's layer
            // count in place of the six faces.
            auto& tracker = vk->LayoutTracker();
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            tracker.RegisterImage(m_Image, m_MipLevels, layerCount, info != nullptr ? info->RegistrationId : 0u,
                                  info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED);
            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, layerCount };
            const VkImageLayout prior = tracker.CurrentLayout(m_Image, whole);
            record(vk->CurrentCommandBuffer(), prior);
            tracker.SetLayout(m_Image, whole, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        else
        {
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            const VkImageLayout prior = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
            // Gated on QUEUE ACCEPTANCE, not on the bool. A chain that never
            // reached the queue leaves the image in `prior`, so recording
            // SHADER_READ_ONLY would make the NEXT barrier name an oldLayout
            // the image never reached — the desync InitialLayout exists to
            // prevent (#800's family). But a chain that WAS accepted and only
            // outran the fence wait will still execute, so skipping the record
            // there is the same desync in the other direction. See
            // VulkanOneShot::Outcome.
            VulkanOneShot::Outcome outcome = VulkanOneShot::Outcome::NotSubmitted;
            VulkanOneShot::Submit("VulkanTexture2DArray::GenerateMipmaps", [&](VkCommandBuffer cmd)
                                  { record(cmd, prior); }, &outcome);
            if (outcome == VulkanOneShot::Outcome::NotSubmitted)
            {
                OLO_CORE_ERROR("VulkanTexture2DArray::GenerateMipmaps: the mip chain never reached the queue — "
                               "tracked layout left unchanged");
                return;
            }
        }
        VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
