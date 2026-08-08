#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <string>
#include <vector>

namespace OloEngine
{
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

        std::string DeviceName;
        u32 ApiVersion = 0;

        bool HasSwapchain = false;             // VK_KHR_swapchain (needed to present at all)
        bool HasDescriptorHeap = false;        // VK_EXT_descriptor_heap listed
        bool DescriptorHeapFeature = false;    // ...and its descriptorHeap feature bit
        bool HasShaderUntypedPointers = false; // VK_KHR_shader_untyped_pointers listed
        bool ShaderUntypedPointersFeature = false;
    };

    class VulkanCapabilities
    {
      public:
        // Vulkan 1.4 minimum: VK_EXT_descriptor_heap is a 1.4-era extension, and on a
        // 1.4 device its vk.xml dependency chain is satisfied by core alone.
        static constexpr u32 kMinApiVersion = VK_API_VERSION_1_4;

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
