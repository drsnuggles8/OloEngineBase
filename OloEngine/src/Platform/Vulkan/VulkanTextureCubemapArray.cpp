#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTextureCubemapArray.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

namespace OloEngine
{
    namespace
    {
        void WarnCubemapArrayCpuPathOnce(const char* what)
        {
            static std::unordered_set<std::string> s_Warned;
            if (s_Warned.insert(what).second)
            {
                OLO_CORE_WARN("[RHI/Vulkan] TextureCubemapArray::{} is not implemented (#691: the probe "
                              "bake fill path is capture work) — no-op",
                              what);
            }
        }
    } // namespace

    VulkanTextureCubemapArray::VulkanTextureCubemapArray(const CubemapArraySpecification& spec)
        : m_ArraySpecification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTextureCubemapArray requires a live VulkanDevice");

        const u32 resolution = std::max(spec.Resolution, 1u);
        const u32 layers = std::max(spec.Layers, 1u);
        m_Specification.Width = resolution;
        m_Specification.Height = resolution;
        m_Specification.Format = spec.Format;

        // Clamp an authored mip count to the chain the resolution supports —
        // the same guard VulkanTextureCubemap applies; an over-long chain is
        // invalid image creation.
        const u32 fullCubeArrayChain = 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(resolution))));
        m_MipLevels = spec.MipLevels > 0u ? std::min(spec.MipLevels, fullCubeArrayChain) : fullCubeArrayChain;

        const VkFormat format = VulkanUpload::ImageFormatToVkFormat(spec.Format, false);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        // CUBE_COMPATIBLE + 6*N layers is what makes the image addressable as
        // samplerCubeArray (the view type carries the rest).
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { resolution, resolution, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 6u * layers;
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
            OLO_CORE_ERROR("VulkanTextureCubemapArray: image creation failed ({}x{}, {} layers, {} mips)", resolution,
                           resolution, layers, m_MipLevels);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTextureCubemapArray");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = format;
        registryInfo.Width = resolution;
        registryInfo.Height = resolution;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = 6u * layers;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        // §4f sampler table: cubemaps are CLAMP_TO_EDGE on GL — the array
        // flavour follows its element type.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTextureCubemapArray::~VulkanTextureCubemapArray()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, not here — the
                // VulkanDescriptorSlotCache recycling contract.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw ({}); the array leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw; the array leaks");
        }
    }

    void VulkanTextureCubemapArray::Bind(u32 slot) const
    {
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTextureCubemapArray::SetData(void* /*data*/, u32 /*size*/)
    {
        WarnCubemapArrayCpuPathOnce("SetData");
    }

    void VulkanTextureCubemapArray::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                               const void* /*data*/, u32 /*channels*/)
    {
        WarnCubemapArrayCpuPathOnce("Invalidate");
    }

    bool VulkanTextureCubemapArray::SetLayerMipData(u32 /*layer*/, u32 /*mip*/, const void* /*data*/,
                                                    sizet /*sizeBytes*/)
    {
        WarnCubemapArrayCpuPathOnce("SetLayerMipData");
        return false;
    }

    bool VulkanTextureCubemapArray::CopyLayerFromCubemap(u32 /*layer*/, const TextureCubemap& /*source*/)
    {
        WarnCubemapArrayCpuPathOnce("CopyLayerFromCubemap");
        return false;
    }

    bool VulkanTextureCubemapArray::GetData(std::vector<u8>& outData, u32 /*mipLevel*/) const
    {
        outData.clear();
        WarnCubemapArrayCpuPathOnce("GetData");
        return false;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
