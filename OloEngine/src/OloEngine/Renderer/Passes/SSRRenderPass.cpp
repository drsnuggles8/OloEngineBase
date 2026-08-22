#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
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

        // Pick the latest upstream colour to reflect: SSGI (if the indirect-diffuse
        // bounce ran), AOApply (if AO ran), SSS, else raw SceneColor.
        // PostProcessColor is intentionally NOT a candidate — its alias is
        // repointed to SSRColor downstream, so reading it here would form a cycle.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
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

        // Dedicated min-depth HZB for HiZ ray traversal (#284).
        m_MinHZB.Initialize();
        m_MinHZB.SetReduceMode(HZBGenerator::ReduceMode::Min);
        if (spec.Width > 0 && spec.Height > 0)
            m_MinHZB.Resize(spec.Width, spec.Height);

        OLO_CORE_INFO("SSRRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    glm::vec2 SSRRenderPass::GetHZBUVFactor() const noexcept
    {
        return HZBGenerator::ComputeDimensions(m_FramebufferSpec.Width, m_FramebufferSpec.Height).UVFactor;
    }

    u32 SSRRenderPass::GetHZBMipCount() const noexcept
    {
        return HZBGenerator::ComputeDimensions(m_FramebufferSpec.Width, m_FramebufferSpec.Height).MipCount;
    }

    void SSRRenderPass::Execute(RGCommandContext& context)
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
                OLO_CORE_WARN("SSRRenderPass: missing input/output (inputTex={}, outputFB={}, depthTex={}, normalTex={}, albedoTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              sceneDepthID,
                              gbufferNormalID,
                              gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSRRenderPass enabled without resolved graph input/output");
            return;
        }

        if (const bool shaderReady = m_SSRShader && m_SSRShader->IsReady();
            !shaderReady || !sceneDepthID.IsValid() || !gbufferNormalID.IsValid() || !gbufferAlbedoID.IsValid())
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

        // Build this frame's min-depth HZB pyramid from scene depth, then bind it
        // for the HiZ ray march (#284). Generation is compute (dispatches + its
        // own TextureFetch barrier inside Generate), so it must run before the
        // fullscreen draw that samples it. Resize is a cheap no-op when the
        // viewport hasn't changed bucket. If generation is unavailable the march
        // still works against full-res scene depth — the HZB only skips empty
        // space, the actual hit test is always against real depth — so fall back
        // to binding scene depth so the sampler is never left unbound.
        m_MinHZB.Resize(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        m_MinHZB.Generate(sceneDepthID);
        const bool hzbIsPassOwned = m_MinHZB.GetHZBTexture().IsValid();
        const RHI::ResourceHandle minHZBID = hzbIsPassOwned ? m_MinHZB.GetHZBTexture() : sceneDepthID;
        // THE LIFETIME FOLLOWS WHICH TEXTURE WE ACTUALLY PICKED, not which slot it
        // is bound to. The HZB is a pass-owned Ref<Texture2D> (Persistent, so its
        // descriptor is memoised), but the fallback above is the GRAPH's scene
        // depth — a pooled resource the planner may hand to a different logical
        // texture next frame, which is exactly what Persistent must not memoise
        // (ADR 0011 §1.2). Binding the fallback as Persistent would be a latent
        // stale-descriptor bug that only appears when HZB generation is
        // unavailable.
        const RHI::HeapSlotLifetime hzbLifetime =
            hzbIsPassOwned ? RHI::HeapSlotLifetime::Persistent : RHI::HeapSlotLifetime::FrameTransient;

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
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        // Heap-bindless conversion (issue #691, bucket 1). The shader is
        // bound FIRST because the seam forks on Shader::IsBoundProgramBindless(),
        // which describes the program in flight — a bind issued before it would
        // silently take the slot-path fallback even with the heap on.
        //
        // FrameTransient for the four graph-resolved inputs (context.Resolve-
        // TextureHandle hands back pooled resources); see hzbLifetime above for
        // the one input whose ownership is conditional.
        m_SSRShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, gbufferAlbedoID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_SSR_HZB, minHZBID, hzbLifetime);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        // Publishes the descriptors minted above AND the offsets indexing them,
        // in that order, immediately before the draw that reads them.
        context.FlushHeapOffsets();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void SSRRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (width > 0 && height > 0)
            m_MinHZB.Resize(width, height);
    }

    void SSRRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_MinHZB.Resize(width, height);
    }

    void SSRRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
    }

} // namespace OloEngine
