#pragma once

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#include <GLFW/glfw3.h>

namespace OloEngine::Tests
{
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
