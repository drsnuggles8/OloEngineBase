#pragma once

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "../TestOptions.h"

#include <volk.h>
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    struct VulkanDeviceTestGateResult
    {
        bool Available = false;
        std::string Reason;
    };

    class ScopedVulkanProbeInstance
    {
      public:
        explicit ScopedVulkanProbeInstance(VkInstance instance)
            : m_Instance(instance)
        {
        }

        ~ScopedVulkanProbeInstance()
        {
            static_cast<void>(ResetVolk());
        }

        ScopedVulkanProbeInstance(const ScopedVulkanProbeInstance&) = delete;
        ScopedVulkanProbeInstance& operator=(const ScopedVulkanProbeInstance&) = delete;

        [[nodiscard]] bool ResetVolk()
        {
            if (m_Instance == VK_NULL_HANDLE)
                return true;

            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;

            // volkInitialize() refreshes loader functions but deliberately
            // leaves the loaded instance/device and their entry points alone.
            // Finalize first so no global pointer can retain this destroyed
            // probe instance, then leave Volk ready for the real bring-up.
            volkFinalize();
            return volkInitialize() == VK_SUCCESS;
        }

      private:
        VkInstance m_Instance = VK_NULL_HANDLE;
    };

    // Use the same deliberately small bring-up ladder for every device tenant.
    // A loader-without-ICD Windows ASan runner survives this bare-instance probe,
    // while entering the full VulkanDevice/VulkanContext bring-up there can SEH-
    // fault before an exception can be reported. Restore volk's loader-scoped
    // entry points after the instance probe so the real bring-up starts cleanly.
    [[nodiscard]] inline VulkanDeviceTestGateResult ProbeVulkanDeviceTestGate()
    {
        // `--olo-gl-backend=none` is the suite's "this run tests no GPU"
        // contract (#1015): the sanitizer jobs pass it on the self-hosted box so
        // a run there means what the hosted run means. A Vulkan device is a GPU
        // too, and the box's Mesa may well satisfy this gate, so honour the
        // flag here as well rather than letting the Vulkan suites become the
        // one place the box still tests hardware the hosted arm does not.
        if (OloEngine::Tests::Options().GlBackend == OloEngine::Tests::GlBackend::None)
            return { false, "No GPU / GL 4.5+ context available in this environment (GL backend pinned to "
                            "'none' by --olo-gl-backend=none; the Vulkan gate honours it too)." };

        if (volkInitialize() != VK_SUCCESS)
            return { false, "No Vulkan loader on this machine." };

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OloEngine-Tests";
        appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        VkInstance probe = VK_NULL_HANDLE;
        if (vkCreateInstance(&instanceInfo, nullptr, &probe) != VK_SUCCESS)
            return { false, "vkCreateInstance failed (no Vulkan 1.4-capable ICD available)." };

        volkLoadInstance(probe);
        ScopedVulkanProbeInstance scopedProbe(probe);
        const auto finishProbe = [&](const bool available, std::string reason)
        {
            if (!scopedProbe.ResetVolk())
                return VulkanDeviceTestGateResult{ false, "Vulkan loader re-initialisation failed." };
            return VulkanDeviceTestGateResult{ available, std::move(reason) };
        };

        std::uint32_t deviceCount = 0;
        if (vkEnumeratePhysicalDevices(probe, &deviceCount, nullptr) != VK_SUCCESS)
            return finishProbe(false, "vkEnumeratePhysicalDevices (count) failed on this machine.");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        if (deviceCount > 0)
        {
            const VkResult listResult = vkEnumeratePhysicalDevices(probe, &deviceCount, devices.data());
            if (listResult != VK_SUCCESS && listResult != VK_INCOMPLETE)
                return finishProbe(false, "vkEnumeratePhysicalDevices (list) failed on this machine.");
            devices.resize(deviceCount);
        }

        for (const VkPhysicalDevice device : devices)
        {
            if (VulkanCapabilities::Evaluate(device).Satisfied)
                return finishProbe(true, {});
        }

        return finishProbe(false,
                           "No device satisfies the ADR 0010 capability contract here — the gate would refuse --rhi=vulkan.");
    }

    // Temporarily installs the real process-wide Vulkan RenderCommand facade,
    // then restores the initialized OpenGL facade used by the shared GPU-test
    // process. Keep this sequence centralized: restoring only RendererAPI's
    // selector leaves later tests dispatching through the wrong backend.
    class ScopedVulkanRenderCommandSelection
    {
      public:
        ScopedVulkanRenderCommandSelection()
            : m_PreviousDescriptorHeapEnabled(RHI::DescriptorHeap::Get().IsEnabled()),
              m_HadOpenGlContext(glfwGetCurrentContext() != nullptr)
        {
            if (m_HadOpenGlContext)
                RenderCommand::ShutdownGpuResources();
            RendererAPI::SetAPI(RendererAPI::API::Vulkan);
            RenderCommand::RecreateForSelectedBackend();
        }

        ~ScopedVulkanRenderCommandSelection()
        {
            RendererAPI::SetAPI(RendererAPI::API::OpenGL);
            RenderCommand::RecreateForSelectedBackend();
            if (m_HadOpenGlContext)
            {
                RenderCommand::Init();
                RHI::DescriptorHeap::Get().SetEnabled(m_PreviousDescriptorHeapEnabled);
            }
        }

        ScopedVulkanRenderCommandSelection(const ScopedVulkanRenderCommandSelection&) = delete;
        ScopedVulkanRenderCommandSelection& operator=(const ScopedVulkanRenderCommandSelection&) = delete;

        [[nodiscard]] VulkanRendererAPI& Get() const
        {
            return static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        }

      private:
        bool m_PreviousDescriptorHeapEnabled;
        bool m_HadOpenGlContext;
    };
} // namespace OloEngine::Tests
