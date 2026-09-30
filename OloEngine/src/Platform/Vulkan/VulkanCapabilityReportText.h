#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanCapabilities.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::VulkanCapabilityReportText
{
    // What the real gate (VulkanDevice::Init, headless) did on this machine, as
    // opposed to what Evaluate() predicts per device. Filled by RunGate().
    struct GateOutcome
    {
        bool Ran = false;
        bool Created = false;
        std::string Refusal;    // the exception text when the gate refused
        std::string DeviceName; // the device the gate actually picked
        bool RayQueryEnabled = false;
        std::string RayQueryReason; // why not, when not
    };

    // Facts about the machine and the build that are not per-device.
    struct Environment
    {
        u32 LoaderApiVersion = 0; // vkEnumerateInstanceVersion; 0 = no loader
        std::string LoaderError;  // set when the loader or instance could not be created
        u32 HeaderVersion = 0;    // VK_HEADER_VERSION the engine was built against
        std::string MinimumSdk;
        std::string Toolchain; // compiler + build type
        std::string Os;
    };

    // One row of "which tests can this machine execute": a test population, the
    // filter that selects it, and whether the device set can run it.
    struct ExecutableTier
    {
        std::string Population;
        std::string Filter;
        bool Executable = false;
        std::string Why; // what decides it, in words
    };

    // Derives the tiers from the per-device reports and the gate outcome. Pure:
    // device-free, so the classification is unit-tested with hand-built reports.
    [[nodiscard]] std::vector<ExecutableTier> ClassifyTests(const std::vector<VulkanCapabilityReport>& devices,
                                                            const GateOutcome& gate);

    // Renders the Markdown report. Pure.
    [[nodiscard]] std::string Format(const Environment& environment,
                                     const std::vector<VulkanCapabilityReport>& devices,
                                     const GateOutcome& gate);

    // Runs the real thing: creates a probe instance, evaluates every physical
    // device through VulkanCapabilities::Evaluate, then runs VulkanDevice::Init
    // headless and records the choice. Never throws. Must run before anything
    // else in the process owns a VulkanDevice.
    [[nodiscard]] std::string Build();
} // namespace OloEngine::VulkanCapabilityReportText

#endif // OLO_WITH_VULKAN
