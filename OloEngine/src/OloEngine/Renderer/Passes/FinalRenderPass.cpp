#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"

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
        
        // Create a single triangle that covers the entire screen
        // We use a large triangle (larger than NDC space) to ensure full screen coverage
        
        // Vertex layout:
        // position (vec3) + texcoord (vec2)
        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };
        
        // Compile-time verification that vertex structure is correctly sized
        static_assert(sizeof(FullscreenVertex) == sizeof(f32) * 5, 
                      "FullscreenVertex must be exactly 5 floats (3 position + 2 texcoord)");
        
        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } }, // Bottom left
            { {  3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } }, // Bottom right (extended)
            { { -1.0f,  3.0f, 0.0f }, { 0.0f, 2.0f } }  // Top left (extended)
        };
        
        u32 indices[3] = { 0, 1, 2 }; // Triangle
        
        // Create vertex array
        m_FullscreenTriangleVA = VertexArray::Create();
        
        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            static_cast<const void*>(vertices), 
            static_cast<u32>(sizeof(vertices))
        );
        
        vertexBuffer->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" }
        });
        
        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);
        
        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);
        
        OLO_CORE_INFO("FinalRenderPass: Created fullscreen triangle.");
        
        // Load or create the blit shader
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
       
        OLO_CORE_INFO("FinalRenderPass: Initialized with viewport dimensions {}x{}", 
				m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void FinalRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input) 
    { 
        m_InputFramebuffer = input; 
    }

	void FinalRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

		// Reset OpenGL state to engine defaults (matches RenderState defaults)
		RenderCommand::SetBlendState(false);
		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthMask(true);
		RenderCommand::SetDepthFunc(GL_LESS);
		RenderCommand::DisableStencilTest();
		RenderCommand::DisableCulling();
		RenderCommand::SetCullFace(GL_BACK);
		RenderCommand::SetLineWidth(1.0f);
		RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		RenderCommand::DisableScissorTest();
		RenderCommand::SetColorMask(true, true, true, true);
		RenderCommand::SetPolygonOffset(0.0f, 0.0f);
		RenderCommand::EnableMultisampling();

        RenderCommand::BindDefaultFramebuffer();
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();
        
        m_BlitShader->Bind();
        
        // Bind the color attachment from the input framebuffer as a texture
        u32 colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);
        
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    Ref<Framebuffer> FinalRenderPass::GetTarget() const
    {
        return m_Target;
    }

    Ref<Framebuffer> FinalRenderPass::GetInputFramebuffer() const
    {
        return m_InputFramebuffer;
    }

    void FinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

		OLO_CORE_INFO("FinalRenderPass setup with dimensions: {}x{}", width, height);
    }

    void FinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FinalRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        // Update stored dimensions
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        OLO_CORE_INFO("FinalRenderPass: Resized viewport to {}x{}", width, height);
    }

    void FinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // TODO: Recreate the fullscreen triangle and shader if needed
    }        
}
