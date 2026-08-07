#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanCapabilities.h"

// volk before both VMA and GLFW: vk_mem_alloc.h keys its volk import helper on
// VOLK_HEADER_VERSION, and glfw3.h only declares its Vulkan entry points
// (glfwCreateWindowSurface et al.) when VK_VERSION_1_0 is already visible.
#include <volk.h>

// Function-pointer config must match VulkanMemoryAllocator.cpp (the
// VMA_IMPLEMENTATION TU) exactly — VMA is imported through volk's loaded
// pointers, never linked statically (nothing links vulkan-1.lib).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The vendored Vulkan-Headers (SDK 1.4.357.0, the ADR 0010 tooling floor) must be
// the ones this TU compiles against. An installed SDK's include dir is also on the
// include path (via the shaderc toolchain's find_package(Vulkan)); if it ever wins
// the include search, an older SDK would break the VK_EXT_descriptor_heap
// declarations silently — fail the build instead.
static_assert(VK_HEADER_VERSION >= 357,
              "Vulkan headers older than the vendored 1.4.357 floor — the installed SDK's include dir "
              "won the include search over OloEngine/vendor's Vulkan-Headers (see vendor/CMakeLists.txt)");

namespace OloEngine
{
    namespace
    {
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan bring-up: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }

#ifdef OLO_DEBUG
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT /*types*/,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void* /*userData*/)
        {
            const char* message = (callbackData != nullptr && callbackData->pMessage != nullptr)
                                      ? callbackData->pMessage
                                      : "(no message)";
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            {
                OLO_CORE_ERROR("[Vulkan] {}", message);
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                OLO_CORE_WARN("[Vulkan] {}", message);
            }
            else
            {
                OLO_CORE_TRACE("[Vulkan] {}", message);
            }
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo()
        {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = DebugMessengerCallback;
            return info;
        }

        bool ValidationLayerAvailable()
        {
            u32 count = 0;
            if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
            {
                return false;
            }
            std::vector<VkLayerProperties> layers(count);
            if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
            {
                return false;
            }
            for (const VkLayerProperties& layer : layers)
            {
                if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation")
                {
                    return true;
                }
            }
            return false;
        }
#endif
    } // namespace

    struct VulkanContextData
    {
        static constexpr u32 kFramesInFlight = 2;

        VkInstance Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR Surface = VK_NULL_HANDLE;
        VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
        VkDevice Device = VK_NULL_HANDLE;
        u32 QueueFamily = 0;
        VkQueue Queue = VK_NULL_HANDLE;
        VmaAllocator Allocator = VK_NULL_HANDLE;

        VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
        VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D SwapchainExtent{};
        std::vector<VkImage> SwapchainImages;
        // Present-wait semaphores are PER SWAPCHAIN IMAGE, not per frame in flight:
        // vkQueuePresentKHR gives no way to know when its wait semaphore is done, so
        // re-signalling a per-frame one from a later submit trips
        // VUID-vkQueueSubmit-pSignalSemaphores-00067 (Khronos guide, "Swapchain
        // Semaphore Reuse"). The per-frame fence below is what legalises reusing the
        // per-frame ACQUIRE semaphore.
        std::vector<VkSemaphore> RenderFinished;

        VkCommandPool CommandPool = VK_NULL_HANDLE;
        struct Frame
        {
            VkSemaphore ImageAvailable = VK_NULL_HANDLE;
            VkFence InFlight = VK_NULL_HANDLE;
            VkCommandBuffer Cmd = VK_NULL_HANDLE;
        };
        Frame Frames[kFramesInFlight]{};
        u32 FrameIndex = 0;
    };

    VulkanContext::VulkanContext(GLFWwindow* windowHandle)
        : m_WindowHandle(windowHandle), m_Data(CreateScope<VulkanContextData>())
    {
        OLO_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    VulkanContext::~VulkanContext()
    {
        VulkanContextData& d = *m_Data;
        if (d.Device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(d.Device);

            DestroySwapchain();
            for (VulkanContextData::Frame& frame : d.Frames)
            {
                if (frame.ImageAvailable != VK_NULL_HANDLE)
                {
                    vkDestroySemaphore(d.Device, frame.ImageAvailable, nullptr);
                }
                if (frame.InFlight != VK_NULL_HANDLE)
                {
                    vkDestroyFence(d.Device, frame.InFlight, nullptr);
                }
            }
            if (d.CommandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(d.Device, d.CommandPool, nullptr);
            }
            if (d.Allocator != VK_NULL_HANDLE)
            {
                vmaDestroyAllocator(d.Allocator);
            }
            vkDestroyDevice(d.Device, nullptr);
        }
        if (d.Instance != VK_NULL_HANDLE)
        {
            if (d.Surface != VK_NULL_HANDLE)
            {
                vkDestroySurfaceKHR(d.Instance, d.Surface, nullptr);
            }
#ifdef OLO_DEBUG
            if (d.DebugMessenger != VK_NULL_HANDLE)
            {
                vkDestroyDebugUtilsMessengerEXT(d.Instance, d.DebugMessenger, nullptr);
            }
#endif
            vkDestroyInstance(d.Instance, nullptr);
        }
        OLO_CORE_INFO("[Vulkan] Context shut down cleanly");
    }

    void VulkanContext::Init()
    {
        OLO_PROFILE_FUNCTION();
        VulkanContextData& d = *m_Data;

        // --- Loader ---------------------------------------------------------
        if (volkInitialize() != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Vulkan bring-up: no Vulkan loader found on this system. Run with --rhi=opengl.");
        }
        // Hand GLFW volk's loader so it doesn't LoadLibrary vulkan-1 a second time
        // (GLFW >= 3.4 API; safe to call after glfwInit).
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
        if (glfwVulkanSupported() != GLFW_TRUE)
        {
            throw std::runtime_error(
                "Vulkan bring-up: GLFW reports no Vulkan surface support. Run with --rhi=opengl.");
        }

        // --- Instance -------------------------------------------------------
        u32 glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0)
        {
            throw std::runtime_error(
                "Vulkan bring-up: glfwGetRequiredInstanceExtensions returned nothing. Run with --rhi=opengl.");
        }
        std::vector<const char*> instanceExtensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        std::vector<const char*> instanceLayers;

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OloEngine";
        appInfo.pEngineName = "OloEngine";
        appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

#ifdef OLO_DEBUG
        // Chained into pNext so create/destroy of the instance itself is covered
        // before/after the persistent messenger exists.
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeDebugMessengerCreateInfo();
        const bool useValidation = ValidationLayerAvailable();
        if (useValidation)
        {
            instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            instanceInfo.pNext = &messengerInfo;
        }
        else
        {
            OLO_CORE_WARN("[Vulkan] VK_LAYER_KHRONOS_validation not available — running unvalidated");
        }
#endif
        instanceInfo.enabledExtensionCount = static_cast<u32>(instanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
        instanceInfo.enabledLayerCount = static_cast<u32>(instanceLayers.size());
        instanceInfo.ppEnabledLayerNames = instanceLayers.empty() ? nullptr : instanceLayers.data();

        VkCheck(vkCreateInstance(&instanceInfo, nullptr, &d.Instance), "vkCreateInstance");
        volkLoadInstance(d.Instance);

#ifdef OLO_DEBUG
        if (useValidation)
        {
            VkCheck(vkCreateDebugUtilsMessengerEXT(d.Instance, &messengerInfo, nullptr, &d.DebugMessenger),
                    "vkCreateDebugUtilsMessengerEXT");
        }
#endif

        // --- Surface --------------------------------------------------------
        VkCheck(glfwCreateWindowSurface(d.Instance, m_WindowHandle, nullptr, &d.Surface),
                "glfwCreateWindowSurface");

        // --- Physical device: gate HARD on ADR 0010's capability contract ----
        u32 deviceCount = 0;
        VkCheck(vkEnumeratePhysicalDevices(d.Instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
        if (deviceCount == 0)
        {
            throw std::runtime_error("Vulkan bring-up: no Vulkan devices present. Run with --rhi=opengl.");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        VkCheck(vkEnumeratePhysicalDevices(d.Instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

        // Find a queue family doing BOTH graphics and present. Split-family
        // hardware is refused for bring-up — every desktop GPU this backend's
        // hardware floor admits has a combined family, and supporting the split
        // doubles the sync/sharing surface for no Phase 4 gain.
        auto findCombinedQueueFamily = [&d](VkPhysicalDevice device) -> std::optional<u32>
        {
            u32 familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            for (u32 i = 0; i < familyCount; ++i)
            {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                {
                    continue;
                }
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, d.Surface, &presentSupport);
                if (presentSupport == VK_TRUE)
                {
                    return i;
                }
            }
            return std::nullopt;
        };

        VulkanCapabilityReport bestRejected;
        for (VkPhysicalDevice candidate : devices)
        {
            VulkanCapabilityReport report = VulkanCapabilities::Evaluate(candidate);
            const std::optional<u32> family = findCombinedQueueFamily(candidate);
            if (!family.has_value())
            {
                report.Missing.emplace_back("a combined graphics+present queue family");
                report.Satisfied = false;
            }
            if (report.Satisfied)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                // First satisfying discrete GPU wins; a satisfying integrated one is
                // kept only until a discrete appears.
                if (d.PhysicalDevice == VK_NULL_HANDLE ||
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    d.PhysicalDevice = candidate;
                    d.QueueFamily = *family;
                    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    {
                        break;
                    }
                }
            }
            else if (bestRejected.Missing.empty() || report.Missing.size() < bestRejected.Missing.size())
            {
                bestRejected = report;
            }
        }

        if (d.PhysicalDevice == VK_NULL_HANDLE)
        {
            // Refuse to initialise, naming the missing capability — never degrade
            // (ADR 0010; the OpenGL fallback is a USER decision, not an engine one).
            std::string missing;
            for (const std::string& item : bestRejected.Missing)
            {
                missing += (missing.empty() ? "" : ", ") + item;
            }
            throw std::runtime_error(
                "Vulkan bring-up: no device satisfies the capability contract (ADR 0010). Closest device '" +
                bestRejected.DeviceName + "' is missing: " + missing + ". Run with --rhi=opengl.");
        }

        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(d.PhysicalDevice, &properties);
            OLO_CORE_INFO("[Vulkan] Device: {} (driver {}.{}.{}, API {}.{}.{})", properties.deviceName,
                          VK_API_VERSION_MAJOR(properties.driverVersion), VK_API_VERSION_MINOR(properties.driverVersion),
                          VK_API_VERSION_PATCH(properties.driverVersion), VK_API_VERSION_MAJOR(properties.apiVersion),
                          VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
        }

        // --- Logical device + queue -----------------------------------------
        const f32 queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = d.QueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        // Enable the contract's feature bits now, not in Phase 5: a driver that
        // advertises the features but rejects enabling them should fail HERE, at
        // the gate, not later at first descriptor-heap use.
        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{};
        heapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        heapFeatures.descriptorHeap = VK_TRUE;
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedFeatures{};
        untypedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        untypedFeatures.shaderUntypedPointers = VK_TRUE;
        untypedFeatures.pNext = &heapFeatures;

        // synchronization2 backs the vkCmdPipelineBarrier2/vkQueueSubmit2 calls in
        // SwapBuffers. Core in 1.3 and MANDATORY for 1.3+ devices, so it needs no
        // capability-gate entry — but core-promoted features still default OFF at
        // device creation, and validation flags every sync2 call without this bit
        // (the NVIDIA driver happens to tolerate it, which is exactly the kind of
        // works-by-accident the validation layers exist to catch).
        VkPhysicalDeviceVulkan13Features vulkan13Features{};
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.synchronization2 = VK_TRUE;
        vulkan13Features.pNext = &untypedFeatures;

        const std::vector<const char*> deviceExtensions = VulkanCapabilities::RequiredDeviceExtensions();

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &vulkan13Features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();

        VkCheck(vkCreateDevice(d.PhysicalDevice, &deviceInfo, nullptr, &d.Device), "vkCreateDevice");
        volkLoadDevice(d.Device);
        vkGetDeviceQueue(d.Device, d.QueueFamily, 0, &d.Queue);

        // --- VMA (vendoring proof + the allocator Phase 5 inherits) ----------
        {
            VmaVulkanFunctions vulkanFunctions{};
            VmaAllocatorCreateInfo allocatorInfo{};
            allocatorInfo.physicalDevice = d.PhysicalDevice;
            allocatorInfo.device = d.Device;
            allocatorInfo.instance = d.Instance;
            allocatorInfo.vulkanApiVersion = VulkanCapabilities::kMinApiVersion;
            VkCheck(vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vulkanFunctions),
                    "vmaImportVulkanFunctionsFromVolk");
            allocatorInfo.pVulkanFunctions = &vulkanFunctions;
            VkCheck(vmaCreateAllocator(&allocatorInfo, &d.Allocator), "vmaCreateAllocator");
        }

        // --- Commands + per-frame sync ---------------------------------------
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = d.QueueFamily;
        VkCheck(vkCreateCommandPool(d.Device, &poolInfo, nullptr, &d.CommandPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = d.CommandPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait must not block

        for (VulkanContextData::Frame& frame : d.Frames)
        {
            VkCheck(vkAllocateCommandBuffers(d.Device, &cmdInfo, &frame.Cmd), "vkAllocateCommandBuffers");
            VkCheck(vkCreateSemaphore(d.Device, &semaphoreInfo, nullptr, &frame.ImageAvailable), "vkCreateSemaphore");
            VkCheck(vkCreateFence(d.Device, &fenceInfo, nullptr, &frame.InFlight), "vkCreateFence");
        }

        // --- Swapchain (+ its per-image semaphores) ---------------------------
        CreateSwapchain();

        OLO_CORE_INFO("[Vulkan] Bring-up context initialised ({} swapchain images, {}x{})",
                      m_Data->SwapchainImages.size(), m_Data->SwapchainExtent.width, m_Data->SwapchainExtent.height);
    }

    void VulkanContext::CreateSwapchain()
    {
        VulkanContextData& d = *m_Data;

        VkSurfaceCapabilitiesKHR caps{};
        VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.PhysicalDevice, d.Surface, &caps),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        // The bring-up clear uses vkCmdClearColorImage, which needs TRANSFER_DST on
        // the presentable images. Only COLOR_ATTACHMENT is spec-guaranteed, so this
        // is checked, not assumed (universal on desktop in practice).
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
        {
            throw std::runtime_error(
                "Vulkan bring-up: surface does not support TRANSFER_DST presentable images. Run with --rhi=opengl.");
        }

        u32 formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(d.PhysicalDevice, d.Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(d.PhysicalDevice, d.Surface, &formatCount, formats.data());
        VkSurfaceFormatKHR surfaceFormat = formats.at(0);
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                surfaceFormat = format;
                break;
            }
        }

        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(m_WindowHandle, &fbWidth, &fbHeight);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu)
        {
            extent.width = std::clamp(static_cast<u32>(fbWidth), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<u32>(fbHeight), caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        u32 imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0)
        {
            imageCount = std::min(imageCount, caps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR swapchainInfo{};
        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = d.Surface;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = surfaceFormat.format;
        swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
        swapchainInfo.imageExtent = extent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = caps.currentTransform;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        // FIFO is the one mode every conformant device carries; it is also vsync,
        // which matches the GL path's swap-interval default well enough for
        // bring-up. Window::SetVSync is a no-op under Vulkan until a real present
        // -mode policy exists (Phase 5+).
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;

        VkCheck(vkCreateSwapchainKHR(d.Device, &swapchainInfo, nullptr, &d.Swapchain), "vkCreateSwapchainKHR");
        d.SwapchainFormat = surfaceFormat.format;
        d.SwapchainExtent = extent;

        u32 actualImageCount = 0;
        vkGetSwapchainImagesKHR(d.Device, d.Swapchain, &actualImageCount, nullptr);
        d.SwapchainImages.resize(actualImageCount);
        vkGetSwapchainImagesKHR(d.Device, d.Swapchain, &actualImageCount, d.SwapchainImages.data());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        d.RenderFinished.resize(actualImageCount, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : d.RenderFinished)
        {
            VkCheck(vkCreateSemaphore(d.Device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        }
    }

    void VulkanContext::DestroySwapchain()
    {
        VulkanContextData& d = *m_Data;
        for (VkSemaphore semaphore : d.RenderFinished)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(d.Device, semaphore, nullptr);
            }
        }
        d.RenderFinished.clear();
        d.SwapchainImages.clear();
        if (d.Swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(d.Device, d.Swapchain, nullptr);
            d.Swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanContext::RecreateSwapchain()
    {
        VulkanContextData& d = *m_Data;
        // Blunt but correct without VK_KHR_swapchain_maintenance1: nothing may
        // reference the old swapchain or its per-image semaphores afterwards.
        vkDeviceWaitIdle(d.Device);
        DestroySwapchain();
        CreateSwapchain();
        OLO_CORE_INFO("[Vulkan] Swapchain recreated ({}x{})", d.SwapchainExtent.width, d.SwapchainExtent.height);
    }

    void VulkanContext::SwapBuffers()
    {
        OLO_PROFILE_FUNCTION();
        VulkanContextData& d = *m_Data;

        // Minimised: a 0-sized framebuffer cannot host a swapchain — skip frames
        // until the window has area again.
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(m_WindowHandle, &fbWidth, &fbHeight);
        if (fbWidth == 0 || fbHeight == 0)
        {
            return;
        }

        VulkanContextData::Frame& frame = d.Frames[d.FrameIndex];
        VkCheck(vkWaitForFences(d.Device, 1, &frame.InFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        u32 imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(d.Device, d.Swapchain, UINT64_MAX,
                                                             frame.ImageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            RecreateSwapchain();
            return;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        {
            VkCheck(acquireResult, "vkAcquireNextImageKHR");
        }

        // Only reset the fence once this frame will actually submit (a reset
        // without a submit would deadlock the next wait).
        VkCheck(vkResetFences(d.Device, 1, &frame.InFlight), "vkResetFences");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkResetCommandBuffer(frame.Cmd, 0), "vkResetCommandBuffer");
        VkCheck(vkBeginCommandBuffer(frame.Cmd, &beginInfo), "vkBeginCommandBuffer");

        VkImage image = d.SwapchainImages[imageIndex];
        VkImageSubresourceRange fullColor{};
        fullColor.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fullColor.levelCount = 1;
        fullColor.layerCount = 1;

        // UNDEFINED -> TRANSFER_DST: previous contents are irrelevant (we clear).
        VkImageMemoryBarrier2 toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = fullColor;

        VkDependencyInfo depInfo{};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(frame.Cmd, &depInfo);

        VkClearColorValue clearColor{};
        clearColor.float32[0] = kClearColor[0];
        clearColor.float32[1] = kClearColor[1];
        clearColor.float32[2] = kClearColor[2];
        clearColor.float32[3] = kClearColor[3];
        vkCmdClearColorImage(frame.Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &fullColor);

        // TRANSFER_DST -> PRESENT_SRC.
        VkImageMemoryBarrier2 toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toPresent.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = image;
        toPresent.subresourceRange = fullColor;

        depInfo.pImageMemoryBarriers = &toPresent;
        vkCmdPipelineBarrier2(frame.Cmd, &depInfo);

        VkCheck(vkEndCommandBuffer(frame.Cmd), "vkEndCommandBuffer");

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = frame.ImageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = d.RenderFinished[imageIndex];
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cmdSubmitInfo{};
        cmdSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdSubmitInfo.commandBuffer = frame.Cmd;

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        VkCheck(vkQueueSubmit2(d.Queue, 1, &submitInfo, frame.InFlight), "vkQueueSubmit2");

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &d.RenderFinished[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &d.Swapchain;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(d.Queue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        {
            RecreateSwapchain();
        }
        else
        {
            VkCheck(presentResult, "vkQueuePresentKHR");
        }

        d.FrameIndex = (d.FrameIndex + 1) % VulkanContextData::kFramesInFlight;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
