// OLO_TEST_LAYER: plumbing
//
// #691: Vulkan bring-up. Headless (no window, no surface, no swapchain):
// what is testable without a display is the device-selection gate — the ADR 0010
// capability contract — and that a satisfying device actually accepts a logical
// device created with the contract's extensions + feature bits enabled. The
// contract is read through VulkanCapabilities::Evaluate, the SAME reader
// VulkanContext's device pick uses ("one list, two readers" — an open-coded
// extension check here would drift from the gate it claims to test).
//
// SKIP ladder mirrors the GL-4.6-context pattern (RendererAttachedTest): each
// missing environment rung SKIPs cleanly rather than failing, so headless CI
// passes while a GPU-equipped run gates. The clears+presents checkpoint itself
// is verified live (OloEditor --rhi=vulkan + OS-level screenshot), not here —
// MCP capture is GL-readback for now.

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"

#if !OLO_WITH_VULKAN

TEST(VulkanBringUp, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "Platform/Vulkan/VulkanCapabilities.h"

#include <volk.h>

#include <optional>
#include <vector>

namespace
{
    using namespace OloEngine;

    class VulkanBringUp : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (volkInitialize() != VK_SUCCESS)
            {
                GTEST_SKIP() << "No Vulkan loader on this machine.";
            }

            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "OloEngine-Tests";
            appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;

            VkInstanceCreateInfo instanceInfo{};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &appInfo;

            if (vkCreateInstance(&instanceInfo, nullptr, &m_Instance) != VK_SUCCESS)
            {
                GTEST_SKIP() << "vkCreateInstance failed (driver below Vulkan 1.4?).";
            }
            volkLoadInstance(m_Instance);

            u32 deviceCount = 0;
            vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
            if (deviceCount == 0)
            {
                GTEST_SKIP() << "No Vulkan physical devices present.";
            }
            m_Devices.resize(deviceCount);
            vkEnumeratePhysicalDevices(m_Instance, &deviceCount, m_Devices.data());
        }

        void TearDown() override
        {
            if (m_Instance != VK_NULL_HANDLE)
            {
                vkDestroyInstance(m_Instance, nullptr);
                m_Instance = VK_NULL_HANDLE;
            }
        }

        VkInstance m_Instance = VK_NULL_HANDLE;
        std::vector<VkPhysicalDevice> m_Devices;
    };

    // The report must be internally consistent for EVERY device, satisfying or
    // not — Satisfied is exactly "nothing missing", and each absent extension
    // names itself in Missing (that list is the refuse-to-init error text).
    TEST_F(VulkanBringUp, CapabilityReportIsInternallyConsistent)
    {
        for (VkPhysicalDevice device : m_Devices)
        {
            const VulkanCapabilityReport report = VulkanCapabilities::Evaluate(device);

            EXPECT_FALSE(report.DeviceName.empty());
            EXPECT_EQ(report.Satisfied, report.Missing.empty()) << report.DeviceName;

            auto missingContains = [&report](const char* item)
            {
                for (const std::string& entry : report.Missing)
                {
                    if (entry == item)
                    {
                        return true;
                    }
                }
                return false;
            };
            EXPECT_EQ(!report.HasDescriptorHeap, missingContains(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME))
                << report.DeviceName;
            EXPECT_EQ(!report.HasShaderUntypedPointers,
                      missingContains(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME))
                << report.DeviceName;
            EXPECT_EQ(!report.HasSwapchain, missingContains(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) << report.DeviceName;

            // A feature bit can only be reported true when its extension is listed.
            EXPECT_LE(report.DescriptorHeapFeature, report.HasDescriptorHeap) << report.DeviceName;
            EXPECT_LE(report.ShaderUntypedPointersFeature, report.HasShaderUntypedPointers) << report.DeviceName;
        }
    }

    // A device the gate would ACCEPT must also accept the logical device the gate
    // then creates — same extension list, contract feature bits enabled. This is
    // the "fail at the gate, not at first descriptor-heap use" property; if the
    // driver advertises the contract but rejects enabling it, this catches it in
    // the suite instead of at editor launch.
    TEST_F(VulkanBringUp, SatisfyingDeviceAcceptsTheContractEnabledLogicalDevice)
    {
        std::optional<VkPhysicalDevice> satisfying;
        for (VkPhysicalDevice device : m_Devices)
        {
            if (VulkanCapabilities::Evaluate(device).Satisfied)
            {
                satisfying = device;
                break;
            }
        }
        if (!satisfying.has_value())
        {
            GTEST_SKIP() << "No device satisfies the ADR 0010 capability contract "
                            "(VK_EXT_descriptor_heap et al.) — the gate would refuse --rhi=vulkan here.";
        }

        u32 familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(*satisfying, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(*satisfying, &familyCount, families.data());
        std::optional<u32> graphicsFamily;
        for (u32 i = 0; i < familyCount; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                graphicsFamily = i;
                break;
            }
        }
        ASSERT_TRUE(graphicsFamily.has_value());

        const f32 queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = *graphicsFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{};
        heapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        heapFeatures.descriptorHeap = VK_TRUE;
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedFeatures{};
        untypedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        untypedFeatures.shaderUntypedPointers = VK_TRUE;
        untypedFeatures.pNext = &heapFeatures;

        // Mirror VulkanContext::Init's chain exactly, sync2 included — this test's
        // claim is "the gate's enables are accepted", so the enable list must not
        // drift from the gate's.
        VkPhysicalDeviceVulkan13Features vulkan13Features{};
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.synchronization2 = VK_TRUE;
        vulkan13Features.pNext = &untypedFeatures;

        const std::vector<const char*> extensions = VulkanCapabilities::RequiredDeviceExtensions();

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &vulkan13Features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();

        VkDevice device = VK_NULL_HANDLE;
        const VkResult result = vkCreateDevice(*satisfying, &deviceInfo, nullptr, &device);
        EXPECT_EQ(result, VK_SUCCESS)
            << "Device advertises the ADR 0010 contract but rejected enabling it — the gate would "
               "accept a device the driver then refuses.";
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device, nullptr);
        }
    }
} // namespace

#endif // OLO_WITH_VULKAN
