#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTextureCubemap.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    namespace
    {
        void WarnCubemapCpuPathOnce(const char* what)
        {
            static std::unordered_set<std::string> s_Warned;
            if (s_Warned.insert(what).second)
            {
                OLO_CORE_WARN("[RHI/Vulkan] TextureCubemap::{} is not implemented (#691: the IBL bake "
                              "path is GPU-side capture work) — no-op",
                              what);
            }
        }
    } // namespace

    VulkanTextureCubemap::VulkanTextureCubemap(const CubemapSpecification& spec)
        : m_CubemapSpecification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTextureCubemap requires a live VulkanDevice");

        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_Specification.Format = spec.Format;

        // Clamp an authored MipLevels to the chain the extent supports (the
        // DeriveMipLevels rule): an over-large count from a stale IBL cache
        // is a vkCreateImage failure, not a request (#691).
        const u32 fullChain = 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height)))));
        m_MipLevels = 1u;
        if (spec.MipLevels > 0u)
        {
            m_MipLevels = std::min(spec.MipLevels, fullChain);
        }
        else if (spec.GenerateMips)
        {
            m_MipLevels = fullChain;
        }

        const VkFormat format = VulkanUpload::ImageFormatToVkFormat(spec.Format, false);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        // The one flag that makes a 6-layer 2D image addressable as a cube.
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 6u;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap: image creation failed ({}x{}, {} mips)", width, height, m_MipLevels);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTextureCubemap");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = format;
        registryInfo.Width = width;
        registryInfo.Height = height;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = 6u;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_CUBE;
        // §4f sampler table: cubemaps are CLAMP_TO_EDGE on GL.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTextureCubemap::~VulkanTextureCubemap()
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
            OLO_CORE_ERROR("VulkanTextureCubemap dtor: reclaim enqueue threw ({}); the cubemap leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap dtor: reclaim enqueue threw; the cubemap leaks");
        }
    }

    void VulkanTextureCubemap::Bind(u32 slot) const
    {
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTextureCubemap::SetData(void* /*data*/, u32 /*size*/)
    {
        WarnCubemapCpuPathOnce("SetData");
    }

    void VulkanTextureCubemap::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                          const void* /*data*/, u32 /*channels*/)
    {
        WarnCubemapCpuPathOnce("Invalidate");
    }

    void VulkanTextureCubemap::SetFaceData(u32 faceIndex, void* data, u32 size)
    {
        // GL contract (OpenGLTextureCubemap::SetFaceData): mip-0 face upload
        // plus a full mip regeneration when the spec asks for mips.
        if (!SetFaceDataMip(faceIndex, 0u, data, size))
        {
            return;
        }
        if (m_CubemapSpecification.GenerateMips && m_MipLevels > 1u)
        {
            GenerateMipmaps();
        }
    }

    bool VulkanTextureCubemap::SetFaceDataMip(u32 faceIndex, u32 mipLevel, void* data, u32 size)
    {
        // #691: the cubemap CPU face upload — six layers of the
        // VulkanTexture2D staging shape. This is the IBL cache's load path
        // and the reflection-probe baker's face write, i.e. most of what
        // stood between the flat grey sky and a lit environment.
        auto* device = VulkanDevice::Get();
        const u32 clientBpp = VulkanUpload::EngineFormatClientBpp(m_CubemapSpecification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || clientBpp == 0u || faceIndex >= 6u ||
            mipLevel >= m_MipLevels)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: no upload path (face {}, mip {}/{})", faceIndex,
                           mipLevel, m_MipLevels);
            return false;
        }
        const u32 mipWidth = std::max(m_CubemapSpecification.Width >> mipLevel, 1u);
        const u32 mipHeight = std::max(m_CubemapSpecification.Height >> mipLevel, 1u);
        const u64 expected = static_cast<u64>(mipWidth) * mipHeight * clientBpp;
        if (size < expected)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: got {} bytes, face mip needs {}", size, expected);
            return false;
        }

        const void* uploadData = data;
        u64 uploadSize = expected;
        std::vector<u8> expanded;
        if (m_CubemapSpecification.Format == ImageFormat::RGB8 || m_CubemapSpecification.Format == ImageFormat::RGB32F)
        {
            expanded = VulkanUpload::ExpandRgbToRgba(m_CubemapSpecification.Format, data, static_cast<u64>(mipWidth) * mipHeight);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        // Mid-frame (the IBL cache load runs inside the frame on this
        // backend): record into the FRAME command buffer through the API's
        // tracker — a one-shot here would submit BEFORE the frame and race
        // the layout tracking (the 1c ordering rule).
        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            return vk->RecordStagedImageUpload(m_Image, mipLevel, faceIndex, mipWidth, mipHeight, uploadData,
                                               uploadSize);
        }

        // Load time (no recording): the blocking one-shot, the
        // VulkanTexture2D::UploadPixels shape with the face as the layer.
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
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: staging allocation failed ({} bytes)", uploadSize);
            return false;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool ok = VulkanOneShot::Submit(
            "VulkanTextureCubemap::SetFaceDataMip",
            [&](VkCommandBuffer cmd)
            {
                // Whole image through the transition, not just this face:
                // partial-face-uploaded cubes must keep a UNIFORM tracked
                // layout or CurrentLayout answers UNDEFINED (a legal discard)
                // for whole-image queries — the mixed-layout trap.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 priorLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE
                                                                                          : VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                                                 6u);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, faceIndex, 1u };
                region.imageExtent = { mipWidth, mipHeight, 1u };
                vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels, 0u, 6u);
            });
        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
        if (ok)
        {
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return ok;
    }

    void VulkanTextureCubemap::GenerateMipmaps() const
    {
        // Mip chain by blit, all six faces per level in ONE blit (the
        // subresource layerCount carries the fan-out). Runs in the frame
        // command buffer when one is live (transfer ops are legal outside the
        // rendering scope), else as a one-shot.
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || m_MipLevels <= 1u)
        {
            return;
        }

        const auto recordChain = [this](VkCommandBuffer cmd, const VkImageLayout priorLayout)
        {
            // src access stays MEMORY_WRITE even when priorLayout is
            // UNDEFINED: for this image UNDEFINED can mean "mixed after
            // per-face copies" (SetFaceData collapses a partially-uploaded
            // layout to UNDEFINED), so the face uploads' TRANSFER_WRITEs may
            // still be in flight — an empty src scope is the WRITE_AFTER_WRITE
            // sync-validation hazard the live editor hit. A transition FROM
            // UNDEFINED with a non-empty src scope is legal; it only orders
            // the prior writes, the contents are discarded either way.
            VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                             VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                                             6u);
            u32 srcWidth = m_CubemapSpecification.Width;
            u32 srcHeight = m_CubemapSpecification.Height;
            for (u32 mip = 1u; mip < m_MipLevels; ++mip)
            {
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                 VK_ACCESS_2_TRANSFER_READ_BIT, mip - 1u, 1u, 0u, 6u);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 0u, 6u };
                blit.srcOffsets[1] = { static_cast<i32>(std::max(srcWidth, 1u)),
                                       static_cast<i32>(std::max(srcHeight, 1u)), 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, 6u };
                blit.dstOffsets[1] = { static_cast<i32>(std::max(srcWidth >> 1u, 1u)),
                                       static_cast<i32>(std::max(srcHeight >> 1u, 1u)), 1 };
                // Integer formats reject LINEAR blits — same guard as the 2D
                // mip path (review finding; latent, cubemaps are HDR color
                // today).
                vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit,
                               IsIntegerFormat(m_CubemapSpecification.Format) ? VK_FILTER_NEAREST
                                                                              : VK_FILTER_LINEAR);
                srcWidth = std::max(srcWidth >> 1u, 1u);
                srcHeight = std::max(srcHeight >> 1u, 1u);
            }
            // Unify: mips [0, N-1) sit in TRANSFER_SRC, the last in DST.
            VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                             VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels - 1u, 0u, 6u);
            VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             VK_ACCESS_2_MEMORY_READ_BIT, m_MipLevels - 1u, 1u, 0u, 6u);
        };

        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            // In-frame: the tracker must agree with the chain's transitions.
            // The chain works in whole-subresource strokes, so drive it with
            // the tracker's whole-image answer and settle everything to
            // SHADER_READ_ONLY afterwards.
            auto& tracker = vk->LayoutTracker();
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            tracker.RegisterImage(m_Image, m_MipLevels, 6u, info != nullptr ? info->RegistrationId : 0u,
                                  info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED);
            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 6u };
            const VkImageLayout prior = tracker.CurrentLayout(m_Image, whole);
            recordChain(vk->CurrentCommandBuffer(), prior);
            tracker.SetLayout(m_Image, whole, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        else
        {
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            const VkImageLayout prior = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
            // Only on success — see VulkanTexture2DArray::GenerateMipmaps for
            // the contract: a failed submit leaves the image in `prior`, and
            // recording the new layout anyway is the wrong-oldLayout desync.
            if (!VulkanOneShot::Submit("VulkanTextureCubemap::GenerateMipmaps",
                                       [&](VkCommandBuffer cmd)
                                       { recordChain(cmd, prior); }))
            {
                OLO_CORE_ERROR("VulkanTextureCubemap::GenerateMipmaps: one-shot submit failed — tracked layout "
                               "left unchanged");
                return;
            }
        }
        VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    bool VulkanTextureCubemap::ReadFaces(u32 baseFace, u32 faceCount, u32 mipLevel, std::vector<u8>& outData,
                                         const char* what) const
    {
        outData.clear();
        auto* device = VulkanDevice::Get();
        const u32 clientBpp = VulkanUpload::EngineFormatClientBpp(m_CubemapSpecification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || clientBpp == 0u || faceCount == 0u || baseFace >= 6u ||
            faceCount > 6u - baseFace || mipLevel >= m_MipLevels)
        {
            return false;
        }
        // Mid-frame: the faces' content may still sit unsubmitted in the
        // frame command buffer — the StorageBuffer::GetData rule. Flush; a
        // refusal falls back to previous-frame data with the 1c warn-once.
        if (VulkanUpload::TryGetRecordingVulkanAPI() != nullptr)
        {
            auto* context = VulkanContext::Get();
            if (context == nullptr || !context->FlushFrameRecordingAndWait())
            {
                static bool s_Warned = false;
                if (!s_Warned)
                {
                    s_Warned = true;
                    OLO_CORE_WARN("[Vulkan] mid-frame cubemap GetFaceData without a frame flush — the readback "
                                  "may return stale contents");
                }
            }
        }

        const u32 mipWidth = std::max(m_CubemapSpecification.Width >> mipLevel, 1u);
        const u32 mipHeight = std::max(m_CubemapSpecification.Height >> mipLevel, 1u);
        // The engine face format is what GL hands back (RGB stays RGB); the
        // backend stores RGB widened to RGBA, so read RGBA and narrow.
        const bool widened =
            m_CubemapSpecification.Format == ImageFormat::RGB8 || m_CubemapSpecification.Format == ImageFormat::RGB32F;
        const u32 storedBpp = widened ? (clientBpp / 3u) * 4u : clientBpp;
        const u64 faceStoredSize = static_cast<u64>(mipWidth) * mipHeight * storedBpp;
        const u64 storedSize = faceStoredSize * faceCount;

        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = storedSize;
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

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        if (priorLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            // Nothing was ever uploaded/rendered — a read would be garbage.
            vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
            return false;
        }
        const bool ok = VulkanOneShot::Submit(
            what,
            [&](VkCommandBuffer cmd)
            {
                VulkanUpload::RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0u, m_MipLevels, 0u,
                                                 6u);
                // One buffer-image-copy region per face, packed contiguously
                // in face order — the whole read is ONE submit regardless of
                // how many faces the caller asked for.
                std::vector<VkBufferImageCopy> regions(faceCount);
                for (u32 i = 0; i < faceCount; ++i)
                {
                    regions[i] = VkBufferImageCopy{};
                    regions[i].bufferOffset = faceStoredSize * i;
                    regions[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, baseFace + i, 1u };
                    regions[i].imageExtent = { mipWidth, mipHeight, 1u };
                }
                vkCmdCopyImageToBuffer(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, faceCount,
                                       regions.data());
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, priorLayout,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels,
                                                 0u, 6u);
            });
        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, storedSize);
            const auto* stored = static_cast<const u8*>(readbackOut.pMappedData);
            if (widened)
            {
                // Narrow RGBA back to the RGB the caller's format promises —
                // the faces are packed back to back, so one pass over every
                // texel covers them all.
                const u64 texels = static_cast<u64>(mipWidth) * mipHeight * faceCount;
                outData.resize(texels * clientBpp);
                for (u64 i = 0; i < texels; ++i)
                {
                    std::memcpy(outData.data() + i * clientBpp, stored + i * storedBpp,
                                static_cast<sizet>(clientBpp));
                }
            }
            else
            {
                outData.assign(stored, stored + storedSize);
            }
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok;
    }

    bool VulkanTextureCubemap::GetFaceData(u32 faceIndex, std::vector<u8>& outData, u32 mipLevel) const
    {
        return ReadFaces(faceIndex, 1u, mipLevel, outData, "VulkanTextureCubemap::GetFaceData");
    }

    bool VulkanTextureCubemap::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        // GL contract (OpenGLTextureCubemap::GetData): all six faces
        // contiguous in face order. ONE flush (if mid-frame) + ONE one-shot
        // submit reads all six via ReadFaces (#691, PR #794 review —
        // the old shape looped GetFaceData six times, paying the flush +
        // blocking submit + readback-buffer create/destroy round per face).
        return ReadFaces(0u, 6u, mipLevel, outData, "VulkanTextureCubemap::GetData");
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
