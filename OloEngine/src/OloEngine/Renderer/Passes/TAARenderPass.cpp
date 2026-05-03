#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    TAARenderPass::TAARenderPass()
    {
        SetName("TAAPass");
    }

    void TAARenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_TAAShader = Shader::Create("assets/shaders/PostProcess_TAA.glsl");
        m_TAAUBO = UniformBuffer::Create(TAAUBOData::GetSize(), ShaderBindingLayout::UBO_TAA);

        DeclareRead(ResourceNames::MotionBlurColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::TAAColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("TAARenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void TAARenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("TAARenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            m_TAAHistoryFB = nullptr;
            m_TAAHistoryValid = false;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

        m_OutputFB = Framebuffer::Create(fbSpec);
        m_TAAHistoryFB = Framebuffer::Create(fbSpec);
        m_TAAHistoryValid = false;
    }

    void TAARenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void TAARenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 40 — self-resolving input framebuffer.
        // Prefer MotionBlurColor, then DOFColor, then BloomColor, else PostProcessColor.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            const auto inputHandle = board->MotionBlurColor.IsValid() ? board->MotionBlurColor
                                     : board->DOFColor.IsValid()      ? board->DOFColor
                                     : board->BloomColor.IsValid()    ? board->BloomColor
                                                                      : board->PostProcessColor;
            if (inputHandle.IsValid())
            {
                if (auto resolved = context.ResolveFramebuffer(inputHandle))
                    inputFramebuffer = resolved;
            }
        }
        if (!m_Enabled)
        {
            m_TAAHistoryValid = false;
            return;
        }

        if (!inputFramebuffer || !m_OutputFB || !m_TAAShader || !m_TAAHistoryFB || !m_TAAUBO)
        {
            m_TAAHistoryValid = false;
            return;
        }

        // Phase F slice 40 — self-resolving SceneDepth and Velocity.
        u32 sceneDepthTextureID = 0;
        u32 velocityTextureID = 0;
        if (const auto* board = context.GetBlackboard())
        {
            sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);
            velocityTextureID = context.ResolveTexture(board->Velocity);
        }
        if (sceneDepthTextureID == 0)
        {
            m_TAAHistoryValid = false;
            return;
        }

        m_OutputFB->Bind();

        const auto& outSpec = m_OutputFB->GetSpecification();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_TAAShader->Bind();

        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_TAAShader->SetInt("u_Current", 0);

        const u32 historyID = m_TAAHistoryValid
                                  ? m_TAAHistoryFB->GetColorAttachmentRendererID(0)
                                  : srcColorID;
        context.BindTexture(1, historyID);
        m_TAAShader->SetInt("u_History", 1);

        context.BindTexture(2, velocityTextureID);
        m_TAAShader->SetInt("u_Velocity", 2);

        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
        m_TAAShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

        TAAUBOData taaData;
        taaData.FeedbackSharpnessHasVelocity = glm::vec4(
            m_Settings.TAAFeedback,
            m_Settings.TAASharpness,
            velocityTextureID != 0 ? 1.0f : 0.0f,
            0.0f);
        taaData.TexelSize = glm::vec4(
            1.0f / static_cast<f32>(outSpec.Width),
            1.0f / static_cast<f32>(outSpec.Height),
            0.0f,
            0.0f);
        m_TAAUBO->SetData(&taaData, TAAUBOData::GetSize());
        m_TAAUBO->Bind();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        glBlitNamedFramebuffer(m_OutputFB->GetRendererID(),
                               m_TAAHistoryFB->GetRendererID(),
                               0, 0, static_cast<GLint>(outSpec.Width), static_cast<GLint>(outSpec.Height),
                               0, 0, static_cast<GLint>(outSpec.Width), static_cast<GLint>(outSpec.Height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        m_TAAHistoryValid = true;

        context.SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    Ref<Framebuffer> TAARenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
    }

    void TAARenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void TAARenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void TAARenderPass::OnReset()
    {
        m_TAAHistoryValid = false;
    }
} // namespace OloEngine
