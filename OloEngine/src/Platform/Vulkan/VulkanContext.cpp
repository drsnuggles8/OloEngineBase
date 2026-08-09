#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanContext.h"
// VulkanDevice.h pulls <volk.h> (and <vk_mem_alloc.h>) — volk must come before
// GLFW: glfw3.h only declares its Vulkan entry points (glfwCreateWindowSurface
// et al.) when VK_VERSION_1_0 is already visible.
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanGpuFence.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
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
        // Attachment views for dynamic rendering (#691 Phase 6 — the frame
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

        // --- #691 Phase 6 pilot pass -----------------------------------------
        // The first REAL rendered content in the Vulkan frame loop: the
        // golden-tested FXAA pass over an uploaded hard-edge pattern, drawn
        // fullscreen into the swapchain through the root-data-pointer +
        // thin-PSO + vertex-pulling path (the same path the device-gated
        // VulkanShaderPipelineTest holds to the GL golden). Deliberately
        // scaffolding: Phase 7 replaces this with render-graph execution; the
        // clear-only Phase 4 path remains the fallback when the pilot cannot
        // come up (missing assets when cwd isn't OloEditor/, shader failure).
        struct Phase6Pilot
        {
            bool Attempted = false;
            bool Ready = false;
            bool PatternUploaded = false;
            Ref<Shader> FxaaShader;
            VkImage Pattern = VK_NULL_HANDLE;
            VmaAllocation PatternAlloc = VK_NULL_HANDLE;
            u32 HeapSlot = 0;
            VulkanRootDataLayout Layout;
            static constexpr u32 kPatternSize = 128;
        };
        Phase6Pilot Pilot;
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

            // The frame arena's slot buffers enqueue into the deferred-reclaim
            // queue; drain it NOW, while the device is provably idle — nothing
            // else flushes the queue at shutdown (Phase 5 built it without a
            // production drain), and entries surviving past Device.Shutdown()
            // are dropped with a leak warning instead of destroyed.
            // Pilot teardown first: dropping the shader Ref invalidates its
            // pipelines (erase + enqueue, exactly once — the builder owns
            // both maps); the pattern image joins the reclaim queue.
            d.Pilot.FxaaShader = nullptr;
            if (d.Pilot.Pattern != VK_NULL_HANDLE)
            {
                VulkanDeferredReclaim::Get().Enqueue(d.Pilot.Pattern, d.Pilot.PatternAlloc);
                d.Pilot.Pattern = VK_NULL_HANDLE;
                d.Pilot.PatternAlloc = VK_NULL_HANDLE;
            }
            VulkanPipelineBuilder::Get().ReleaseAll();
            // The engine heap's slots index the resource heap below — retire
            // them first (amendment (33): state must not outlive what gives
            // it meaning).
            RHI::DescriptorHeap::Get().Shutdown();
            VulkanResourceHeap::Get().Release();
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

        // --- Engine descriptor heap (#691 Phase 7) ----------------------------
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

        // Attachment views for dynamic rendering (#691 Phase 6).
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

    namespace
    {
        // --- #691 Phase 6 pilot pass (see VulkanContextData::Phase6Pilot) ----

        void EnsurePhase6Pilot(VulkanContextData& d)
        {
            auto& pilot = d.Pilot;
            if (pilot.Attempted)
            {
                return;
            }
            pilot.Attempted = true;

            Ref<Shader> shader = Shader::Create("assets/shaders/PostProcess_FXAA.glsl");
            if (!shader || shader->GetCompilationStatus() != ShaderCompilationStatus::Ready)
            {
                OLO_CORE_WARN("[Vulkan] Phase 6 pilot: FXAA shader unavailable — staying on the clear-only frame");
                return;
            }
            auto* vkShader = static_cast<VulkanShader*>(shader.get());
            pilot.Layout = VulkanRootDataLayout::Build(vkShader->GetBindings());
            if (pilot.Layout.Find(0, 7) == nullptr || pilot.Layout.Find(0, 0) == nullptr ||
                pilot.Layout.Find(0, 57) == nullptr)
            {
                OLO_CORE_WARN("[Vulkan] Phase 6 pilot: FXAA reflection missing expected bindings — clear-only frame");
                return;
            }

            // The hard-edge input pattern the FXAA golden pins.
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = { VulkanContextData::Phase6Pilot::kPatternSize,
                                 VulkanContextData::Phase6Pilot::kPatternSize, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            if (vmaCreateImage(d.Device.GetAllocator(), &imageInfo, &allocInfo, &pilot.Pattern, &pilot.PatternAlloc,
                               nullptr) != VK_SUCCESS)
            {
                OLO_CORE_WARN("[Vulkan] Phase 6 pilot: pattern image creation failed — clear-only frame");
                return;
            }

            if (!VulkanResourceHeap::Get().EnsureCreated())
            {
                OLO_CORE_WARN("[Vulkan] Phase 6 pilot: resource heap unavailable — clear-only frame");
                return;
            }
            pilot.HeapSlot = VulkanResourceHeap::Get().AllocateSlot();
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = pilot.Pattern;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (pilot.HeapSlot == VulkanResourceHeap::InvalidSlot ||
                !VulkanResourceHeap::Get().WriteSampledImage(pilot.HeapSlot, viewInfo,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                OLO_CORE_WARN("[Vulkan] Phase 6 pilot: heap descriptor write failed — clear-only frame");
                return;
            }

            pilot.FxaaShader = shader;
            pilot.Ready = true;
            OLO_CORE_INFO("[Vulkan] Phase 6 pilot pass up: FXAA over the golden hard-edge pattern "
                          "(root-data pointer + thin PSO + vertex pulling)");
        }

        // Records the pilot frame. Returns false when the pilot is not
        // available (caller keeps the Phase 4 clear path).
        bool TryRecordPhase6Pilot(VulkanContextData& d, VkCommandBuffer cmd, u32 imageIndex)
        {
            EnsurePhase6Pilot(d);
            auto& pilot = d.Pilot;
            if (!pilot.Ready)
            {
                return false;
            }
            constexpr u32 kSize = VulkanContextData::Phase6Pilot::kPatternSize;
            auto& arena = VulkanFrameArena::Get();

            VulkanResourceHeap::Get().CmdBind(cmd);

            // One-time pattern upload, staged through the frame arena.
            if (!pilot.PatternUploaded)
            {
                std::vector<u8> rgba8(static_cast<sizet>(kSize) * kSize * 4);
                for (u32 y = 0; y < kSize; ++y)
                {
                    for (u32 x = 0; x < kSize; ++x)
                    {
                        const bool bright = (x + (y % 8)) >= (kSize / 2 + ((y / 8) % 2) * 4);
                        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
                        rgba8[i + 0] = bright ? 255 : 0;
                        rgba8[i + 1] = bright ? 255 : 0;
                        rgba8[i + 2] = bright ? 255 : 0;
                        rgba8[i + 3] = 255;
                    }
                }
                const auto staging = arena.Push(rgba8.data(), rgba8.size(), 16);
                if (!staging.IsValid())
                {
                    return false;
                }
                VkImageMemoryBarrier2 toDst{};
                toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.image = pilot.Pattern;
                toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &toDst;
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferImageCopy region{};
                region.bufferOffset = staging.Offset;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { kSize, kSize, 1 };
                vkCmdCopyBufferToImage(cmd, arena.GetSlotBuffer(arena.GetCurrentSlot()), pilot.Pattern,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                VkImageMemoryBarrier2 toRead = toDst;
                toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                dep.pImageMemoryBarriers = &toRead;
                vkCmdPipelineBarrier2(cmd, &dep);
                pilot.PatternUploaded = true;
            }

            // Per-frame root data: UBO payload, pulled vertices, root struct.
            auto* vkShader = static_cast<VulkanShader*>(pilot.FxaaShader.get());
            PostProcessUBOData ubo{};
            ubo.TexelSizeX = 1.0f / static_cast<f32>(kSize);
            ubo.TexelSizeY = 1.0f / static_cast<f32>(kSize);
            ubo.InverseScreenWidth = 1.0f / static_cast<f32>(d.SwapchainExtent.width);
            ubo.InverseScreenHeight = 1.0f / static_cast<f32>(d.SwapchainExtent.height);
            const auto uboAlloc = arena.Push(&ubo, sizeof(ubo), 256);
            const f32 fullscreenTriangle[] = {
                -1.0f,
                -1.0f,
                0.0f,
                0.0f,
                0.0f, //
                3.0f,
                -1.0f,
                0.0f,
                2.0f,
                0.0f, //
                -1.0f,
                3.0f,
                0.0f,
                0.0f,
                2.0f, //
            };
            const auto pullAlloc = arena.Push(fullscreenTriangle, sizeof(fullscreenTriangle), 16);
            std::vector<u8> rootData(pilot.Layout.SizeBytes, 0);
            if (!uboAlloc.IsValid() || !pullAlloc.IsValid())
            {
                return false;
            }
            const u64 uboAddress = uboAlloc.Gpu;
            const u64 pullAddress = pullAlloc.Gpu;
            std::memcpy(rootData.data() + pilot.Layout.Find(0, 7)->Offset, &uboAddress, sizeof(u64));
            std::memcpy(rootData.data() + pilot.Layout.Find(0, 57)->Offset, &pullAddress, sizeof(u64));
            std::memcpy(rootData.data() + pilot.Layout.Find(0, 0)->Offset, &pilot.HeapSlot, sizeof(u32));
            const auto rootAlloc = arena.Push(rootData.data(), rootData.size(), 16);
            if (!rootAlloc.IsValid())
            {
                return false;
            }

            VulkanRenderTargetDesc targets;
            targets.ColorCount = 1;
            targets.ColorFormats[0] = d.SwapchainFormat;
            VulkanRecordedPipelineState state{};
            state.DepthTest = false;
            state.DepthWrite = false;
            const VkPipeline pipeline =
                VulkanPipelineBuilder::Get().GetOrCreateGraphics(*vkShader, pilot.Layout, state, targets);
            if (pipeline == VK_NULL_HANDLE)
            {
                return false;
            }

            // Acquire wait stage is COLOR_ATTACHMENT_OUTPUT on this path —
            // the barrier's srcStage must match it (the Phase 5 sync-
            // validation rule, same reasoning as the clear path's CLEAR).
            VkImageMemoryBarrier2 toColor{};
            toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toColor.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor.image = d.SwapchainImages[imageIndex];
            toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &toColor;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = d.SwapchainViews[imageIndex];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = { { VulkanContext::kClearColor[0], VulkanContext::kClearColor[1],
                                         VulkanContext::kClearColor[2], VulkanContext::kClearColor[3] } };
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = { { 0, 0 }, d.SwapchainExtent };
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(cmd, &rendering);

            const VkViewport viewport{ 0.0f, 0.0f, static_cast<f32>(d.SwapchainExtent.width),
                                       static_cast<f32>(d.SwapchainExtent.height), 0.0f, 1.0f };
            vkCmdSetViewportWithCount(cmd, 1, &viewport);
            const VkRect2D scissor{ { 0, 0 }, d.SwapchainExtent };
            vkCmdSetScissorWithCount(cmd, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VulkanPipelineBuilder::FlushDynamicState(cmd, state, targets);
            VkPushDataInfoEXT pushInfo{};
            pushInfo.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
            pushInfo.offset = 0;
            VkDeviceAddress rootAddress = rootAlloc.Gpu;
            pushInfo.data = { .address = &rootAddress, .size = sizeof(rootAddress) };
            vkCmdPushDataEXT(cmd, &pushInfo);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            VkImageMemoryBarrier2 toPresent{};
            toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.image = d.SwapchainImages[imageIndex];
            toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            dep.pImageMemoryBarriers = &toPresent;
            vkCmdPipelineBarrier2(cmd, &dep);
            return true;
        }
    } // namespace

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

        // This fence wait is the one point in the frame that PROVES the GPU is
        // done with frame slot FrameIndex (kFramesInFlight ago). It is therefore
        // the only correct place to advance the deferred-reclaim generation —
        // Phase 5 built the queue but nothing in production drained it, so a
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

        // #691 Phase 6: the pilot pass renders (FXAA over the golden pattern,
        // root-data pointer + thin PSO + vertex pulling); the Phase 4 clear
        // path stays as the fallback when the pilot cannot come up.
        const bool pilotActive = TryRecordPhase6Pilot(d, frame.Cmd, imageIndex);
        if (!pilotActive)
        {
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
        // The wait stage must equal the first swapchain-image barrier's
        // srcStageMask (see the comment there): CLEAR on the fallback path,
        // COLOR_ATTACHMENT_OUTPUT when the pilot renders.
        waitInfo.stageMask = pilotActive ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                         : VK_PIPELINE_STAGE_2_CLEAR_BIT;
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
