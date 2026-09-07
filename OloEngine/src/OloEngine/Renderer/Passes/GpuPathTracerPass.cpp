#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GpuPathTracerPass.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>

namespace OloEngine
{
    namespace
    {
        constexpr std::string_view kVersionTag = "GpuPathTracerPass";

        // Attachment indices of the pass's one framebuffer. The shader's
        // layout(location = N) outputs are the same list.
        constexpr u32 kAttachmentColor = 0u;
        constexpr u32 kAttachmentAccum = 1u;
        constexpr u32 kAttachmentMoments = 2u;
        constexpr u32 kAttachmentAlbedo = 3u;
        constexpr u32 kAttachmentNormal = 4u;
        constexpr u32 kAttachmentVariance = 5u;
        constexpr u32 kAttachmentCount = 6u;

        // Texture units, matching the shader's layout(binding = N) samplers.
        constexpr u32 kUnitSceneColor = 0u;
        constexpr u32 kUnitHistory = 1u;
        constexpr u32 kUnitNormalHistory = 2u;
        constexpr u32 kUnitAlbedoHistory = 3u;
        constexpr u32 kUnitMomentsHistory = 4u;

        // After this many restarts in a row the accumulation is not converging
        // and the log says so once; a camera move is one restart, an edit two.
        constexpr u32 kConsecutiveRestartsWorthReporting = 8u;
    } // namespace

    GpuPathTracerPass::GpuPathTracerPass()
    {
        SetName("GpuPathTracerPass");
        OLO_CORE_INFO("Creating GpuPathTracerPass.");
    }

    bool GpuPathTracerPass::SettingsChangeInvalidatesAccumulation(const GpuPathTracerSettings& previous,
                                                                  const GpuPathTracerSettings& next) noexcept
    {
        // Everything that changes the integral or the sample sequence. Not on
        // the list, deliberately: SamplesPerFrame and MaxSamples only decide
        // how many of the SAME sequence are drawn per frame; the debug view
        // and the display scales change what is shown, not what is summed.
        return previous.Enabled != next.Enabled || previous.MaxBounces != next.MaxBounces ||
               previous.RussianRouletteStartBounce != next.RussianRouletteStartBounce ||
               previous.Seed != next.Seed ||
               previous.EnableNextEventEstimation != next.EnableNextEventEstimation ||
               !Math::BitwiseEqual(previous.MaxRadianceClamp, next.MaxRadianceClamp) ||
               !Math::BitwiseEqual(previous.RayEpsilon, next.RayEpsilon) ||
               !Math::BitwiseEqual(previous.MaxRayDistance, next.MaxRayDistance) ||
               !Math::BitwiseEqual(previous.UniformEnvironmentRadiance, next.UniformEnvironmentRadiance) ||
               !Math::BitwiseEqual(previous.EnvironmentCubeIntensity, next.EnvironmentCubeIntensity);
    }

    void GpuPathTracerPass::SetSettings(const GpuPathTracerSettings& settings) noexcept
    {
        if (m_HasSettings && SettingsChangeInvalidatesAccumulation(m_Settings, settings) && !m_RestartCause)
            m_RestartCause = TemporalHistoryInvalidationCause::FeatureToggled;
        m_Settings = settings;
        m_HasSettings = true;
    }

    void GpuPathTracerPass::SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                                              const glm::vec3& renderOrigin) noexcept
    {
        // Bitwise, not tolerant: TAA can reproject a sub-pixel move, a path
        // tracer cannot reproject anything, so ANY change of pose restarts the
        // sum. A camera that does not move produces bit-identical matrices
        // frame to frame, which is what keeps a static shot converging.
        if (m_HasCamera && (!Math::BitwiseEqual(m_View, view) || !Math::BitwiseEqual(m_Projection, projection) || !Math::BitwiseEqual(m_RenderOrigin, renderOrigin)) &&
            !m_RestartCause)
        {
            m_RestartCause = TemporalHistoryInvalidationCause::CameraCut;
        }
        m_View = view;
        m_Projection = projection;
        m_RenderOrigin = renderOrigin;
        m_HasCamera = true;
    }

    void GpuPathTracerPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedHistoryTexture = {};
        m_SelectedMomentsHistoryTexture = {};
        m_SelectedAlbedoHistoryTexture = {};
        m_SelectedNormalHistoryTexture = {};
        m_SelectedPrefilterTexture = {};

        // The upstream colour, passed through when the tracer stands down.
        // Everything the screen-space chain produced ranks above the lit
        // colour; PostProcessColor is not a candidate because its alias is
        // repointed downstream and reading it here would form a cycle.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ContactShadowColor, ResourceNames::ContactShadowColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::RTReflectionColor, ResourceNames::RTReflectionColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        // PathTracerColor is declared only when the tracer is enabled on a
        // backend that loaded the shader, so its absence is the off / non-RT
        // path and downstream aliases straight back to the upstream colour.
        if (!m_Enabled || !blackboard.Post.PathTracerColor.IsValid())
            return;

        // The by-name execution dependency RayTracingScenePass::Setup reserved
        // for ray-query consumers. The acceleration structure is not a graph
        // resource — there is no handle to Read — so this edge is the only
        // thing stopping a reorder from putting the AS build after the pass
        // that traces against it.
        builder.DependsOnPass("RayTracingScenePass");

        if (blackboard.Temporal.PathTracerHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.PathTracerHistory;
            [[maybe_unused]] const auto read = builder.Read(m_SelectedHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.PathTracerMomentsHistory.IsValid())
        {
            m_SelectedMomentsHistoryTexture = blackboard.Temporal.PathTracerMomentsHistory;
            [[maybe_unused]] const auto read = builder.Read(m_SelectedMomentsHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.PathTracerAlbedoHistory.IsValid())
        {
            m_SelectedAlbedoHistoryTexture = blackboard.Temporal.PathTracerAlbedoHistory;
            [[maybe_unused]] const auto read = builder.Read(m_SelectedAlbedoHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.PathTracerNormalHistory.IsValid())
        {
            m_SelectedNormalHistoryTexture = blackboard.Temporal.PathTracerNormalHistory;
            [[maybe_unused]] const auto read = builder.Read(m_SelectedNormalHistoryTexture, RGReadUsage::ShaderSample);
        }

        // The environment a ray escapes into. Optional: without it the
        // uniform radiance alone is the environment, and the shader is told
        // which case it is rather than sampling a dangling cube.
        if (blackboard.IBL.PrefilterMap.IsValid())
        {
            m_SelectedPrefilterTexture = blackboard.IBL.PrefilterMap;
            [[maybe_unused]] const auto read = builder.Read(m_SelectedPrefilterTexture, RGReadUsage::ShaderSample);
        }

        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Post.PathTracerColor, RGWriteUsage::RenderTarget, kVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        const auto viewName = [](std::string_view base)
        { return std::string(base) + "@" + std::string(kVersionTag); };
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerColorTexture), outputHandle, kAttachmentColor));
        // Named views of the AOV attachments, so a benchmark manifest can
        // capture them by base name. The handles are not kept: nothing in the
        // frame reads them except the extraction below and the capture.
        [[maybe_unused]] const auto accumView =
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerAccum), outputHandle, kAttachmentAccum);
        [[maybe_unused]] const auto momentsView =
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerMoments), outputHandle, kAttachmentMoments);
        [[maybe_unused]] const auto albedoView =
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerAlbedo), outputHandle, kAttachmentAlbedo);
        [[maybe_unused]] const auto normalView =
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerNormal), outputHandle, kAttachmentNormal);
        [[maybe_unused]] const auto varianceView =
            builder.CreateFramebufferAttachmentView(viewName(ResourceNames::PathTracerVariance), outputHandle, kAttachmentVariance);

        // Next frame's sums come from this frame's attachments. Declared from
        // Setup, not from PopulateBlackboard: BuildFrameGraph clears and
        // rebuilds extraction contracts before visiting nodes, so an earlier
        // declaration is discarded on every cache miss.
        builder.ExtractHistoryTexture(ResourceNames::PathTracerHistory, outputHandle, kAttachmentAccum);
        builder.ExtractHistoryTexture(ResourceNames::PathTracerMomentsHistory, outputHandle, kAttachmentMoments);
        builder.ExtractHistoryTexture(ResourceNames::PathTracerAlbedoHistory, outputHandle, kAttachmentAlbedo);
        builder.ExtractHistoryTexture(ResourceNames::PathTracerNormalHistory, outputHandle, kAttachmentNormal);
    }

    void GpuPathTracerPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Created ONLY where GL_EXT_ray_query exists. Loading it anywhere else
        // is a compile error in the log on every OpenGL run. The null shader
        // is what IsReadyForExecution reports, which ResolveAvailabilityForFrame
        // turns into a counted ShaderUnavailable every frame — it runs from the
        // per-frame wiring, not from Execute, so a pass whose target is never
        // declared and is culled still reports itself.
        if (!RenderCommand::SupportsRayTracing())
        {
            OLO_CORE_INFO("GpuPathTracerPass: hardware ray tracing unavailable — the GPU path tracer stays inert and "
                          "the rasterised frame shows.");
            return;
        }

        m_Shader = Shader::Create("assets/shaders/GpuPathTracer.glsl");

        OLO_CORE_INFO("GpuPathTracerPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void GpuPathTracerPass::ReportFallback(GpuPathTracerFallbackReason reason)
    {
        m_Stats.Fallback = reason;
        m_Stats.Active = (reason == GpuPathTracerFallbackReason::None);

        if (reason == m_LastReportedFallback)
            return;
        m_LastReportedFallback = reason;

        // Once per CHANGE of reason, not once per frame.
        if (reason == GpuPathTracerFallbackReason::None)
            OLO_CORE_INFO("GpuPathTracerPass: the GPU path tracer is active.");
        else if (reason == GpuPathTracerFallbackReason::GPUSceneUnavailable)
        {
            OLO_CORE_WARN("GpuPathTracerPass: the GPU path tracer stood down — {} (gpuScene={}, instanceSlots={}, "
                          "geometrySlots={}, materialSlots={})",
                          ToString(reason), m_GPUScene != nullptr,
                          m_GPUScene != nullptr ? m_GPUScene->GetInstanceSlotCount() : 0u,
                          m_GPUScene != nullptr ? m_GPUScene->GetGeometrySlotCount() : 0u,
                          m_GPUScene != nullptr ? m_GPUScene->GetMaterialSlotCount() : 0u);
        }
        else if (reason != GpuPathTracerFallbackReason::NotRequested)
            OLO_CORE_WARN("GpuPathTracerPass: the GPU path tracer stood down — {}", ToString(reason));
    }

    void GpuPathTracerPass::ResolveAvailabilityForFrame()
    {
        // The accumulation state outlives the per-frame counters: it is what
        // the history planes carry, and the next frame's sample indices and
        // the MaxSamples cap are computed from it.
        m_Stats.Reset();
        m_Stats.AccumulatedSamplesPerPixel = m_AccumulatedSamplesPerPixel;
        m_Stats.ConsecutiveRestarts = m_ConsecutiveRestarts;

        // Ordered most-fundamental first, so the reported reason names the
        // ROOT cause rather than the first symptom. m_Settings.Enabled is the
        // user's intent and is tested first and alone; m_Enabled folds in the
        // readiness check and would report a non-RT device as "switched off".
        GpuPathTracerFallbackReason reason = GpuPathTracerFallbackReason::None;
        if (!m_Settings.Enabled)
            reason = GpuPathTracerFallbackReason::NotRequested;
        else if (!m_Shader || !m_Shader->IsReady() || !m_ParamsUBO)
            reason = GpuPathTracerFallbackReason::ShaderUnavailable;
        else if (m_RayTracingScene == nullptr || !m_RayTracingScene->IsAvailable())
            reason = GpuPathTracerFallbackReason::RayTracingUnavailable;
        // A zero TLAS address is a DIFFERENT state from "no RT device": no
        // structure has been built yet, or the scene staged nothing traceable.
        else if (m_RayTracingScene->GetTlasDeviceAddress() == 0u)
            reason = GpuPathTracerFallbackReason::AccelerationStructureEmpty;
        else if (m_GPUScene == nullptr || m_GPUScene->GetInstanceSlotCount() == 0u)
            reason = GpuPathTracerFallbackReason::GPUSceneUnavailable;
        // The area-light gather is decided at BeginScene from the same setting
        // this pass reads at EndScene. A setting that flipped between the two
        // (a benchmark or an MCP write landing mid-frame) leaves a frame with
        // a live tracer and no emitter table; traced, its samples would be a
        // different (unbiased, far noisier) estimator summed into the same
        // planes. One frame is skipped instead and the sums carry forward.
        else if (m_EmissiveTable != nullptr && !m_EmissiveTable->IsGathering())
            reason = GpuPathTracerFallbackReason::EmissiveTableNotGathered;
        // TargetUnavailable is Execute's: only it can see whether the graph
        // produced the target.

        ReportFallback(reason);

        if (m_Stats.Active)
        {
            // Standing limitations of this slice, true whenever it ran at all.
            m_Stats.HitsShadedUntextured = true;
            m_Stats.MaskedGeometryTracedAsSolid = true;
            CountSceneForStats();
        }
    }

    void GpuPathTracerPass::CountSceneForStats()
    {
        if (m_GPUScene == nullptr)
            return;

        // What the shader will see, counted from the same committed records.
        // The shader's light loop stops at kGpuPathTracerMaxLights slots; a
        // live light past that is counted separately, because it lights the
        // raster frame and the CPU reference and not this tracer.
        const u32 lightSlots = m_GPUScene->GetLightSlotCount();
        for (u32 slot = 0; slot < lightSlots; ++slot)
        {
            const GPUSceneLight* light = m_GPUScene->GetLiveLightRecordBySlot(slot);
            if (light == nullptr)
                continue;
            if (light->Type == std::to_underlying(GPUSceneLightType::SphereArea))
                ++m_Stats.SphereAreaLightsIgnored;
            else if (slot >= kGpuPathTracerMaxLights)
                ++m_Stats.PunctualLightsBeyondShaderBound;
            else
                ++m_Stats.PunctualLights;
        }
        if ((m_Stats.PunctualLightsBeyondShaderBound > 0u) != m_ReportedLightsBeyondShaderBound)
        {
            m_ReportedLightsBeyondShaderBound = m_Stats.PunctualLightsBeyondShaderBound > 0u;
            if (m_ReportedLightsBeyondShaderBound)
            {
                OLO_CORE_WARN("GpuPathTracerPass: {} punctual light(s) sit at GPU Scene slots past the shader's bound "
                              "of {} and do not light the traced image",
                              m_Stats.PunctualLightsBeyondShaderBound, kGpuPathTracerMaxLights);
            }
        }

        const u32 instanceSlots = m_GPUScene->GetInstanceSlotCount();
        for (u32 slot = 0; slot < instanceSlots; ++slot)
        {
            const GPUSceneInstance* instance = m_GPUScene->GetLiveInstanceRecordBySlot(slot);
            if (instance == nullptr)
                continue;
            const GPUSceneMaterial* material =
                m_GPUScene->GetLiveMaterialRecordBySlot(instance->MaterialIndex, instance->MaterialGeneration);
            if (material != nullptr && material->ClosureVersion == std::to_underlying(PBRModel::Legacy))
                ++m_Stats.LegacyMaterialsShadedAsClosureV2;
        }

        if (m_EmissiveTable != nullptr)
        {
            m_Stats.EmissiveTriangles = m_EmissiveTable->GetTriangleCount();
            m_Stats.EmissiveTotalArea = m_EmissiveTable->GetTotalArea();
            // A table with triangles and no address is a backend without
            // buffer addresses: NEE would silently see no area lights.
            m_Stats.EmissiveTableUnaddressable =
                m_Stats.EmissiveTriangles > 0u && m_EmissiveTable->GetDeviceAddress() == 0u;
            if (m_Stats.EmissiveTableUnaddressable != m_ReportedEmissiveTableUnaddressable)
            {
                m_ReportedEmissiveTableUnaddressable = m_Stats.EmissiveTableUnaddressable;
                if (m_ReportedEmissiveTableUnaddressable)
                {
                    OLO_CORE_WARN("GpuPathTracerPass: the emissive table holds {} triangles but has no device address "
                                  "on this backend — next-event estimation sees no area lights",
                                  m_Stats.EmissiveTriangles);
                }
            }
        }
    }

    void GpuPathTracerPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        RHI::ResourceHandle inputColorID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            outputFramebuffer = context.ResolveFramebuffer(outputHandle);

        RHI::ResourceHandle historyID{};
        RHI::ResourceHandle momentsHistoryID{};
        RHI::ResourceHandle albedoHistoryID{};
        RHI::ResourceHandle normalHistoryID{};
        RHI::ResourceHandle prefilterID{};
        if (m_SelectedHistoryTexture.IsValid())
            historyID = context.ResolveTextureHandle(m_SelectedHistoryTexture);
        if (m_SelectedMomentsHistoryTexture.IsValid())
            momentsHistoryID = context.ResolveTextureHandle(m_SelectedMomentsHistoryTexture);
        if (m_SelectedAlbedoHistoryTexture.IsValid())
            albedoHistoryID = context.ResolveTextureHandle(m_SelectedAlbedoHistoryTexture);
        if (m_SelectedNormalHistoryTexture.IsValid())
            normalHistoryID = context.ResolveTextureHandle(m_SelectedNormalHistoryTexture);
        if (m_SelectedPrefilterTexture.IsValid())
            prefilterID = context.ResolveTextureHandle(m_SelectedPrefilterTexture);

        const bool graphResolved = outputFramebuffer && inputColorID.IsValid();

        // The verdict was resolved from the per-frame wiring, before the graph
        // was built; the one reason only THIS point can see is a target the
        // graph did not produce. The verdict does NOT gate the draw: when the
        // tracer cannot answer, running the draw is still correct, because the
        // shader passes the input colour through on a zero TLAS address and
        // carries the history planes forward unchanged — and the extraction
        // publishes whatever the attachments hold, so the draw is also what
        // keeps the history planes DEFINED on a stood-down frame.
        if (!graphResolved)
        {
            ReportFallback(GpuPathTracerFallbackReason::TargetUnavailable);
            return;
        }
        const bool active = m_Stats.Active;

        const auto& outSpec = outputFramebuffer->GetSpecification();
        const f32 width = static_cast<f32>(outSpec.Width);
        const f32 height = static_cast<f32>(outSpec.Height);
        if (width <= 0.0f || height <= 0.0f)
            return;

        // All four planes must be present for the sums to be consistent; a
        // partially imported history (one plane created later than the rest)
        // restarts rather than mixing counts.
        const bool historyValid = historyID.IsValid() && momentsHistoryID.IsValid() && albedoHistoryID.IsValid() &&
                                  normalHistoryID.IsValid();

        UBOStructures::RayTracingPathTracerUBO params{};
        const GpuPathTracerSettings settings = SanitizeGpuPathTracerSettings(m_Settings);

        // RENDER-RELATIVE, not world: the TLAS is built from the GPU Scene's
        // render-relative instance transforms, so a world-space ray origin
        // would miss every instance by the origin offset. inverse(P * V) in
        // the engine's GL clip convention — see the UBO's note for why the
        // backend-adjusted inverse is NOT wanted here.
        const glm::mat4 relativeView = MakeViewRelative(m_View, m_RenderOrigin);
        const glm::mat4 invView = glm::inverse(relativeView);
        params.InvViewProjection = glm::inverse(m_Projection * relativeView);
        params.CameraPosition = glm::vec4(glm::vec3(invView[3]), 0.0f);

        // A ZERO TLAS ADDRESS IS THE OFF SWITCH, uploaded deliberately: it is
        // the value the shader's first guard tests, so an inactive tracer
        // reaches the GPU as "pass through" instead of as a stale address.
        const u64 tlasAddress = active ? m_RayTracingScene->GetTlasDeviceAddress() : 0u;
        params.TlasAddress = glm::uvec4(static_cast<u32>(tlasAddress & 0xFFFFFFFFull),
                                        static_cast<u32>(tlasAddress >> 32u), RayTracing::kInstanceMaskAll,
                                        settings.Seed);
        // The light slot count is clamped to the shader's loop bound; what the
        // clamp cuts off is counted in PunctualLightsBeyondShaderBound.
        params.SlotCounts = active ? glm::uvec4(m_GPUScene->GetInstanceSlotCount(), m_GPUScene->GetGeometrySlotCount(),
                                                m_GPUScene->GetMaterialSlotCount(),
                                                std::min(m_GPUScene->GetLightSlotCount(), kGpuPathTracerMaxLights))
                                   : glm::uvec4(0u);

        const u64 emissiveAddress = (active && m_EmissiveTable != nullptr) ? m_EmissiveTable->GetDeviceAddress() : 0u;
        const u32 emissiveCount = (emissiveAddress != 0u) ? m_EmissiveTable->GetTriangleCount() : 0u;
        u32 flags = 0u;
        if (settings.EnableNextEventEstimation)
            flags |= kGpuPathTracerFlagNextEventEstimation;
        if (prefilterID.IsValid() && settings.EnvironmentCubeIntensity > 0.0f)
            flags |= kGpuPathTracerFlagEnvironmentCube;
        if (historyValid)
            flags |= kGpuPathTracerFlagHistoryValid;
        params.EmissiveTable = glm::uvec4(static_cast<u32>(emissiveAddress & 0xFFFFFFFFull),
                                          static_cast<u32>(emissiveAddress >> 32u), emissiveCount, flags);

        // Inside the shader's compile-time loop bounds and finite, from the one
        // sanitizer every write path shares (see GpuPathTracerTypes.h).
        params.PathParams = glm::uvec4(settings.MaxBounces, settings.RussianRouletteStartBounce, settings.SamplesPerFrame,
                                       settings.MaxSamples);
        params.RayParams = glm::vec4(settings.RayEpsilon, settings.MaxRadianceClamp, settings.MaxRayDistance,
                                     settings.EnvironmentCubeIntensity);

        const f32 pdfArea = (emissiveCount != 0u) ? m_EmissiveTable->GetPdfArea() : 0.0f;
        params.Environment = glm::vec4(settings.UniformEnvironmentRadiance, pdfArea);

        params.ScreenParams = glm::vec4(width, height, 1.0f / width, 1.0f / height);

        params.DebugParams = glm::vec4(static_cast<f32>(std::to_underlying(settings.DebugView)),
                                       settings.SampleCountDisplayScale, settings.VarianceDisplayScale, 0.0f);

        // The accumulation state as the shader will read it. The per-pixel
        // count is uniform across the image by construction (every pixel
        // draws the same number of samples per frame), so one CPU counter
        // mirrors it; it restarts whenever the history is not imported, and a
        // stood-down frame (active false) adds nothing while the shader
        // carries the imported sums forward unchanged.
        if (!historyValid)
        {
            m_AccumulatedSamplesPerPixel = 0;
            ++m_ConsecutiveRestarts;
            if (m_ConsecutiveRestarts == kConsecutiveRestartsWorthReporting)
            {
                OLO_CORE_WARN("GpuPathTracerPass: the accumulation restarted {} frames in a row — something invalidates "
                              "it every frame (an animated entity, a camera that never holds still) and the image "
                              "cannot converge",
                              m_ConsecutiveRestarts);
            }
        }
        else
        {
            m_ConsecutiveRestarts = 0;
        }
        m_Stats.AccumulatedSamplesPerPixel = m_AccumulatedSamplesPerPixel;
        m_Stats.ConsecutiveRestarts = m_ConsecutiveRestarts;
        m_Stats.HistoryValid = historyValid;
        u32 samplesThisFrame = active ? settings.SamplesPerFrame : 0u;
        if (active && settings.MaxSamples != 0u)
        {
            samplesThisFrame = (m_AccumulatedSamplesPerPixel >= settings.MaxSamples)
                                   ? 0u
                                   : std::min(settings.SamplesPerFrame, settings.MaxSamples - m_AccumulatedSamplesPerPixel);
        }
        m_Stats.SamplesTracedThisFrame = samplesThisFrame;

        // The issue's ray telemetry. An UPPER bound, derived: every sample
        // traces at most maxBounces closest-hit rays, plus per bounce one
        // shadow ray per punctual light and one for the emissive sample.
        const u64 pixels = static_cast<u64>(outSpec.Width) * static_cast<u64>(outSpec.Height);
        const u64 raysPerBounce = 1ull + (settings.EnableNextEventEstimation
                                              ? static_cast<u64>(m_Stats.PunctualLights) + (emissiveCount != 0u ? 1ull : 0ull)
                                              : 0ull);
        m_Stats.RaysDispatchedUpperBound =
            pixels * static_cast<u64>(samplesThisFrame) * static_cast<u64>(settings.MaxBounces) * raysPerBounce;

        // Rebind binding 65 before writing: three other blocks share it and
        // each owner rebinds its own buffer before its own draws.
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, UBOStructures::RayTracingPathTracerUBO::GetSize());

        // The instance / geometry / material / light tables at their canonical
        // SSBO bindings. Bound HERE rather than relied on from an earlier pass:
        // an indexed buffer binding is global state any draw may displace.
        if (m_GPUScene != nullptr)
            m_GPUScene->Bind();

        auto& gpuTimers = GPUPassTimerPool::GetInstance();
        gpuTimers.BeginSubPass("GpuPathTracerTrace");

        outputFramebuffer->Bind();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        {
            constexpr std::array<u32, kAttachmentCount> drawBuffers{ 0u, 1u, 2u, 3u, 4u, 5u };
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            RenderCommand::SetDrawBuffers(std::span<const u32>(drawBuffers.data(), drawBuffers.size()));
        }

        m_Shader->Bind();
        context.BindTextureOrHeapOffset(kUnitSceneColor, inputColorID, RHI::HeapSlotLifetime::FrameTransient);
        // With no history the shader never samples units 1..4 (the flag is
        // clear), but a sampler declared by the shader and left unbound is
        // undefined behaviour, not a zero read — so every unit gets a valid
        // sampler2D, the input colour standing in. Registry-owned histories
        // are pass-lifetime textures, hence Persistent; the pooled input is
        // FrameTransient.
        context.BindTextureOrHeapOffset(kUnitHistory, historyValid ? historyID : inputColorID,
                                        historyValid ? RHI::HeapSlotLifetime::Persistent
                                                     : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(kUnitNormalHistory, historyValid ? normalHistoryID : inputColorID,
                                        historyValid ? RHI::HeapSlotLifetime::Persistent
                                                     : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(kUnitAlbedoHistory, historyValid ? albedoHistoryID : inputColorID,
                                        historyValid ? RHI::HeapSlotLifetime::Persistent
                                                     : RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(kUnitMomentsHistory, historyValid ? momentsHistoryID : inputColorID,
                                        historyValid ? RHI::HeapSlotLifetime::Persistent
                                                     : RHI::HeapSlotLifetime::FrameTransient);
        // UNCONDITIONALLY, with a null handle when there is no environment —
        // exactly what the reflection tier does at this same unit: TEX_USER_1
        // is a general-purpose slot other passes fill with a sampler2D, and a
        // samplerCube declaration reading a sampler2D binding is a type
        // mismatch for the whole draw. The guard is in the SHADER (the
        // environment-cube flag) and the bind always happens.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_USER_1,
                                        prefilterID.IsValid() ? prefilterID : RHI::NullResource,
                                        RHI::HeapSlotLifetime::FrameTransient);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(va);
        }

        // Restore the depth mask the fullscreen state turned off, as every
        // neighbouring post pass does.
        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // After the draw, the per-pixel count the NEXT frame's history carries.
        m_AccumulatedSamplesPerPixel += samplesThisFrame;
    }

    void GpuPathTracerPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GpuPathTracerPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GpuPathTracerPass::OnReset()
    {
        m_SelectedHistoryTexture = {};
        m_SelectedMomentsHistoryTexture = {};
        m_SelectedAlbedoHistoryTexture = {};
        m_SelectedNormalHistoryTexture = {};
        m_SelectedPrefilterTexture = {};
        m_Stats.Reset();
        m_AccumulatedSamplesPerPixel = 0;
        m_ConsecutiveRestarts = 0;
        m_LastReportedFallback = GpuPathTracerFallbackReason::Count;
        m_ReportedLightsBeyondShaderBound = false;
        m_ReportedEmissiveTableUnaddressable = false;
    }

} // namespace OloEngine
