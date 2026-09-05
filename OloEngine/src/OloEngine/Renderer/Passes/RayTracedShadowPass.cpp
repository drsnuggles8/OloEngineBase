#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/RayTracedShadowPass.h"

#include "OloEngine/Renderer/BlueNoiseTexture.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kDegreesToRadians = 0.01745329251994329577f;
    }

    RayTracedShadowPass::RayTracedShadowPass()
    {
        SetName("RayTracedShadowPass");
        OLO_CORE_INFO("Creating RayTracedShadowPass.");
    }

    RayTracedShadowPass::~RayTracedShadowPass()
    {
        DestroyBlueNoiseTexture(m_BlueNoiseTexture);
    }

    void RayTracedShadowPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};
        m_SelectedMomentsHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};

        if (!m_Enabled || !blackboard.Shadows.RayTracedShadowMask.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() || !blackboard.GBuffer.GBufferNormal.IsValid())
        {
            return;
        }

        // The by-name execution dependency RayTracingScenePass::Setup reserved
        // for the first ray-query consumer. The acceleration structure is not a
        // graph resource — there is no handle to Read — so this edge is the
        // only thing that stops a reordering from putting the AS build after
        // the pass that traces against it. The symptom would be a frame with no
        // shadows and nothing in the log.
        builder.DependsOnPass("RayTracingScenePass");

        [[maybe_unused]] const auto sceneDepthRead =
            builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto gbufferNormalRead =
            builder.Read(blackboard.GBuffer.GBufferNormal, RGReadUsage::ShaderSample);
        m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
        m_SelectedGBufferNormalTexture = blackboard.GBuffer.GBufferNormal;

        // Preserve the exact packed surface metadata this resolve sampled.
        // Declared from Setup, not from PopulateBlackboard: BuildFrameGraph
        // clears and rebuilds extraction contracts before visiting nodes, so an
        // earlier declaration is discarded on every cache miss.
        builder.ExtractHistoryTexture(ResourceNames::RayTracedShadowSurfaceHistory, m_SelectedGBufferNormalTexture);

        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityTexture = blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead =
                builder.Read(m_SelectedVelocityTexture, RGReadUsage::ShaderSample);
        }

        if (blackboard.Temporal.RayTracedShadowHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.RayTracedShadowHistory;
            [[maybe_unused]] const auto historyRead = builder.Read(m_SelectedHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.RayTracedShadowSurfaceHistory.IsValid())
        {
            m_SelectedSurfaceHistoryTexture = blackboard.Temporal.RayTracedShadowSurfaceHistory;
            [[maybe_unused]] const auto surfaceRead =
                builder.Read(m_SelectedSurfaceHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.RayTracedShadowMomentsHistory.IsValid())
        {
            m_SelectedMomentsHistoryTexture = blackboard.Temporal.RayTracedShadowMomentsHistory;
            [[maybe_unused]] const auto momentsRead =
                builder.Read(m_SelectedMomentsHistoryTexture, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.RayTracedShadowSignal.IsValid())
        {
            m_SelectedSignalFramebuffer = blackboard.Scratch.RayTracedShadowSignal;
            // Intra-pass write-then-sample: draw A renders the raw trace, draw
            // B samples it (including a 3x3 neighbourhood) inside the same
            // Execute. Same idiom as SSGISignal.
            builder.AllowSamePassReadWrite(m_SelectedSignalFramebuffer);
            builder.Write(m_SelectedSignalFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto signalRead =
                builder.Read(m_SelectedSignalFramebuffer, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.RayTracedShadowResolved.IsValid())
        {
            m_SelectedResolvedFramebuffer = blackboard.Scratch.RayTracedShadowResolved;
            builder.AllowSamePassReadWrite(m_SelectedResolvedFramebuffer);
            builder.Write(m_SelectedResolvedFramebuffer, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto resolvedRead =
                builder.Read(m_SelectedResolvedFramebuffer, RGReadUsage::ShaderSample);
            // Next frame's history comes from the RESOLVED buffer, never from
            // the filtered mask: re-accumulating an already spatially blurred
            // signal compounds the blur every frame until the penumbra swallows
            // the whole shadow.
            builder.ExtractHistoryTexture(ResourceNames::RayTracedShadowHistory, m_SelectedResolvedFramebuffer);
            builder.ExtractHistoryTexture(ResourceNames::RayTracedShadowMomentsHistory, m_SelectedResolvedFramebuffer,
                                          1u);
        }

        constexpr std::string_view versionTag = "RayTracedShadowPass";
        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Shadows.RayTracedShadowMask, RGWriteUsage::RenderTarget, versionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::RayTracedShadowMaskTexture) + "@" + std::string(versionTag), outputHandle, 0u));
        blackboard.Shadows.RayTracedShadowMaskTexture = GetPrimaryOutputTextureHandle();
    }

    void RayTracedShadowPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Created ONLY where GL_EXT_ray_query exists. Loading them anywhere
        // else is not a graceful degradation, it is a compile error in the log
        // on every OpenGL run — and the fallback this pass falls back TO does
        // not need them. The null shaders are what IsReadyForExecution reports,
        // and ResolveTechniqueForFrame turns that into a counted
        // RayTracingUnavailable rather than a warning nobody reads.
        if (!RenderCommand::SupportsRayTracing())
        {
            OLO_CORE_INFO("RayTracedShadowPass: hardware ray tracing unavailable — the pass stays inert and every "
                          "light falls back to its shadow map.");
            return;
        }

        m_TraceShader = Shader::Create("assets/shaders/RayTracedShadow.glsl");
        m_ResolveShader = Shader::Create("assets/shaders/RayTracedShadowResolve.glsl");
        m_FilterShader = Shader::Create("assets/shaders/RayTracedShadowFilter.glsl");

        if (!m_BlueNoiseTexture.IsValid())
            m_BlueNoiseTexture = CreateBlueNoiseTexture();

        OLO_CORE_INFO("RayTracedShadowPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    u32 RayTracedShadowPass::ResolveTechniqueForFrame(bool graphResourcesResolved)
    {
        m_Stats.Reset();
        m_Stats.MaskedOccludersShadowedAsSolid = m_MaskedOccluderCount;
        m_ChannelLights.fill(RayTracedShadowLightRequest{});

        const bool rayTracingAvailable = m_RayTracingScene != nullptr && m_RayTracingScene->IsAvailable();
        // A TLAS device address of zero means no TLAS has ever been built —
        // which is a DIFFERENT state from "no RT device", and conflating them
        // is how "the first frame has no shadows" gets misread as "this GPU
        // cannot ray trace".
        const bool tlasReady = rayTracingAvailable && m_RayTracingScene->GetTlasDeviceAddress() != 0u;
        // The mask only exists if the graph resolved every target the three
        // draws write. Asking the graph rather than assuming is the point:
        // SSGI's #902 postmortem is that a resolve blended against a buffer
        // that had not been created yet.
        const bool maskAvailable = m_Enabled && graphResourcesResolved && IsReadyForExecution();

        u32 assigned = 0;
        for (const auto& request : m_LightRequests)
        {
            const ShadowTechniqueInputs inputs{
                .Requested = ShadowTechnique::RayTraced,
                // A light only reaches this list because it casts shadows AND
                // opted in; the field is still passed rather than hard-coded
                // true so the policy function has one caller-independent shape.
                .LightCastsShadows = true,
                // The mask is a G-Buffer consumer, so it only exists on the
                // deferred path — and this pass is only registered there, so
                // reaching Execute at all IS that fact.
                .DeferredPathActive = true,
                .RayTracingAvailable = rayTracingAvailable,
                .TlasReady = tlasReady,
                .MaskAvailable = maskAvailable,
            };
            const auto decision = SelectShadowTechnique(inputs, assigned);
            m_Stats.Record(decision);

            if (decision.IsRayTraced() && request.UboLightIndex >= 0)
            {
                m_ChannelLights[static_cast<sizet>(decision.MaskChannel)] = request;
                ++assigned;
            }
        }

        if (m_Stats.FallbackLights > 0)
        {
            // Once per change of reason, not once per frame: this is the line
            // that tells a user why their sun is not ray traced, and a
            // per-frame version of it would be indistinguishable from spam and
            // therefore ignored.
            static ShadowTechniqueFallbackReason s_LastReported = ShadowTechniqueFallbackReason::None;
            const auto reason = m_Stats.DominantFallbackReason();
            if (reason != s_LastReported)
            {
                s_LastReported = reason;
                OLO_CORE_WARN("RayTracedShadowPass: {} of {} light(s) fell back to shadow maps — {}",
                              m_Stats.FallbackLights, m_LightRequests.size(), ToString(reason));
            }
        }

        return assigned;
    }

    void RayTracedShadowPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        Ref<Framebuffer> outputFramebuffer;
        Ref<Framebuffer> signalFramebuffer;
        Ref<Framebuffer> resolvedFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            outputFramebuffer = context.ResolveFramebuffer(outputHandle);
        if (m_SelectedSignalFramebuffer.IsValid())
            signalFramebuffer = context.ResolveFramebuffer(m_SelectedSignalFramebuffer);
        if (m_SelectedResolvedFramebuffer.IsValid())
            resolvedFramebuffer = context.ResolveFramebuffer(m_SelectedResolvedFramebuffer);

        RHI::ResourceHandle sceneDepthID{};
        RHI::ResourceHandle gbufferNormalID{};
        RHI::ResourceHandle velocityID{};
        RHI::ResourceHandle historyID{};
        RHI::ResourceHandle surfaceHistoryID{};
        RHI::ResourceHandle momentsHistoryID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedGBufferNormalTexture.IsValid())
            gbufferNormalID = context.ResolveTextureHandle(m_SelectedGBufferNormalTexture);
        if (m_SelectedVelocityTexture.IsValid())
            velocityID = context.ResolveTextureHandle(m_SelectedVelocityTexture);
        if (m_SelectedHistoryTexture.IsValid())
            historyID = context.ResolveTextureHandle(m_SelectedHistoryTexture);
        if (m_SelectedSurfaceHistoryTexture.IsValid())
            surfaceHistoryID = context.ResolveTextureHandle(m_SelectedSurfaceHistoryTexture);
        if (m_SelectedMomentsHistoryTexture.IsValid())
            momentsHistoryID = context.ResolveTextureHandle(m_SelectedMomentsHistoryTexture);

        const bool graphResourcesResolved = outputFramebuffer && signalFramebuffer && resolvedFramebuffer &&
                                            sceneDepthID.IsValid() && gbufferNormalID.IsValid();

        // Decide FIRST, and unconditionally. Every exit below this point has
        // already counted its lights and left the routing lanes off, which is
        // what makes "the mask was not produced" and "the lighting shader took
        // the raster branch" the same event rather than two that have to be
        // kept in sync.
        const u32 channelCount = ResolveTechniqueForFrame(graphResourcesResolved);

        // Turning the lighting shader's branch ON is deferred until after the
        // draws below; turning it OFF is not. That asymmetry is the whole
        // fallback design: every early return from here leaves the routing at
        // the inactive state ShadowMap::UploadUBO published this frame, so the
        // raster branch is what happens by default and the ray-traced branch
        // has to be actively earned.
        const auto publishRouting = [this, channelCount]()
        {
            if (m_ShadowMap == nullptr)
                return;
            std::array<i32, kRayTracedShadowMaskChannels> lightIndices{};
            lightIndices.fill(kNoRayTracedShadowChannel);
            for (u32 channel = 0; channel < channelCount; ++channel)
                lightIndices[channel] = m_ChannelLights[channel].UboLightIndex;
            m_ShadowMap->SetRayTracedShadowRouting(lightIndices, true);
        };

        if (channelCount == 0)
        {
            // Nothing to trace. Not an error and not warned about here — the
            // overwhelmingly common case is a scene where no light opted in,
            // and ResolveTechniqueForFrame already spoke for the cases that are
            // a genuine fallback.
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        // ------------------------------------------------------------------
        // The one UBO. Filled here rather than by the renderer's per-frame
        // upload because half of it — the TLAS address, the resolved channel
        // routing, whether a history exists — is only known now.
        // ------------------------------------------------------------------
        const auto& outSpec = outputFramebuffer->GetSpecification();
        const auto width = static_cast<f32>(std::max(outSpec.Width, 1u));
        const auto height = static_cast<f32>(std::max(outSpec.Height, 1u));

        UBOStructures::RayTracingShadowUBO params{};
        // RENDER-RELATIVE, not world (issue #429). The TLAS is built from GPU
        // Scene's render-relative instance transforms, and every other shader's
        // reconstructed "world position" is render-relative too because the
        // camera UBO is made relative in CommandDispatch. Handing this pass the
        // absolute view matrix put the ray origin a whole render-origin away
        // from the geometry it was tracing against — a no-op near the world
        // origin, which is exactly why no test on a small scene could see it,
        // and a 1024 m displacement the moment the origin grid snaps.
        const glm::mat4 relativeView = MakeViewRelative(m_View, m_RenderOrigin);
        params.InvView = glm::inverse(relativeView);
        params.InvProjection = glm::inverse(m_Projection);
        params.View = relativeView;

        const u64 tlasAddress = m_RayTracingScene != nullptr ? m_RayTracingScene->GetTlasDeviceAddress() : 0u;
        params.TlasAddressAndCounts = glm::uvec4(static_cast<u32>(tlasAddress & 0xFFFFFFFFull),
                                                 static_cast<u32>(tlasAddress >> 32u), channelCount, m_FrameIndex);

        for (u32 channel = 0; channel < channelCount; ++channel)
        {
            const RayTracedShadowLightRequest& light = m_ChannelLights[channel];
            // A punctual light's Vector is a world POSITION and has to move into
            // the same render-relative space as the ray origin; a directional
            // light's is a direction, which is translation-invariant and must
            // NOT be shifted.
            const glm::vec3 lightVector =
                light.Directional ? light.Vector : (light.Vector - m_RenderOrigin);
            params.LightVectors[channel] = glm::vec4(lightVector, light.Directional ? 1.0f : 2.0f);
            // A directional light's angular radius is converted to a tangent
            // here rather than in the shader: it is a per-light constant, so
            // doing it per pixel would be a transcendental per ray for a value
            // that never changes within the frame. A punctual light's radius
            // cannot be converted here — it needs the receiver distance, which
            // only the shader has — so it travels in metres.
            params.LightShapes[channel] =
                glm::vec4(light.Directional ? std::tan(std::max(light.Shape, 0.0f) * kDegreesToRadians)
                                            : std::max(light.Shape, 0.0f),
                          light.Range, 0.0f, 0.0f);
        }

        const u32 raysPerPixel = std::clamp(m_Settings.RaysPerPixel, 1u, 8u);
        const f32 maxRayDistance =
            std::isfinite(m_Settings.MaxRayDistance) ? std::max(m_Settings.MaxRayDistance, 0.0f) : 0.0f;
        const f32 normalBias =
            std::isfinite(m_Settings.RayOriginNormalBias) ? std::max(m_Settings.RayOriginNormalBias, 0.0f) : 0.0f;
        params.RayParams = glm::vec4(static_cast<f32>(raysPerPixel), maxRayDistance, normalBias, 0.0f);
        params.ScreenParams = glm::vec4(width, height, 1.0f / width, 1.0f / height);

        // Sanitize here, not only where the settings are loaded: these values
        // also arrive from a live edit, a script or an MCP write, and the
        // shader cannot be the backstop — OloTemporalBlendScalar's
        // clamp(feedback, 0, 0.98) is UNDEFINED for a NaN, which would then
        // spread through the history and never wash out.
        const f32 feedback =
            std::isfinite(m_Settings.TemporalFeedback) ? std::clamp(m_Settings.TemporalFeedback, 0.0f, 0.98f) : 0.0f;
        const f32 clipGamma =
            std::isfinite(m_Settings.TemporalClipGamma) ? std::clamp(m_Settings.TemporalClipGamma, 0.1f, 8.0f) : 1.5f;
        const bool historyUsable = m_Settings.TemporalAccumulation && historyID.IsValid() &&
                                   surfaceHistoryID.IsValid() && momentsHistoryID.IsValid();
        params.TemporalParams =
            glm::vec4(feedback, velocityID.IsValid() ? 1.0f : 0.0f, historyUsable ? 1.0f : 0.0f, clipGamma);

        const f32 filterRadius =
            std::isfinite(m_Settings.SpatialFilterRadius) ? std::clamp(m_Settings.SpatialFilterRadius, 0.0f, 8.0f) : 0.0f;
        params.FilterParams = glm::vec4(filterRadius, m_Settings.SpatialFilter ? 1.0f : 0.0f, 0.0f, 0.0f);

        // The issue's "ray count" telemetry. Derived rather than measured — see
        // ShadowTechniqueStats::ShadowRaysDispatchedUpperBound for why that is
        // the honest form. Computed in u64 because 1080p x 8 rays x 4 lights is
        // 66 million, which still fits u32 but is one setting change from not.
        m_Stats.ShadowRaysDispatchedUpperBound = static_cast<u64>(outSpec.Width) *
                                                 static_cast<u64>(outSpec.Height) *
                                                 static_cast<u64>(raysPerPixel) * static_cast<u64>(channelCount);

        // Rebind binding 65 before writing: other passes may displace this
        // indexed binding, and RayTracingProbe.comp declares its own block at
        // the same number (see the UBO's comment for why that is safe).
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, UBOStructures::RayTracingShadowUBO::GetSize());

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
            // Clear to FULLY LIT, not to zero. A clear colour of black would
            // make every pixel the draw does not cover — and every pixel of a
            // frame where the draw fails after the clear — fully shadowed, so
            // the failure mode of this pass would be a black screen rather than
            // an unshadowed one.
            context.SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f });
            context.Clear();
        };

        const auto drawFullscreen = [&context]()
        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(va);
        };

        // ------------------------------------------------------------------
        // Draw A — one ray per pixel per light, into RayTracedShadowSignal.
        // ------------------------------------------------------------------
        // Per-STAGE GPU time through the existing surface (issue #1056's
        // telemetry criterion). RenderGraphPlanExecutor already brackets the
        // whole node, which answers "what do ray-traced shadows cost"; these
        // three answer the question that actually drives a decision — whether
        // the cost is the trace or the denoiser. Same channel
        // RayTracingScenePass uses for its AS build.
        auto& gpuTimers = GPUPassTimerPool::GetInstance();

        gpuTimers.BeginSubPass("RayTracedShadowTrace");
        signalFramebuffer->Bind();
        {
            const auto& signalSpec = signalFramebuffer->GetSpecification();
            context.SetViewport(0, 0, signalSpec.Width, signalSpec.Height);
        }
        setFullscreenState();
        {
            // Attachment 1 carries the blocker distance, whose "no blocker"
            // value is 0 — the opposite of the visibility clear above, so the
            // two cannot share one clear colour. The draw writes both on every
            // covered pixel, and the fullscreen triangle covers all of them, so
            // attachment 1's clear only matters if the draw itself fails.
            constexpr std::array<u32, 2> signalAttachments{ 0u, 1u };
            RenderCommand::SetDrawBuffers(signalAttachments);
        }

        m_TraceShader->Bind();
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        BindBlueNoiseTexture(context, m_BlueNoiseTexture);
        drawFullscreen();
        signalFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // ------------------------------------------------------------------
        // Draw B — temporal resolve, into RayTracedShadowResolved.
        // ------------------------------------------------------------------
        const RHI::ResourceHandle signalTextureID = signalFramebuffer->GetColorAttachmentHandle(0);
        const RHI::ResourceHandle hitDistanceTextureID = signalFramebuffer->GetColorAttachmentHandle(1);

        gpuTimers.BeginSubPass("RayTracedShadowTemporalResolve");
        resolvedFramebuffer->Bind();
        {
            const auto& resolvedSpec = resolvedFramebuffer->GetSpecification();
            context.SetViewport(0, 0, resolvedSpec.Width, resolvedSpec.Height);
        }
        setFullscreenState();
        {
            constexpr std::array<u32, 2> resolvedAttachments{ 0u, 1u };
            RenderCommand::SetDrawBuffers(resolvedAttachments);
        }

        m_ResolveShader->Bind();
        context.BindTextureOrHeapOffset(0, signalTextureID, RHI::HeapSlotLifetime::FrameTransient);
        // With no history the shader ignores units 1..3 (TemporalParams.z == 0),
        // but they must still be bound to something valid or the samplers
        // dangle — a dangling sampler is undefined behaviour, not a zero read.
        context.BindTextureOrHeapOffset(1, historyID.IsValid() ? historyID : signalTextureID,
                                        historyID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                            : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(2, surfaceHistoryID.IsValid() ? surfaceHistoryID : gbufferNormalID,
                                        surfaceHistoryID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                                   : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(3, momentsHistoryID.IsValid() ? momentsHistoryID : signalTextureID,
                                        momentsHistoryID.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                                   : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(4, hitDistanceTextureID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                        velocityID.IsValid() ? velocityID : signalTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();
        resolvedFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // ------------------------------------------------------------------
        // Draw C — variance-guided spatial filter, into RayTracedShadowMask.
        // ------------------------------------------------------------------
        gpuTimers.BeginSubPass("RayTracedShadowSpatialFilter");
        outputFramebuffer->Bind();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        setFullscreenState();

        m_FilterShader->Bind();
        context.BindTextureOrHeapOffset(0, resolvedFramebuffer->GetColorAttachmentHandle(0),
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(1, resolvedFramebuffer->GetColorAttachmentHandle(1),
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();

        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // The mask exists now, so the lighting shader may read it.
        publishRouting();
    }

    void RayTracedShadowPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void RayTracedShadowPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void RayTracedShadowPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};
        m_SelectedMomentsHistoryTexture = {};
        m_SelectedSignalFramebuffer = {};
        m_SelectedResolvedFramebuffer = {};
        m_ChannelLights.fill(RayTracedShadowLightRequest{});
    }
} // namespace OloEngine
