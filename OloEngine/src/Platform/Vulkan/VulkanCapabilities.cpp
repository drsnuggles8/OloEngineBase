#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanCapabilities.h"

#include <cstring>

namespace OloEngine
{
    namespace
    {
        constexpr std::size_t kExtensionCount = static_cast<std::size_t>(VulkanContractExtension::Count);

        // ADR 0010's capability contract, one row per device extension. Order is
        // the order RequiredDeviceExtensions() enables them in.
        constexpr std::array<VulkanContractExtensionRow, kExtensionCount> kContract{ {
            { VK_KHR_SWAPCHAIN_EXTENSION_NAME, nullptr },
            { VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, "VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap" },
            { VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
              "VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers" },
            { VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
              "VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands" },
        } };

        [[nodiscard]] constexpr std::size_t Index(VulkanContractExtension extension)
        {
            return static_cast<std::size_t>(extension);
        }
    } // namespace

    const VulkanContractExtensionRow& VulkanCapabilities::Row(VulkanContractExtension extension)
    {
        return kContract[Index(extension)];
    }

    std::vector<const char*> VulkanCapabilities::RequiredDeviceExtensions()
    {
        std::vector<const char*> names;
        names.reserve(kContract.size());
        for (const VulkanContractExtensionRow& row : kContract)
        {
            names.push_back(row.Extension);
        }
        return names;
    }

    VulkanContractFeatureChain::VulkanContractFeatureChain()
    {
        m_Heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        m_Untyped.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        m_AddressCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
    }

    void VulkanContractFeatureChain::RequestAll()
    {
        m_Heap.descriptorHeap = VK_TRUE;
        m_Untyped.shaderUntypedPointers = VK_TRUE;
        m_AddressCommands.deviceAddressCommands = VK_TRUE;
    }

    void** VulkanContractFeatureChain::Link(void** tail, const std::array<bool, kExtensionCount>& listed)
    {
        const auto append = [&tail](auto& feature)
        {
            *tail = &feature;
            tail = &feature.pNext;
        };
        if (listed[Index(VulkanContractExtension::DescriptorHeap)])
        {
            append(m_Heap);
        }
        if (listed[Index(VulkanContractExtension::ShaderUntypedPointers)])
        {
            append(m_Untyped);
        }
        if (listed[Index(VulkanContractExtension::DeviceAddressCommands)])
        {
            append(m_AddressCommands);
        }
        return tail;
    }

    void** VulkanContractFeatureChain::LinkAll(void** tail)
    {
        std::array<bool, kExtensionCount> listed{};
        listed.fill(true);
        return Link(tail, listed);
    }

    bool VulkanContractFeatureChain::Value(VulkanContractExtension extension) const
    {
        switch (extension)
        {
            case VulkanContractExtension::DescriptorHeap:
                return m_Heap.descriptorHeap == VK_TRUE;
            case VulkanContractExtension::ShaderUntypedPointers:
                return m_Untyped.shaderUntypedPointers == VK_TRUE;
            case VulkanContractExtension::DeviceAddressCommands:
                return m_AddressCommands.deviceAddressCommands == VK_TRUE;
            default:
                return false;
        }
    }

    VulkanCapabilityReport VulkanCapabilities::Evaluate(VkPhysicalDevice device)
    {
        VulkanCapabilityReport report;

        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driverProperties;
        vkGetPhysicalDeviceProperties2(device, &properties2);
        const VkPhysicalDeviceProperties& properties = properties2.properties;

        report.DeviceName = properties.deviceName;
        report.ApiVersion = properties.apiVersion;
        report.VendorId = properties.vendorID;
        report.DriverVersion = properties.driverVersion;
        report.DeviceType = properties.deviceType;
        report.DriverName = driverProperties.driverName;
        report.DriverInfo = driverProperties.driverInfo;
        report.IsSoftware = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                            driverProperties.driverID == VK_DRIVER_ID_MESA_LLVMPIPE ||
                            driverProperties.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER;

        const bool versionMet = properties.apiVersion >= kMinApiVersion;
        report.Requirements.push_back({ "Vulkan API version 1.4", versionMet });
        if (!versionMet)
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

        std::array<bool, kExtensionCount> listed{};
        for (std::size_t i = 0; i < kExtensionCount; ++i)
        {
            listed[i] = hasExtension(kContract[i].Extension);
        }
        report.HasSwapchain = listed[Index(VulkanContractExtension::Swapchain)];
        report.HasDescriptorHeap = listed[Index(VulkanContractExtension::DescriptorHeap)];
        report.HasShaderUntypedPointers = listed[Index(VulkanContractExtension::ShaderUntypedPointers)];
        report.HasDeviceAddressCommands = listed[Index(VulkanContractExtension::DeviceAddressCommands)];
        report.HasUnifiedImageLayouts = hasExtension(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
        report.HasRayQueryExtensions = hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                                       hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                                       hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

        // Feature bits — chained only for extensions the device actually lists, so
        // the query never hands the driver a struct it cannot recognise.
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VulkanContractFeatureChain contractFeatures;
        VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedLayoutFeatures{};
        unifiedLayoutFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{};
        accelFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

        void** chainTail = contractFeatures.Link(&features2.pNext, listed);
        if (report.HasUnifiedImageLayouts)
        {
            *chainTail = &unifiedLayoutFeatures;
            chainTail = &unifiedLayoutFeatures.pNext;
        }
        if (report.HasRayQueryExtensions)
        {
            *chainTail = &accelFeatures;
            accelFeatures.pNext = &rayQueryFeatures;
        }
        vkGetPhysicalDeviceFeatures2(device, &features2);

        report.DescriptorHeapFeature =
            report.HasDescriptorHeap && contractFeatures.Value(VulkanContractExtension::DescriptorHeap);
        report.ShaderUntypedPointersFeature =
            report.HasShaderUntypedPointers && contractFeatures.Value(VulkanContractExtension::ShaderUntypedPointers);
        report.DeviceAddressCommandsFeature =
            report.HasDeviceAddressCommands && contractFeatures.Value(VulkanContractExtension::DeviceAddressCommands);
        report.UnifiedImageLayoutsFeature =
            report.HasUnifiedImageLayouts && unifiedLayoutFeatures.unifiedImageLayouts == VK_TRUE;
        report.RayQueryFeatures = report.HasRayQueryExtensions && accelFeatures.accelerationStructure == VK_TRUE &&
                                  rayQueryFeatures.rayQuery == VK_TRUE;

        // Per-row verdict, straight off the table: an unlisted extension is
        // missing by name; a listed one with its feature bit off is missing by
        // the feature's name.
        for (std::size_t i = 0; i < kExtensionCount; ++i)
        {
            const VulkanContractExtensionRow& row = kContract[i];
            report.Requirements.push_back({ row.Extension, listed[i] });
            if (!listed[i])
            {
                report.Missing.emplace_back(row.Extension);
            }
            if (row.FeatureBit != nullptr)
            {
                const bool featureMet = listed[i] && contractFeatures.Value(static_cast<VulkanContractExtension>(i));
                report.Requirements.push_back({ row.FeatureBit, featureMet });
                if (listed[i] && !featureMet)
                {
                    report.Missing.emplace_back(row.FeatureBit);
                }
            }
        }

        report.Satisfied = report.Missing.empty();
        return report;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
