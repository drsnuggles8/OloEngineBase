// OLO_TEST_LAYER: meta
// =============================================================================
// VulkanCapabilityReportTextTest -- the #1358 capability report's classification
// is a claim about what a machine can run, so it is tested on hand-built device
// reports and needs no Vulkan device. The report is only worth publishing if a
// software driver can never read as a second hardware vendor and a machine that
// fails the contract can never read as able to run the device suites.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanCapabilityReportText, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF -- the Vulkan backend is not compiled in.";
}

#else

#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanCapabilityReportText.h"

#include <algorithm>
#include <string>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::VulkanCapabilityReportText;

    VulkanCapabilityReport MakeDevice(const char* name, bool satisfied, bool software, bool rayQueryFeatures)
    {
        VulkanCapabilityReport report;
        report.DeviceName = name;
        report.ApiVersion = VK_API_VERSION_1_4;
        report.IsSoftware = software;
        report.DeviceType = software ? VK_PHYSICAL_DEVICE_TYPE_CPU : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        report.Satisfied = satisfied;
        report.RayQueryFeatures = rayQueryFeatures;
        report.HasRayQueryExtensions = rayQueryFeatures;
        if (!satisfied)
        {
            report.Missing.emplace_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        }
        report.Requirements.push_back({ VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, satisfied });
        return report;
    }

    const ExecutableTier* Find(const std::vector<ExecutableTier>& tiers, const std::string& needle)
    {
        const auto it = std::ranges::find_if(tiers, [&needle](const ExecutableTier& t)
                                             { return t.Population.find(needle) != std::string::npos; });
        return it == tiers.end() ? nullptr : &*it;
    }

    TEST(VulkanCapabilityReportText, NoSatisfyingDeviceMeansNothingDeviceGatedRuns)
    {
        const auto tiers = ClassifyTests({ MakeDevice("RDNA2", false, false, false) }, GateOutcome{});
        ASSERT_NE(Find(tiers, "Device-gated"), nullptr);
        EXPECT_FALSE(Find(tiers, "Device-gated")->Executable);
        EXPECT_FALSE(Find(tiers, "ray-query")->Executable);
    }

    TEST(VulkanCapabilityReportText, ASoftwareDeviceRunsDeviceSuitesButIsLabelledAsSuch)
    {
        GateOutcome gate{ .Ran = true, .Created = true, .DeviceName = "llvmpipe", .RayQueryEnabled = false, .RayQueryReason = "not exposed" };
        const std::vector<VulkanCapabilityReport> devices{ MakeDevice("llvmpipe", true, true, false) };
        const auto tiers = ClassifyTests(devices, gate);
        EXPECT_TRUE(Find(tiers, "Device-gated")->Executable);
        EXPECT_NE(Find(tiers, "Device-gated")->Why.find("SOFTWARE"), std::string::npos);
        EXPECT_FALSE(Find(tiers, "ray-query")->Executable);

        const std::string text = Format(Environment{}, devices, gate);
        EXPECT_NE(text.find("SOFTWARE (not a hardware vendor)"), std::string::npos) << text;
        EXPECT_NE(text.find("**0 hardware**, 1 software"), std::string::npos) << text;
    }

    TEST(VulkanCapabilityReportText, RayQueryFollowsTheRealGateNotTheFeaturePrediction)
    {
        // Features predict ray query, the real gate says no: the gate wins.
        GateOutcome refused{ .Ran = true, .Created = true, .DeviceName = "X", .RayQueryEnabled = false, .RayQueryReason = "the ray-tracing entry points were not exported" };
        const std::vector<VulkanCapabilityReport> devices{ MakeDevice("X", true, false, true) };
        EXPECT_FALSE(Find(ClassifyTests(devices, refused), "ray-query")->Executable);

        GateOutcome enabled = refused;
        enabled.RayQueryEnabled = true;
        EXPECT_TRUE(Find(ClassifyTests(devices, enabled), "ray-query")->Executable);
    }

    TEST(VulkanCapabilityReportText, AContractMetDeviceWhoseRealGateRefusedRunsNothing)
    {
        GateOutcome gate;
        gate.Ran = true;
        gate.Refusal = "vkCreateDevice failed";
        const std::vector<VulkanCapabilityReport> devices{ MakeDevice("X", true, false, true) };
        const auto tiers = ClassifyTests(devices, gate);
        EXPECT_FALSE(Find(tiers, "Device-gated")->Executable);
        EXPECT_NE(Find(tiers, "Device-gated")->Why.find("vkCreateDevice failed"), std::string::npos);
        EXPECT_FALSE(Find(tiers, "ray-query")->Executable);
    }

    TEST(VulkanCapabilityReportText, ARefusingGateIsReportedVerbatim)
    {
        GateOutcome gate;
        gate.Ran = true;
        gate.Refusal = "Vulkan bring-up: no device satisfies the capability contract (ADR 0010).";
        const std::string text = Format(Environment{}, { MakeDevice("RDNA2", false, false, false) }, gate);
        EXPECT_NE(text.find("**refused**"), std::string::npos) << text;
        EXPECT_NE(text.find("no device satisfies the capability contract"), std::string::npos) << text;
        EXPECT_NE(text.find("NOT satisfied"), std::string::npos) << text;
    }
} // namespace

#endif // OLO_WITH_VULKAN
