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
    }

    void FinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;
        m_FramebufferSpec.SwapChainTarget = true; // This pass renders to the default framebuffer

        // Create fullscreen triangle mesh
        CreateFullscreenTriangle();

        // Load the shader for blitting the texture to the screen
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    }

    void FinalRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // We're rendering to the default framebuffer
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
        m_BlitShader->Bind();
        
        // Bind the color attachment from the input framebuffer as a texture
        uint32_t colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);
        
        // Draw the fullscreen triangle
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    void FinalRenderPass::SetupFramebuffer(uint32_t width, uint32_t height)
    {
        // We don't create a framebuffer for the final pass since it renders to the default framebuffer
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void FinalRenderPass::ResizeFramebuffer(uint32_t width, uint32_t height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void FinalRenderPass::OnReset()
    {
        // Nothing to reset for now
    }

    void FinalRenderPass::CreateFullscreenTriangle()
    {
        OLO_PROFILE_FUNCTION();

        // Create a fullscreen triangle
        // We use a single triangle that covers the entire screen
        // This is more efficient than using two triangles (quad)
        float vertices[] = {
            // positions      // texture coords
            -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // bottom left
             3.0f, -1.0f, 0.0f, 2.0f, 0.0f, // bottom right (far off screen)
            -1.0f,  3.0f, 0.0f, 0.0f, 2.0f  // top left (far off screen)
        };

        uint32_t indices[] = { 0, 1, 2 };

        m_FullscreenTriangleVA = VertexArray::Create();

        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
        vertexBuffer->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" }
        });

        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(uint32_t));

        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);
    }
} 