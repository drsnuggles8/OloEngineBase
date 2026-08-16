#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanImageInfoRegistry.h"

namespace OloEngine
{
    VulkanImageInfoRegistry& VulkanImageInfoRegistry::Get()
    {
        static auto* s_Instance = new VulkanImageInfoRegistry(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanImageInfoRegistry::Register(VkImage image, const VulkanImageInfo& info)
    {
        if (image == VK_NULL_HANDLE)
        {
            return;
        }
        // Stamp every registration uniquely so a layout tracker can tell a
        // driver-recycled handle VALUE apart from the image it tracked — see
        // VulkanImageInfo::RegistrationId.
        static u64 s_NextRegistrationId = 0;
        m_Infos[image] = info;
        m_Infos[image].RegistrationId = ++s_NextRegistrationId;
    }

    const VulkanImageInfo* VulkanImageInfoRegistry::Lookup(VkImage image) const
    {
        const auto it = m_Infos.find(image);
        return it != m_Infos.end() ? &it->second : nullptr;
    }

    void VulkanImageInfoRegistry::Unregister(VkImage image)
    {
        m_Infos.erase(image);
    }

    void VulkanImageInfoRegistry::SetInitialLayout(VkImage image, VkImageLayout layout)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.InitialLayout = layout;
        }
    }

    void VulkanImageInfoRegistry::SetSamplerFilter(VkImage image, const VkFilter minFilter, const VkFilter magFilter)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.MinFilter = minFilter;
            it->second.MagFilter = magFilter;
            // GL couples the mip filter into MIN_FILTER; NEAREST min means
            // no linear mip blend either (the GL_NEAREST /
            // GL_NEAREST_MIPMAP_NEAREST shape callers actually use).
            it->second.MipmapMode =
                minFilter == VK_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    void VulkanImageInfoRegistry::SetSamplerAddressMode(VkImage image, const VkSamplerAddressMode mode)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.AddressMode = mode;
        }
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
