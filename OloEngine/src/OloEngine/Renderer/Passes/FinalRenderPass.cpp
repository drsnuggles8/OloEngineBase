#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
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

        // Resource-aware RDG: final pass reads the composited (post-process
        // + UI) color piped in via SetInputFramebuffer and blits to the
        // backbuffer / swapchain image.
        DeclareRead(ResourceNames::UIComposite, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::FinalColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("FinalRenderPass: Initialized with viewport dimensions {}x{}",
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void FinalRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input)
    {
        m_InputFramebuffer = input;
    }

    void FinalRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void FinalRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        context.ResetGraphicsStateToDefault();
        context.BindDefaultFramebuffer();
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_BlitShader->Bind();

        // Bind the color attachment from the input framebuffer as a texture
        const auto colorAttachmentID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);
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
