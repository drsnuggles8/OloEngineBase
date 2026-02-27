#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create own output framebuffer (RGBA16F, no depth — fullscreen effect)
        CreateOutputFramebuffer(spec.Width, spec.Height);

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        OLO_CORE_INFO("SSSRenderPass: Initialized with {}x{} framebuffer", spec.Width, spec.Height);
    }

    void SSSRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Only run when snow is enabled AND SSS blur is explicitly turned on.
        // When disabled, GetTarget() returns m_InputFramebuffer (passthrough),
        // so downstream passes read the unmodified scene color.
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled ||
            !m_InputFramebuffer || !m_SSSBlurShader)
        {
            return;
        }

        // SSS UBO is already uploaded by Renderer3D::EndScene each frame.

        m_Target->Bind();

        const auto& targetSpec = m_Target->GetSpecification();
        RenderCommand::SetViewport(0, 0, targetSpec.Width, targetSpec.Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSSBlurShader->Bind();

        // Bind input scene color as texture — no read-write hazard since we
        // read from m_InputFramebuffer and write to m_Target.
        u32 colorID = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorID);

        // Bind scene depth for bilateral filtering
        u32 depthID = m_InputFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();

        m_Target->Unbind();
    }

    void SSSRenderPass::DrawFullscreenTriangle()
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        // Passthrough when SSS is disabled — downstream reads the input directly
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        {
            return m_InputFramebuffer;
        }
        return m_Target;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateOutputFramebuffer(width, height);
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_Target)
        {
            m_Target->Resize(width, height);
        }
    }

    void SSSRenderPass::OnReset()
    {
        // Framebuffer managed by Ref<> — nothing to manually clean up
    }

    void SSSRenderPass::CreateOutputFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        FramebufferSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Samples = 1;
        spec.Attachments = {
            FramebufferTextureFormat::RGBA16F
        };

        m_Target = Framebuffer::Create(spec);
    }
} // namespace OloEngine
