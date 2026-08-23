#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanContext.h"
// VulkanDevice.h pulls <volk.h> (and <vk_mem_alloc.h>) — volk must come before
// GLFW: glfw3.h only declares its Vulkan entry points (glfwCreateWindowSurface
// et al.) when VK_VERSION_1_0 is already visible.
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanGpuFence.h"
#include "Platform/Vulkan/VulkanImGuiBackend.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanSamplerHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
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
                if (result == VK_ERROR_DEVICE_LOST)
                {
                    // Get the driver's fault report (address + fault type)
                    // into the log BEFORE the throw unwinds — the crash
                    // handler only sees the VkResult.
                    if (auto* device = VulkanDevice::Get())
                    {
                        device->LogDeviceFaultInfo();
                    }
                }
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
        // lives in VulkanDevice — this struct keeps only the
        // presentation / frame-loop state. The surface is created by this
        // context (via the surfaceProvider callback) and OWNED by it;
        // VulkanDevice only borrows the handle for the queue-family pick.
        VulkanDevice Device;
        VkSurfaceKHR Surface = VK_NULL_HANDLE;

        VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
        VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D SwapchainExtent{};
        // The minImageCount requested at vkCreateSwapchainKHR — the ImGui
        // renderer backend's InitInfo wants it alongside the actual image
        // count (#691).
        u32 SwapchainMinImageCount = 0;
        std::vector<VkImage> SwapchainImages;
        // Attachment views for dynamic rendering (#691 — the frame
        // loop renders now, it no longer only clears).
        std::vector<VkImageView> SwapchainViews;
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

        // --- #691 (Stage 1.6b) render seam ---------------------------
        // Each swapchain image published as neutral handle currency for the
        // frame callback (replacing the pilot pass, whose golden proof
        // now lives in VulkanPassSuiteTest): an adopted RHI handle plus a
        // VulkanImageInfoRegistry entry make the backbuffer reachable by
        // IssueBarrierBatch / the layout tracker exactly like any texture.
        std::vector<RHI::ScopedResourceHandle> SwapchainImageHandles;
    };

    VulkanContext::VulkanContext(GLFWwindow* windowHandle)
        : m_WindowHandle(windowHandle), m_Data(CreateScope<VulkanContextData>())
    {
        OLO_CORE_ASSERT(windowHandle, "Window handle is null!");
        OLO_CORE_ASSERT(s_Instance == nullptr, "A VulkanContext already exists");
        s_Instance = this;
    }

    VulkanContext::~VulkanContext()
    {
        s_Instance = nullptr;
        VulkanContextData& d = *m_Data;
        const VkDevice device = d.Device.GetDevice();
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);

            // The frame arena's slot buffers enqueue into the deferred-reclaim
            // queue; drain it NOW, while the device is provably idle — nothing
            // else flushes the queue at shutdown (it was built without a
            // production drain), and entries surviving past Device.Shutdown()
            // are dropped with a leak warning instead of destroyed.
            VulkanPipelineBuilder::Get().ReleaseAll();
            // The raw-facade registries (deliberately leaked singletons) hold
            // the LAST Ref on adopted textures/framebuffers a caller never
            // Destroy()ed — release them here so they retire through the
            // reclaim drain below; left alone they outlive vmaDestroyAllocator
            // and abort (review finding, #691). Framebuffers first:
            // they hold Refs to the textures.
            VulkanRawFramebufferRegistry::Get().ReleaseAll();
            VulkanRawTextureRegistry::Get().ReleaseAll();
            // Snapshot clones (#810) are owned separately — see
            // VulkanRawImageRegistry's class comment.
            VulkanRawImageRegistry::Get().ReleaseAll();
            // The engine heap's slots index the resource heap below — retire
            // them first (amendment (33): state must not outlive what gives
            // it meaning).
            RHI::DescriptorHeap::Get().Shutdown();
            VulkanResourceHeap::Get().Release(); // cascades to the sampler heap
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanDeferredReclaim::Get().FlushAll();
            // Serialise + destroy the process-wide pipeline cache while the
            // device is still alive (§3(c): a failed save is a log line).
            VulkanPipelineCache::Get().SaveAndDestroy();

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
        // Anything still in the root registry at this point outlived the
        // full teardown — name the owners (Debug) before the allocator
        // teardown turns them into a bare VMA leak count, and force-release
        // surviving shaders' modules so a stray static Ref cannot leak them
        // into vkDestroyDevice.
        VulkanRootObjectRegistry::Get().LogSurvivingVertexArrays();
        VulkanRootObjectRegistry::Get().ReleaseSurvivingShaderModules();
        VulkanLogSurvivingTransients();
        // Second (final) reclaim drain: objects destroyed AFTER the FlushAll
        // above — swapchain teardown, late Ref releases on other threads —
        // re-populate the queue, and an entry that reaches
        // vmaDestroyAllocator is the "allocations not freed" abort. The
        // device is idle (nothing has submitted since the wait above), so
        // the flush precondition holds.
        VulkanDeferredReclaim::Get().FlushAll();
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
        // as the original bring-up did. volkInitialize is safe to run twice.
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

        // --- Engine descriptor heap (#691) ----------------------------
        // RHI::DescriptorHeap runs on this backend too: slots [0,
        // kDescriptorHeapSlots) of the resource heap belong to it (reserved
        // BEFORE any draw-path slot-cache use), with the GL-parity
        // capacities. Failure leaves the engine heap disabled — every caller
        // falls back to the slot path, same graceful shape as a GL device
        // without ARB_bindless_texture.
        if (!VulkanDescriptorHeapBackend::InstallOntoEngineHeap())
        {
            OLO_CORE_WARN("[Vulkan] engine descriptor heap unavailable — slot-path fallback stays in effect");
        }

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
        d.SwapchainMinImageCount = imageCount;

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
        // -mode policy exists.
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;

        VkCheck(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &d.Swapchain), "vkCreateSwapchainKHR");
        d.SwapchainFormat = surfaceFormat.format;
        d.SwapchainExtent = extent;

        u32 actualImageCount = 0;
        vkGetSwapchainImagesKHR(device, d.Swapchain, &actualImageCount, nullptr);
        d.SwapchainImages.resize(actualImageCount);
        vkGetSwapchainImagesKHR(device, d.Swapchain, &actualImageCount, d.SwapchainImages.data());

        // Attachment views for dynamic rendering (#691).
        d.SwapchainViews.resize(actualImageCount, VK_NULL_HANDLE);
        for (u32 i = 0; i < actualImageCount; ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = d.SwapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = d.SwapchainFormat;
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkCheck(vkCreateImageView(device, &viewInfo, nullptr, &d.SwapchainViews[i]), "vkCreateImageView (swapchain)");
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        d.RenderFinished.resize(actualImageCount, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : d.RenderFinished)
        {
            VkCheck(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        }

        // Publish each presentable image as neutral handle currency (the
        // Stage 1.6b render seam): metadata for the barrier lowering, and an
        // adopted RHI handle the frame callback can name in Barrier batches.
        // Register() stamps a fresh RegistrationId, so a tracker following a
        // driver-recycled VkImage value detects the new image.
        d.SwapchainImageHandles.resize(actualImageCount);
        for (u32 i = 0; i < actualImageCount; ++i)
        {
            VulkanImageInfo info{};
            info.Format = d.SwapchainFormat;
            info.Width = extent.width;
            info.Height = extent.height;
            info.MipLevels = 1;
            info.ArrayLayers = 1;
            VulkanImageInfoRegistry::Get().Register(d.SwapchainImages[i], info);
            d.SwapchainImageHandles[i].Adopt(RHI::ResourceKind::Texture,
                                             reinterpret_cast<u64>(d.SwapchainImages[i]), RHI::Backend::Vulkan);
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
        for (VkImageView view : d.SwapchainViews)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        d.SwapchainViews.clear();
        // Retire the published identities BEFORE the images die: the RAII
        // handles unregister from the RHI registry, and the info-registry
        // entries go so a later swapchain reusing a handle VALUE re-registers
        // with a fresh RegistrationId.
        d.SwapchainImageHandles.clear();
        for (VkImage image : d.SwapchainImages)
        {
            VulkanImageInfoRegistry::Get().Unregister(image);
        }
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

    bool VulkanContext::FlushFrameRecordingAndWait()
    {
        if (!m_InSwapBuffers)
        {
            return false;
        }
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        if (api.BackbufferWasWrittenThisRecording())
        {
            // The acquire semaphore is a single-wait binary the FINAL submit
            // owns; swapchain-image work in a flush would need it first.
            // TRACE, not WARN: the caller's borrow-mode fallback (previous-
            // frame read, ReadTextureSubImage) is the DESIGNED behavior for
            // this case — it is GL's double-buffered PBO pick semantics —
            // so a viewport click after the final blit lands here in every
            // editor session by design.
            static bool s_Traced = false;
            if (!s_Traced)
            {
                s_Traced = true;
                OLO_CORE_TRACE("[Vulkan] mid-frame flush declined (backbuffer already written) — the caller "
                               "reads previous-frame contents instead, GL's double-buffered pick semantics");
            }
            return false;
        }
        const VkCommandBuffer cmd = api.SuspendRecordingForFlush();
        if (cmd == VK_NULL_HANDLE)
        {
            return false;
        }

        VulkanContextData& d = *m_Data;
        const VkDevice device = d.Device.GetDevice();

        // Runtime path, not bring-up: no VkCheck throws here. Whatever
        // fails, the fence must not leak and the recording bracket must be
        // re-opened, so the caller can fall back to the one-shot
        // (previous-frame) read on false and the frame can continue.
        bool ok = true;
        VkResult result = vkEndCommandBuffer(cmd);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[Vulkan] flush: vkEndCommandBuffer failed (VkResult {})", static_cast<int>(result));
            ok = false;
        }

        VkFence fence = VK_NULL_HANDLE;
        if (ok)
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
            if (result != VK_SUCCESS)
            {
                OLO_CORE_ERROR("[Vulkan] flush: vkCreateFence failed (VkResult {})", static_cast<int>(result));
                fence = VK_NULL_HANDLE;
                ok = false;
            }
        }
        if (ok)
        {
            VkCommandBufferSubmitInfo cmdInfo{};
            cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmdInfo.commandBuffer = cmd;
            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmdInfo;
            // Deliberately NO semaphores: the acquire wait and the present signal
            // belong to the frame's final submit, and any staged RHI::GpuFence
            // queue ops stay staged for it too — this submission is an ordering
            // detail inside the frame, invisible to frame pacing.
            result = vkQueueSubmit2(d.Device.GetQueue(), 1, &submit, fence);
            if (result != VK_SUCCESS)
            {
                OLO_CORE_ERROR("[Vulkan] flush: vkQueueSubmit2 failed (VkResult {})", static_cast<int>(result));
                ok = false;
            }
            else
            {
                constexpr u64 kTimeoutNs = 10'000'000'000ull; // 10 s — a flush slower than this is a hang
                result = vkWaitForFences(device, 1, &fence, VK_TRUE, kTimeoutNs);
                if (result == VK_TIMEOUT)
                {
                    // The submission may STILL be executing, and the
                    // vkResetCommandBuffer below on a PENDING command buffer
                    // is invalid usage (review finding) — so wait it out
                    // rather than recycle under it. A truly hung GPU turns
                    // into DEVICE_LOST via the OS watchdog (Windows TDR),
                    // which exits this wait and makes the reset legal
                    // (pending buffers become invalid-state on device loss).
                    OLO_CORE_ERROR("[Vulkan] flush: vkWaitForFences timed out after 10 s — waiting for "
                                   "completion or device loss before recycling the command buffer");
                    result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
                }
                else
                {
                    // The recording is on the queue AND retired: recorded and
                    // executed layouts have converged (issue #800). This is
                    // deliberately NOT gated on the aggregate `ok` below — a
                    // failure to REOPEN the bracket does not un-execute what
                    // the GPU already ran, and leaving the executed layout
                    // stale would make the fallback read barrier from a layout
                    // the image is no longer in.
                    api.LayoutTracker().CommitRecordedToExecuted();
                }
                if (result != VK_SUCCESS)
                {
                    OLO_CORE_ERROR("[Vulkan] flush: vkWaitForFences failed (VkResult {})", static_cast<int>(result));
                    if (result == VK_ERROR_DEVICE_LOST)
                    {
                        d.Device.LogDeviceFaultInfo();
                    }
                    else
                    {
                        // An error wait (OOM) proves nothing about the fence:
                        // destroying a possibly-in-flight fence is invalid
                        // usage (VUID-vkDestroyFence-fence-01120).
                        // Deliberately leak this one instead — the leak beats
                        // UB. (On DEVICE_LOST the destroy below is legal.)
                        fence = VK_NULL_HANDLE;
                    }
                    ok = false;
                }
            }
        }
        if (fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, fence, nullptr);
        }

        // Re-open the recording bracket on EVERY path (reset also recovers a
        // command buffer left invalid by a failed end above), and always hand
        // it back to the API so its recording state stays consistent.
        result = vkResetCommandBuffer(cmd, 0);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[Vulkan] flush: vkResetCommandBuffer failed (VkResult {})", static_cast<int>(result));
            ok = false;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(cmd, &beginInfo);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[Vulkan] flush: vkBeginCommandBuffer failed (VkResult {})", static_cast<int>(result));
            ok = false;
        }
        api.ResumeRecordingAfterFlush(cmd);
        return ok;
    }

    u32 VulkanContext::GetSwapchainImageCount() const
    {
        return static_cast<u32>(m_Data->SwapchainImages.size());
    }

    u32 VulkanContext::GetSwapchainMinImageCount() const
    {
        return m_Data->SwapchainMinImageCount;
    }

    u32 VulkanContext::GetSwapchainColorFormat() const
    {
        return static_cast<u32>(m_Data->SwapchainFormat);
    }

    void VulkanContext::SwapBuffers()
    {
        OLO_PROFILE_FUNCTION();
        VulkanContextData& d = *m_Data;
        const VkDevice device = d.Device.GetDevice();

        // NESTED PRESENT. On GL, SwapBuffers is a pure present and calling it
        // from inside frame work is merely odd; here it owns acquire, the
        // frame's ONE command buffer, submit and present, so a nested call
        // resets a command buffer that the outer call is still recording into
        // and re-opens an already-open recording bracket. The engine does
        // present from inside frame work — ShaderWarmup's progress screen
        // swaps once per compiled shader, and Renderer3D::Init is reachable
        // from a layer callback — so this is a real path, not a theoretical
        // one. The nested present is simply dropped: the outer frame is still
        // in flight and will present.
        if (m_InSwapBuffers)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("[Vulkan] nested SwapBuffers ignored (a present issued from inside frame "
                              "recording — e.g. the shader-warmup progress screen)");
            }
            return;
        }
        m_InSwapBuffers = true;
        struct SwapLatch
        {
            bool& Flag;
            ~SwapLatch()
            {
                Flag = false;
            }
        } swapLatch{ m_InSwapBuffers };

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

        // This fence wait is the one point in the frame that PROVES the GPU is
        // done with frame slot FrameIndex (kFramesInFlight ago). It is therefore
        // the only correct place to advance the deferred-reclaim generation —
        // The queue was built but nothing in production drained it, so a
        // --rhi=vulkan session leaked every enqueued resource until exit.
        // NOTE: the acquire below can abort this frame without a submit
        // (OUT_OF_DATE), so this slot's generation advance can happen twice
        // against one submission. That is safe ONLY because the abort path
        // goes through RecreateSwapchain, whose vkDeviceWaitIdle drains every
        // in-flight submission first — if RecreateSwapchain ever loses that
        // wait (VK_KHR_swapchain_maintenance1 migration), this advance must
        // move to after a successful vkQueueSubmit2 instead.
        VulkanDeferredReclaim::Get().NotifyFrameCompleted();
        // Same gate for the root-data arena: slot FrameIndex's bump cursor may
        // only rewind once the GPU can no longer read the slot's memory.
        VulkanFrameArena::Get().BeginFrame(d.FrameIndex);

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

        // #691 (Stage 1.6b): the render seam that replaced the
        // pilot. The frame callback records through the global facade inside
        // this bracket, with the acquired backbuffer published as neutral
        // handle currency; its contract is to finish with a barrier to
        // RHI::Access::Present. The original clear path stays as the fallback
        // when no callback is installed (or it declines the frame).
        bool rendered = false;
        if (m_FrameRenderCallback)
        {
            auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            api.BeginRecording(frame.Cmd);
            // Publish the acquired image as this recording's DEFAULT
            // framebuffer: FinalRenderPass's BindDefaultFramebuffer (GL's
            // glBindFramebuffer(0)) resolves to it, and the lazy rendering
            // scope opens against it exactly as it does against a
            // VulkanFramebuffer. The publication dies with the recording.
            api.SetFrameBackbuffer(d.SwapchainImageHandles[imageIndex].Get(), d.SwapchainViews[imageIndex],
                                   d.SwapchainExtent.width, d.SwapchainExtent.height);
            const GraphicsContext::FrameRenderTarget target{
                d.SwapchainImageHandles[imageIndex].Get(),
                d.SwapchainExtent.width,
                d.SwapchainExtent.height,
            };
            bool callbackRendered = false;
            try
            {
                callbackRendered = m_FrameRenderCallback(target);
            }
            catch (const std::exception& e)
            {
                // The callback runs arbitrary engine code inside an OPEN
                // command buffer; letting an exception escape would leave the
                // bracket unbalanced and take the process down with it. Log
                // and fall back to the clear frame instead.
                OLO_CORE_ERROR("[Vulkan] frame render callback threw: {} — falling back to the clear frame", e.what());
                callbackRendered = false;
            }
            // --- ImGui overlay (#691) ------------------------------
            // The UI is recorded INSIDE this bracket, after the 3D frame,
            // before the present transition. ImGuiLayer::End produced the
            // draw data during the frame callback above (RenderFrameLayers
            // runs inside it); RecordOverlay opens its own dynamic-rendering
            // scope on the acquired backbuffer view — loadOp LOAD over a
            // written frame, loadOp CLEAR (the bring-up colour) when the
            // callback declined but UI exists, in which case the overlay also
            // owns the transition to Present (Finalize below reports false
            // for a frame whose facade scope never opened). This seam — a
            // direct call between the frame callback and Finalize — was
            // chosen over a second registered callback: both live in
            // Platform/Vulkan, the overlay needs backend types (view,
            // extent, the facade), and a std::function adds a seam nothing
            // else would ever plug into.
            const VulkanImGuiBackend::OverlayResult overlay = VulkanImGuiBackend::RecordOverlay(
                api, d.SwapchainViews[imageIndex], d.SwapchainExtent, d.SwapchainImageHandles[imageIndex].Get(),
                api.BackbufferWasWrittenThisRecording());

            // Owning the present transition here (rather than asking the
            // callback for it) keeps swapchain-layout knowledge inside the
            // presenting backend, and lets the fallback below stay correct:
            // it returns false ONLY when nothing touched the image, so the
            // fallback's UNDEFINED oldLayout can never discard real work.
            rendered = api.FinalizeBackbufferForPresent(callbackRendered);
            if (overlay == VulkanImGuiBackend::OverlayResult::DrawnAndPresented)
            {
                // The overlay cleared + drew the UI and transitioned the
                // image to Present itself — the frame holds defined content,
                // so the clear-only fallback below must not run (it would
                // clobber the UI via its UNDEFINED oldLayout).
                rendered = true;
            }
            api.EndRecording();
        }
        if (!rendered)
        {
            // UNDEFINED -> TRANSFER_DST: previous contents are irrelevant (we clear).
            VkImageMemoryBarrier2 toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            // srcStageMask must be the ACQUIRE SEMAPHORE'S WAIT STAGE (CLEAR, see
            // waitInfo.stageMask below), not TOP_OF_PIPE: the semaphore orders
            // this submission against the presentation engine's read only at the
            // wait stage, and a layout transition whose srcStage is earlier than
            // that can begin before the wait — a WRITE_AFTER_READ hazard against
            // vkAcquireNextImageKHR. Bring-up shipped TOP_OF_PIPE here and passed,
            // because only core validation ran; the synchronization
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
        }

        VkCheck(vkEndCommandBuffer(frame.Cmd), "vkEndCommandBuffer");

        // Wait/signal lists: the swapchain's own binary pair plus any staged
        // RHI::GpuFence queue ops (ADR 0011 §6 — a split-barrier Signal/Wait
        // attaches to the frame's one submission; this drain is what makes
        // GpuFence live in the real loop, not just the device-gated test).
        std::vector<VkSemaphoreSubmitInfo> waitInfos;
        std::vector<VkSemaphoreSubmitInfo> signalInfos;
        VulkanGpuFence::DrainPendingSubmitOps(waitInfos, signalInfos);

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = frame.ImageAvailable;
        // The wait stage must cover the first swapchain-image access in the
        // submission (the sync-validation rule): CLEAR on the
        // fallback path; with a frame callback the first access is whatever
        // the callback recorded, so ALL_COMMANDS is the conservative cover
        // (refine when the real frame loop settles on a fixed first stage).
        waitInfo.stageMask =
            rendered ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_CLEAR_BIT;
        waitInfos.push_back(waitInfo);
        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = d.RenderFinished[imageIndex];
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signalInfos.push_back(signalInfo);
        VkCommandBufferSubmitInfo cmdSubmitInfo{};
        cmdSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdSubmitInfo.commandBuffer = frame.Cmd;

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = static_cast<u32>(waitInfos.size());
        submitInfo.pWaitSemaphoreInfos = waitInfos.data();
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
        submitInfo.signalSemaphoreInfoCount = static_cast<u32>(signalInfos.size());
        submitInfo.pSignalSemaphoreInfos = signalInfos.data();
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
