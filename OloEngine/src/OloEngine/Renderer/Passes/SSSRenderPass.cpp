#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    SSSRenderPass::~SSSRenderPass()
    {
        if (m_StagingTexture)
        {
            RenderCommand::DeleteTexture(m_StagingTexture);
        }
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        OLO_CORE_INFO("SSSRenderPass: Initialized");
    }

    void SSSRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Only run when snow is enabled AND SSS blur is explicitly turned on.
        // The alpha channel (SSS mask) is only consumed by this pass, so when
        // blur is off we simply leave the scene FB untouched — downstream passes
        // (PostProcess) write alpha=1.0 on all output paths, preventing leaks.
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled ||
            !m_SceneFramebuffer || !m_SSSBlurShader)
        {
            return;
        }

        // SSS UBO is already uploaded by Renderer3D::EndScene each frame.

        const auto& fbSpec = m_SceneFramebuffer->GetSpecification();

        // Copy scene color to staging texture to avoid read-write hazard.
        u32 colorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
        EnsureStagingTexture(fbSpec.Width, fbSpec.Height);
        RenderCommand::CopyImageSubData(
            colorID, GL_TEXTURE_2D,
            m_StagingTexture, GL_TEXTURE_2D,
            fbSpec.Width, fbSpec.Height);

        m_SceneFramebuffer->Bind();

        // Restrict drawing to color attachment 0 only — the scene FB has
        // multiple attachments (entity ID, normals) that must not be overwritten.
        u32 drawBuf = 0;
        RenderCommand::SetDrawBuffers(std::span<const u32>(&drawBuf, 1));

        RenderCommand::SetViewport(0, 0, fbSpec.Width, fbSpec.Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSSBlurShader->Bind();

        // Bind staging copy for reading (not the live FB attachment)
        RenderCommand::BindTexture(0, m_StagingTexture);

        // Bind scene depth for bilateral filtering
        u32 depthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();

        // Restore all draw buffers for subsequent passes.
        // Count color attachments (exclude depth formats).
        const auto& attachments = fbSpec.Attachments.Attachments;
        u32 colorCount = 0;
        for (const auto& att : attachments)
        {
            const auto fmt = att.TextureFormat;
            if (fmt != FramebufferTextureFormat::DEPTH24STENCIL8 && fmt != FramebufferTextureFormat::DEPTH_COMPONENT32F)
            {
                ++colorCount;
            }
        }
        RenderCommand::RestoreAllDrawBuffers(colorCount);

        m_SceneFramebuffer->Unbind();
    }

    void SSSRenderPass::DrawFullscreenTriangle()
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        // SSS pass operates in-place on the scene framebuffer
        return m_SceneFramebuffer;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — operates on scene FB
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — operates on scene FB
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::OnReset()
    {
        if (m_StagingTexture)
        {
            RenderCommand::DeleteTexture(m_StagingTexture);
            m_StagingTexture = 0;
            m_StagingWidth = 0;
            m_StagingHeight = 0;
        }
    }

    void SSSRenderPass::EnsureStagingTexture(u32 width, u32 height)
    {
        if (m_StagingTexture && m_StagingWidth == width && m_StagingHeight == height)
        {
            return;
        }

        if (m_StagingTexture)
        {
            RenderCommand::DeleteTexture(m_StagingTexture);
        }

        m_StagingTexture = RenderCommand::CreateTexture2D(width, height, GL_RGBA16F);
        RenderCommand::SetTextureParameter(m_StagingTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        RenderCommand::SetTextureParameter(m_StagingTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        RenderCommand::SetTextureParameter(m_StagingTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        RenderCommand::SetTextureParameter(m_StagingTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_StagingWidth = width;
        m_StagingHeight = height;
    }
} // namespace OloEngine
