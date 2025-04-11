#include "OloEnginePCH.h"
#include "CommandFinalRenderPass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"

namespace OloEngine
{
    CommandFinalRenderPass::CommandFinalRenderPass()
    {
        SetName("CommandFinalRenderPass");
        OLO_CORE_INFO("Creating CommandFinalRenderPass.");
    }

    void CommandFinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;
        
        CreateFullscreenTriangle();
        
        // Load or create the blit shader
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
       
        OLO_CORE_INFO("CommandFinalRenderPass: Initialized with viewport dimensions {}x{}", 
				m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void CommandFinalRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input) 
    { 
        m_InputFramebuffer = input; 
    }

	void CommandFinalRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // We're rendering to the default framebuffer (swap chain)
        RenderCommand::BindDefaultFramebuffer();
        
        // Clear the framebuffer
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        // Only execute if we have an input framebuffer
        if (!m_InputFramebuffer)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::Execute: No input framebuffer set!");
            return;
        }

        // Bind the shader and the input texture
        if (!m_BlitShader)
        {
            OLO_CORE_ERROR("CommandFinalRenderPass::Execute: Blit shader not loaded!");
            return;
        }
        
        m_BlitShader->Bind();
        
        // Bind the color attachment from the input framebuffer as a texture
        u32 colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        
        if (colorAttachmentID == 0)
        {
            OLO_CORE_ERROR("CommandFinalRenderPass::Execute: Invalid color attachment ID!");
            return;
        }
        
        OLO_CORE_TRACE("CommandFinalRenderPass::Execute: Using color attachment ID {}", colorAttachmentID);
        
        RenderCommand::BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);
        
        // Draw the fullscreen triangle
        if (!m_FullscreenTriangleVA)
        {
            OLO_CORE_ERROR("CommandFinalRenderPass::Execute: Fullscreen triangle vertex array not created!");
            return;
        }
        
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    void CommandFinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        // We don't create a framebuffer for the final pass since it renders to the default framebuffer
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

		OLO_CORE_INFO("FinalRenderPass setup with dimensions: {}x{}", width, height);
    }

    void CommandFinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        // Update stored dimensions
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        OLO_CORE_INFO("CommandFinalRenderPass: Resized viewport to {}x{}", width, height);
    }

    void CommandFinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // Recreate the fullscreen triangle
        CreateFullscreenTriangle();
        
        // Reload the shader
        if (m_BlitShader)
            m_BlitShader->Reload();
    }

    void CommandFinalRenderPass::CreateFullscreenTriangle()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create a single triangle that covers the entire screen
        // We use a large triangle (larger than NDC space) to ensure full screen coverage
        
        // Vertex layout:
        // position (vec3) + texcoord (vec2)
        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };
        
        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } }, // Bottom left
            { {  3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } }, // Bottom right (extended)
            { { -1.0f,  3.0f, 0.0f }, { 0.0f, 2.0f } }  // Top left (extended)
        };
        
        u32 indices[3] = { 0, 1, 2 }; // Triangle
        
        // Create vertex array
        m_FullscreenTriangleVA = VertexArray::Create();
        
        // Create vertex buffer with the correct data type
        // Cast the vertices to float* as required by the VertexBuffer::Create method
        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            reinterpret_cast<f32*>(vertices), 
            static_cast<u32>(sizeof(vertices))
        );
        
        vertexBuffer->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" }
        });
        
        // Create index buffer
        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);
        
        // Add buffers to vertex array
        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);
        
        OLO_CORE_INFO("CommandFinalRenderPass: Created fullscreen triangle");
    }
}