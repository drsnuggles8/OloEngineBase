#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    SSRRenderPass::SSRRenderPass()
    {
        SetName("SSRPass");
        OLO_CORE_INFO("Creating SSRRenderPass.");
    }

    void SSRRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};

        // Pick the latest upstream colour to reflect: AOApply (if AO ran), SSS,
        // else raw SceneColor. PostProcessColor is intentionally NOT a candidate
        // — its alias is repointed to SSRColor downstream, so reading it here
        // would form a cycle.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        // SSRColor is only declared (deferred path) when the G-Buffer + depth are
        // available; without them the pass cannot run and downstream aliases back.
        if (!m_Enabled || !blackboard.Post.SSRColor.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() ||
            !blackboard.GBuffer.GBufferNormal.IsValid() ||
            !blackboard.GBuffer.GBufferAlbedo.IsValid())
            return;

        [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto gbufferNormalRead = builder.Read(blackboard.GBuffer.GBufferNormal, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto gbufferAlbedoRead = builder.Read(blackboard.GBuffer.GBufferAlbedo, RGReadUsage::ShaderSample);
        m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
        m_SelectedGBufferNormalTexture = blackboard.GBuffer.GBufferNormal;
        m_SelectedGBufferAlbedoTexture = blackboard.GBuffer.GBufferAlbedo;

        constexpr std::string_view ssrVersionTag = "SSRPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.SSRColor, RGWriteUsage::RenderTarget, ssrVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::SSRColorTexture) + "@" +
                                                        std::string(ssrVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void SSRRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_SSRShader = Shader::Create("assets/shaders/PostProcess_SSR.glsl");

        OLO_CORE_INFO("SSRRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void SSRRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: the input framebuffer is intentionally not
        // resolved as an FBO here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        u32 sceneDepthID = 0;
        u32 gbufferNormalID = 0;
        u32 gbufferAlbedoID = 0;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedGBufferNormalTexture.IsValid())
            gbufferNormalID = context.ResolveTexture(m_SelectedGBufferNormalTexture);
        if (m_SelectedGBufferAlbedoTexture.IsValid())
            gbufferAlbedoID = context.ResolveTexture(m_SelectedGBufferAlbedoTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputOrOutputWarnings = 0; s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("SSRRenderPass: missing input/output (inputTex={}, outputFB={}, depthTex={}, normalTex={}, albedoTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRendererID() : 0u,
                              sceneDepthID,
                              gbufferNormalID,
                              gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSRRenderPass enabled without resolved graph input/output");
            return;
        }

        if (const bool shaderReady = m_SSRShader && m_SSRShader->IsReady();
            !shaderReady || sceneDepthID == 0 || gbufferNormalID == 0 || gbufferAlbedoID == 0)
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("SSRRenderPass: enabled without complete execution state (shaderReady={}, depthTex={}, normalTex={}, albedoTex={})",
                              shaderReady, sceneDepthID, gbufferNormalID, gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSRRenderPass enabled without ready shader or resolved G-Buffer/depth inputs");
            return;
        }

        m_Target = outputFramebuffer;

        // Rebind the SSR UBO (binding 38) — other passes may displace this
        // indexed binding between EndScene()'s upload and this Execute() call.
        if (m_SSRUBO)
            m_SSRUBO->Bind();

        constexpr u32 colorAttachment = 0;
        outputFramebuffer->Bind();

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_SSRShader->Bind();
        context.BindTexture(0, inputColorTextureID);
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID);
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID);
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, gbufferAlbedoID);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void SSRRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSRRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSRRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
    }

} // namespace OloEngine
