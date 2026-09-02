#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
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
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace OloEngine
{
    SSGIRenderPass::SSGIRenderPass()
    {
        SetName("SSGIPass");
        OLO_CORE_INFO("Creating SSGIRenderPass.");
    }

    SSGIRenderPass::~SSGIRenderPass()
    {
        DestroyBlueNoiseTexture(m_BlueNoiseTexture);
    }

    void SSGIRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};
        m_SelectedFirstMomentsHistoryTexture = {};
        m_SelectedSecondMomentsHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};

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
        // Preserve the exact packed surface metadata sampled by this resolve.
        // This must be declared from Setup(): BuildFrameGraph clears and rebuilds
        // extraction contracts before visiting nodes, so a pre-build declaration
        // from PopulateBlackboard would be discarded on every cache miss.
        builder.ExtractHistoryTexture(ResourceNames::SSGISurfaceHistory, m_SelectedGBufferNormalTexture);

        // G-Buffer velocity (RT3) drives the resolve's reprojection. SSGI runs
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
        if (blackboard.Temporal.SSGIHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.SSGIHistory;
            [[maybe_unused]] const auto historyRead = builder.Read(m_SelectedHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.SSGISurfaceHistory.IsValid())
        {
            m_SelectedSurfaceHistoryTexture = blackboard.Temporal.SSGISurfaceHistory;
            [[maybe_unused]] const auto surfaceHistoryRead = builder.Read(
                m_SelectedSurfaceHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.SSGIMomentsFirstHistory.IsValid())
        {
            m_SelectedFirstMomentsHistoryTexture = blackboard.Temporal.SSGIMomentsFirstHistory;
            [[maybe_unused]] const auto firstMomentsRead = builder.Read(
                m_SelectedFirstMomentsHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.SSGIMomentsSecondHistory.IsValid())
        {
            m_SelectedSecondMomentsHistoryTexture = blackboard.Temporal.SSGIMomentsSecondHistory;
            [[maybe_unused]] const auto secondMomentsRead = builder.Read(
                m_SelectedSecondMomentsHistoryTexture, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.SSGISignal.IsValid())
        {
            m_SelectedSignalFramebuffer = blackboard.Scratch.SSGISignal;
            // Intra-pass write-then-sample: draw A renders the stochastic
            // signal, draw B (temporal resolve) samples it — including a 3x3
            // neighbourhood — inside the same Execute. Same idiom as
            // CloudscapeRenderPass's CloudsRaw.
            builder.AllowSamePassReadWrite(m_SelectedSignalFramebuffer);
            builder.Write(m_SelectedSignalFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto signalRead = builder.Read(m_SelectedSignalFramebuffer, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.SSGIResolved.IsValid())
        {
            m_SelectedResolvedFramebuffer = blackboard.Scratch.SSGIResolved;
            // Intra-pass write-then-sample again: draw B writes the resolve,
            // draw C (composite) samples it.
            builder.AllowSamePassReadWrite(m_SelectedResolvedFramebuffer);
            builder.Write(m_SelectedResolvedFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto resolvedRead = builder.Read(m_SelectedResolvedFramebuffer, RGReadUsage::ShaderSample);
            // Next-frame history: the graph copies the resolved signal into the
            // pipeline-owned SSGIHistory sink after this pass executes. It must
            // come from the RESOLVED buffer, not the composited output — the
            // composite carries the base colour and accumulating that is the
            // exact failure #902 exists to prevent.
            builder.ExtractHistoryTexture(ResourceNames::SSGIHistory, m_SelectedResolvedFramebuffer);
            builder.ExtractHistoryTexture(ResourceNames::SSGIMomentsFirstHistory, m_SelectedResolvedFramebuffer, 1u);
            builder.ExtractHistoryTexture(ResourceNames::SSGIMomentsSecondHistory, m_SelectedResolvedFramebuffer, 2u);
        }

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
        m_SSGIResolveShader = Shader::Create("assets/shaders/PostProcess_SSGIResolve.glsl");
        m_SSGICompositeShader = Shader::Create("assets/shaders/PostProcess_SSGIComposite.glsl");

        if (!m_BlueNoiseTexture.IsValid())
            m_BlueNoiseTexture = CreateBlueNoiseTexture();

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

        Ref<Framebuffer> signalFramebuffer;
        Ref<Framebuffer> resolvedFramebuffer;
        if (m_SelectedSignalFramebuffer.IsValid())
            signalFramebuffer = context.ResolveFramebuffer(m_SelectedSignalFramebuffer);
        if (m_SelectedResolvedFramebuffer.IsValid())
            resolvedFramebuffer = context.ResolveFramebuffer(m_SelectedResolvedFramebuffer);

        RHI::ResourceHandle velocityID{};
        RHI::ResourceHandle historyID{};
        RHI::ResourceHandle surfaceHistoryID{};
        RHI::ResourceHandle firstMomentsHistoryID{};
        RHI::ResourceHandle secondMomentsHistoryID{};
        if (m_SelectedVelocityTexture.IsValid())
            velocityID = context.ResolveTextureHandle(m_SelectedVelocityTexture);
        if (m_SelectedHistoryTexture.IsValid())
            historyID = context.ResolveTextureHandle(m_SelectedHistoryTexture);
        if (m_SelectedSurfaceHistoryTexture.IsValid())
            surfaceHistoryID = context.ResolveTextureHandle(m_SelectedSurfaceHistoryTexture);
        if (m_SelectedFirstMomentsHistoryTexture.IsValid())
            firstMomentsHistoryID = context.ResolveTextureHandle(m_SelectedFirstMomentsHistoryTexture);
        if (m_SelectedSecondMomentsHistoryTexture.IsValid())
            secondMomentsHistoryID = context.ResolveTextureHandle(m_SelectedSecondMomentsHistoryTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !signalFramebuffer || !resolvedFramebuffer)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputOrOutputWarnings = 0; s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("SSGIRenderPass: missing input/output (inputTex={}, outputFB={}, signalFB={}, resolvedFB={}, depthTex={}, normalTex={}, albedoTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              signalFramebuffer ? signalFramebuffer->GetRHIHandle() : RHI::NullResource,
                              resolvedFramebuffer ? resolvedFramebuffer->GetRHIHandle() : RHI::NullResource,
                              sceneDepthID,
                              gbufferNormalID,
                              gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSGIRenderPass enabled without resolved graph input/output");
            return;
        }

        if (const bool shaderReady = IsReadyForExecution();
            !shaderReady || !sceneDepthID.IsValid() || !gbufferNormalID.IsValid() || !gbufferAlbedoID.IsValid())
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("SSGIRenderPass: enabled without complete execution state (shaderReady={}, depthTex={}, normalTex={}, albedoTex={})",
                              shaderReady, sceneDepthID, gbufferNormalID, gbufferAlbedoID);
            }
            OLO_CORE_ASSERT(false, "SSGIRenderPass enabled without ready shaders or resolved G-Buffer/depth inputs");
            return;
        }

        m_Target = outputFramebuffer;

        // Rebind the SSGI UBO (binding 40) - other passes may displace this
        // indexed binding between EndScene's upload and this Execute call.
        if (m_SSGIUBO)
        {
            m_SSGIUBO->Bind();

            // Patch the temporal lanes the pipeline could not know (issue #902).
            // `historyUsable` is the AND of the user toggle and the history
            // actually having resolved - the graph only imports SSGIHistory once
            // a previous frame produced one, so on the first frame (and after any
            // resize or re-enable) this is 0 and the resolve outputs the current
            // frame rather than blending against an uninitialised buffer.
            // Sanitize here, not just in SanitizeSSGI(): that runs on
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
                (m_TemporalResolveEnabled && historyID.IsValid() && surfaceHistoryID.IsValid() &&
                 firstMomentsHistoryID.IsValid() && secondMomentsHistoryID.IsValid())
                    ? 1.0f
                    : 0.0f,
                m_TemporalClipGamma);
            m_SSGIUBO->SetData(&temporalParams, static_cast<u32>(sizeof(glm::vec4)),
                               static_cast<u32>(offsetof(SSGIUBOData, TemporalParams)));
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

        const auto drawFullscreen = [&context]()
        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(va);
        };

        // ----------------------------------------------------------------
        // Draw A - the stochastic signal ONLY, into SSGISignal.
        // ----------------------------------------------------------------
        signalFramebuffer->Bind();
        {
            const auto& signalSpec = signalFramebuffer->GetSpecification();
            context.SetViewport(0, 0, signalSpec.Width, signalSpec.Height);
        }
        setFullscreenState();

        // Heap-bindless conversion (issue #691, bucket 1). Shader bound
        // first - the seam forks on the program in flight. All four inputs are
        // graph-resolved (pooled), so FrameTransient rather than Persistent.
        m_SSGIShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, gbufferAlbedoID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        // Pass-owned and immutable, so Persistent rather than FrameTransient
        // (issue #706). Nearest+Repeat sampler state rides in the descriptor.
        BindBlueNoiseTexture(context, m_BlueNoiseTexture);
        drawFullscreen();
        signalFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Draw B - temporal resolve of that signal, into SSGIResolved.
        // ----------------------------------------------------------------
        const RHI::ResourceHandle signalTextureID = signalFramebuffer->GetColorAttachmentHandle(0);

        resolvedFramebuffer->Bind();
        {
            const auto& resolvedSpec = resolvedFramebuffer->GetSpecification();
            context.SetViewport(0, 0, resolvedSpec.Width, resolvedSpec.Height);
        }
        setFullscreenState();

        constexpr std::array<u32, 5> resolvedAttachments{ 0u, 1u, 2u, 3u, 4u };
        RenderCommand::SetDrawBuffers(resolvedAttachments);

        m_SSGIResolveShader->Bind();
        context.BindTextureOrHeapOffset(0, signalTextureID, RHI::HeapSlotLifetime::FrameTransient);
        // With no history the shader ignores unit 1 (TemporalParams.z == 0), but
        // it must still be bound to something valid or the sampler dangles.
        context.BindTextureOrHeapOffset(1, historyID.IsValid() ? historyID : signalTextureID,
                                        historyID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                            : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(2, surfaceHistoryID.IsValid() ? surfaceHistoryID : gbufferNormalID,
                                        surfaceHistoryID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                                   : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(3, firstMomentsHistoryID.IsValid() ? firstMomentsHistoryID : signalTextureID,
                                        firstMomentsHistoryID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                                        : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(4, secondMomentsHistoryID.IsValid() ? secondMomentsHistoryID : signalTextureID,
                                        secondMomentsHistoryID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                                         : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                        velocityID.IsValid() ? velocityID : signalTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();
        resolvedFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Draw C - composite the RESOLVED signal onto the upstream colour.
        // ----------------------------------------------------------------
        outputFramebuffer->Bind();
        {
            const auto& outSpec = outputFramebuffer->GetSpecification();
            context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        }
        setFullscreenState();

        m_SSGICompositeShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(1, resolvedFramebuffer->GetColorAttachmentHandle(0),
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();

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
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};
        m_SelectedFirstMomentsHistoryTexture = {};
        m_SelectedSecondMomentsHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};
    }

} // namespace OloEngine
