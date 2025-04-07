#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/CommandSceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

namespace OloEngine
{
    CommandSceneRenderPass::CommandSceneRenderPass()
    {
        SetName("CommandSceneRenderPass");
        OLO_CORE_INFO("Creating CommandSceneRenderPass");
    }

    void CommandSceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;
        
        // Create framebuffer with appropriate attachments for a G-buffer
        FramebufferSpecification fbSpec = spec;
        fbSpec.Attachments = FramebufferAttachmentSpecification({
            FramebufferTextureSpecification(FramebufferTextureFormat::RGBA8),      // Color buffer
            FramebufferTextureSpecification(FramebufferTextureFormat::DEPTH24STENCIL8)  // Depth-stencil buffer
        });
        
        m_Target = Framebuffer::Create(fbSpec);
        OLO_CORE_INFO("CommandSceneRenderPass: Created framebuffer with dimensions {}x{}", 
                       spec.Width, spec.Height);
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
        
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        if (m_Target)
        {
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
            Init(m_FramebufferSpec);
        }
    }

    void CommandSceneRenderPass::SetCamera(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();
        
        m_Camera = camera;
        m_CameraTransform = transform;
        
        // Calculate the view and projection matrices
        m_ViewMatrix = glm::inverse(m_CameraTransform);
        m_ProjectionMatrix = m_Camera.GetProjection();
        
        m_HasValidCamera = true;
    }

    void CommandSceneRenderPass::BeginRender()
    {
        OLO_PROFILE_FUNCTION();
        
        // Call the base class implementation to bind the framebuffer
        CommandRenderPass::BeginRender();
        
        // Clear the framebuffer
        ClearCommand clearCmd;
        clearCmd.clearColor = true;
        clearCmd.clearDepth = true;
        
        if (m_Allocator)
        {
            CommandPacket* packet = m_Allocator->CreateCommandPacket(clearCmd);
            if (packet)
            {
                packet->Execute(RenderCommand::GetRendererAPI());
            }
        }
        
        // Set up the viewport to match the framebuffer size
        if (m_Target)
        {
            SetViewportCommand viewportCmd;
            viewportCmd.x = 0;
            viewportCmd.y = 0;
            viewportCmd.width = m_Target->GetSpecification().Width;
            viewportCmd.height = m_Target->GetSpecification().Height;
            
            if (m_Allocator)
            {
                CommandPacket* packet = m_Allocator->CreateCommandPacket(viewportCmd);
                if (packet)
                {
                    packet->Execute(RenderCommand::GetRendererAPI());
                }
            }
        }
    }

    void CommandSceneRenderPass::EndRender()
    {
        OLO_PROFILE_FUNCTION();
        
        // Call the base class implementation to unbind the framebuffer
        CommandRenderPass::EndRender();
    }

    void CommandSceneRenderPass::BuildCommandBucket(CommandBucket& bucket)
    {
        // No scene iteration logic here - this is handled externally
        // Commands are added to the bucket by external systems
        OLO_PROFILE_FUNCTION();
        
        // Nothing to do here - commands are already in the bucket
    }

    void CommandSceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Bind the target framebuffer
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
        if (m_Allocator) // Using m_Allocator to check if we're initialized
        {
            m_CommandBucket.SortCommands(); // Use dot (.) instead of arrow (->) for non-pointer object
            m_CommandBucket.Execute(RenderCommand::GetRendererAPI()); // Use dot (.) syntax
        }
        else
        {
            OLO_CORE_WARN("CommandSceneRenderPass::Execute: No command allocator available");
        }

        // Unbind the framebuffer
        m_Target->Unbind();
    }
}