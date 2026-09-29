#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanCapabilityReportText.h"

#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/ShaderToolchainFloor.h"
#include "Platform/Vulkan/VulkanDevice.h"

#include <algorithm>
#include <exception>
#include <format>
#include <memory>

namespace OloEngine::VulkanCapabilityReportText
{
    namespace
    {
        [[nodiscard]] std::string VersionString(u32 version)
        {
            return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                               VK_API_VERSION_PATCH(version));
        }

        [[nodiscard]] std::string_view KindOf(const VulkanCapabilityReport& report)
        {
            if (report.IsSoftware)
            {
                return "SOFTWARE (not a hardware vendor)";
            }
            switch (report.DeviceType)
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return "hardware, discrete";
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return "hardware, integrated";
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    return "virtual GPU";
                default:
                    return "hardware, other";
            }
        }

        [[nodiscard]] std::string MissingList(const VulkanCapabilityReport& report)
        {
            std::string joined;
            for (const std::string& item : report.Missing)
            {
                joined += (joined.empty() ? "" : ", ") + item;
            }
            return joined;
        }
    } // namespace

    std::vector<ExecutableTier> ClassifyTests(const std::vector<VulkanCapabilityReport>& devices, const GateOutcome& gate)
    {
        const auto anyDevice = [&devices](auto&& predicate)
        {
            return std::ranges::any_of(devices, predicate);
        };
        const bool contractMet = anyDevice([](const VulkanCapabilityReport& r)
                                           { return r.Satisfied; });
        const bool hardwareContractMet = anyDevice([](const VulkanCapabilityReport& r)
                                                   { return r.Satisfied && !r.IsSoftware; });
        const bool rayQueryFeatures = anyDevice([](const VulkanCapabilityReport& r)
                                                { return r.Satisfied && r.RayQueryFeatures; });

        // The tests call VulkanDevice::Init after the per-device predicate, so a
        // machine whose contract is met but whose real gate did not create a
        // device still skips at bring-up.
        const bool gatePassed = gate.Ran && gate.Created;

        std::vector<ExecutableTier> tiers;

        ExecutableTier deviceGated;
        deviceGated.Population = "Device-gated Vulkan suites (OLO_VULKAN_DEVICE_OR_SKIP)";
        deviceGated.Filter = "Vulkan*:VirtualShadowMapVulkanShaders*";
        deviceGated.Executable = contractMet && gatePassed;
        deviceGated.Why = !contractMet          ? "no device satisfies the ADR 0010 contract; every such test skips"
                          : !gatePassed         ? "a device satisfies the contract but the real VulkanDevice::Init gate did not admit one: " + gate.Refusal
                          : hardwareContractMet ? "a hardware device satisfies the ADR 0010 contract"
                                                : "only a SOFTWARE device satisfies the ADR 0010 contract";
        tiers.push_back(std::move(deviceGated));

        ExecutableTier rayQuery;
        rayQuery.Population = "L7 ray-query device suites (#1294)";
        rayQuery.Filter = "ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*";
        if (gatePassed)
        {
            // The real gate's verdict beats the feature-level prediction.
            rayQuery.Executable = gate.RayQueryEnabled;
            rayQuery.Why = gate.RayQueryEnabled ? "the gate's device enabled ray query (IsRayQueryEnabled)"
                                                : "IsRayQueryEnabled is false on the gate's device: " + gate.RayQueryReason;
        }
        else
        {
            rayQuery.Executable = false;
            rayQuery.Why = contractMet
                               ? (rayQueryFeatures ? "features are present but the real gate did not run to confirm"
                                                   : "no contract-satisfying device reports accelerationStructure + rayQuery")
                               : "no contract-satisfying device, so the device suites never start";
        }
        tiers.push_back(std::move(rayQuery));

        return tiers;
    }

    std::string Format(const Environment& environment, const std::vector<VulkanCapabilityReport>& devices,
                       const GateOutcome& gate)
    {
        std::string out;
        out += "# Vulkan capability report\n\n";
        out += "Produced by the engine's own device-selection reader, `VulkanCapabilities::Evaluate`, and the real\n";
        out += "gate, `VulkanDevice::Init` (headless). Nothing below is inferred from a vendor name.\n\n";

        out += "## Environment\n\n";
        out += std::format("- OS: {}\n- Toolchain: {}\n", environment.Os, environment.Toolchain);
        out += std::format("- Vulkan headers the engine was built against: 1.4.{}\n", environment.HeaderVersion);
        out += std::format("- Vulkan SDK floor (ADR 0010 tooling floor): {}\n", environment.MinimumSdk);
        if (environment.LoaderApiVersion != 0)
        {
            out += std::format("- Loader instance version: {}\n", VersionString(environment.LoaderApiVersion));
        }
        if (!environment.LoaderError.empty())
        {
            out += std::format("- **Loader/instance problem:** {}\n", environment.LoaderError);
        }
        out += "\n";

        out += std::format("## Devices ({})\n\n", devices.size());
        if (devices.empty())
        {
            out += "No Vulkan physical device is visible to this process.\n\n";
        }
        sizet hardwareSatisfying = 0;
        sizet softwareSatisfying = 0;
        for (sizet i = 0; i < devices.size(); ++i)
        {
            const VulkanCapabilityReport& r = devices[i];
            if (r.Satisfied)
            {
                (r.IsSoftware ? softwareSatisfying : hardwareSatisfying) += 1;
            }
            out += std::format("### Device {}: {} - {}\n\n", i, r.DeviceName, KindOf(r));
            out += std::format("- Driver: {} {} (driverVersion {}), vendorID 0x{:04X}\n", r.DriverName, r.DriverInfo,
                               VersionString(r.DriverVersion), r.VendorId);
            out += std::format("- API version: {}\n", VersionString(r.ApiVersion));
            out += std::format("- **ADR 0010 contract: {}**{}\n\n", r.Satisfied ? "SATISFIED" : "NOT satisfied",
                               r.Satisfied ? "" : " - missing: " + MissingList(r));
            out += "| Requirement | Met |\n| --- | --- |\n";
            for (const VulkanRequirementResult& requirement : r.Requirements)
            {
                out += std::format("| `{}` | {} |\n", requirement.Name, requirement.Met ? "yes" : "**no**");
            }
            out += std::format("\nOptional (never part of the verdict): VK_KHR_unified_image_layouts {}; "
                               "ray-query extensions {}, accelerationStructure+rayQuery features {}.\n\n",
                               r.UnifiedImageLayoutsFeature ? "usable" : "not usable",
                               r.HasRayQueryExtensions ? "listed" : "not listed",
                               r.RayQueryFeatures ? "true" : "false");
        }
        out += std::format("Satisfying devices: **{} hardware**, {} software (software is reported, never counted "
                           "as another hardware vendor).\n\n",
                           hardwareSatisfying, softwareSatisfying);

        out += "## The real gate\n\n";
        if (!gate.Ran)
        {
            out += "Not run (no loader or no instance).\n\n";
        }
        else if (gate.Created)
        {
            out += std::format("`VulkanDevice::Init` accepted **{}**. Ray query enabled: **{}**{}.\n\n", gate.DeviceName,
                               gate.RayQueryEnabled ? "yes" : "no",
                               gate.RayQueryEnabled ? "" : " (" + gate.RayQueryReason + ")");
        }
        else
        {
            out += std::format("`VulkanDevice::Init` **refused**: {}\n\n", gate.Refusal);
        }

        out += "## Tests this machine can execute\n\n";
        out += "| Population | Filter | Executable | Why |\n| --- | --- | --- | --- |\n";
        for (const ExecutableTier& tier : ClassifyTests(devices, gate))
        {
            out += std::format("| {} | `{}` | {} | {} |\n", tier.Population, tier.Filter,
                               tier.Executable ? "**yes**" : "no - skips", tier.Why);
        }
        out += "\nA skipped test is not a passing test. To make a designated run fail instead of skip, pass "
               "`--olo-require-vulkan` (issue #1300); the rows above say whether that run could succeed here.\n";
        return out;
    }

    std::string Build()
    {
        Environment environment;
        environment.HeaderVersion = VK_HEADER_VERSION;
        environment.MinimumSdk = std::string(ShaderToolchainFloor::kMinimumVulkanSdk);
#if defined(_MSC_VER) && !defined(__clang__)
        environment.Toolchain = std::format("MSVC {}", _MSC_VER);
#elif defined(__clang__)
        environment.Toolchain = std::format("clang {}", __clang_version__);
#elif defined(__GNUC__)
        environment.Toolchain = std::format("gcc {}", __VERSION__);
#endif
#ifdef OLO_DEBUG
        environment.Toolchain += ", Debug";
#else
        environment.Toolchain += ", Release/RelWithDebInfo";
#endif
#if defined(_WIN32)
        environment.Os = "Windows";
#elif defined(__linux__)
        environment.Os = "Linux";
#else
        environment.Os = "other";
#endif

        std::vector<VulkanCapabilityReport> devices;
        GateOutcome gate;

        if (volkInitialize() != VK_SUCCESS)
        {
            environment.LoaderError = "no Vulkan loader on this machine";
            return Format(environment, devices, gate);
        }
        if (vkEnumerateInstanceVersion != nullptr)
        {
            vkEnumerateInstanceVersion(&environment.LoaderApiVersion);
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OloEngine-CapabilityReport";
        appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        VkInstance probe = VK_NULL_HANDLE;
        const VkResult created = vkCreateInstance(&instanceInfo, nullptr, &probe);
        if (created != VK_SUCCESS)
        {
            environment.LoaderError = std::format("vkCreateInstance(apiVersion 1.4) failed with VkResult {}",
                                                  static_cast<int>(created));
            return Format(environment, devices, gate);
        }
        volkLoadInstance(probe);

        u32 deviceCount = 0;
        std::vector<VkPhysicalDevice> physicalDevices;
        // A failed or partial enumeration is reported as a loader problem and the
        // list is rejected, never presented as "no devices".
        const VkResult countResult = vkEnumeratePhysicalDevices(probe, &deviceCount, nullptr);
        if (countResult != VK_SUCCESS)
        {
            environment.LoaderError =
                std::format("vkEnumeratePhysicalDevices (count) failed with VkResult {}", static_cast<int>(countResult));
        }
        else if (deviceCount > 0)
        {
            physicalDevices.resize(deviceCount);
            const VkResult listResult = vkEnumeratePhysicalDevices(probe, &deviceCount, physicalDevices.data());
            if (listResult != VK_SUCCESS)
            {
                environment.LoaderError = std::format("vkEnumeratePhysicalDevices (list) returned VkResult {}; "
                                                      "the device list is incomplete and was discarded",
                                                      static_cast<int>(listResult));
                physicalDevices.clear();
            }
        }
        for (VkPhysicalDevice device : physicalDevices)
        {
            devices.push_back(VulkanCapabilities::Evaluate(device));
        }
        vkDestroyInstance(probe, nullptr);
        volkFinalize();
        static_cast<void>(volkInitialize());

        // The real gate, exactly as the tests' device fixture drives it: headless,
        // no surface. Its choice is what a live editor start would also make.
        gate.Ran = true;
        try
        {
            auto device = std::make_unique<VulkanDevice>();
            device->Init([](VkInstance)
                         { return VK_NULL_HANDLE; });
            gate.Created = true;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device->GetPhysicalDevice(), &properties);
            gate.DeviceName = properties.deviceName;
            gate.RayQueryEnabled = device->IsRayQueryEnabled();
            gate.RayQueryReason = std::string(RayTracing::ToString(device->GetRayTracingUnsupportedReason()));
            device->Shutdown();
        }
        catch (const std::exception& e)
        {
            gate.Refusal = e.what();
        }
        return Format(environment, devices, gate);
    }
} // namespace OloEngine::VulkanCapabilityReportText

#endif // OLO_WITH_VULKAN
