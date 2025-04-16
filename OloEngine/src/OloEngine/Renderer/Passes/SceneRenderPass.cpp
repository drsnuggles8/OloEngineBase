#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

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
                FramebufferTextureFormat::RGBA8,      // Color buffer
                FramebufferTextureFormat::Depth  	  // Depth attachment
            };
		}
        
        m_Target = Framebuffer::Create(m_FramebufferSpec);
		
        OLO_CORE_INFO("SceneRenderPass: Created framebuffer with dimensions {}x{}", 
			m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target)
        {
            OLO_CORE_ERROR("SceneRenderPass::Execute: No target framebuffer!");
            return;
        }
        
        m_Target->Bind();
        
        // Clear the framebuffer
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        // Reset to default OpenGL state to ensure consistent rendering
        auto& rendererAPI = RenderCommand::GetRendererAPI();
        rendererAPI.SetDepthTest(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        
		//m_CommandBucket.SortCommands();
		m_CommandBucket.Execute(rendererAPI);


        m_Target->Unbind();
    }

    Ref<Framebuffer> SceneRenderPass::GetTarget() const
    {
        return m_Target;
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
        
        // Create or recreate the framebuffer
        if (!m_Target)
        {
            Init(m_FramebufferSpec);
        }
        else
        {
            m_Target->Resize(width, height);
        }
    }

    void SceneRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }        
        
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_Target)
        {
            m_Target->Resize(width, height);
            OLO_CORE_INFO("SceneRenderPass: Resized framebuffer to {}x{}", width, height);
        }
    }

    void SceneRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // Recreate the framebuffer with current specs
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
			OLO_CORE_INFO("SceneRenderPass reset with framebuffer dimensions: {}x{}", 
				m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            Init(m_FramebufferSpec);
        }
    }
}