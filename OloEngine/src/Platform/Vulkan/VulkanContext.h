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

        // Acquire → clear (vkCmdClearColorImage) → present. Handles swapchain
        // recreation on resize (VK_ERROR_OUT_OF_DATE_KHR / suboptimal) and skips
        // the frame entirely while the framebuffer is 0-sized (minimised).
        void SwapBuffers() override;

        // The fixed bring-up clear colour (classic XNA cornflower blue — instantly
        // recognisable as "a cleared backbuffer", and nothing the GL editor draws).
        static constexpr f32 kClearColor[4] = { 0.392f, 0.584f, 0.929f, 1.0f };

      private:
        void CreateSwapchain();
        void DestroySwapchain();
        void RecreateSwapchain();

        GLFWwindow* m_WindowHandle;
        Scope<VulkanContextData> m_Data;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
