#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanImGuiBackend.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <imgui.h>
// The engine's Vulkan calls all go through volk's function pointers; the
// vendored imgui_impl_vulkan supports sharing them via this macro (it makes
// the backend header include <volk.h> instead of <vulkan/vulkan.h>). The
// backend TU itself is compiled with the same macro by ImGuiBuild.cpp.
#define IMGUI_IMPL_VULKAN_USE_VOLK
#include <backends/imgui_impl_vulkan.h>

#include <span>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // One cached ImGui texture binding per engine VkImage (see
        // GetTextureID). The view is OURS (whole-image mip-0 color view —
        // the engine's own attachment views live on the resource classes and
        // are not reachable through a neutral RHI handle); the descriptor set
        // belongs to imgui_impl_vulkan's internal pool.
        struct TextureEntry
        {
            VkImageView View = VK_NULL_HANDLE;
            VkDescriptorSet Set = VK_NULL_HANDLE;
            u64 RegistrationId = 0;
            RHI::ResourceHandle Handle{};
            bool UsedThisFrame = false;
        };

        // Retirement ring: a descriptor set drawn LAST frame may still be
        // referenced by an in-flight command buffer, so RemoveTexture (which
        // frees the set immediately) must wait frames-in-flight + 1 NewFrame
        // ticks. The view rides along and goes through VulkanDeferredReclaim
        // for the same reason.
        struct PendingRetire
        {
            VkDescriptorSet Set = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            u32 FramesLeft = 0;
        };
        constexpr u32 kRetireDelayFrames = 3; // kFramesInFlight (2) + 1

        bool s_Initialized = false;
        bool s_DrawDataReady = false;
        // Must OUTLIVE Init: ImGui_ImplVulkan_Init shallow-copies the
        // InitInfo, and PipelineRenderingCreateInfo.pColorAttachmentFormats
        // is dereferenced at (re)pipeline creation.
        VkFormat s_SwapchainFormat = VK_FORMAT_UNDEFINED;
        std::unordered_map<VkImage, TextureEntry> s_Textures;
        std::vector<PendingRetire> s_PendingRetires;

        void ImGuiVkCheck(VkResult err)
        {
            if (err != VK_SUCCESS)
            {
                OLO_CORE_ERROR("[ImGui/Vulkan] imgui_impl_vulkan call failed (VkResult {})", static_cast<int>(err));
            }
        }

        void RetireEntry(const TextureEntry& entry)
        {
            s_PendingRetires.push_back({ entry.Set, entry.View, kRetireDelayFrames });
        }

        void FlushPendingRetires(bool immediate)
        {
            for (auto it = s_PendingRetires.begin(); it != s_PendingRetires.end();)
            {
                if (!immediate && it->FramesLeft > 0)
                {
                    --it->FramesLeft;
                    ++it;
                    continue;
                }
                if (it->Set != VK_NULL_HANDLE)
                {
                    ImGui_ImplVulkan_RemoveTexture(it->Set);
                }
                if (it->View != VK_NULL_HANDLE)
                {
                    if (immediate)
                    {
                        // Shutdown path — caller guarantees device idle.
                        if (auto* device = VulkanDevice::Get(); device != nullptr && device->GetDevice() != VK_NULL_HANDLE)
                        {
                            vkDestroyImageView(device->GetDevice(), it->View, nullptr);
                        }
                    }
                    else
                    {
                        VulkanDeferredReclaim::Get().Enqueue(it->View);
                    }
                }
                it = s_PendingRetires.erase(it);
            }
        }
    } // namespace

    bool VulkanImGuiBackend::Init()
    {
        if (s_Initialized)
        {
            return true;
        }
        VulkanContext* context = VulkanContext::Get();
        VulkanDevice* device = VulkanDevice::Get();
        if (context == nullptr || device == nullptr || device->GetDevice() == VK_NULL_HANDLE)
        {
            return false;
        }
        const u32 imageCount = context->GetSwapchainImageCount();
        s_SwapchainFormat = static_cast<VkFormat>(context->GetSwapchainColorFormat());
        if (imageCount == 0 || s_SwapchainFormat == VK_FORMAT_UNDEFINED)
        {
            // No swapchain yet (window minimised at startup): the UI pipeline
            // bakes the swapchain color format, so initialisation cannot
            // proceed. The layer stays in platform-only mode.
            return false;
        }

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VulkanCapabilities::kMinApiVersion;
        info.Instance = device->GetInstance();
        info.PhysicalDevice = device->GetPhysicalDevice();
        info.Device = device->GetDevice();
        info.QueueFamily = device->GetQueueFamily();
        info.Queue = device->GetQueue();
        // ImGui's classic per-texture descriptor sets are ImGui-internal
        // middleware state: the engine's heap-bindless rule (ADR 0010/0011)
        // governs ENGINE binding code, and ADR 0011 §1.2a explicitly
        // anticipated middleware wanting the classic path. DescriptorPoolSize
        // makes the backend create its own small internal pool — nothing of
        // it leaks into VulkanResourceHeap / the engine descriptor heap.
        info.DescriptorPoolSize = 256;
        info.MinImageCount = std::max(context->GetSwapchainMinImageCount(), 2u);
        info.ImageCount = std::max(imageCount, info.MinImageCount);
        // No VkRenderPass exists on this backend — the overlay records into a
        // dynamic-rendering scope on the swapchain image (RecordOverlay), so
        // the UI pipeline is built against the swapchain format via
        // PipelineRenderingCreateInfo.
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_SwapchainFormat;
        info.CheckVkResultFn = &ImGuiVkCheck;

        if (!ImGui_ImplVulkan_Init(&info))
        {
            OLO_CORE_ERROR("[ImGui/Vulkan] ImGui_ImplVulkan_Init failed — staying in platform-only mode");
            return false;
        }
        s_Initialized = true;
        OLO_CORE_INFO("[ImGui/Vulkan] renderer backend initialised (#691): dynamic rendering, "
                      "format {}, {} swapchain images",
                      static_cast<u32>(s_SwapchainFormat), imageCount);
        return true;
    }

    bool VulkanImGuiBackend::IsInitialized()
    {
        return s_Initialized;
    }

    void VulkanImGuiBackend::NewFrame()
    {
        if (!s_Initialized)
        {
            return;
        }
        FlushPendingRetires(false);

        // Sweep dead texture entries: a Resize recreated the VkImage (new
        // RegistrationId under a possibly-recycled handle value) or the image
        // is gone entirely. Also reset the per-frame used marks that
        // RecordOverlay's barrier pass consumes.
        for (auto it = s_Textures.begin(); it != s_Textures.end();)
        {
            const VulkanImageInfo* info = VulkanImageInfoRegistry::Get().Lookup(it->first);
            if (info == nullptr || info->RegistrationId != it->second.RegistrationId)
            {
                RetireEntry(it->second);
                it = s_Textures.erase(it);
                continue;
            }
            it->second.UsedThisFrame = false;
            ++it;
        }

        ImGui_ImplVulkan_NewFrame();
    }

    void VulkanImGuiBackend::NotifyDrawDataReady()
    {
        if (s_Initialized)
        {
            s_DrawDataReady = true;
        }
    }

    u64 VulkanImGuiBackend::GetTextureID(const RHI::ResourceHandle textureHandle)
    {
        if (!s_Initialized)
        {
            return 0;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(textureHandle);
        if (native == 0u)
        {
            return 0;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const VulkanImageInfo* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr || info->HasDepth || info->HasStencil || info->ViewType != VK_IMAGE_VIEW_TYPE_2D)
        {
            // Depth targets and 3D volumes need a dedicated visualisation
            // path, not a straight ImGui sample — refuse rather than bind a
            // view Vulkan would reject.
            return 0;
        }

        auto it = s_Textures.find(image);
        if (it != s_Textures.end())
        {
            if (it->second.RegistrationId == info->RegistrationId)
            {
                it->second.UsedThisFrame = true;
                return reinterpret_cast<u64>(it->second.Set);
            }
            // Same handle value, different image (driver recycle) — retire
            // the stale binding and rebuild below.
            RetireEntry(it->second);
            s_Textures.erase(it);
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr || device->GetDevice() == VK_NULL_HANDLE)
        {
            return 0;
        }
        // Mip 0 / layer 0 only: it is what the editor shows, and a
        // single-subresource view keeps the SHADER_READ_ONLY layout contract
        // exact for RecordOverlay's pre-sample barrier (one run, one layout).
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info->Format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS)
        {
            OLO_CORE_WARN("[ImGui/Vulkan] vkCreateImageView failed for an ImGui texture binding");
            return 0;
        }
        const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (set == VK_NULL_HANDLE)
        {
            vkDestroyImageView(device->GetDevice(), view, nullptr);
            return 0;
        }
        s_Textures.emplace(image, TextureEntry{ view, set, info->RegistrationId, textureHandle, true });
        return reinterpret_cast<u64>(set);
    }

    VulkanImGuiBackend::OverlayResult VulkanImGuiBackend::RecordOverlay(VulkanRendererAPI& api,
                                                                        const VkImageView backbufferView,
                                                                        const VkExtent2D extent,
                                                                        const RHI::ResourceHandle backbufferHandle,
                                                                        const bool backbufferWritten)
    {
        if (!s_Initialized)
        {
            return OverlayResult::Skipped;
        }
        // Consume the freshness mark unconditionally: stale draw data (a
        // declined frame — minimised, the re-entrancy latch) must not be
        // re-recorded over the fallback clear.
        const bool ready = s_DrawDataReady;
        s_DrawDataReady = false;
        if (!ready || backbufferView == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0)
        {
            return OverlayResult::Skipped;
        }
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr || !drawData->Valid || drawData->CmdListsCount == 0)
        {
            return OverlayResult::Skipped;
        }
        const VkCommandBuffer cmd = api.CurrentCommandBuffer();
        if (cmd == VK_NULL_HANDLE)
        {
            return OverlayResult::Skipped;
        }

        // --- Barrier batch -------------------------------------------------
        // (a) Engine textures ImGui samples this frame (viewport image) must
        //     sit in SHADER_READ_ONLY — the layout their descriptors were
        //     registered with. The tracker supplies the exact oldLayout, so a
        //     texture the frame already left in SHADER_READ_ONLY is skipped.
        // (b) The backbuffer: ColorAttachmentWrite -> ColorAttachmentWrite.
        //     This one barrier does three jobs — it closes any still-open
        //     facade rendering scope (vkCmdPipelineBarrier2 is illegal inside
        //     one, and IssueBarrierBatch ends the scope first), it orders our
        //     loadOp LOAD against the frame's color writes (WAW/RAW between
        //     rendering instances needs an explicit dependency), and on the
        //     unwritten arm it transitions whatever stale layout the tracker
        //     holds (PRESENT from a previous cycle of this swapchain image,
        //     or UNDEFINED) into COLOR_ATTACHMENT.
        std::vector<RHI::Barrier> barriers;
        const VkImageSubresourceRange baseRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (auto& [image, entry] : s_Textures)
        {
            if (!entry.UsedThisFrame)
            {
                continue;
            }
            entry.UsedThisFrame = false;
            if (api.LayoutTracker().CurrentLayout(image, baseRange) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                continue;
            }
            RHI::Barrier toSample{};
            toSample.Resource = entry.Handle;
            toSample.Range.BaseMip = 0u;
            toSample.Range.MipCount = 1u;
            toSample.Range.BaseLayer = 0u;
            toSample.Range.LayerCount = 1u;
            // Before only contributes source masks (the tracker owns
            // oldLayout); ColorAttachmentWrite covers the common producer —
            // these images are framebuffer color attachments.
            toSample.Before = RHI::Access::ColorAttachmentWrite;
            toSample.After = RHI::Access::ShaderSampleRead;
            barriers.push_back(toSample);
        }
        {
            RHI::Barrier toColor{};
            toColor.Resource = backbufferHandle;
            toColor.Range.BaseMip = 0u;
            toColor.Range.MipCount = 1u;
            toColor.Range.BaseLayer = 0u;
            toColor.Range.LayerCount = 1u;
            toColor.Before = RHI::Access::ColorAttachmentWrite;
            toColor.After = RHI::Access::ColorAttachmentWrite;
            barriers.push_back(toColor);
        }
        api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ barriers.data(), barriers.size() });

        // --- The UI rendering scope ---------------------------------------
        // LOAD over a written frame (UI chrome on top of the 3D image);
        // CLEAR to the bring-up colour when the frame callback declined but
        // the UI exists (the editor keeps its panels through a declined
        // scene frame instead of losing them to the raw fallback clear).
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = backbufferView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = backbufferWritten ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = { { VulkanContext::kClearColor[0], VulkanContext::kClearColor[1],
                                               VulkanContext::kClearColor[2], VulkanContext::kClearColor[3] } };

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = { { 0, 0 }, extent };
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
        vkCmdEndRendering(cmd);

        if (backbufferWritten)
        {
            // FinalizeBackbufferForPresent (called next by SwapBuffers) sees
            // m_BackbufferWritten == true and issues the Present transition;
            // our scope left the image in COLOR_ATTACHMENT, which is exactly
            // the Before it lowers from. Nothing more to do here.
            return OverlayResult::Drawn;
        }

        // Unwritten arm: Finalize will report false (the facade never opened
        // a backbuffer scope), so the present transition is ours — through
        // the facade, so the layout tracker stays exact.
        RHI::Barrier toPresent{};
        toPresent.Resource = backbufferHandle;
        toPresent.Range.BaseMip = 0u;
        toPresent.Range.MipCount = 1u;
        toPresent.Range.BaseLayer = 0u;
        toPresent.Range.LayerCount = 1u;
        toPresent.Before = RHI::Access::ColorAttachmentWrite;
        toPresent.After = RHI::Access::Present;
        api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ &toPresent, 1 });
        return OverlayResult::DrawnAndPresented;
    }

    void VulkanImGuiBackend::Shutdown()
    {
        if (!s_Initialized)
        {
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device != nullptr && device->GetDevice() != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device->GetDevice());
        }
        for (auto& entry : s_Textures)
        {
            RetireEntry(entry.second);
        }
        s_Textures.clear();
        FlushPendingRetires(true); // device idle — RemoveTexture + destroy views inline
        ImGui_ImplVulkan_Shutdown();
        s_Initialized = false;
        s_DrawDataReady = false;
        s_SwapchainFormat = VK_FORMAT_UNDEFINED;
        OLO_CORE_INFO("[ImGui/Vulkan] renderer backend shut down");
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
