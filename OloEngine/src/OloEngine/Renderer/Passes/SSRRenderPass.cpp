#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/BlueNoiseTexture.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace OloEngine
{
    SSRRenderPass::SSRRenderPass()
    {
        SetName("SSRPass");
        OLO_CORE_INFO("Creating SSRRenderPass.");
    }

    SSRRenderPass::~SSRRenderPass()
    {
        DestroyBlueNoiseTexture(m_BlueNoiseTexture);
    }

    void SSRRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedPreBlurredFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};
        m_SelectedDenoisedFramebuffer = {};

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

        // G-Buffer velocity (RT3) drives the resolve's reprojection. SSR runs
        // before the upscale band, so this is the scene-band velocity, matching
        // the resolution the signal targets are declared at. Optional: without
        // it the resolve falls back to a zero-motion reprojection, which is
        // correct for a static camera and ghosts under motion — the shader is
        // told which case it is rather than guessing.
        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityTexture = blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead = builder.Read(m_SelectedVelocityTexture, RGReadUsage::ShaderSample);
        }

        // Prior-frame resolved signal, imported by the pipeline only when the
        // previous frame's history copy-back succeeded.
        if (blackboard.Temporal.SSRHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.SSRHistory;
            [[maybe_unused]] const auto historyRead = builder.Read(m_SelectedHistoryTexture, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.SSRSignal.IsValid())
        {
            m_SelectedSignalFramebuffer = blackboard.Scratch.SSRSignal;
            // Intra-pass write-then-sample: draw A renders the stochastic
            // signal, every later draw samples it — including neighbourhoods —
            // inside the same Execute. Same idiom as CloudscapeRenderPass's
            // CloudsRaw.
            builder.AllowSamePassReadWrite(m_SelectedSignalFramebuffer);
            builder.Write(m_SelectedSignalFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto signalRead = builder.Read(m_SelectedSignalFramebuffer, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.SSRResolved.IsValid())
        {
            m_SelectedResolvedFramebuffer = blackboard.Scratch.SSRResolved;
            // Intra-pass write-then-sample again: draw C writes the resolve,
            // draw D (post-blur) samples it.
            builder.AllowSamePassReadWrite(m_SelectedResolvedFramebuffer);
            builder.Write(m_SelectedResolvedFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto resolvedRead = builder.Read(m_SelectedResolvedFramebuffer, RGReadUsage::ShaderSample);
            // Next-frame history: the graph copies the resolved signal into the
            // pipeline-owned SSRHistory sink after this pass executes. It must
            // come from the RESOLVED buffer, not the composited output — the
            // composite carries the base colour and accumulating that is the
            // exact failure #902 exists to prevent.
            builder.ExtractHistoryTexture(ResourceNames::SSRHistory, m_SelectedResolvedFramebuffer);
        }

        // Pre-blur (stage 2) and post-blur (stage 4) scratch (issue #708).
        // Declared on the same gate as the signal even when their radius is 0 —
        // the graph shape must not depend on a UBO value, or the radius would
        // have to be hashed into the blackboard fingerprint. Execute simply
        // skips the draw and rebinds its consumer.
        if (blackboard.Scratch.SSRPreBlurred.IsValid())
        {
            m_SelectedPreBlurredFramebuffer = blackboard.Scratch.SSRPreBlurred;
            builder.AllowSamePassReadWrite(m_SelectedPreBlurredFramebuffer);
            builder.Write(m_SelectedPreBlurredFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto preBlurRead =
                builder.Read(m_SelectedPreBlurredFramebuffer, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.SSRDenoised.IsValid())
        {
            m_SelectedDenoisedFramebuffer = blackboard.Scratch.SSRDenoised;
            builder.AllowSamePassReadWrite(m_SelectedDenoisedFramebuffer);
            builder.Write(m_SelectedDenoisedFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto denoisedRead =
                builder.Read(m_SelectedDenoisedFramebuffer, RGReadUsage::ShaderSample);
        }

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
        m_SSRPreBlurShader = Shader::Create("assets/shaders/PostProcess_SSRPreBlur.glsl");
        m_SSRResolveShader = Shader::Create("assets/shaders/PostProcess_SSRResolve.glsl");
        m_SSRPostBlurShader = Shader::Create("assets/shaders/PostProcess_SSRPostBlur.glsl");
        m_SSRCompositeShader = Shader::Create("assets/shaders/PostProcess_SSRComposite.glsl");

        if (!m_BlueNoiseTexture.IsValid())
            m_BlueNoiseTexture = CreateBlueNoiseTexture();

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

        Ref<Framebuffer> signalFramebuffer;
        Ref<Framebuffer> preBlurredFramebuffer;
        Ref<Framebuffer> resolvedFramebuffer;
        Ref<Framebuffer> denoisedFramebuffer;
        if (m_SelectedSignalFramebuffer.IsValid())
            signalFramebuffer = context.ResolveFramebuffer(m_SelectedSignalFramebuffer);
        if (m_SelectedPreBlurredFramebuffer.IsValid())
            preBlurredFramebuffer = context.ResolveFramebuffer(m_SelectedPreBlurredFramebuffer);
        if (m_SelectedResolvedFramebuffer.IsValid())
            resolvedFramebuffer = context.ResolveFramebuffer(m_SelectedResolvedFramebuffer);
        if (m_SelectedDenoisedFramebuffer.IsValid())
            denoisedFramebuffer = context.ResolveFramebuffer(m_SelectedDenoisedFramebuffer);

        RHI::ResourceHandle velocityID{};
        RHI::ResourceHandle historyID{};
        if (m_SelectedVelocityTexture.IsValid())
            velocityID = context.ResolveTextureHandle(m_SelectedVelocityTexture);
        if (m_SelectedHistoryTexture.IsValid())
            historyID = context.ResolveTextureHandle(m_SelectedHistoryTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !signalFramebuffer ||
            !preBlurredFramebuffer || !resolvedFramebuffer || !denoisedFramebuffer)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputOrOutputWarnings = 0; s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("SSRRenderPass: missing input/output (inputTex={}, outputFB={}, signalFB={}, resolvedFB={}, depthTex={}, normalTex={}, albedoTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              signalFramebuffer ? signalFramebuffer->GetRHIHandle() : RHI::NullResource,
                              resolvedFramebuffer ? resolvedFramebuffer->GetRHIHandle() : RHI::NullResource,
                              sceneDepthID,
                              gbufferNormalID,
                              gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSRRenderPass enabled without resolved graph input/output");
            return;
        }

        if (const bool shaderReady = IsReadyForExecution();
            !shaderReady || !sceneDepthID.IsValid() || !gbufferNormalID.IsValid() || !gbufferAlbedoID.IsValid())
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("SSRRenderPass: enabled without complete execution state (shaderReady={}, depthTex={}, normalTex={}, albedoTex={})",
                              shaderReady, sceneDepthID, gbufferNormalID, gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSRRenderPass enabled without ready shaders or resolved G-Buffer/depth inputs");
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

        // Rebind the SSR UBO (binding 38) - other passes may displace this
        // indexed binding between EndScene's upload and this Execute call.
        if (m_SSRUBO)
        {
            m_SSRUBO->Bind();

            // Patch the temporal lanes the pipeline could not know (issue #902).
            // `historyUsable` is the AND of the user toggle and the history
            // actually having resolved - the graph only imports SSRHistory once
            // a previous frame produced one, so on the first frame (and after any
            // resize or re-enable) this is 0 and the resolve outputs the current
            // frame rather than blending against an uninitialised buffer.
            // Sanitize here, not just in SanitizeSSR(): that runs on
            // settings loaded from disk, but this value can also arrive from a
            // live edit, a script or an MCP write. The shader cannot be the
            // backstop — OloTemporalBlend's clamp(feedback, 0, 0.98) is
            // UNDEFINED for a NaN input, so a NaN would reach the blend and
            // spread through the history.
            const f32 feedback = std::isfinite(m_TemporalFeedback)
                                     ? std::clamp(m_TemporalFeedback, 0.0f, 0.98f)
                                     : 0.0f;
            const glm::vec4 temporalParams(
                feedback,
                velocityID.IsValid() ? 1.0f : 0.0f,
                (m_TemporalResolveEnabled && historyID.IsValid()) ? 1.0f : 0.0f,
                m_TemporalClipGamma);
            m_SSRUBO->SetData(&temporalParams, static_cast<u32>(sizeof(glm::vec4)),
                              static_cast<u32>(offsetof(SSRUBOData, TemporalParams)));
        }

        // Common fullscreen-blit state, re-established for each of the three
        // draws because each one binds a different framebuffer.
        const auto setFullscreenState = [&context]()
        {
            // Local, not a captured constexpr: SetDrawBuffers takes its ADDRESS,
            // which odr-uses it and a capture-less lambda cannot reach it.
            constexpr u32 colorAttachment = 0;
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
            context.Clear();
        };

        // By value, not by const reference: Ref<T>::operator-> on a CONST Ref
        // hands back a const T*, and Framebuffer::Bind() is non-const. The copy
        // is a refcount bump five times per frame.
        const auto bindTarget = [&context, &setFullscreenState](Ref<Framebuffer> target)
        {
            target->Bind();
            const auto& spec = target->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
            setFullscreenState();
        };

        const auto drawFullscreen = [&context]()
        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            // Publishes the descriptors minted above AND the offsets indexing
            // them, in that order, immediately before the draw that reads them.
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(va);
        };

        // ----------------------------------------------------------------
        // Draw A - the stochastic reflection delta ONLY, into SSRSignal.
        // Two attachments: the delta, and the guide plane whose roughness sets
        // both spatial stages' radius.
        // ----------------------------------------------------------------
        bindTarget(signalFramebuffer);

        constexpr std::array<u32, 2> signalAttachments{ 0u, 1u };
        RenderCommand::SetDrawBuffers(signalAttachments);

        // Heap-bindless conversion (issue #691, bucket 1). The shader is
        // bound FIRST because the seam forks on Shader::IsBoundProgramBindless(),
        // which describes the program in flight - a bind issued before it would
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
        // Pass-owned and immutable, so Persistent rather than FrameTransient
        // (issue #706). Nearest+Repeat sampler state rides in the descriptor.
        BindBlueNoiseTexture(context, m_BlueNoiseTexture);
        drawFullscreen();
        signalFramebuffer->Unbind();

        const RHI::ResourceHandle signalTextureID = signalFramebuffer->GetColorAttachmentHandle(0);
        const RHI::ResourceHandle guideTextureID = signalFramebuffer->GetColorAttachmentHandle(1);

        // ----------------------------------------------------------------
        // Draw B - pre-blur (issue #708 stage 2), into SSRPreBlurred.
        //
        // Skipped outright when the radius is 0: the resolve then accumulates
        // the raw signal, which is exactly the pre-#708 behaviour and the arm an
        // A/B measures against.
        // ----------------------------------------------------------------
        RHI::ResourceHandle resolveInputTextureID = signalTextureID;
        if (m_PreBlurEnabled)
        {
            bindTarget(preBlurredFramebuffer);
            m_SSRPreBlurShader->Bind();
            context.BindTextureOrHeapOffset(0, signalTextureID, RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(1, guideTextureID, RHI::HeapSlotLifetime::FrameTransient);
            drawFullscreen();
            preBlurredFramebuffer->Unbind();
            resolveInputTextureID = preBlurredFramebuffer->GetColorAttachmentHandle(0);
        }

        // ----------------------------------------------------------------
        // Draw C - temporal resolve of that signal, into SSRResolved.
        // ----------------------------------------------------------------
        bindTarget(resolvedFramebuffer);

        m_SSRResolveShader->Bind();
        context.BindTextureOrHeapOffset(0, resolveInputTextureID, RHI::HeapSlotLifetime::FrameTransient);
        // With no history the shader ignores unit 1 (TemporalParams.z == 0), but
        // it must still be bound to something valid or the sampler dangles.
        context.BindTextureOrHeapOffset(1, historyID.IsValid() ? historyID : resolveInputTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                        velocityID.IsValid() ? velocityID : signalTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();
        resolvedFramebuffer->Unbind();

        const RHI::ResourceHandle resolvedTextureID = resolvedFramebuffer->GetColorAttachmentHandle(0);

        // ----------------------------------------------------------------
        // Draw D - post-blur (issue #708 stage 4), into SSRDenoised. Its output
        // is deliberately NOT what the graph extracts into SSRHistory: the
        // history must carry the resolve's own estimate, not a filtered copy of
        // it, or the filter compounds with itself frame after frame.
        // ----------------------------------------------------------------
        RHI::ResourceHandle compositeSignalTextureID = resolvedTextureID;
        if (m_PostBlurEnabled)
        {
            bindTarget(denoisedFramebuffer);
            m_SSRPostBlurShader->Bind();
            context.BindTextureOrHeapOffset(0, resolvedTextureID, RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(1, guideTextureID, RHI::HeapSlotLifetime::FrameTransient);
            drawFullscreen();
            denoisedFramebuffer->Unbind();
            compositeSignalTextureID = denoisedFramebuffer->GetColorAttachmentHandle(0);
        }

        // ----------------------------------------------------------------
        // Draw E - composite the DENOISED delta onto the upstream colour.
        // ----------------------------------------------------------------
        bindTarget(outputFramebuffer);

        m_SSRCompositeShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(1, compositeSignalTextureID, RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();

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
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedPreBlurredFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};
        m_SelectedDenoisedFramebuffer = {};
    }

} // namespace OloEngine
