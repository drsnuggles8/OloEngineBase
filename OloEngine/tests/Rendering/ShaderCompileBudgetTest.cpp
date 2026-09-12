// OLO_TEST_LAYER: shaderpipe
//
// Every compute shader must build on Mesa's AMD compiler within a time and
// memory budget -- measured on a RADV "null device", which is the AMD compiler
// with no AMD hardware behind it.
//
// Why: BC6HEncodeCommon.glsl compiled in about a second on NVIDIA and on
// llvmpipe, and every PR check is green on those. On the self-hosted AMD box
// the same shader took Mesa's AMD path (radeonsi on the GL nightly, RADV here)
// over 200 s and 14 GB, so every BC6HGpuEncoder case timed out or was
// OOM-killed on every nightly for a week, and nothing a PR runs could see it.
// A `for (s < subsets)` loop with a data-dependent trip count over dynamically
// indexed arrays was the trigger; docs/agent-rules/amd-mesa-shader-compile-blowup.md
// has the bisection. This test is what makes that class of regression a PR
// failure instead of a nightly mystery.
//
// How: RADV_FORCE_FAMILY=navi10 (the box's RX 5600 XT) makes Mesa's Vulkan ICD
// enumerate a null device for that chip. Building a compute pipeline on it runs
// the whole SPIR-V -> NIR -> ACO pipeline and touches no GPU, no /dev/dri and
// no display, so it runs anywhere the RADV ICD is installed -- the self-hosted
// box and the hosted Ubuntu runners (setup-linux-build installs
// mesa-vulkan-drivers). Where it is not installed, this test SKIPS and says so;
// it does not fall back to another driver, because another driver is not the
// compiler under test.
//
// The GL nightly feeds the driver GLSL, not SPIR-V, and radeonsi is not RADV;
// both share the NIR passes that blew up (ACO and LLVM measured identical on
// the null device), so the null device is the closest stand-in this side of
// an AMD card, not a proof about radeonsi. The budget is generous for the
// same reason: the failure this guards is minutes and gigabytes, not seconds.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN
TEST(ShaderCompileBudget, EveryComputeShaderBuildsOnTheRadvNullDeviceWithinBudget)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF -- volk and the Vulkan loader are not compiled in.";
}
#else

#include "MemoryCeiling.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <volk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake -- see OloEngine/tests/CMakeLists.txt"
#endif

namespace
{
    using namespace OloEngine;

    // The chip the CI box carries (AMD Radeon RX 5600 XT, navi10). A different
    // family compiles differently; this one is where the failure was measured.
    constexpr const char* kForcedFamily = "navi10";

    // Per shader. The measured cost of the healthy BC6H shader on the null
    // device is 1.1 s / 210 MB (ACO) and 2.6 s / 220 MB (LLVM backend); the
    // failure it guards was 207 s / 14 300 MB. Sanitizer builds instrument the
    // engine but not Mesa, so the driver side of this cost barely moves.
    //
    // The memory budget is on the PEAK resident set sampled every 25 ms while
    // vkCreateComputePipelines runs, not on what is retained after it returns:
    // the compiler frees its IR on the way out, so a before/after delta would
    // pass a shader that took gigabytes and gave them back. The hard stops are
    // the process-wide memory ceiling (MemoryCeiling.h: a compile that keeps
    // growing is killed at 6 GiB with this test's name) and ctest's timeout for
    // a hang; the shader being compiled is printed BEFORE the call so either
    // stop names it. A child process per shader would give each its own hard
    // limits, but the null device would have to be brought up 80+ times; the
    // in-process sampler plus the two stops covers the failure this guards.
    constexpr double kWallBudgetSeconds = 30.0;
    constexpr u64 kPeakResidentBudgetMb = 1024;

    // The shaders whose blow-up this test exists for. If they ever stop being
    // part of the measured set (renamed, made Vulkan-incompatible, moved), the
    // guard has quietly lost its subject; that is asserted, not assumed.
    constexpr const char* kMustBeMeasured[] = { "BC6HEncode.comp", "BC6HEncodeSigned.comp" };

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void SetEnvironmentVariable(const char* name, const char* value)
    {
#if defined(_WIN32)
        ::_putenv_s(name, value);
#else
        ::setenv(name, value, /*overwrite*/ 1);
#endif
    }

    // Mirrors VulkanComputeShader::BuildFromSource's option set: the SPIR-V the
    // driver sees in production is what the budget is measured on.
    bool CompileComputeToSpirv(const std::string& source, const std::string& name, std::vector<u32>& outSpirv,
                               std::string& outError)
    {
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
        options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
        constexpr auto kShadercSpirv16 = static_cast<shaderc_spirv_version>((1u << 16) | (6u << 8));
        options.SetTargetSpirv(kShadercSpirv16);
        options.SetPreserveBindings(true);
        options.SetAutoBindUniforms(false);
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        options.SetSuppressWarnings();
        options.AddMacroDefinition("OLO_VULKAN", "1");

        const auto result = compiler.CompileGlslToSpv(source, shaderc_glsl_compute_shader, name.c_str(), options);
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            outError = result.GetErrorMessage();
            return false;
        }
        outSpirv.assign(result.cbegin(), result.cend());
        return true;
    }

    struct ReflectedLayout
    {
        std::map<u32, std::vector<VkDescriptorSetLayoutBinding>> Sets; // set index -> bindings
        u32 PushConstantBytes = 0;
        bool NeedsRayTracing = false;
    };

    // The pipeline layout must describe every descriptor the SPIR-V declares,
    // so it is read off the module rather than guessed per shader.
    ReflectedLayout ReflectLayout(const std::vector<u32>& spirv)
    {
        ReflectedLayout layout;
        spirv_cross::Compiler compiler(spirv);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        const auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, VkDescriptorType type)
        {
            for (const auto& res : list)
            {
                const u32 set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
                const u32 binding = compiler.get_decoration(res.id, spv::DecorationBinding);
                const auto& spirvType = compiler.get_type(res.type_id);
                u32 count = 1;
                for (sizet i = 0; i < spirvType.array.size(); ++i)
                {
                    // A runtime-sized array reports 0; one descriptor is enough
                    // for pipeline creation (the shader is never dispatched).
                    count *= spirvType.array[i] == 0 ? 1u : spirvType.array[i];
                }
                VkDescriptorSetLayoutBinding b{};
                b.binding = binding;
                b.descriptorType = type;
                b.descriptorCount = count;
                b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                layout.Sets[set].push_back(b);
            }
        };
        add(resources.uniform_buffers, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        add(resources.storage_buffers, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        add(resources.storage_images, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        add(resources.sampled_images, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        add(resources.separate_images, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        add(resources.separate_samplers, VK_DESCRIPTOR_TYPE_SAMPLER);
        layout.NeedsRayTracing = !resources.acceleration_structures.empty();

        for (const auto& pc : resources.push_constant_buffers)
        {
            const auto& type = compiler.get_type(pc.base_type_id);
            layout.PushConstantBytes = static_cast<u32>(compiler.get_declared_struct_size(type));
        }
        return layout;
    }

    struct NullDevice
    {
        VkInstance Instance = VK_NULL_HANDLE;
        VkPhysicalDevice Physical = VK_NULL_HANDLE;
        VkDevice Device = VK_NULL_HANDLE;
        std::string Name;
        std::string SkipReason; // set when the null device could not be brought up

        ~NullDevice()
        {
            if (Device != VK_NULL_HANDLE)
            {
                vkDestroyDevice(Device, nullptr);
            }
            if (Instance != VK_NULL_HANDLE)
            {
                vkDestroyInstance(Instance, nullptr);
            }
        }
    };

    void BringUpNullDevice(NullDevice& out)
    {
        // Read by the RADV ICD when the loader enumerates physical devices, so
        // it must be set before vkCreateInstance. Every gtest case is its own
        // process, so nothing else in this process has touched Vulkan yet.
        SetEnvironmentVariable("RADV_FORCE_FAMILY", kForcedFamily);

        if (volkInitialize() != VK_SUCCESS)
        {
            out.SkipReason = "no Vulkan loader on this machine";
            return;
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OloEngine-Tests ShaderCompileBudget";
        appInfo.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        if (vkCreateInstance(&instanceInfo, nullptr, &out.Instance) != VK_SUCCESS)
        {
            out.SkipReason = "vkCreateInstance failed (no ICD, or the loader is below Vulkan 1.3)";
            return;
        }
        volkLoadInstance(out.Instance);

        u32 deviceCount = 0;
        vkEnumeratePhysicalDevices(out.Instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(out.Instance, &deviceCount, devices.data());

        std::string seen;
        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);
            seen += seen.empty() ? "" : ", ";
            seen += props.deviceName;
            // RADV names its null device "Null hardware (RADV <FAMILY>)".
            if (std::string(props.deviceName).find("RADV") != std::string::npos && props.apiVersion >= VK_API_VERSION_1_3)
            {
                out.Physical = device;
                out.Name = props.deviceName;
                break;
            }
        }
        if (out.Physical == VK_NULL_HANDLE)
        {
            out.SkipReason = "no RADV null device: Mesa's AMD Vulkan ICD (mesa-vulkan-drivers, libvulkan_radeon) is "
                             "not installed, so the AMD compiler cannot be measured here. Devices seen: [" +
                             seen + "]";
            return;
        }

        // Enable every feature the device supports: a shader that uses int64
        // atomics, subgroup ops or descriptor indexing then builds instead of
        // failing at pipeline creation for a reason unrelated to the budget.
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.pNext = &features12;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features11;
        vkGetPhysicalDeviceFeatures2(out.Physical, &features2);

        u32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(out.Physical, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(out.Physical, &queueFamilyCount, families.data());
        u32 computeFamily = 0;
        for (u32 i = 0; i < queueFamilyCount; ++i)
        {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                computeFamily = i;
                break;
            }
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = computeFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features2;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (const VkResult r = vkCreateDevice(out.Physical, &deviceInfo, nullptr, &out.Device); r != VK_SUCCESS)
        {
            out.SkipReason = "vkCreateDevice on '" + out.Name + "' failed with VkResult " + std::to_string(r);
            return;
        }
        volkLoadDevice(out.Device);
    }

    struct BuildOutcome
    {
        bool Built = false;
        VkResult Result = VK_SUCCESS;
        double Seconds = 0.0;
        u64 PeakResidentGrowthMb = 0; // sampled peak during the compile, above the pre-compile resident set
    };

    // Samples the resident set on a thread while the compile runs and keeps the peak.
    class ResidentPeakSampler
    {
      public:
        ResidentPeakSampler()
            : m_Baseline(Tests::CurrentResidentBytes()),
              m_Thread(
                  [this]
                  {
                      while (!m_Stop.load(std::memory_order_acquire))
                      {
                          const u64 now = Tests::CurrentResidentBytes();
                          u64 seen = m_Peak.load(std::memory_order_relaxed);
                          while (now > seen && !m_Peak.compare_exchange_weak(seen, now, std::memory_order_relaxed))
                          {
                          }
                          std::this_thread::sleep_for(std::chrono::milliseconds(25));
                      }
                  })
        {
        }
        ~ResidentPeakSampler()
        {
            Stop();
        }
        ResidentPeakSampler(const ResidentPeakSampler&) = delete;
        ResidentPeakSampler& operator=(const ResidentPeakSampler&) = delete;

        // Stops sampling and returns the peak growth over the baseline, in MB.
        u64 Stop()
        {
            if (m_Thread.joinable())
            {
                m_Stop.store(true, std::memory_order_release);
                m_Thread.join();
            }
            // One last sample after the call returned, so a short compile that
            // ended between two polls is still measured.
            const u64 last = Tests::CurrentResidentBytes();
            const u64 peak = std::max(m_Peak.load(std::memory_order_relaxed), last);
            return peak > m_Baseline ? (peak - m_Baseline) / (1024ull * 1024ull) : 0;
        }

      private:
        u64 m_Baseline = 0;
        std::atomic<u64> m_Peak{ 0 };
        std::atomic<bool> m_Stop{ false };
        std::thread m_Thread;
    };

    BuildOutcome BuildComputePipeline(VkDevice device, const std::vector<u32>& spirv, const ReflectedLayout& layout)
    {
        BuildOutcome outcome;

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spirv.size() * sizeof(u32);
        moduleInfo.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        outcome.Result = vkCreateShaderModule(device, &moduleInfo, nullptr, &module);
        if (outcome.Result != VK_SUCCESS)
        {
            return outcome;
        }

        std::vector<VkDescriptorSetLayout> setLayouts;
        const u32 setCount = layout.Sets.empty() ? 0u : layout.Sets.rbegin()->first + 1u;
        for (u32 set = 0; set < setCount; ++set)
        {
            const auto it = layout.Sets.find(set);
            VkDescriptorSetLayoutCreateInfo setInfo{};
            setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setInfo.bindingCount = it == layout.Sets.end() ? 0u : static_cast<u32>(it->second.size());
            setInfo.pBindings = it == layout.Sets.end() ? nullptr : it->second.data();
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout);
            setLayouts.push_back(setLayout);
        }
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = layout.PushConstantBytes;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = layout.PushConstantBytes > 0 ? 1u : 0u;
        layoutInfo.pPushConstantRanges = &pushRange;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        {
            ResidentPeakSampler sampler;
            const auto start = std::chrono::steady_clock::now();
            outcome.Result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
            outcome.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            outcome.PeakResidentGrowthMb = sampler.Stop();
        }
        outcome.Built = outcome.Result == VK_SUCCESS;

        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        for (VkDescriptorSetLayout setLayout : setLayouts)
        {
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        }
        vkDestroyShaderModule(device, module, nullptr);
        return outcome;
    }
} // namespace

TEST(ShaderCompileBudget, EveryComputeShaderBuildsOnTheRadvNullDeviceWithinBudget)
{
    NullDevice null;
    BringUpNullDevice(null);
    if (null.Device == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "AMD compile budget not measured: " << null.SkipReason;
    }

    const std::filesystem::path shaderDir = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "compute";
    std::vector<std::filesystem::path> shaders;
    for (const auto& entry : std::filesystem::directory_iterator(shaderDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".comp")
        {
            shaders.push_back(entry.path());
        }
    }
    std::ranges::sort(shaders);
    ASSERT_FALSE(shaders.empty()) << "no compute shaders under " << shaderDir.string();

    std::printf("[ ShaderCompileBudget ] %s: %zu compute shaders, budget %.0f s / peak +%llu MB each\n",
                null.Name.c_str(), shaders.size(), kWallBudgetSeconds, static_cast<unsigned long long>(kPeakResidentBudgetMb));

    std::vector<std::string> measured;
    std::vector<std::string> notVulkanCompilable;
    std::vector<std::string> notBuilt;
    std::vector<std::string> overBudget;
    for (const auto& path : shaders)
    {
        const std::string fileName = path.filename().string();
        const std::string source =
            OpenGLShader::ProcessIncludes(ReadTextFile(path), path.parent_path().generic_string());

        std::vector<u32> spirv;
        std::string error;
        if (!CompileComputeToSpirv(source, fileName, spirv, error))
        {
            // Compiled for the GL path only; this guard cannot see it. Counted
            // and printed so a shader cannot slip out of the measured set unnoticed.
            const auto firstLine = error.substr(0, error.find('\n'));
            std::printf("[ ShaderCompileBudget ] %-44s not Vulkan-compilable: %s\n", fileName.c_str(), firstLine.c_str());
            notVulkanCompilable.push_back(fileName);
            continue;
        }
        const ReflectedLayout layout = ReflectLayout(spirv);
        if (layout.NeedsRayTracing)
        {
            std::printf("[ ShaderCompileBudget ] %-44s not measured: declares an acceleration structure (needs the "
                        "ray-tracing extensions, which the null device is not created with)\n",
                        fileName.c_str());
            notVulkanCompilable.push_back(fileName);
            continue;
        }

        // Announced BEFORE the compile: if the memory ceiling or ctest's timeout
        // stops this process mid-compile, the last line printed names the shader.
        std::printf("[ ShaderCompileBudget ] %-44s compiling...\n", fileName.c_str());
        std::fflush(stdout);
        const BuildOutcome outcome = BuildComputePipeline(null.Device, spirv, layout);
        if (!outcome.Built)
        {
            std::printf("[ ShaderCompileBudget ] %-44s pipeline creation failed: VkResult %d\n", fileName.c_str(),
                        static_cast<int>(outcome.Result));
            notBuilt.push_back(fileName);
            continue;
        }
        measured.push_back(fileName);
        const bool within = outcome.Seconds <= kWallBudgetSeconds && outcome.PeakResidentGrowthMb <= kPeakResidentBudgetMb;
        std::printf("[ ShaderCompileBudget ] %-44s %7.2f s  peak +%6llu MB%s\n", fileName.c_str(), outcome.Seconds,
                    static_cast<unsigned long long>(outcome.PeakResidentGrowthMb), within ? "" : "   OVER BUDGET");
        ::testing::Test::RecordProperty(fileName + "_seconds", std::to_string(outcome.Seconds));
        ::testing::Test::RecordProperty(fileName + "_peak_resident_growth_mb", std::to_string(outcome.PeakResidentGrowthMb));
        if (!within)
        {
            std::ostringstream what;
            what << fileName << ": " << outcome.Seconds << " s, peak +" << outcome.PeakResidentGrowthMb << " MB";
            overBudget.push_back(what.str());
        }
    }

    std::printf("[ ShaderCompileBudget ] measured %zu, not Vulkan-compilable %zu, pipeline creation failed %zu\n",
                measured.size(), notVulkanCompilable.size(), notBuilt.size());

    EXPECT_TRUE(overBudget.empty()) << overBudget.size()
                                    << " compute shader(s) over the AMD compile budget (" << kWallBudgetSeconds
                                    << " s / peak +" << kPeakResidentBudgetMb
                                    << " MB). This is the BC6HGpuEncoder failure class: fine on NVIDIA and "
                                       "llvmpipe, minutes and gigabytes on Mesa's AMD path; see "
                                       "docs/agent-rules/amd-mesa-shader-compile-blowup.md.\n"
                                    << [&]
    {
        std::string list;
        for (const auto& s : overBudget)
        {
            list += "  " + s + "\n";
        }
        return list;
    }();

    // A pipeline the null device refuses is not a budget breach, but it is a
    // shader this guard does not cover; that must be visible, not silent.
    EXPECT_TRUE(notBuilt.empty()) << notBuilt.size()
                                  << " compute shader(s) compiled to SPIR-V but did not build on the null "
                                     "device (VkResult printed above); they are unmeasured by this guard.";

    for (const char* required : kMustBeMeasured)
    {
        EXPECT_NE(std::ranges::find(measured, required), measured.end())
            << required << " was not measured -- the shader this guard exists for has dropped out of its "
                           "measured set (renamed, no longer Vulkan-compilable, or failed to build).";
    }
}

#endif // OLO_WITH_VULKAN
