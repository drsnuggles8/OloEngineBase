#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanImageInfoRegistry.h"

#include "Platform/Vulkan/VulkanDebugNames.h"

#include <format>

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
        const u64 registrationId = ++s_NextRegistrationId;
        m_Infos[image].RegistrationId = registrationId;

        // Name the image for the validation layer (#800). One site covers
        // every image the barrier machinery can see, and the registration id
        // is exactly the identity a layout message needs to be actionable:
        // it distinguishes a driver-recycled handle VALUE from the image the
        // trackers were following. No-op outside a validated Debug run.
        if (VulkanDebugNames::Enabled())
        {
            VulkanDebugNames::SetImageName(
                image, std::format("image#{} {}x{} fmt={} mips={} layers={}", registrationId, info.Width, info.Height,
                                   static_cast<u32>(info.Format), info.MipLevels, info.ArrayLayers));
        }
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
