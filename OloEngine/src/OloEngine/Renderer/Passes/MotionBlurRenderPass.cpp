#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    MotionBlurRenderPass::MotionBlurRenderPass()
    {
        SetName("MotionBlurPass");
    }

    void MotionBlurRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        // Per-pixel velocity (Deferred G-Buffer RT3 / Forward attachment 3). Optional:
        // when absent the shader reconstructs camera-only motion for every pixel.
        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityTexture = blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead = builder.Read(blackboard.GBuffer.Velocity, RGReadUsage::ShaderSample);
        }
        if (blackboard.Post.MotionBlurColor.IsValid())
        {
            constexpr std::string_view motionBlurVersionTag = "MotionBlurPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.MotionBlurColor, RGWriteUsage::RenderTarget, motionBlurVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::MotionBlurColorTexture) + "@" +
                                                            std::string(motionBlurVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void MotionBlurRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_MotionBlurShader = Shader::Create("assets/shaders/PostProcess_MotionBlur.glsl");
        m_MotionBlurParamsUBO = UniformBuffer::Create(MotionBlurParamsUBOData::GetSize(), ShaderBindingLayout::UBO_MOTION_BLUR_PARAMS);

        OLO_CORE_INFO("MotionBlurRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void MotionBlurRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("MotionBlurRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void MotionBlurRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_MotionBlurShader)
        {
            m_Target = nullptr;
            return;
        }

        const u32 sceneDepthTextureID = m_SelectedSceneDepthTexture.IsValid()
                                            ? context.ResolveTexture(m_SelectedSceneDepthTexture)
                                            : 0u;

        if (sceneDepthTextureID == 0)
        {
            m_Target = nullptr;
            return;
        }

        const u32 velocityTextureID = m_SelectedVelocityTexture.IsValid()
                                          ? context.ResolveTexture(m_SelectedVelocityTexture)
                                          : 0u;

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();
        if (m_MotionBlurUBO)
            m_MotionBlurUBO->Bind();
        if (m_MotionBlurParamsUBO)
        {
            MotionBlurParamsUBOData params;
            params.Params.x = (velocityTextureID != 0) ? 1.0f : 0.0f;
            m_MotionBlurParamsUBO->SetData(&params, MotionBlurParamsUBOData::GetSize());
            m_MotionBlurParamsUBO->Bind();
        }

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

        m_MotionBlurShader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_MotionBlurShader->SetInt("u_Texture", 0);

        // Velocity slot mirrors TAA (slot 2 = u_Velocity). Bound even when 0
        // (the params UBO's hasVelocity flag gates whether the shader reads it).
        context.BindTexture(2, velocityTextureID);
        m_MotionBlurShader->SetInt("u_Velocity", 2);

        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
        m_MotionBlurShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void MotionBlurRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void MotionBlurRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void MotionBlurRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};
    }
} // namespace OloEngine
