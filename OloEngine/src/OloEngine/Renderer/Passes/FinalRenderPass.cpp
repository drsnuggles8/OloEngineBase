#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

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
        RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
        RenderCommand::Clear();

        m_BlitShader->Bind();

        // Bind the color attachment from the input framebuffer as a texture
        u32 colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);

        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
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
} // namespace OloEngine
