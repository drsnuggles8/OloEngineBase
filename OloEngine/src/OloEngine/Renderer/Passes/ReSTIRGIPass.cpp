#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ReSTIRGIPass.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
#include "OloEngine/Renderer/PathTracing/MaterialTextureTable.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // The shader-side loop bound on GPU Scene light slots (OLO_LIGHT_MAX_SLOTS
        // in include/LightSampling.glsl). Live lights past it light the clustered
        // frame and the path tracer, and are reachable by neither this tier's NEE
        // draw at the bounce vertex nor its engagement criterion — so the clamp is
        // applied in one place and both halves read the clamped number.
        constexpr u32 kReSTIRGIMaxLightSlots = 256u;
    } // namespace

    ReSTIRGIPass::ReSTIRGIPass()
    {
        SetName("ReSTIRGIPass");
        OLO_CORE_INFO("Creating ReSTIRGIPass.");
    }

    void ReSTIRGIPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepth = {};
        m_SelectedGBufferAlbedo = {};
        m_SelectedGBufferNormal = {};
        m_SelectedGBufferEmissive = {};
        m_SelectedVelocity = {};
        m_SelectedPrefilterTexture = {};
        m_SelectedInitial = {};
        m_SelectedTemporal = {};
        m_SelectedSpatial = {};
        m_SelectedReservoirSampleHistory = {};
        m_SelectedReservoirRadianceHistory = {};
        m_SelectedReservoirStateHistory = {};
        m_SelectedSurfaceHistory = {};
        m_SelectedMomentsHistory = {};

        if (!m_Enabled || !blackboard.Lighting.ReSTIRGIRadiance.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() || !blackboard.GBuffer.GBufferAlbedo.IsValid() ||
            !blackboard.GBuffer.GBufferNormal.IsValid() || !blackboard.GBuffer.GBufferEmissive.IsValid())
        {
            return;
        }

        // The by-name execution dependency RayTracingScenePass::Setup reserved for
        // ray-query consumers. The acceleration structure is not a graph resource
        // — there is no handle to Read — so this edge is the only thing that stops
        // a reordering from putting the AS build after the pass that traces
        // against it. The symptom would be a frame with no ReSTIR GI and nothing
        // in the log.
        builder.DependsOnPass("RayTracingScenePass");

        m_SelectedSceneDepth = blackboard.Scene.SceneDepth;
        m_SelectedGBufferAlbedo = blackboard.GBuffer.GBufferAlbedo;
        m_SelectedGBufferNormal = blackboard.GBuffer.GBufferNormal;
        m_SelectedGBufferEmissive = blackboard.GBuffer.GBufferEmissive;
        [[maybe_unused]] const auto depthRead = builder.Read(m_SelectedSceneDepth, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto albedoRead = builder.Read(m_SelectedGBufferAlbedo, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto normalRead = builder.Read(m_SelectedGBufferNormal, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto emissiveRead =
            builder.Read(m_SelectedGBufferEmissive, RGReadUsage::ShaderSample);

        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocity = blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead = builder.Read(m_SelectedVelocity, RGReadUsage::ShaderSample);
        }

        // The environment a bounce ray escapes into. Optional: without it the
        // uniform radiance alone is the environment, and the shader is told which
        // case it is rather than sampling a dangling cube. Same arrangement as
        // GpuPathTracerPass's.
        if (blackboard.IBL.PrefilterMap.IsValid())
        {
            m_SelectedPrefilterTexture = blackboard.IBL.PrefilterMap;
            [[maybe_unused]] const auto read =
                builder.Read(m_SelectedPrefilterTexture, RGReadUsage::ShaderSample);
        }

        // The four reservoir framebuffers, each written then sampled inside this
        // one Execute — the same intra-pass idiom SSGISignal, RayTracedShadowSignal
        // and ReSTIRDIPass use.
        const auto declareReservoirTarget = [&builder](RGFramebufferHandle handle)
        {
            if (!handle.IsValid())
                return;
            builder.AllowSamePassReadWrite(handle);
            builder.Write(handle, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderSample);
        };
        m_SelectedInitial = blackboard.Scratch.ReSTIRGIInitial;
        m_SelectedTemporal = blackboard.Scratch.ReSTIRGITemporal;
        m_SelectedSpatial[0] = blackboard.Scratch.ReSTIRGISpatial0;
        m_SelectedSpatial[1] = blackboard.Scratch.ReSTIRGISpatial1;
        declareReservoirTarget(m_SelectedInitial);
        declareReservoirTarget(m_SelectedTemporal);
        declareReservoirTarget(m_SelectedSpatial[0]);
        declareReservoirTarget(m_SelectedSpatial[1]);

        // The #976 surface record this frame's reservoirs were selected against,
        // and the plane next frame's temporal draw reconstructs the previous
        // shading point from. Declared from Setup, not from PopulateBlackboard:
        // BuildFrameGraph clears and rebuilds extraction contracts before visiting
        // nodes, so an earlier declaration is discarded on every cache miss.
        //
        // The extraction sources are attachments of the INITIAL target for the
        // surface plane and of the LAST SPATIAL target for the reservoir planes,
        // because the reservoir next frame's temporal draw should merge is the one
        // that survived every reuse.
        if (m_SelectedInitial.IsValid())
        {
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRGISurfaceHistory, m_SelectedInitial, 3u);
        }
        const u32 spatialPasses = std::max(m_Settings.SpatialPasses, 1u);
        const RGFramebufferHandle finalReservoir =
            m_Settings.SpatialReuse ? m_SelectedSpatial[(spatialPasses - 1u) % 2u] : m_SelectedTemporal;
        if (finalReservoir.IsValid())
        {
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRGIReservoirSampleHistory, finalReservoir, 0u);
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRGIReservoirRadianceHistory, finalReservoir, 1u);
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRGIReservoirStateHistory, finalReservoir, 2u);
        }

        // THE NODE'S OWN OUTPUT. Declaring the reads and the scratch writes is not
        // enough: without a WriteNewVersion on the radiance target this node has no
        // primary output, GetPrimaryOutputFramebufferHandle() comes back invalid,
        // and Execute reports TargetUnavailable — while the graph has in fact
        // declared every resource, which is a diagnosis that sends you looking in
        // exactly the wrong place.
        constexpr std::string_view versionTag = "ReSTIRGIPass";
        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Lighting.ReSTIRGIRadiance, RGWriteUsage::RenderTarget, versionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::ReSTIRGIRadianceTexture) + "@" + std::string(versionTag), outputHandle,
            0u));
        blackboard.Lighting.ReSTIRGIRadianceTexture = GetPrimaryOutputTextureHandle();

        // The moments ride on attachment 1 of the version this node just wrote, not
        // on the pre-write handle: extracting from the latter would publish whatever
        // the transient pool held before the resolve ran.
        builder.ExtractHistoryTexture(ResourceNames::ReSTIRGIMomentsHistory, outputHandle, 1u);

        const auto readHistory = [&builder](RGTextureHandle handle, RGTextureHandle& out)
        {
            if (!handle.IsValid())
                return;
            out = handle;
            [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderSample);
        };
        readHistory(blackboard.Temporal.ReSTIRGIReservoirSampleHistory, m_SelectedReservoirSampleHistory);
        readHistory(blackboard.Temporal.ReSTIRGIReservoirRadianceHistory, m_SelectedReservoirRadianceHistory);
        readHistory(blackboard.Temporal.ReSTIRGIReservoirStateHistory, m_SelectedReservoirStateHistory);
        readHistory(blackboard.Temporal.ReSTIRGISurfaceHistory, m_SelectedSurfaceHistory);
        readHistory(blackboard.Temporal.ReSTIRGIMomentsHistory, m_SelectedMomentsHistory);
    }

    void ReSTIRGIPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;

        // Created ONLY where GL_EXT_ray_query exists. Loading them anywhere else is
        // not a graceful degradation, it is four compile errors in the log on every
        // OpenGL run — and the tier this falls back TO does not need them. The null
        // shaders are what IsReadyForExecution reports, and ResolveTechniqueForFrame
        // turns that into a counted RayTracingUnavailable rather than a warning
        // nobody reads.
        if (!RenderCommand::SupportsRayTracing())
        {
            OLO_CORE_INFO("ReSTIRGIPass: hardware ray tracing unavailable — the pass stays inert and indirect "
                          "diffuse comes from the probe ladder.");
            return;
        }

        m_InitialShader = Shader::Create("assets/shaders/ReSTIR_GI_InitialSample.glsl");
        m_TemporalShader = Shader::Create("assets/shaders/ReSTIR_GI_TemporalReuse.glsl");
        m_SpatialShader = Shader::Create("assets/shaders/ReSTIR_GI_SpatialReuse.glsl");
        m_ResolveShader = Shader::Create("assets/shaders/ReSTIR_GI_Resolve.glsl");

        OLO_CORE_INFO("ReSTIRGIPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    bool ReSTIRGIPass::ResolveTechniqueForFrame(bool graphResourcesResolved)
    {
        m_Stats.Reset();
        m_Stats.BiasMode = m_Settings.BiasMode;
        m_Stats.ReservoirLayoutVersion = ReSTIR::kGIReservoirLayoutVersion;
        m_Stats.SettingsClamped = m_SettingsClamped;
        m_Stats.MaxSampleAge = m_Settings.MaxSampleAge;

        const bool rayTracingAvailable = m_RayTracingScene != nullptr && m_RayTracingScene->IsAvailable();
        // A TLAS device address of zero means no TLAS has ever been built — a
        // DIFFERENT state from "no RT device", and conflating them is how "the
        // first frame has probe-grid GI" gets misread as "this GPU cannot ray
        // trace".
        const bool tlasReady = rayTracingAvailable && m_RayTracingScene->GetTlasDeviceAddress() != 0u;
        const bool gpuSceneAvailable = m_GPUScene != nullptr && m_GPUScene->GetInstanceSlotCount() != 0u;

        // The emitter set, counted the way the BOUNCE VERTEX's NEE draw weights it:
        // one candidate per live light slot the shader's loop can reach, plus one
        // per emissive triangle.
        //
        // EVERY LIGHT TYPE, DIRECTIONAL INCLUDED, which is where this differs from
        // ReSTIRDIPass's count. DI excludes directional lights because the clustered
        // loop keeps them for their cascades, their mask channel and their cloud
        // shadow; none of that applies at a bounce vertex, and the sun bouncing off
        // a floor is exactly the indirect light this tier exists for.
        u32 lightCount = 0;
        if (m_GPUScene != nullptr)
        {
            // The same loop shape GpuPathTracerPass uses, and for the same reason:
            // GetLightSlotCount() is a SLOT count, and a retired slot returns null
            // — so a subtraction of the two counts would report retired slots as
            // lights the shader cannot reach.
            const u32 lightSlotCount = std::min(m_GPUScene->GetLightSlotCount(), kReSTIRGIMaxLightSlots);
            for (u32 slot = 0; slot < lightSlotCount; ++slot)
            {
                if (m_GPUScene->GetLiveLightRecordBySlot(slot) != nullptr)
                    ++lightCount;
            }
        }
        const u32 emissiveTriangles =
            (m_EmissiveTable != nullptr && m_EmissiveTable->GetDeviceAddress() != 0u)
                ? m_EmissiveTable->GetTriangleCount()
                : 0u;

        // An environment source counts as something for one bounce to gather: an
        // outdoor scene with no lights at all is lit entirely by it, so leaving it
        // out of the criterion would stand the tier down on exactly the scenes it
        // renders best. Non-zero intensity OR a non-black uniform term — either is
        // a sky an escaping ray can collect.
        // A BOUND CUBE at non-zero intensity, or a non-black uniform term. The
        // bound-ness is what makes this a measurement rather than a constant: the
        // intensity defaults to 1.0, so testing it alone made this true in every
        // scene ever loaded and NoIndirectSourceInScene unreachable.
        const bool environmentAvailable =
            (m_EnvironmentCubeBound && m_EnvironmentCubeIntensity > 0.0f) ||
            glm::dot(m_UniformEnvironmentRadiance, m_UniformEnvironmentRadiance) > 0.0f;

        m_Stats.Engagement.LightCount = lightCount;
        m_Stats.Engagement.EmissiveTriangles = emissiveTriangles;
        m_Stats.Engagement.EnvironmentAvailable = environmentAvailable;

        const ReSTIRGITechniqueInputs inputs{
            .Requested = m_Settings.Enabled,
            // The reservoir passes are G-Buffer consumers, and this pass is only
            // registered on the deferred path — so reaching Execute at all IS that
            // fact. The field is still passed rather than hard-coded so the policy
            // function has one caller-independent shape.
            .DeferredPathActive = true,
            .ShadersReady = IsReadyForExecution(),
            .RayTracingAvailable = rayTracingAvailable,
            .TlasReady = tlasReady,
            .GPUSceneAvailable = gpuSceneAvailable,
            .TargetsAvailable = graphResourcesResolved,
            .HistoryLayoutMatches = m_HistoryLayoutMatches,
            .Engagement = m_Stats.Engagement,
        };
        const auto decision = SelectReSTIRGITechnique(inputs);
        m_Stats.Record(decision);

        // THE HAND-OFF, from the one function that owns it. Computed here rather
        // than in the three passes that act on it, so "who added indirect diffuse
        // to this frame" has a single answer that a test can assert and a user can
        // read out of the statistics.
        m_Stats.Sources = SelectIndirectDiffuseSources(IndirectDiffuseInputs{
            .ReSTIRGIActive = decision.IsReSTIR(),
            .SSGIRequested = m_SSGIRequested,
            .DDGITailRequested = m_Settings.DDGITail && m_ProbeVolumeAvailable,
        });
        // SSGI was asked for and this tier took the term. Counted, because "my
        // SSGI slider does nothing" is otherwise unanswerable from the image.
        m_Stats.SSGIStoodDown = (m_SSGIRequested && !m_Stats.Sources.SSGIComposite) ? 1u : 0u;

        if (!decision.IsReSTIR() && decision.Reason != ReSTIRGIFallbackReason::NotRequested)
        {
            if (decision.Reason != m_LastReportedFallback)
            {
                m_LastReportedFallback = decision.Reason;
                OLO_CORE_WARN("ReSTIRGIPass: indirect diffuse fell back to the probe ladder — {} "
                              "(lights={}, emissive triangles={}, environment={})",
                              ToString(decision.Reason), m_Stats.Engagement.LightCount,
                              m_Stats.Engagement.EmissiveTriangles,
                              m_Stats.Engagement.EnvironmentAvailable ? "yes" : "no");
            }
        }
        else
        {
            // Nothing fell back this frame, so the next one that does is news
            // again. Without this, a user who fixes the cause and later
            // reintroduces it would get silence the second time.
            m_LastReportedFallback = ReSTIRGIFallbackReason::None;
        }

        return decision.IsReSTIR();
    }

    void ReSTIRGIPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        Ref<Framebuffer> radianceFramebuffer;
        Ref<Framebuffer> initialFramebuffer;
        Ref<Framebuffer> temporalFramebuffer;
        std::array<Ref<Framebuffer>, 2> spatialFramebuffers{};
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            radianceFramebuffer = context.ResolveFramebuffer(outputHandle);
        if (m_SelectedInitial.IsValid())
            initialFramebuffer = context.ResolveFramebuffer(m_SelectedInitial);
        if (m_SelectedTemporal.IsValid())
            temporalFramebuffer = context.ResolveFramebuffer(m_SelectedTemporal);
        for (sizet i = 0; i < spatialFramebuffers.size(); ++i)
        {
            if (m_SelectedSpatial[i].IsValid())
                spatialFramebuffers[i] = context.ResolveFramebuffer(m_SelectedSpatial[i]);
        }

        const auto resolveTexture = [&context](RGTextureHandle handle)
        { return handle.IsValid() ? context.ResolveTextureHandle(handle) : RHI::ResourceHandle{}; };
        const RHI::ResourceHandle sceneDepthID = resolveTexture(m_SelectedSceneDepth);
        const RHI::ResourceHandle albedoID = resolveTexture(m_SelectedGBufferAlbedo);
        const RHI::ResourceHandle normalID = resolveTexture(m_SelectedGBufferNormal);
        const RHI::ResourceHandle emissiveID = resolveTexture(m_SelectedGBufferEmissive);
        const RHI::ResourceHandle velocityID = resolveTexture(m_SelectedVelocity);
        const RHI::ResourceHandle prefilterID = resolveTexture(m_SelectedPrefilterTexture);
        const RHI::ResourceHandle sampleHistoryID = resolveTexture(m_SelectedReservoirSampleHistory);
        const RHI::ResourceHandle radianceHistoryID = resolveTexture(m_SelectedReservoirRadianceHistory);
        const RHI::ResourceHandle stateHistoryID = resolveTexture(m_SelectedReservoirStateHistory);
        const RHI::ResourceHandle surfaceHistoryID = resolveTexture(m_SelectedSurfaceHistory);
        const RHI::ResourceHandle momentsHistoryID = resolveTexture(m_SelectedMomentsHistory);

        const bool graphResourcesResolved = radianceFramebuffer && initialFramebuffer && temporalFramebuffer &&
                                            spatialFramebuffers[0] && spatialFramebuffers[1] &&
                                            sceneDepthID.IsValid() && albedoID.IsValid() && normalID.IsValid() &&
                                            emissiveID.IsValid();

        // Decide FIRST, and unconditionally. Every exit below has already counted
        // its reason.
        const bool active = ResolveTechniqueForFrame(graphResourcesResolved);
        if (!active)
        {
            // EVERY target this node owns keeps whatever the transient pool left in
            // it, and nothing downstream may read that. Clearing to zero is what
            // makes "the tier stood down" a value a consumer can see: for the
            // radiance target that is alpha 0 = "no value", so the deferred
            // lighting shader takes its ambient-ladder branch instead of dropping
            // a leftover frame's indirect into the image.
            //
            // AND THE RESERVOIR PLANES, not just the radiance one. Setup() declares
            // ExtractHistoryTexture on the reservoir targets unconditionally — an
            // extraction CONTRACT, established before this verdict exists — so a
            // stand-down that clears only the radiance target publishes
            // transient-pool contents as next frame's reservoir history. Zero is
            // the EMPTY reservoir (kind 0 = None), which every consumer already
            // handles; garbage is a reservoir that claims a vertex nothing traced.
            //
            // AND THE PREVIOUS-FRAME MATRICES ARE INVALIDATED. The temporal draw
            // reconstructs last frame's shading point from them, and a stood-down
            // frame uploaded none — so carrying them forward would have the next
            // live frame reconstruct a point from a camera two frames old, which
            // is a wrong Jacobian rather than a missing one.
            m_HavePrevFrame = false;

            // Ref<T> propagates const through its dereference, and Bind/Unbind
            // mutate the framebuffer, so this takes a non-const reference.
            const auto clearToZero = [&context](Ref<Framebuffer>& targetRef)
            {
                if (!targetRef)
                    return;
                Framebuffer& target = *targetRef;
                target.Bind();
                const auto& spec = target.GetSpecification();
                context.SetViewport(0, 0, spec.Width, spec.Height);
                const u32 attachmentCount = static_cast<u32>(spec.Attachments.Attachments.size());
                std::array<u32, 5> attachmentIndices{ 0u, 1u, 2u, 3u, 4u };
                RenderCommand::SetDrawBuffers(
                    std::span<const u32>(attachmentIndices.data(), std::min(attachmentCount, 5u)));
                RenderCommand::SetColorMask(true, true, true, true);
                // A scissor box left enabled by an earlier pass would confine this
                // clear to its rectangle and leave stale values everywhere outside
                // it — a partially cleared stand-down, which is worse than an
                // uncleared one because it looks deliberate.
                RenderCommand::DisableScissorTest();
                context.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
                context.Clear();
                target.Unbind();
            };
            clearToZero(radianceFramebuffer);
            clearToZero(initialFramebuffer);
            clearToZero(temporalFramebuffer);
            clearToZero(spatialFramebuffers[0]);
            clearToZero(spatialFramebuffers[1]);
            return;
        }

        m_Target = radianceFramebuffer;

        // ------------------------------------------------------------------
        // The one UBO, shared verbatim by all four draws.
        // ------------------------------------------------------------------
        const auto& outSpec = radianceFramebuffer->GetSpecification();
        const auto width = static_cast<f32>(std::max(outSpec.Width, 1u));
        const auto height = static_cast<f32>(std::max(outSpec.Height, 1u));

        UBOStructures::ReSTIRGIUBO params{};
        // RENDER-RELATIVE, not world (issue #429) — see the header. The same helper
        // the ray-traced shadow tier and ReSTIR DI use, so the three cannot
        // disagree about the space their rays live in.
        const glm::mat4 relativeView = MakeViewRelative(m_View, m_RenderOrigin);
        params.InvView = glm::inverse(relativeView);
        // THE SHADER-RECONSTRUCTION SEAM, not a plain inverse. These shaders do the
        // `ndc = vec3(uv*2-1, depth*2-1)` reconstruction, the family
        // RHIProjectionSeam.h says must carry the Vulkan row flip — sampled uv v=0
        // is the TOP row there. A plain glm::inverse renders correctly on GL and
        // reconstructs every shading point vertically mirrored on Vulkan, which
        // puts a hard diagonal band of wrong lighting across the frame.
        params.InvProjection = RHI::AdjustedInverseForShaderReconstruction(m_Projection);
        params.View = relativeView;

        // LAST FRAME'S PAIR, for the temporal draw's reconnection Jacobian. On the
        // first live frame there is no previous camera, so this frame's is used and
        // the reconstruction degenerates to "the same point" — which makes the
        // Jacobian 1, which is exactly what it should be when nothing moved. The
        // alternative (an identity matrix) would put every previous shading point
        // at the origin and reject every reuse, silently, on the one frame nobody
        // inspects.
        params.PrevInvView = m_HavePrevFrame ? glm::inverse(m_PrevRelativeView) : params.InvView;
        params.PrevInvProjection = m_HavePrevFrame
                                       ? RHI::AdjustedInverseForShaderReconstruction(m_PrevProjection)
                                       : params.InvProjection;
        params.PrevOriginDelta =
            glm::vec4(m_HavePrevFrame ? (m_PrevRenderOrigin - m_RenderOrigin) : glm::vec3(0.0f), 0.0f);

        const u64 tlasAddress = m_RayTracingScene != nullptr ? m_RayTracingScene->GetTlasDeviceAddress() : 0u;
        params.TlasAddressAndFrame = glm::uvec4(static_cast<u32>(tlasAddress & 0xFFFFFFFFull),
                                                static_cast<u32>(tlasAddress >> 32u),
                                                // The bounce ray and the NEE shadow ray at its vertex both
                                                // ride this lane. The SHADOW-CASTER mask, not a visibility
                                                // one: an instance that opted out of casting (issue #1144)
                                                // must not cast an indirect shadow here either, or the two
                                                // tiers disagree about what blocks light.
                                                RayTracing::kInstanceMaskShadowCaster, m_FrameIndex);

        const u32 lightSlots = std::min(m_GPUScene->GetLightSlotCount(), kReSTIRGIMaxLightSlots);
        params.SlotCounts = glm::uvec4(m_GPUScene->GetInstanceSlotCount(), m_GPUScene->GetGeometrySlotCount(),
                                       m_GPUScene->GetMaterialSlotCount(), lightSlots);

        const u64 emissiveAddress = m_EmissiveTable != nullptr ? m_EmissiveTable->GetDeviceAddress() : 0u;
        const u32 emissiveCount = (emissiveAddress != 0u) ? m_EmissiveTable->GetTriangleCount() : 0u;

        const bool texturesAvailable = m_MaterialTextures != nullptr &&
                                       m_MaterialTextures->GetDeviceAddress() != 0u &&
                                       m_MaterialTextures->GetSamplerHeapOffset() != RHI::HeapOffset::Invalid;

        m_Stats.HistoryPlanesAvailable =
            (sampleHistoryID.IsValid() ? 1u : 0u) + (radianceHistoryID.IsValid() ? 1u : 0u) +
            (stateHistoryID.IsValid() ? 1u : 0u) + (surfaceHistoryID.IsValid() ? 1u : 0u) +
            (momentsHistoryID.IsValid() ? 1u : 0u);
        // THE VELOCITY PLANE IS IN THIS CONJUNCTION, and so is the PREVIOUS FRAME.
        // The temporal draw reprojects through the velocity plane and has no other
        // way back to last frame's pixel; and its reconnection Jacobian needs a
        // previous camera that actually existed. Either missing makes the stage run,
        // find nothing usable, and look like a scene with no coherence — which is
        // the failure that never gets reported. Requiring both makes it a
        // stand-down TemporalReuseRan counts.
        //
        // THE MOMENTS PLANE IS NOT IN IT, unlike DI's. Only the RESOLVE draw reads
        // moments, and it reads them under its own OLO_RESTIR_GI_FLAG_MOMENTS_VALID
        // (set below, on the plane alone) because the Variance view is a diagnostic
        // for the temporal-reuse-off configuration too. Requiring it here as well
        // would let a missing diagnostic plane stand the whole ESTIMATOR down -
        // every pixel restarting each frame so that a debug view could stay
        // consistent, reported as a truthful `temporalReuseRan=false` that names
        // the wrong cause.
        const bool historyUsable = m_Settings.TemporalReuse && m_HavePrevFrame && velocityID.IsValid() &&
                                   sampleHistoryID.IsValid() && radianceHistoryID.IsValid() &&
                                   stateHistoryID.IsValid() && surfaceHistoryID.IsValid();

        const bool environmentCube = prefilterID.IsValid() && m_EnvironmentCubeIntensity > 0.0f;
        // The DDGI TAIL, as it will actually run. The setting alone is not enough:
        // the probe ladder has to have something to hand the vertex, which is a
        // scene fact this pass is told rather than probes.
        const bool ddgiTail = m_Stats.Sources.DDGIAtSecondary;

        u32 flags = 0;
        if (historyUsable)
            flags |= ReSTIRGIFlags::HistoryValid;
        if (texturesAvailable)
            flags |= ReSTIRGIFlags::Textures;
        if (m_Settings.ReconnectionVisibility)
            flags |= ReSTIRGIFlags::ReconnectionVisibility;
        if (m_Settings.TemporalReuse)
            flags |= ReSTIRGIFlags::TemporalReuse;
        if (m_Settings.SpatialReuse)
            flags |= ReSTIRGIFlags::SpatialReuse;
        // The moments plane on its own terms: bound, therefore accumulating. NOT
        // gated on TemporalReuse, because the Variance view is a diagnostic for the
        // temporal-reuse-off configuration too — and unit 5 falls back to the
        // raw-candidate target when this is clear, so the flag is what keeps the
        // accumulator off that target's alpha, which carries the glossy-vertex
        // fraction and would read as a history length.
        if (momentsHistoryID.IsValid())
            flags |= ReSTIRGIFlags::MomentsValid;
        if (ddgiTail)
            flags |= ReSTIRGIFlags::DDGITail;
        if (m_Settings.SpatialReconnectionVisibility)
            flags |= ReSTIRGIFlags::SpatialReconnectionVisibility;
        if (environmentCube)
            flags |= ReSTIRGIFlags::Environment;
        params.EmissiveTable = glm::uvec4(static_cast<u32>(emissiveAddress & 0xFFFFFFFFull),
                                          static_cast<u32>(emissiveAddress >> 32u), emissiveCount, flags);

        const u64 materialAddress = texturesAvailable ? m_MaterialTextures->GetDeviceAddress() : 0u;
        params.MaterialTable = glm::uvec4(
            static_cast<u32>(materialAddress & 0xFFFFFFFFull), static_cast<u32>(materialAddress >> 32u),
            materialAddress != 0u ? m_MaterialTextures->GetRecordCount() : 0u,
            materialAddress != 0u ? static_cast<u32>(m_MaterialTextures->GetSamplerHeapOffset())
                                  : static_cast<u32>(RHI::HeapOffset::Invalid));

        params.ReuseParams = glm::vec4(m_Settings.TemporalMCap, m_Settings.SpatialRadiusPixels,
                                       // The ray epsilon is the same 1e-3 the path tracer uses; sharing it
                                       // keeps a vertex the oracle calls reachable reachable here too.
                                       1.0e-3f, m_Settings.RayOriginNormalBias);
        // The emissive AREA pdf is READ FROM THE SAME TABLE the path tracer's NEE
        // reads it from, not recomputed. That is the "shared rather than re-derived"
        // requirement at the data level, and it is why a parity run between the two
        // is meaningful.
        const f32 pdfArea = (emissiveCount != 0u) ? m_EmissiveTable->GetPdfArea() : 0.0f;
        params.EstimatorParams =
            glm::vec4(pdfArea, m_Settings.MaxBounceDistance, m_Settings.MaxRadianceClamp,
                      static_cast<f32>(std::to_underlying(m_Settings.DebugView)));
        params.GIParams = glm::vec4(m_Settings.MinReconnectionDistance,
                                    static_cast<f32>(m_Settings.MaxSampleAge),
                                    ReSTIR::kDefaultGlossyVertexRoughness, 0.0f);
        params.Environment = glm::vec4(m_UniformEnvironmentRadiance, m_EnvironmentCubeIntensity);
        params.ScreenParams = glm::vec4(width, height, 1.0f / width, 1.0f / height);
        // Filled BEFORE the first upload, not only inside the spatial loop: draws A,
        // B and D read this lane too, and a zero here would mean one candidate per
        // pixel and the biased normalisation whatever the settings said. The loop
        // below overwrites only the pass index.
        params.ResamplingCounts = glm::uvec4(m_Settings.InitialCandidates, m_Settings.SpatialNeighbours, 0u,
                                             static_cast<u32>(std::to_underlying(m_Settings.BiasMode)));

        const u32 spatialPasses = m_Settings.SpatialReuse ? std::max(m_Settings.SpatialPasses, 1u) : 0u;
        m_Stats.InitialCandidatesPerPixel = m_Settings.InitialCandidates;
        m_Stats.SpatialNeighboursPerPixel = m_Settings.SpatialReuse ? m_Settings.SpatialNeighbours : 0u;
        m_Stats.SpatialPasses = spatialPasses;
        m_Stats.TemporalReuseRan = historyUsable;
        m_Stats.ReconnectionVisibilityRan = m_Settings.ReconnectionVisibility;
        m_Stats.SpatialReconnectionVisibilityRan =
            m_Settings.SpatialReuse && m_Settings.SpatialReconnectionVisibility;
        m_Stats.DDGITailRan = ddgiTail;

        // The issue's ray-count telemetry. An UPPER BOUND, derived rather than
        // measured — the honest form the shadow, reflection and path-tracing tiers
        // all use. Per pixel: one BOUNCE ray and one NEE SHADOW ray per initial
        // candidate, plus the resolve's one reconnection ray, plus one per spatial
        // neighbour per pass when the per-neighbour test is on. Sky pixels and
        // pixels whose reservoir is empty trace none, and the shader cannot report
        // how many did.
        //
        // THE PER-CANDIDATE FACTOR IS WHY THE DEFAULT CANDIDATE COUNT IS 1. DI's
        // bound is a flat two rays per pixel however many candidates it draws,
        // because its candidates cost no rays at all; this one is linear.
        const u64 pixels = static_cast<u64>(outSpec.Width) * static_cast<u64>(outSpec.Height);
        u64 raysPerPixel = 2ull * static_cast<u64>(m_Settings.InitialCandidates);
        if (m_Settings.ReconnectionVisibility)
            raysPerPixel += 1ull;
        if (m_Stats.SpatialReconnectionVisibilityRan)
            raysPerPixel += static_cast<u64>(m_Settings.SpatialNeighbours) * static_cast<u64>(spatialPasses);
        m_Stats.RaysDispatchedUpperBound = pixels * raysPerPixel;

        // Rebind binding 65 before writing: other passes may displace this indexed
        // binding, and the path tracer, the shadow tier and ReSTIR DI declare their
        // own blocks at the same number.
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, UBOStructures::ReSTIRGIUBO::GetSize());

        // The instance / geometry / material / LIGHT tables at their canonical SSBO
        // bindings. Bound HERE rather than relied on from an earlier pass: an
        // indexed buffer binding is global state any draw may displace.
        //
        // Omitting this does not fail loudly. Every light record reads back
        // inactive, so the bounce vertex's NEE draw returns zero irradiance for
        // every candidate; M still counts them, so the reservoir looks populated
        // while its weight sum stays zero, W is zero, and the resolve writes a
        // BLACK radiance target with alpha 1 — which the deferred shader then uses
        // INSTEAD of its ambient ladder, so the frame loses all indirect light.
        // That reads as "the tier does nothing" rather than "a buffer was not
        // bound".
        if (m_GPUScene != nullptr)
            m_GPUScene->Bind();

        const auto setFullscreenState = [&context](std::span<const u32> attachments)
        {
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            RenderCommand::SetDrawBuffers(attachments);
            // Clear to ZERO, which for a reservoir is the EMPTY reservoir (kind 0 =
            // None) and for the radiance target is alpha 0 = "no value". So a draw
            // that fails after the clear produces a frame lit by the ambient ladder
            // rather than one with no indirect light at all.
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

        // Every ReSTIR GI draw reads the same G-Buffer set, so the four bindings are
        // one helper rather than four copies that can drift apart.
        const auto bindGBuffer = [&context, sceneDepthID, albedoID, normalID, emissiveID]()
        {
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                            RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, albedoID,
                                            RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, normalID,
                                            RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_EMISSIVE, emissiveID,
                                            RHI::HeapSlotLifetime::FrameTransient);
        };

        auto& gpuTimers = GPUPassTimerPool::GetInstance();

        // ------------------------------------------------------------------
        // Draw A — cosine-hemisphere bounces, each vertex shaded with its
        // diffuse lobe plus the probe cache read AT THAT VERTEX.
        // ------------------------------------------------------------------
        gpuTimers.BeginSubPass("ReSTIRGIInitialSample");
        initialFramebuffer->Bind();
        {
            const auto& spec = initialFramebuffer->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
        }
        {
            constexpr std::array<u32, 5> attachments{ 0u, 1u, 2u, 3u, 4u };
            setFullscreenState(attachments);
        }
        m_InitialShader->Bind();
        bindGBuffer();
        // UNCONDITIONALLY, with a null handle when there is no environment —
        // exactly what the path tracer and the reflection tier do at this same
        // unit: TEX_USER_1 is a general-purpose slot other passes fill with a
        // sampler2D, and a samplerCube declaration reading a sampler2D binding is a
        // type mismatch for the whole draw. The guard is in the SHADER (the
        // environment flag) and the bind always happens.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_USER_1,
                                        prefilterID.IsValid() ? prefilterID : RHI::NullResource,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();
        initialFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // ------------------------------------------------------------------
        // Draw B — temporal reuse, gated on #976 and on the sample's AGE.
        // ------------------------------------------------------------------
        const RHI::ResourceHandle initial0 = initialFramebuffer->GetColorAttachmentHandle(0);
        const RHI::ResourceHandle initial1 = initialFramebuffer->GetColorAttachmentHandle(1);
        const RHI::ResourceHandle initial2 = initialFramebuffer->GetColorAttachmentHandle(2);
        const RHI::ResourceHandle initialSurface = initialFramebuffer->GetColorAttachmentHandle(3);
        const RHI::ResourceHandle initialRaw = initialFramebuffer->GetColorAttachmentHandle(4);

        gpuTimers.BeginSubPass("ReSTIRGITemporalReuse");
        temporalFramebuffer->Bind();
        {
            const auto& spec = temporalFramebuffer->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
        }
        {
            constexpr std::array<u32, 4> attachments{ 0u, 1u, 2u, 3u };
            setFullscreenState(attachments);
        }
        m_TemporalShader->Bind();
        context.BindTextureOrHeapOffset(0, initial0, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(1, initial1, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(2, initial2, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(3, initialSurface, RHI::HeapSlotLifetime::FrameTransient);
        // With no history the shader ignores units 4..7 (the HISTORY_VALID flag is
        // clear), but they must still be bound to something valid or the samplers
        // dangle — a dangling sampler is undefined behaviour, not a zero read.
        const auto bindHistoryOr = [&context](u32 unit, RHI::ResourceHandle history, RHI::ResourceHandle fallback)
        {
            context.BindTextureOrHeapOffset(unit, history.IsValid() ? history : fallback,
                                            history.IsValid() ? RHI::HeapSlotLifetime::Persistent
                                                              : RHI::HeapSlotLifetime::FrameTransient);
        };
        bindHistoryOr(4, sampleHistoryID, initial0);
        bindHistoryOr(5, radianceHistoryID, initial1);
        bindHistoryOr(6, stateHistoryID, initial2);
        bindHistoryOr(7, surfaceHistoryID, initialSurface);
        bindGBuffer();
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                        velocityID.IsValid() ? velocityID : initial0,
                                        RHI::HeapSlotLifetime::FrameTransient);
        drawFullscreen();
        temporalFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // ------------------------------------------------------------------
        // Draw C — spatial reuse, ping-ponged across SpatialPasses.
        // ------------------------------------------------------------------
        Ref<Framebuffer> finalReservoir = temporalFramebuffer;
        for (u32 pass = 0; pass < spatialPasses; ++pass)
        {
            // By value, not by const reference: Framebuffer::Bind / Unbind are
            // non-const, and a const Ref<T>& yields a const T.
            Ref<Framebuffer> target = spatialFramebuffers[pass % 2u];
            Ref<Framebuffer> source = (pass == 0u) ? temporalFramebuffer : spatialFramebuffers[(pass + 1u) % 2u];

            // The pass index goes into the UBO so each pass draws a DIFFERENT
            // neighbour set. Without it, a second pass at the same radius would
            // resample the same neighbours and only re-normalise — more cost, no
            // more independent samples.
            params.ResamplingCounts =
                glm::uvec4(m_Settings.InitialCandidates, m_Settings.SpatialNeighbours, pass,
                           static_cast<u32>(std::to_underlying(m_Settings.BiasMode)));
            m_ParamsUBO->Bind();
            m_ParamsUBO->SetData(&params, UBOStructures::ReSTIRGIUBO::GetSize());

            gpuTimers.BeginSubPass("ReSTIRGISpatialReuse");
            target->Bind();
            {
                const auto& spec = target->GetSpecification();
                context.SetViewport(0, 0, spec.Width, spec.Height);
            }
            {
                constexpr std::array<u32, 3> attachments{ 0u, 1u, 2u };
                setFullscreenState(attachments);
            }
            m_SpatialShader->Bind();
            context.BindTextureOrHeapOffset(0, source->GetColorAttachmentHandle(0),
                                            RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(1, source->GetColorAttachmentHandle(1),
                                            RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(2, source->GetColorAttachmentHandle(2),
                                            RHI::HeapSlotLifetime::FrameTransient);
            bindGBuffer();
            drawFullscreen();
            target->Unbind();
            gpuTimers.EndSubPass();

            finalReservoir = target;
        }

        // ------------------------------------------------------------------
        // Draw D — resolve: one reconnection ray, the AOVs, the moments.
        // ------------------------------------------------------------------
        gpuTimers.BeginSubPass("ReSTIRGIResolve");
        radianceFramebuffer->Bind();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        {
            constexpr std::array<u32, 2> attachments{ 0u, 1u };
            setFullscreenState(attachments);
        }
        m_ResolveShader->Bind();
        context.BindTextureOrHeapOffset(0, finalReservoir->GetColorAttachmentHandle(0),
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(1, finalReservoir->GetColorAttachmentHandle(1),
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(2, finalReservoir->GetColorAttachmentHandle(2),
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(3, initialRaw, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(4, temporalFramebuffer->GetColorAttachmentHandle(3),
                                        RHI::HeapSlotLifetime::FrameTransient);
        bindHistoryOr(5, momentsHistoryID, initialRaw);
        bindGBuffer();
        drawFullscreen();

        RenderCommand::SetDepthMask(true);
        radianceFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // Remember what this frame uploaded, for next frame's temporal Jacobian.
        // LAST, so a frame that returned early above does not record a camera it
        // never traced with.
        m_PrevRelativeView = relativeView;
        m_PrevProjection = m_Projection;
        m_PrevRenderOrigin = m_RenderOrigin;
        m_HavePrevFrame = true;
    }

    void ReSTIRGIPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ReSTIRGIPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ReSTIRGIPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepth = {};
        m_SelectedGBufferAlbedo = {};
        m_SelectedGBufferNormal = {};
        m_SelectedGBufferEmissive = {};
        m_SelectedVelocity = {};
        m_SelectedPrefilterTexture = {};
        m_SelectedInitial = {};
        m_SelectedTemporal = {};
        m_SelectedSpatial = {};
        m_SelectedReservoirSampleHistory = {};
        m_SelectedReservoirRadianceHistory = {};
        m_SelectedReservoirStateHistory = {};
        m_SelectedSurfaceHistory = {};
        m_SelectedMomentsHistory = {};
        // A reset means the history the previous matrices describe is gone too.
        m_HavePrevFrame = false;
    }
} // namespace OloEngine
