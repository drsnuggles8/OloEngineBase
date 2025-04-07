#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Renderer3D.h"

namespace OloEngine
{
    SceneRenderPass::SceneRenderPass()
    {
        SetName("SceneRenderPass");
		OLO_CORE_INFO("Creating SceneRenderPass.");
    }

    void SceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;
        
        // Ensure the specification includes color and depth attachments
        if (m_FramebufferSpec.Attachments.Attachments.empty())
        {
            OLO_CORE_WARN("SceneRenderPass::Init: No attachments specified, adding default color and depth attachments");
            m_FramebufferSpec.Attachments = { 
                FramebufferTextureFormat::RGBA8,       // Color attachment
                FramebufferTextureFormat::Depth        // Depth attachment
            };
        }
        
        // Create the framebuffer for this pass
        m_Target = Framebuffer::Create(m_FramebufferSpec);
        
        OLO_CORE_INFO("SceneRenderPass initialized with framebuffer dimensions: {}x{}", 
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Bind the target framebuffer
        if (!m_Target)
        {
            OLO_CORE_ERROR("SceneRenderPass::Execute: No target framebuffer!");
            return;
        }
        
        m_Target->Bind();
        
        // Clear the framebuffer
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        // Execute the render queue to render all queued objects to this framebuffer
        RenderQueue::Flush();

        // Unbind the framebuffer so it can be used as a texture by subsequent passes
        m_Target->Unbind();
    }

    void SceneRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::SetupFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        m_Target = Framebuffer::Create(m_FramebufferSpec);
        
        OLO_CORE_INFO("SceneRenderPass framebuffer set up with dimensions: {}x{}", width, height);
    }

    void SceneRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        if (m_Target)
        {
            m_FramebufferSpec.Width = width;
            m_FramebufferSpec.Height = height;
            m_Target->Resize(width, height);            
            OLO_CORE_INFO("SceneRenderPass framebuffer resized to: {}x{}", width, height);
        }
        else
        {
            OLO_CORE_WARN("SceneRenderPass::ResizeFramebuffer: No target framebuffer, creating new one");
            SetupFramebuffer(width, height);
        }
    }

    void SceneRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // Recreate the framebuffer if needed
        m_Target = Framebuffer::Create(m_FramebufferSpec);
        OLO_CORE_INFO("SceneRenderPass reset with framebuffer dimensions: {}x{}", 
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }
}
