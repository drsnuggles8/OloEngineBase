#pragma once

// =============================================================================
// VulkanImGuiBackend — the Dear ImGui renderer backend on --rhi=vulkan
// (issue #691 Phase 8).
//
// Phase 7 ran the ImGui layer PLATFORM-ONLY under Vulkan (GLFW input, panel
// logic, no draw-data submission — see ImGuiLayer.cpp). This class wires the
// vendored imgui_impl_vulkan (ImGui 1.93 WIP, IMGUI_IMPL_VULKAN_USE_VOLK,
// dynamic rendering — compiled by ImGuiBuild.cpp) into the engine's frame
// loop:
//
//  - ImGuiLayer::OnAttach/Begin/End/OnDetach call Init/NewFrame/
//    NotifyDrawDataReady/Shutdown, mirroring the ImGui_ImplOpenGL3_* calls.
//  - VulkanContext::SwapBuffers calls RecordOverlay AFTER the frame callback
//    returned and BEFORE FinalizeBackbufferForPresent: the UI is recorded
//    into the frame's one command buffer, into its own dynamic-rendering
//    scope targeting the acquired swapchain image, on top of the 3D frame.
//  - The editor's ImGui::Image sites (scene viewport panel) get their
//    ImTextureID through GetTextureID: ImGui_ImplVulkan_AddTexture descriptor
//    sets cached per VkImage, invalidated by RegistrationId (resize mints a
//    new image), retired through a frames-in-flight delay ring.
//
// DESCRIPTOR-POOL NOTE (ADR 0011 §1.2a): imgui_impl_vulkan binds classic
// per-texture descriptor sets, not the engine's descriptor heap. That is
// deliberate and allowed — the heap-bindless rule governs ENGINE binding
// code, and §1.2a explicitly anticipated middleware wanting the classic
// path. The backend creates its own small internal VkDescriptorPool
// (InitInfo::DescriptorPoolSize), fully ImGui-internal middleware state.
//
// Multi-viewport stays DISABLED on Vulkan for now (ImGuiLayer gates
// ImGuiConfigFlags_ViewportsEnable on the GL backend) — follow-up work.
//
// Platform-internal header (includes <volk.h>), same convention as
// VulkanRendererAPI.h. ImGuiLayer.cpp includes it under the same
// "OLO_WITH_VULKAN-guarded factory TU" precedent as the Renderer factory
// files that include Platform/Vulkan/ headers.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <volk.h>

namespace OloEngine
{
    class VulkanRendererAPI;

    class VulkanImGuiBackend
    {
      public:
        VulkanImGuiBackend() = delete;

        // What RecordOverlay did with the frame (see VulkanContext::SwapBuffers).
        enum class OverlayResult : u8
        {
            Skipped,           ///< nothing recorded — the caller's rendered/fallback logic stands
            Drawn,             ///< UI recorded onto a written backbuffer; the caller's
                               ///< FinalizeBackbufferForPresent still owns the present transition
            DrawnAndPresented, ///< UI recorded onto an UNWRITTEN backbuffer (clear + UI) and the
                               ///< overlay issued the present transition itself — the caller must
                               ///< treat the frame as rendered and skip the fallback clear
        };

        // Initialise imgui_impl_vulkan against the live VulkanContext/Device.
        // Requires ImGui::CreateContext() and the GLFW platform backend to be
        // up (ImGuiLayer::OnAttach calls this right after
        // ImGui_ImplGlfw_InitForVulkan). Returns false — leaving the layer in
        // Phase 7 platform-only mode — when no context/device/swapchain
        // exists (e.g. minimised at startup: the swapchain format is unknown,
        // and the UI pipeline bakes it). Idempotent.
        [[nodiscard]] static bool Init();
        [[nodiscard]] static bool IsInitialized();

        // Per-frame hook (ImGuiLayer::Begin, Vulkan arm): ages the
        // descriptor-retirement ring, sweeps dead texture entries, then
        // ImGui_ImplVulkan_NewFrame().
        static void NewFrame();

        // ImGuiLayer::End (Vulkan arm) calls this right after ImGui::Render():
        // marks this frame's draw data as fresh. RecordOverlay consumes the
        // mark — without it, a declined frame (minimised, re-entrancy latch)
        // would re-record LAST frame's draw data over the fallback clear.
        static void NotifyDrawDataReady();

        // Record ImGui's draw data into the CURRENT frame command buffer
        // (api.CurrentCommandBuffer()), targeting the published backbuffer
        // view. Called by VulkanContext::SwapBuffers between the frame
        // callback and FinalizeBackbufferForPresent, i.e. inside the
        // recording bracket, after the 3D frame.
        //
        //  - Engine textures ImGui will sample this frame (GetTextureID
        //    registrations) are transitioned to SHADER_READ_ONLY via
        //    IssueBarrierBatch (tracker-exact oldLayout).
        //  - The backbuffer is barriered ColorAttachmentWrite ->
        //    ColorAttachmentWrite through the same batch: that closes any
        //    still-open facade rendering scope (barriers are illegal inside
        //    one), orders our loadOp LOAD against the frame's color writes,
        //    and — on the unwritten arm — transitions whatever stale layout
        //    the tracker holds into COLOR_ATTACHMENT.
        //  - `backbufferWritten` picks loadOp LOAD (UI over the 3D frame) vs
        //    CLEAR (UI over the bring-up clear colour when the frame callback
        //    declined but UI exists); on the unwritten arm the overlay also
        //    issues the ColorAttachmentWrite -> Present transition, since
        //    FinalizeBackbufferForPresent will report false for that frame.
        [[nodiscard]] static OverlayResult RecordOverlay(VulkanRendererAPI& api, VkImageView backbufferView,
                                                         VkExtent2D extent, RHI::ResourceHandle backbufferHandle,
                                                         bool backbufferWritten);

        // ImTextureID for an engine texture (a framebuffer color attachment,
        // a Texture2D) named by its RHI handle: a cached
        // ImGui_ImplVulkan_AddTexture descriptor set (whole-image mip-0
        // color view, SHADER_READ_ONLY layout). 0 when unresolvable (stale
        // handle, depth format, backend not initialised) — callers must skip
        // the ImGui::Image draw on 0 (a null descriptor set crashes the
        // vkCmdBindDescriptorSets in RenderDrawData; GL tolerated id 0).
        // Cache invalidation: a RegistrationId change (Resize recreates the
        // VkImage) retires the old view/descriptor through a
        // frames-in-flight-delayed ring.
        [[nodiscard]] static u64 GetTextureID(RHI::ResourceHandle textureHandle);

        // ImGuiLayer::OnDetach (Vulkan arm), BEFORE ImGui_ImplGlfw_Shutdown
        // and while the VulkanContext/device are still alive (the layer stack
        // clears before the Window — and with it the context — is destroyed).
        // Waits for device idle, retires every cached texture descriptor and
        // view, then ImGui_ImplVulkan_Shutdown().
        static void Shutdown();
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
