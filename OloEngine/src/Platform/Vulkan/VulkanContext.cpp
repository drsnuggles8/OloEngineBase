#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanContext.h"
// VulkanDevice.h pulls <volk.h> (and <vk_mem_alloc.h>) — volk must come before
// GLFW: glfw3.h only declares its Vulkan entry points (glfwCreateWindowSurface
// et al.) when VK_VERSION_1_0 is already visible.
#include "Platform/Vulkan/VulkanDevice.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// The vendored Vulkan-Headers (SDK 1.4.357.0, the ADR 0010 tooling floor) must be
// the ones this TU compiles against. An installed SDK's include dir is also on the
// include path (via the shaderc toolchain's find_package(Vulkan)); if it ever wins
// the include search, an older SDK would break the VK_EXT_descriptor_heap
// declarations silently — fail the build instead. (Duplicated in VulkanDevice.cpp.)
static_assert(VK_HEADER_VERSION >= 357,
              "Vulkan headers older than the vendored 1.4.357 floor — the installed SDK's include dir "
              "won the include search over OloEngine/vendor's Vulkan-Headers (see vendor/CMakeLists.txt)");

namespace OloEngine
{
    namespace
    {
        // Kept in sync with VulkanDevice.cpp's copy (both anonymous-namespace,
        // trivially small — not worth a shared header).
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan bring-up: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }
    } // namespace

    struct VulkanContextData
    {
        static constexpr u32 kFramesInFlight = 2;

        // The window-independent half (volk init, instance, debug messenger,
        // physical-device pick, device, queue, VMA allocator, command pool)
        // lives in VulkanDevice since Phase 5 — this struct keeps only the
        // presentation / frame-loop state. The surface is created by this
        // context (via the surfaceProvider callback) and OWNED by it;
        // VulkanDevice only borrows the handle for the queue-family pick.
        VulkanDevice Device;
        VkSurfaceKHR Surface = VK_NULL_HANDLE;

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
        const VkDevice device = d.Device.GetDevice();
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);

            DestroySwapchain();
            for (VulkanContextData::Frame& frame : d.Frames)
            {
                if (frame.ImageAvailable != VK_NULL_HANDLE)
                {
                    vkDestroySemaphore(device, frame.ImageAvailable, nullptr);
                }
                if (frame.InFlight != VK_NULL_HANDLE)
                {
                    vkDestroyFence(device, frame.InFlight, nullptr);
                }
            }
        }
        // The surface is this context's to destroy, and it must go BEFORE the
        // instance — which Device.Shutdown() below is about to destroy (its
        // order: command pool -> VMA -> device -> messenger -> instance).
        if (d.Surface != VK_NULL_HANDLE && d.Device.GetInstance() != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(d.Device.GetInstance(), d.Surface, nullptr);
            d.Surface = VK_NULL_HANDLE;
        }
        d.Device.Shutdown();
        OLO_CORE_INFO("[Vulkan] Context shut down cleanly");
    }

    void VulkanContext::Init()
    {
        OLO_PROFILE_FUNCTION();
        VulkanContextData& d = *m_Data;

        // --- Loader ---------------------------------------------------------
        // VulkanDevice::Init runs volkInitialize itself (headless tests bring a
        // device up without a window); it also runs here FIRST so the GLFW
        // support probe below can fail fast — before any instance work — exactly
        // as the Phase 4 bring-up did. volkInitialize is safe to run twice.
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

        // --- Instance + device (the window-independent half) -----------------
        // The surface must exist AFTER the instance but BEFORE the physical-
        // device pick (present support is per queue family, per surface) —
        // hence the callback shape rather than create-then-pass. The surface
        // handle lands in d.Surface and stays OWNED by this context.
        d.Device.Init([this, &d](VkInstance instance) -> VkSurfaceKHR
                      {
            VkCheck(glfwCreateWindowSurface(instance, m_WindowHandle, nullptr, &d.Surface),
                    "glfwCreateWindowSurface");
            return d.Surface; });

        const VkDevice device = d.Device.GetDevice();

        // --- Commands + per-frame sync ---------------------------------------
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = d.Device.GetCommandPool();
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait must not block

        for (VulkanContextData::Frame& frame : d.Frames)
        {
            VkCheck(vkAllocateCommandBuffers(device, &cmdInfo, &frame.Cmd), "vkAllocateCommandBuffers");
            VkCheck(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.ImageAvailable), "vkCreateSemaphore");
            VkCheck(vkCreateFence(device, &fenceInfo, nullptr, &frame.InFlight), "vkCreateFence");
        }

        // --- Swapchain (+ its per-image semaphores) ---------------------------
        CreateSwapchain();

        OLO_CORE_INFO("[Vulkan] Bring-up context initialised ({} swapchain images, {}x{})",
                      m_Data->SwapchainImages.size(), m_Data->SwapchainExtent.width, m_Data->SwapchainExtent.height);
    }

    void VulkanContext::CreateSwapchain()
    {
        VulkanContextData& d = *m_Data;
        const VkPhysicalDevice physicalDevice = d.Device.GetPhysicalDevice();
        const VkDevice device = d.Device.GetDevice();

        VkSurfaceCapabilitiesKHR caps{};
        VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, d.Surface, &caps),
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
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, d.Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, d.Surface, &formatCount, formats.data());
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

        // A minimised window can reach here with a 0-sized surface (Init while
        // minimised, or an OUT_OF_DATE recreate racing a minimise) — a zero
        // extent is invalid for vkCreateSwapchainKHR. Leave the swapchain null;
        // SwapBuffers retries the creation once the framebuffer has area again.
        if (extent.width == 0 || extent.height == 0)
        {
            return;
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

        VkCheck(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &d.Swapchain), "vkCreateSwapchainKHR");
        d.SwapchainFormat = surfaceFormat.format;
        d.SwapchainExtent = extent;

        u32 actualImageCount = 0;
        vkGetSwapchainImagesKHR(device, d.Swapchain, &actualImageCount, nullptr);
        d.SwapchainImages.resize(actualImageCount);
        vkGetSwapchainImagesKHR(device, d.Swapchain, &actualImageCount, d.SwapchainImages.data());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        d.RenderFinished.resize(actualImageCount, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : d.RenderFinished)
        {
            VkCheck(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        }
    }

    void VulkanContext::DestroySwapchain()
    {
        VulkanContextData& d = *m_Data;
        const VkDevice device = d.Device.GetDevice();
        for (VkSemaphore semaphore : d.RenderFinished)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        d.RenderFinished.clear();
        d.SwapchainImages.clear();
        if (d.Swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, d.Swapchain, nullptr);
            d.Swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanContext::RecreateSwapchain()
    {
        VulkanContextData& d = *m_Data;
        // Blunt but correct without VK_KHR_swapchain_maintenance1: nothing may
        // reference the old swapchain or its per-image semaphores afterwards.
        vkDeviceWaitIdle(d.Device.GetDevice());
        DestroySwapchain();
        CreateSwapchain();
        OLO_CORE_INFO("[Vulkan] Swapchain recreated ({}x{})", d.SwapchainExtent.width, d.SwapchainExtent.height);
    }

    void VulkanContext::SwapBuffers()
    {
        OLO_PROFILE_FUNCTION();
        VulkanContextData& d = *m_Data;
        const VkDevice device = d.Device.GetDevice();

        // Minimised: a 0-sized framebuffer cannot host a swapchain — skip frames
        // until the window has area again.
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(m_WindowHandle, &fbWidth, &fbHeight);
        if (fbWidth == 0 || fbHeight == 0)
        {
            return;
        }

        // The window has area but no swapchain exists: creation was skipped while
        // the surface was 0-sized (see CreateSwapchain). Recreate now; if the
        // surface still reports zero (caps lag the framebuffer), skip the frame.
        if (d.Swapchain == VK_NULL_HANDLE)
        {
            RecreateSwapchain();
            if (d.Swapchain == VK_NULL_HANDLE)
            {
                return;
            }
        }

        VulkanContextData::Frame& frame = d.Frames[d.FrameIndex];
        VkCheck(vkWaitForFences(device, 1, &frame.InFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        u32 imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(device, d.Swapchain, UINT64_MAX,
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
        VkCheck(vkResetFences(device, 1, &frame.InFlight), "vkResetFences");

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
        // srcStageMask must be the ACQUIRE SEMAPHORE'S WAIT STAGE (CLEAR, see
        // waitInfo.stageMask below), not TOP_OF_PIPE: the semaphore orders
        // this submission against the presentation engine's read only at the
        // wait stage, and a layout transition whose srcStage is earlier than
        // that can begin before the wait — a WRITE_AFTER_READ hazard against
        // vkAcquireNextImageKHR. Phase 4 shipped TOP_OF_PIPE here and passed,
        // because only core validation ran; Phase 5's synchronization
        // validation flagged it on its first live run (issue #691).
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
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
        VkCheck(vkQueueSubmit2(d.Device.GetQueue(), 1, &submitInfo, frame.InFlight), "vkQueueSubmit2");

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &d.RenderFinished[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &d.Swapchain;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(d.Device.GetQueue(), &presentInfo);
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
