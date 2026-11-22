#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanAddressCommands.h"
#include "Platform/Vulkan/VulkanTrackedAllocation.h"
#include "Platform/Vulkan/VulkanTexture3D.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        VkFormat Texture3DFormatToVk(const Texture3DFormat format)
        {
            switch (format)
            {
                case Texture3DFormat::RGBA8:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Texture3DFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Texture3DFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case Texture3DFormat::R32F:
                    return VK_FORMAT_R32_SFLOAT;
            }
            return VK_FORMAT_UNDEFINED;
        }

        // Bytes per texel of the CLIENT (SetData) buffer — same values as the
        // OpenGL twin's Texture3DFormatBytesPerPixel (Platform/OpenGL/
        // OpenGLTexture3D.cpp); the two backends must agree on this contract
        // since asset loading is backend-agnostic (VolumeSerializer et al.).
        sizet Texture3DFormatBytesPerTexel(const Texture3DFormat format)
        {
            switch (format)
            {
                case Texture3DFormat::RGBA8:
                    return 4;
                case Texture3DFormat::RGBA16F:
                    return 8;
                case Texture3DFormat::RGBA32F:
                    return 16;
                case Texture3DFormat::R32F:
                    return 4;
            }
            return 0;
        }
    } // namespace

    VulkanTexture3D::VulkanTexture3D(const Texture3DSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture3D requires a live VulkanDevice");

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = Texture3DFormatToVk(spec.Format);
        imageInfo.extent = { std::max(spec.Width, 1u), std::max(spec.Height, 1u), std::max(spec.Depth, 1u) };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Both halves of the volume contract: written as image3D by compute,
        // read as sampler3D by consumers; transfer for clears/debug readback.
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (TrackedVmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture3D: image creation failed ({}x{}x{})", spec.Width, spec.Height,
                           spec.Depth);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        TrackedVmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTexture3D");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = imageInfo.format;
        // Register the CLAMPED extent (imageInfo.extent) — a zero-sized spec
        // creates a 1x1x1 image, and readback/capture sizing reads this.
        registryInfo.Width = imageInfo.extent.width;
        registryInfo.Height = imageInfo.extent.height;
        registryInfo.MipLevels = 1;
        registryInfo.ArrayLayers = 1;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_3D;
        // §4f sampler table: a 3D volume's wrap is the caller's, as on GL
        // (OpenGLTexture3D sets GL_REPEAT or GL_CLAMP_TO_EDGE from the same
        // flag). The froxel, groom and shadow volumes clamp; the cloud noise
        // repeats, and a raymarch that tiles it over kilometres reads one
        // smeared edge texel everywhere past the first tile if this clamps.
        registryInfo.AddressMode =
            spec.Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTexture3D::~VulkanTexture3D()
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
            OLO_CORE_ERROR("VulkanTexture3D dtor: reclaim enqueue threw ({}); the volume leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTexture3D dtor: reclaim enqueue threw; the volume leaks");
        }
    }

    void VulkanTexture3D::Bind(u32 slot) const
    {
        // The facade's slot path — same acquire the handle form performs.
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTexture3D::SetData(const void* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE)
        {
            OLO_CORE_ERROR("VulkanTexture3D::SetData: no live device/image, dropping upload");
            return;
        }

        const sizet bytesPerTexel = Texture3DFormatBytesPerTexel(m_Specification.Format);
        if (bytesPerTexel == 0)
        {
            OLO_CORE_ERROR("VulkanTexture3D::SetData: format {} has no client-upload path",
                           static_cast<u32>(m_Specification.Format));
            return;
        }
        const u64 expected = static_cast<u64>(m_Specification.Width) * m_Specification.Height *
                             m_Specification.Depth * bytesPerTexel;
        if (static_cast<u64>(size) != expected)
        {
            OLO_CORE_ERROR("VulkanTexture3D::SetData: got {} bytes, expected {} ({}x{}x{}) — dropping the upload",
                           size, expected, m_Specification.Width, m_Specification.Height, m_Specification.Depth);
            return;
        }

        // INSIDE A FRAME, a copy recorded on the frame's command buffer (#1594).
        // A coat volume is baked mid-frame, and a one-shot here executes after
        // every frame already submitted and waits for all of them: on the live
        // editor that was a GPU frame or more of CPU stall per bake, so a camera
        // cut's dozens of bakes stalled for seconds whatever their CPU cost.
        // The staged copy ends in the sampled layout (a volume is bound through
        // a persistent heap slot), and the registry is seeded with it, as the
        // one-shot arm below does: the heap resolver serves only a texture at
        // rest in that layout, and a later tracker registers from the registry.
        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            if (vk->RecordStagedImageUpload(m_Image, 0u, 0u, m_Specification.Width, m_Specification.Height, data, size,
                                            m_Specification.Depth, true))
            {
                VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VulkanDevice::Get()->GetSampledImageLayout());
                return;
            }
            // NEVER the one-shot inside a frame: it executes before the frame's
            // command buffer, so an earlier upload to this image recorded in the
            // same frame would copy over it, and the texture would silently
            // hold the older texels. The upload is refused, said here.
            OLO_CORE_ERROR("VulkanTexture3D::SetData: the in-frame upload could not be recorded ({}x{}x{}, {} bytes); "
                           "the texture keeps its previous contents",
                           m_Specification.Width, m_Specification.Height, m_Specification.Depth, size);
            return;
        }

        // Host staging buffer — destroyed right after the blocking one-shot.
        // Mirrors VulkanTexture2D::UploadPixels (VulkanTexture.cpp), minus
        // mip generation: volumes never generate mips (single-level image).
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = size;
        stagingInfo.usage = VulkanAddressCommands::kStagingSrcUsage;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (TrackedVmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                                   &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture3D::SetData: staging allocation failed ({} bytes)", size);
            return;
        }
        std::memcpy(stagingOut.pMappedData, data, size);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, size);
        const VkDeviceAddress stagingAddress = VulkanAddressCommands::QueryAddress(device->GetDevice(), staging);

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture3D::SetData",
            [&](VkCommandBuffer cmd)
            {
                // Whole image -> TRANSFER_DST. oldLayout UNDEFINED is a full
                // overwrite (discard is free); src scope is non-empty in case
                // this is a re-upload of an already-sampled volume (hot
                // reimport), matching the WRITE_AFTER_WRITE reasoning in
                // VulkanTexture2D::UploadPixels.
                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, 1u);

                VulkanAddressCommands::CmdCopyRangeToImage(
                    cmd, stagingAddress, VulkanAddressCommands::StorageUsage::Absent, size, m_Image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u }, { 0, 0, 0 },
                    { m_Specification.Width, m_Specification.Height, m_Specification.Depth });

                VulkanUpload::RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VulkanDevice::Get()->GetSampledImageLayout(), VK_PIPELINE_STAGE_2_COPY_BIT,
                                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_MEMORY_READ_BIT, 0u, 1u);
            });

        TrackedVmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);

        if (ok)
        {
            // Seed the layout tracker's first sight of this image — without
            // this the graph's first barrier would transition from UNDEFINED
            // and could legally discard the texels just uploaded (same
            // reasoning as VulkanTexture2D::UploadPixels).
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VulkanDevice::Get()->GetSampledImageLayout());
        }
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
