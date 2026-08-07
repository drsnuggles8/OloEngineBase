#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include <span>

namespace OloEngine
{
    SSGIRenderPass::SSGIRenderPass()
    {
        SetName("SSGIPass");
        OLO_CORE_INFO("Creating SSGIRenderPass.");
    }

    void SSGIRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};

        // Pick the latest upstream colour to gather indirect light from: AOApply
        // (if AO ran), SSS, else raw SceneColor. SSGI runs before SSR, so SSRColor
        // is intentionally NOT a candidate; PostProcessColor is excluded too (its
        // alias is repointed downstream, so reading it here would form a cycle).
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        // SSGIColor is only declared (deferred path) when the G-Buffer + depth are
        // available; without them the pass cannot run and downstream aliases back.
        if (!m_Enabled || !blackboard.Post.SSGIColor.IsValid() ||
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

        constexpr std::string_view ssgiVersionTag = "SSGIPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.SSGIColor, RGWriteUsage::RenderTarget, ssgiVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::SSGIColorTexture) + "@" +
                                                        std::string(ssgiVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void SSGIRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_SSGIShader = Shader::Create("assets/shaders/PostProcess_SSGI.glsl");

        OLO_CORE_INFO("SSGIRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void SSGIRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: the input framebuffer is intentionally not
        // resolved as an FBO here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        RHI::ResourceHandle sceneDepthID{};
        RHI::ResourceHandle gbufferNormalID{};
        RHI::ResourceHandle gbufferAlbedoID{};
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedGBufferNormalTexture.IsValid())
            gbufferNormalID = context.ResolveTextureHandle(m_SelectedGBufferNormalTexture);
        if (m_SelectedGBufferAlbedoTexture.IsValid())
            gbufferAlbedoID = context.ResolveTextureHandle(m_SelectedGBufferAlbedoTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputOrOutputWarnings = 0; s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("SSGIRenderPass: missing input/output (inputTex={}, outputFB={}, depthTex={}, normalTex={}, albedoTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              sceneDepthID,
                              gbufferNormalID,
                              gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSGIRenderPass enabled without resolved graph input/output");
            return;
        }

        if (const bool shaderReady = m_SSGIShader && m_SSGIShader->IsReady();
            !shaderReady || !sceneDepthID.IsValid() || !gbufferNormalID.IsValid() || !gbufferAlbedoID.IsValid())
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("SSGIRenderPass: enabled without complete execution state (shaderReady={}, depthTex={}, normalTex={}, albedoTex={})",
                              shaderReady, sceneDepthID, gbufferNormalID, gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSGIRenderPass enabled without ready shader or resolved G-Buffer/depth inputs");
            return;
        }

        m_Target = outputFramebuffer;

        // Rebind the SSGI UBO (binding 40) — other passes may displace this
        // indexed binding between EndScene()'s upload and this Execute() call.
        if (m_SSGIUBO)
            m_SSGIUBO->Bind();

        constexpr u32 colorAttachment = 0;
        outputFramebuffer->Bind();

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        // Heap-bindless conversion (issue #691 Phase 3, bucket 1). Shader bound
        // first — the seam forks on the program in flight. All four inputs are
        // graph-resolved (pooled), so FrameTransient rather than Persistent.
        m_SSGIShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, gbufferAlbedoID,
                                        RHI::HeapSlotLifetime::FrameTransient);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.FlushHeapOffsets();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void SSGIRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSGIRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSGIRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
    }

} // namespace OloEngine
