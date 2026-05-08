#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

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
            m_Target = nullptr;
            m_TAAHistoryFB = nullptr;
            m_TAAHistoryValid = false;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

        m_TAAHistoryFB = Framebuffer::Create(fbSpec);
        m_Target = nullptr;
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
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        Ref<Framebuffer> outputFramebuffer;
        u32 sceneDepthTextureID = 0;
        u32 velocityTextureID = 0;
        u32 historyTextureID = 0;
        if (board)
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

            if (board->TAAColor.IsValid())
            {
                if (auto resolvedOutput = context.ResolveFramebuffer(board->TAAColor))
                    outputFramebuffer = resolvedOutput;
            }

            sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);
            velocityTextureID = context.ResolveTexture(board->Velocity);
            historyTextureID = context.ResolveTexture(board->TAAHistory);
        }
        if (!m_Enabled)
        {
            m_TAAHistoryValid = false;
            m_Target = inputFramebuffer;
            return;
        }

        if (!board || !board->TAAColor.IsValid() || !inputFramebuffer || !outputFramebuffer || !m_TAAShader || !m_TAAHistoryFB || !m_TAAUBO)
        {
            m_TAAHistoryValid = false;
            m_Target = nullptr;
            return;
        }
        if (sceneDepthTextureID == 0)
        {
            m_TAAHistoryValid = false;
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        outputFramebuffer->Bind();

        const auto& outSpec = outputFramebuffer->GetSpecification();
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

        const u32 historyID = historyTextureID != 0 ? historyTextureID : srcColorID;
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

        context.ExtractHistoryTexture(ResourceNames::TAAHistory,
                                      board->TAAColor,
                                      [this](const u32 textureID)
                                      {
                                          StoreHistoryTexture(textureID);
                                      });

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void TAARenderPass::StoreHistoryTexture(const u32 textureID)
    {
        if (textureID == 0 || !m_TAAHistoryFB)
            return;

        const u32 historyTextureID = m_TAAHistoryFB->GetColorAttachmentRendererID(0);
        if (historyTextureID == 0)
            return;

        const auto& historySpec = m_TAAHistoryFB->GetSpecification();
        if (historySpec.Width == 0 || historySpec.Height == 0)
            return;

        glCopyImageSubData(textureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                           historyTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                           static_cast<GLsizei>(historySpec.Width), static_cast<GLsizei>(historySpec.Height), 1);
        m_TAAHistoryValid = true;
    }

    Ref<Framebuffer> TAARenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
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
        m_Target = nullptr;
    }
} // namespace OloEngine
