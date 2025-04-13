#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine
{
    FinalRenderPass::FinalRenderPass()
    {
        SetName("FinalRenderPass");
		OLO_CORE_INFO("Creating FinalRenderPass.");
    }

    void FinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;

        CreateFullscreenTriangle();

        // Load the shader for blitting the texture to the screen
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
        
        OLO_CORE_INFO("FinalRenderPass initialized with dimensions: {}x{}", m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void FinalRenderPass::Execute()
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
            OLO_CORE_WARN("FinalRenderPass::Execute: No input framebuffer set!");
            return;
        }

        // Bind the shader and the input texture
        if (!m_BlitShader)
        {
            OLO_CORE_ERROR("FinalRenderPass::Execute: Blit shader not loaded!");
            return;
        }
        
        m_BlitShader->Bind();
        
        // Bind the color attachment from the input framebuffer as a texture
        u32 colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);
        
        // Draw the fullscreen triangle
        if (!m_FullscreenTriangleVA)
        {
            OLO_CORE_ERROR("FinalRenderPass::Execute: Fullscreen triangle vertex array not created!");
            return;
        }
        
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    void FinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        // We don't create a framebuffer for the final pass since it renders to the default framebuffer
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        OLO_CORE_INFO("FinalRenderPass setup with dimensions: {}x{}", width, height);
    }

    void FinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        // Update dimensions for viewport sizing
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        OLO_CORE_INFO("FinalRenderPass resized to: {}x{}", width, height);
    }

    void FinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // Recreate fullscreen triangle if needed
        if (!m_FullscreenTriangleVA)
        {
            CreateFullscreenTriangle();
        }
        
        // Reload shader if needed
        if (!m_BlitShader)
        {
            m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
        }
        
        OLO_CORE_INFO("FinalRenderPass reset");
    }

    void FinalRenderPass::CreateFullscreenTriangle()
    {
        OLO_PROFILE_FUNCTION();

        // Create a fullscreen triangle for efficient screen-space rendering
        // We use a single triangle that covers the entire screen
        // This is more efficient than using two triangles (quad) as it:
        // 1. Reduces the number of vertices (3 vs 4)
        // 2. Eliminates the shared edge and duplicate fragment shader invocations along it
        // 3. Reduces the workload on the vertex shader
        f32 vertices[] = {
            // positions      // texture coords
            -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // bottom left
             3.0f, -1.0f, 0.0f, 2.0f, 0.0f, // bottom right (far off screen)
            -1.0f,  3.0f, 0.0f, 0.0f, 2.0f  // top left (far off screen)
        };

        u32 indices[] = { 0, 1, 2 };

        m_FullscreenTriangleVA = VertexArray::Create();

        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
        vertexBuffer->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" }
        });

        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(u32));

        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);
        
        OLO_CORE_INFO("FinalRenderPass: Fullscreen triangle created");
    }
} 
