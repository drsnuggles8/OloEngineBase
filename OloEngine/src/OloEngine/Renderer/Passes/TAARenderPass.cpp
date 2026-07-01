#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
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

    void TAARenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Post.UpscaledSceneDepthTexture.IsValid() ? blackboard.Post.UpscaledSceneDepthTexture : blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(m_SelectedSceneDepthTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityTexture = blackboard.Post.UpscaledVelocityTexture.IsValid() ? blackboard.Post.UpscaledVelocityTexture : blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead = builder.Read(m_SelectedVelocityTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.TAAHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.TAAHistory;
            [[maybe_unused]] const auto taaHistoryRead = builder.Read(blackboard.Temporal.TAAHistory, RGReadUsage::ShaderSample);
        }
        if (blackboard.Post.TAAColor.IsValid())
        {
            constexpr std::string_view taaVersionTag = "TAAPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.TAAColor, RGWriteUsage::RenderTarget, taaVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::TAAColorTexture) + "@" +
                                                            std::string(taaVersionTag),
                                                        outputHandle,
                                                        0u));
            builder.ExtractHistoryTexture(ResourceNames::TAAHistory, outputHandle);
        }
    }

    void TAARenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_TAAShader = Shader::Create("assets/shaders/PostProcess_TAA.glsl");
        m_TAAUBO = UniformBuffer::Create(TAAUBOData::GetSize(), ShaderBindingLayout::UBO_TAA);

        OLO_CORE_INFO("TAARenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void TAARenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("TAARenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void TAARenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        u32 sceneDepthTextureID = 0;
        u32 velocityTextureID = 0;
        u32 historyTextureID = 0;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthTextureID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedVelocityTexture.IsValid())
            velocityTextureID = context.ResolveTexture(m_SelectedVelocityTexture);
        if (m_SelectedHistoryTexture.IsValid())
            historyTextureID = context.ResolveTexture(m_SelectedHistoryTexture);
        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_TAAShader || !m_TAAUBO)
        {
            m_Target = nullptr;
            return;
        }
        if (sceneDepthTextureID == 0)
        {
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

        context.BindTexture(0, inputColorTextureID);
        m_TAAShader->SetInt("u_Current", 0);

        const u32 historyID = historyTextureID != 0 ? historyTextureID : inputColorTextureID;
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

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
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
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
    }
} // namespace OloEngine
