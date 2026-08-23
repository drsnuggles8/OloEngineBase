#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDebugNames.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace OloEngine::VulkanDebugNames
{
#ifdef OLO_DEBUG
    namespace
    {
        // OPT-IN, and that is not timidity — it is a measured requirement.
        // Naming every image at registration puts a layer round-trip on the
        // image-CREATION path, which is exactly the path a window-resize
        // storm hammers. Doing it unconditionally moved the frame enough to
        // hide #800's race completely (14 storm rounds, 0 errors, against a
        // reliable 2-per-6 on the same build with naming off). A diagnostic
        // that changes the timing of the thing it is meant to identify is
        // worse than no diagnostic, so it is off unless asked for.
        bool ReadOptIn()
        {
            const char* value = std::getenv("OLO_VK_OBJECT_NAMES");
            // The WHOLE value, not its first character: "00" and "0ff" are not
            // the off switch, and unset or empty is off.
            return value != nullptr && std::string_view{ value } != "" && std::string_view{ value } != "0";
        }

        const bool s_OptIn = ReadOptIn();
    } // namespace

    bool Enabled()
    {
        if (!s_OptIn)
        {
            return false;
        }
        // volk leaves the pointer null unless VK_EXT_debug_utils was enabled
        // on the instance, which VulkanDevice only does alongside the
        // validation layer. No layer, no names, no cost.
        if (vkSetDebugUtilsObjectNameEXT == nullptr)
        {
            return false;
        }
        const VulkanDevice* device = VulkanDevice::Get();
        return device != nullptr && device->GetDevice() != VK_NULL_HANDLE;
    }

    void SetImageName(VkImage image, std::string_view name)
    {
        if (image == VK_NULL_HANDLE || !Enabled())
        {
            return;
        }
        // pObjectName is a null-terminated C string; string_view is not.
        const std::string owned(name);
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = VK_OBJECT_TYPE_IMAGE;
        info.objectHandle = reinterpret_cast<u64>(image);
        info.pObjectName = owned.c_str();
        // A naming failure is diagnostics-only — never escalate it.
        (void)vkSetDebugUtilsObjectNameEXT(VulkanDevice::Get()->GetDevice(), &info);
    }
#else
    bool Enabled()
    {
        return false;
    }

    void SetImageName(VkImage, std::string_view)
    {
    }
#endif
} // namespace OloEngine::VulkanDebugNames

#endif // OLO_WITH_VULKAN
