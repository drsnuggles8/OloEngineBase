#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanCapabilities.h"

#include <cstring>

namespace OloEngine
{
    std::vector<const char*> VulkanCapabilities::RequiredDeviceExtensions()
    {
        return {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
            VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        };
    }

    VulkanCapabilityReport VulkanCapabilities::Evaluate(VkPhysicalDevice device)
    {
        VulkanCapabilityReport report;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        report.DeviceName = properties.deviceName;
        report.ApiVersion = properties.apiVersion;

        if (properties.apiVersion < kMinApiVersion)
        {
            report.Missing.push_back(
                "Vulkan API version 1.4 (device reports " +
                std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) + "." +
                std::to_string(VK_API_VERSION_MINOR(properties.apiVersion)) + ")");
        }

        u32 extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

        auto hasExtension = [&extensions](const char* name)
        {
            for (const VkExtensionProperties& ext : extensions)
            {
                if (std::strcmp(ext.extensionName, name) == 0)
                {
                    return true;
                }
            }
            return false;
        };

        report.HasSwapchain = hasExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        report.HasDescriptorHeap = hasExtension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        report.HasShaderUntypedPointers = hasExtension(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);

        if (!report.HasSwapchain)
        {
            report.Missing.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }
        if (!report.HasDescriptorHeap)
        {
            report.Missing.emplace_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        }
        if (!report.HasShaderUntypedPointers)
        {
            report.Missing.emplace_back(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);
        }

        // Feature bits — chained only for extensions the device actually lists, so
        // the query never hands the driver a struct it cannot recognise.
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{};
        heapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedFeatures{};
        untypedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;

        void** chainTail = &features2.pNext;
        if (report.HasDescriptorHeap)
        {
            *chainTail = &heapFeatures;
            chainTail = &heapFeatures.pNext;
        }
        if (report.HasShaderUntypedPointers)
        {
            *chainTail = &untypedFeatures;
            chainTail = &untypedFeatures.pNext;
        }
        vkGetPhysicalDeviceFeatures2(device, &features2);

        report.DescriptorHeapFeature = report.HasDescriptorHeap && heapFeatures.descriptorHeap == VK_TRUE;
        report.ShaderUntypedPointersFeature =
            report.HasShaderUntypedPointers && untypedFeatures.shaderUntypedPointers == VK_TRUE;

        if (report.HasDescriptorHeap && !report.DescriptorHeapFeature)
        {
            report.Missing.emplace_back("VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap");
        }
        if (report.HasShaderUntypedPointers && !report.ShaderUntypedPointersFeature)
        {
            report.Missing.emplace_back("VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers");
        }

        report.Satisfied = report.Missing.empty();
        return report;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
