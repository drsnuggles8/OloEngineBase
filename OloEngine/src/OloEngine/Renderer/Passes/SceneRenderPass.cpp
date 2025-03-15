#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderQueue.h"

namespace OloEngine
{
    SceneRenderPass::SceneRenderPass()
    {
        SetName("SceneRenderPass");
    }

    void SceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;
        
        // Create the framebuffer for this pass
        m_Target = Framebuffer::Create(m_FramebufferSpec);
    }

    void SceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Bind the target framebuffer
        m_Target->Bind();
        
        // Clear the framebuffer
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        // Execute the render queue - this is where all the rendering happens
        RenderQueue::Flush();

        // Unbind the framebuffer so it can be used as a texture
        m_Target->Unbind();
    }

    void SceneRenderPass::SetupFramebuffer(uint32_t width, uint32_t height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Target = Framebuffer::Create(m_FramebufferSpec);
    }

    void SceneRenderPass::ResizeFramebuffer(uint32_t width, uint32_t height)
    {
        if (m_Target)
        {
            m_FramebufferSpec.Width = width;
            m_FramebufferSpec.Height = height;
            m_Target->Resize(width, height);
        }
    }

    void SceneRenderPass::OnReset()
    {
        // Recreate the framebuffer if needed
        m_Target = Framebuffer::Create(m_FramebufferSpec);
    }
} 