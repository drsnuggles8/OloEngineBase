#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/CommandSceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

namespace OloEngine
{
    CommandSceneRenderPass::CommandSceneRenderPass()
    {
        SetName("CommandSceneRenderPass");
        OLO_CORE_INFO("Creating CommandSceneRenderPass.");
    }

    void CommandSceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;
        
		// Ensure the specification includes color and depth attachments
		if (m_FramebufferSpec.Attachments.Attachments.empty())
		{
            OLO_CORE_WARN("CommandSceneRenderPass::Init: No attachments specified, adding default color and depth attachments");
            m_FramebufferSpec.Attachments = {
                FramebufferTextureFormat::RGBA8,      // Color buffer
                FramebufferTextureFormat::Depth  	  // Depth attachment
            };
		}
        
        m_Target = Framebuffer::Create(m_FramebufferSpec);
		
        OLO_CORE_INFO("CommandSceneRenderPass: Created framebuffer with dimensions {}x{}", 
			m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void CommandSceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target)
        {
            OLO_CORE_ERROR("CommandSceneRenderPass::Execute: No target framebuffer!");
            return;
        }
        
        m_Target->Bind();
        
        // Clear the framebuffer
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        // Execute all commands in the bucket
        if (m_Allocator)
        {
            m_CommandBucket.SortCommands();
            m_CommandBucket.Execute(RenderCommand::GetRendererAPI());
        }
        else
        {
            OLO_CORE_WARN("CommandSceneRenderPass::Execute: No command allocator available");
        }

        m_Target->Unbind();
    }

    void CommandSceneRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CommandSceneRenderPass::SetupFramebuffer: Invalid dimensions: {}x{}", width, height);
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

    void CommandSceneRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CommandSceneRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }        
        
        if (m_Target)
        {
			m_FramebufferSpec.Width = width;
			m_FramebufferSpec.Height = height;
            m_Target->Resize(width, height);
            OLO_CORE_INFO("CommandSceneRenderPass: Resized framebuffer to {}x{}", width, height);
        }
    }

    void CommandSceneRenderPass::OnReset()
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