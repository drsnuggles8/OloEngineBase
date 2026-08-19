#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTexture3D.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"

#include <algorithm>

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
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture3D: image creation failed ({}x{}x{})", spec.Width, spec.Height,
                           spec.Depth);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTexture3D");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = imageInfo.format;
        // Register the CLAMPED extent (imageInfo.extent) — a zero-sized spec
        // creates a 1x1x1 image, and readback/capture sizing reads this.
        registryInfo.Width = imageInfo.extent.width;
        registryInfo.Height = imageInfo.extent.height;
        registryInfo.MipLevels = 1;
        registryInfo.ArrayLayers = 1;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_3D;
        // §4f sampler table: 3D volumes are caller-supplied on GL; the
        // engine's volumes (froxel fog, noise) all clamp.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
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
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
