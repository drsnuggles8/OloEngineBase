#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace OloEngine
{
    // ADR 0010's device-extension rows, in contract order. THE table: extension
    // names, feature-bit names, Evaluate()'s verdict, RequiredDeviceExtensions(),
    // the logical-device enable chain (VulkanContractFeatureChain) and the
    // capability report all index it, so adding a row here is the only edit that
    // widens the contract (and it needs the ADR 0010 amendment first).
    enum class VulkanContractExtension : u32
    {
        Swapchain = 0,
        DescriptorHeap,
        ShaderUntypedPointers,
        DeviceAddressCommands,
        Count
    };

    struct VulkanContractExtensionRow
    {
        const char* Extension;
        // The feature bit the row also requires, spelled as the refuse-to-init
        // message prints it. nullptr for a row with no feature bit (swapchain).
        const char* FeatureBit;
    };

    // One requirement as evaluated on one device, met or not. `Rows` is the
    // structured form of `Missing`: the capability report prints every row, the
    // refusal message prints only the unmet ones.
    struct VulkanRequirementResult
    {
        std::string Name;
        bool Met = false;
    };

    // ADR 0010's capability contract, evaluated for one physical device. This is the
    // SINGLE definition both readers use — VulkanContext's device selection and the
    // bring-up test — so the two cannot drift (an acceptance test that open-coded
    // "is VK_EXT_descriptor_heap present?" would silently pass a device missing the
    // shader dependency). Widening or narrowing what Evaluate() checks requires
    // amending ADR 0010's capability-contract section, not just this file.
    struct VulkanCapabilityReport
    {
        // True iff EVERY requirement below is met. The check is all-or-nothing: a
        // device satisfying some of the contract is refused, never partially enabled.
        bool Satisfied = false;

        // One human-readable entry per unmet requirement, for the refuse-to-init
        // error ("refuse, naming the missing capability" — ADR 0010).
        std::vector<std::string> Missing;

        // Every contract requirement in table order (API version, then per
        // extension: listed, then its feature bit), met or not.
        std::vector<VulkanRequirementResult> Requirements;

        std::string DeviceName;
        u32 ApiVersion = 0;

        // Identity, for the capability report. None of it feeds Satisfied.
        u32 VendorId = 0;
        u32 DriverVersion = 0;
        VkPhysicalDeviceType DeviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        std::string DriverName;
        std::string DriverInfo;
        // A software implementation (lavapipe, SwiftShader, WARP). Decided by
        // the driver id and the device type together, so a report can label the
        // row rather than counting it as another hardware vendor.
        bool IsSoftware = false;

        bool HasSwapchain = false;             // VK_KHR_swapchain (needed to present at all)
        bool HasDescriptorHeap = false;        // VK_EXT_descriptor_heap listed
        bool DescriptorHeapFeature = false;    // ...and its descriptorHeap feature bit
        bool HasShaderUntypedPointers = false; // VK_KHR_shader_untyped_pointers listed
        bool ShaderUntypedPointersFeature = false;
        bool HasDeviceAddressCommands = false; // VK_KHR_device_address_commands listed
        bool DeviceAddressCommandsFeature = false;

        // Optional optimisation, never contributes to Missing or Satisfied.
        bool HasUnifiedImageLayouts = false;
        bool UnifiedImageLayoutsFeature = false;

        // Optional hardware ray query, never contributes to Missing or Satisfied.
        // Feature-level only: the extensions listed AND accelerationStructure and
        // rayQuery reporting VK_TRUE. VulkanDevice::IsRayQueryEnabled() is the
        // final verdict (it also needs vkCreateDevice to accept the request and
        // the entry points to load); this is what the report can say without
        // creating a device.
        bool HasRayQueryExtensions = false;
        bool RayQueryFeatures = false;
    };

    // The contract's feature structs, chained for either direction of use: the
    // device pick's vkGetPhysicalDeviceFeatures2 query and vkCreateDevice's
    // enable. One type so the two cannot disagree on which bits the contract
    // has (the bring-up test used to hand-mirror the enable chain).
    class VulkanContractFeatureChain
    {
      public:
        VulkanContractFeatureChain();

        // Sets every contract feature bit to VK_TRUE (the vkCreateDevice form).
        void RequestAll();

        // Appends the structs for the listed extensions to the chain that ends
        // at *tail and returns the new tail slot. A struct for an extension the
        // device does not list must not be handed to the driver, hence `listed`.
        // `listed` is indexed by VulkanContractExtension.
        [[nodiscard]] void** Link(void** tail, const std::array<bool, static_cast<std::size_t>(VulkanContractExtension::Count)>& listed);

        // Link() with every row listed: the vkCreateDevice form.
        [[nodiscard]] void** LinkAll(void** tail);

        // Reads the feature bit of an extension row after a query. False for a
        // row with no feature bit.
        [[nodiscard]] bool Value(VulkanContractExtension extension) const;

      private:
        VkPhysicalDeviceDescriptorHeapFeaturesEXT m_Heap{};
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR m_Untyped{};
        VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR m_AddressCommands{};
    };

    class VulkanCapabilities
    {
      public:
        // Vulkan 1.4 minimum: VK_EXT_descriptor_heap is a 1.4-era extension, and on a
        // 1.4 device its vk.xml dependency chain is satisfied by core alone.
        static constexpr u32 kMinApiVersion = VK_API_VERSION_1_4;

        // The contract table (see VulkanContractExtension).
        [[nodiscard]] static const VulkanContractExtensionRow& Row(VulkanContractExtension extension);

        // The device extensions a satisfying device must both expose and have enabled
        // at logical-device creation. Exposed so VulkanContext enables exactly this
        // list — same one-list rule as the report itself.
        [[nodiscard]] static std::vector<const char*> RequiredDeviceExtensions();

        // Evaluate the full contract for `device`. Requires a live instance with volk
        // loaded (instance-level entry points are used). Never throws; the report
        // carries the verdict.
        [[nodiscard]] static VulkanCapabilityReport Evaluate(VkPhysicalDevice device);
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
