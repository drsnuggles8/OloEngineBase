#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GraphicsContext.h"

#if OLO_WITH_VULKAN

struct GLFWwindow;

namespace OloEngine
{
    // All Vk handles live behind this in the .cpp: a Platform/<Backend>/ include
    // leaks exactly as much as glad/gl.h (rhi-abstraction-boundary.md §2), and
    // GraphicsContext.cpp — engine code — includes this header for the factory.
    struct VulkanContextData;

    // #691 Phase 4 bring-up: instance / device / swapchain / per-frame sync, and a
    // SwapBuffers() that clears the backbuffer to a fixed colour and presents.
    // Since Phase 5 the window-independent half (instance, physical-device pick,
    // device, queue, VMA allocator, command pool) lives in VulkanDevice — this
    // class keeps the surface, swapchain, and frame-loop state.
    // Nothing else — no rendering, no descriptor heaps in use (device selection
    // gates on VK_EXT_descriptor_heap per ADR 0010, but Phase 5/6 are the first
    // consumers). Init() throws std::runtime_error to REFUSE initialisation when
    // the loader is absent or no device satisfies the capability contract; the
    // message names the missing capability and directs the user to --rhi=opengl.
    class VulkanContext : public GraphicsContext
    {
      public:
        explicit VulkanContext(GLFWwindow* windowHandle);
        ~VulkanContext() override;

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;
        VulkanContext(VulkanContext&&) = delete;
        VulkanContext& operator=(VulkanContext&&) = delete;

        void Init() override;

        // Acquire → record (frame callback when set, clear fallback otherwise)
        // → present. Handles swapchain recreation on resize
        // (VK_ERROR_OUT_OF_DATE_KHR / suboptimal) and skips the frame entirely
        // while the framebuffer is 0-sized (minimised).
        void SwapBuffers() override;

        // The Stage 1.6b render seam (see GraphicsContext). The callback runs
        // inside the global VulkanRendererAPI recording bracket with the
        // acquired swapchain image published as an RHI::ResourceHandle.
        void SetFrameRenderCallback(FrameRenderCallback callback) override
        {
            m_FrameRenderCallback = std::move(callback);
        }

        // The live context, or null outside a --rhi=vulkan session — the
        // VulkanDevice::Get() pattern. Consumers: the mid-frame flush below.
        [[nodiscard]] static VulkanContext* Get()
        {
            return s_Instance;
        }

        // #691 Phase 8: submit everything the frame has recorded so far and
        // WAIT for it, then re-enter the recording bracket on the reset
        // command buffer so the frame continues. This is what makes a
        // synchronous mid-frame readback (StorageBuffer::GetData between two
        // dispatches — the fluid solver's body-impulse coupling) read THIS
        // frame's data instead of last frame's: a one-shot submits before the
        // still-recording frame command buffer, in queue-submit order.
        //
        // Returns false (and does nothing) when there is nothing to flush or
        // flushing would be unsound: outside the SwapBuffers frame callback,
        // no live recording, an open occlusion query (a query span cannot
        // cross command buffers), or the backbuffer already written — the
        // acquire semaphore is a binary wait the FINAL submit owns, so a
        // flush containing swapchain-image work would need it first and rob
        // the real submit of it. Callers fall back to the one-shot path
        // (previous-frame data) on false; a full GPU stall on true is the
        // price GL always paid for glGetBufferSubData.
        [[nodiscard]] bool FlushFrameRecordingAndWait();

        // The fixed bring-up clear colour (classic XNA cornflower blue — instantly
        // recognisable as "a cleared backbuffer", and nothing the GL editor draws).
        static constexpr f32 kClearColor[4] = { 0.392f, 0.584f, 0.929f, 1.0f };

      private:
        void CreateSwapchain();
        void DestroySwapchain();
        void RecreateSwapchain();

        GLFWwindow* m_WindowHandle;
        Scope<VulkanContextData> m_Data;
        FrameRenderCallback m_FrameRenderCallback;
        /// Re-entrancy latch — see the nested-present guard in SwapBuffers.
        bool m_InSwapBuffers = false;

        inline static VulkanContext* s_Instance = nullptr;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
