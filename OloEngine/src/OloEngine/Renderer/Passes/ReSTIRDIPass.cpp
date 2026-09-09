#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ReSTIRDIPass.h"

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
        // The shader-side loop bound on GPU Scene light slots
        // (OLO_LIGHT_MAX_SLOTS in include/LightSampling.glsl). Live lights past
        // it light the clustered frame and the path tracer but not this tier, so
        // the clamp is COUNTED rather than merely applied.
        constexpr u32 kReSTIRDIMaxLightSlots = 256u;
    } // namespace

    ReSTIRDIPass::ReSTIRDIPass()
    {
        SetName("ReSTIRDIPass");
        OLO_CORE_INFO("Creating ReSTIRDIPass.");
    }

    void ReSTIRDIPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepth = {};
        m_SelectedGBufferAlbedo = {};
        m_SelectedGBufferNormal = {};
        m_SelectedGBufferEmissive = {};
        m_SelectedVelocity = {};
        m_SelectedInitial = {};
        m_SelectedTemporal = {};
        m_SelectedSpatial = {};
        m_SelectedReservoirSampleHistory = {};
        m_SelectedReservoirRadianceHistory = {};
        m_SelectedReservoirStateHistory = {};
        m_SelectedSurfaceHistory = {};
        m_SelectedMomentsHistory = {};

        if (!m_Enabled || !blackboard.Lighting.ReSTIRDIRadiance.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() || !blackboard.GBuffer.GBufferAlbedo.IsValid() ||
            !blackboard.GBuffer.GBufferNormal.IsValid() || !blackboard.GBuffer.GBufferEmissive.IsValid())
        {
            return;
        }

        // The by-name execution dependency RayTracingScenePass::Setup reserved
        // for ray-query consumers. The acceleration structure is not a graph
        // resource — there is no handle to Read — so this edge is the only thing
        // that stops a reordering from putting the AS build after the pass that
        // traces against it. The symptom would be a frame with no ReSTIR
        // lighting and nothing in the log.
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

        // The four reservoir framebuffers, each written then sampled inside this
        // one Execute — the same intra-pass idiom SSGISignal and
        // RayTracedShadowSignal use.
        const auto declareReservoirTarget = [&builder](RGFramebufferHandle handle)
        {
            if (!handle.IsValid())
                return;
            builder.AllowSamePassReadWrite(handle);
            builder.Write(handle, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderSample);
        };
        m_SelectedInitial = blackboard.Scratch.ReSTIRDIInitial;
        m_SelectedTemporal = blackboard.Scratch.ReSTIRDITemporal;
        m_SelectedSpatial[0] = blackboard.Scratch.ReSTIRDISpatial0;
        m_SelectedSpatial[1] = blackboard.Scratch.ReSTIRDISpatial1;
        declareReservoirTarget(m_SelectedInitial);
        declareReservoirTarget(m_SelectedTemporal);
        declareReservoirTarget(m_SelectedSpatial[0]);
        declareReservoirTarget(m_SelectedSpatial[1]);

        // The #976 surface record this frame's reservoirs were selected against.
        // Declared from Setup, not from PopulateBlackboard: BuildFrameGraph
        // clears and rebuilds extraction contracts before visiting nodes, so an
        // earlier declaration is discarded on every cache miss.
        //
        // The extraction sources are attachments of the INITIAL target for the
        // surface plane and of the LAST SPATIAL target for the reservoir planes,
        // because the reservoir next frame's temporal draw should merge is the
        // one that survived every reuse — extracting the initial reservoir
        // instead would throw the spatial work away once per frame, which is a
        // noisier image and no error anywhere.
        if (m_SelectedInitial.IsValid())
        {
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRDISurfaceHistory, m_SelectedInitial, 3u);
        }
        const u32 spatialPasses = std::max(m_Settings.SpatialPasses, 1u);
        const RGFramebufferHandle finalReservoir =
            m_Settings.SpatialReuse ? m_SelectedSpatial[(spatialPasses - 1u) % 2u] : m_SelectedTemporal;
        if (finalReservoir.IsValid())
        {
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRDIReservoirSampleHistory,
                                                        finalReservoir, 0u);
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRDIReservoirRadianceHistory,
                                                        finalReservoir, 1u);
            builder.ExtractHistoryTexture(ResourceNames::ReSTIRDIReservoirStateHistory,
                                                        finalReservoir, 2u);
        }
        // THE NODE'S OWN OUTPUT. Declaring the reads and the scratch writes is not
        // enough: without a WriteNewVersion on the radiance target this node has
        // no primary output, GetPrimaryOutputFramebufferHandle() comes back
        // invalid, and Execute reports TargetUnavailable — while the graph has in
        // fact declared every resource, which is a diagnosis that sends you
        // looking in exactly the wrong place. Same shape as
        // RayTracedShadowPass's mask.
        constexpr std::string_view versionTag = "ReSTIRDIPass";
        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Lighting.ReSTIRDIRadiance, RGWriteUsage::RenderTarget, versionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::ReSTIRDIRadianceTexture) + "@" + std::string(versionTag), outputHandle, 0u));
        blackboard.Lighting.ReSTIRDIRadianceTexture = GetPrimaryOutputTextureHandle();

        // The moments ride on attachment 1 of the version this node just wrote,
        // not on the pre-write handle: extracting from the latter would publish
        // whatever the transient pool held before the resolve ran.
        builder.ExtractHistoryTexture(ResourceNames::ReSTIRDIMomentsHistory, outputHandle, 1u);

        const auto readHistory = [&builder](RGTextureHandle handle, RGTextureHandle& out)
        {
            if (!handle.IsValid())
                return;
            out = handle;
            [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderSample);
        };
        readHistory(blackboard.Temporal.ReSTIRDIReservoirSampleHistory, m_SelectedReservoirSampleHistory);
        readHistory(blackboard.Temporal.ReSTIRDIReservoirRadianceHistory, m_SelectedReservoirRadianceHistory);
        readHistory(blackboard.Temporal.ReSTIRDIReservoirStateHistory, m_SelectedReservoirStateHistory);
        readHistory(blackboard.Temporal.ReSTIRDISurfaceHistory, m_SelectedSurfaceHistory);
        readHistory(blackboard.Temporal.ReSTIRDIMomentsHistory, m_SelectedMomentsHistory);
    }

    void ReSTIRDIPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;

        // Created ONLY where GL_EXT_ray_query exists. Loading them anywhere else
        // is not a graceful degradation, it is four compile errors in the log on
        // every OpenGL run — and the tier this falls back TO does not need them.
        // The null shaders are what IsReadyForExecution reports, and
        // ResolveTechniqueForFrame turns that into a counted
        // RayTracingUnavailable rather than a warning nobody reads.
        if (!RenderCommand::SupportsRayTracing())
        {
            OLO_CORE_INFO("ReSTIRDIPass: hardware ray tracing unavailable — the pass stays inert and direct "
                          "lighting comes from the clustered tier.");
            return;
        }

        m_InitialShader = Shader::Create("assets/shaders/ReSTIR_DI_InitialSample.glsl");
        m_TemporalShader = Shader::Create("assets/shaders/ReSTIR_DI_TemporalReuse.glsl");
        m_SpatialShader = Shader::Create("assets/shaders/ReSTIR_DI_SpatialReuse.glsl");
        m_ResolveShader = Shader::Create("assets/shaders/ReSTIR_DI_Resolve.glsl");

        OLO_CORE_INFO("ReSTIRDIPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    bool ReSTIRDIPass::ResolveTechniqueForFrame(bool graphResourcesResolved)
    {
        m_Stats.Reset();
        m_Stats.BiasMode = m_Settings.BiasMode;
        m_Stats.ReservoirLayoutVersion = ReSTIR::kReservoirLayoutVersion;
        m_Stats.SettingsClamped = m_SettingsClamped;

        const bool rayTracingAvailable = m_RayTracingScene != nullptr && m_RayTracingScene->IsAvailable();
        // A TLAS device address of zero means no TLAS has ever been built —
        // a DIFFERENT state from "no RT device", and conflating them is how "the
        // first frame is clustered" gets misread as "this GPU cannot ray trace".
        const bool tlasReady = rayTracingAvailable && m_RayTracingScene->GetTlasDeviceAddress() != 0u;
        const bool gpuSceneAvailable = m_GPUScene != nullptr && m_GPUScene->GetInstanceSlotCount() != 0u;

        // The emitter set, counted the way the shader's source pdf weights it:
        // one candidate per live punctual / sphere-area light slot the shader's
        // loop can reach, plus one per emissive triangle. The criterion that
        // decides whether this tier runs and the pdf that runs inside it must
        // count the same thing, or the tier can engage on a scene whose emitters
        // it cannot actually sample.
        if (m_GPUScene != nullptr)
        {
            // The same loop shape GpuPathTracerPass uses, and for the same
            // reason: GetLightSlotCount() is a SLOT count, and a retired slot
            // returns null — so a subtraction of the two counts would report
            // retired slots as lights the shader cannot reach.
            const u32 lightSlotCount = m_GPUScene->GetLightSlotCount();
            for (u32 slot = 0; slot < lightSlotCount; ++slot)
            {
                const GPUSceneLight* light = m_GPUScene->GetLiveLightRecordBySlot(slot);
                if (light == nullptr)
                    continue;
                // Directional lights are NOT this tier's — the clustered loop
                // keeps them so they keep their cascades, their mask channel and
                // their cloud shadow (OloReSTIROwnsLight in
                // include/ReSTIRDICommon.glsl is the shader-side twin). Counting
                // them as candidates here would let the engagement criterion
                // engage the tier for lights it will not touch.
                if (light->Type == std::to_underlying(GPUSceneLightType::Directional))
                    continue;
                if (slot >= kReSTIRDIMaxLightSlots)
                    ++m_Stats.LightsBeyondShaderBound;
                else if (light->Type == std::to_underlying(GPUSceneLightType::SphereArea))
                    ++m_Stats.SphereAreaLights;
                else
                    ++m_Stats.PunctualLights;
            }
        }
        const u32 emissiveTriangles =
            (m_EmissiveTable != nullptr && m_EmissiveTable->GetDeviceAddress() != 0u)
                ? m_EmissiveTable->GetTriangleCount()
                : 0u;
        m_Stats.EmissiveTriangles = emissiveTriangles;

        m_Stats.Engagement.CandidateLightCount =
            m_Stats.PunctualLights + m_Stats.SphereAreaLights + emissiveTriangles;
        // The other half of the criterion: this frame's per-pixel candidate
        // budget, taken from the SANITIZED settings rather than the raw ones, so
        // a clamped value is what the criterion sees too.
        m_Stats.Engagement.CandidateBudget = m_Settings.InitialCandidates;

        const ReSTIRDITechniqueInputs inputs{
            .Requested = m_Settings.Enabled,
            // The reservoir passes are G-Buffer consumers, and this pass is only
            // registered on the deferred path — so reaching Execute at all IS
            // that fact. The field is still passed rather than hard-coded so the
            // policy function has one caller-independent shape.
            .DeferredPathActive = true,
            .ShadersReady = IsReadyForExecution(),
            .RayTracingAvailable = rayTracingAvailable,
            .TlasReady = tlasReady,
            .GPUSceneAvailable = gpuSceneAvailable,
            .TargetsAvailable = graphResourcesResolved,
            // The registry hands back a history only when the LayoutVersion on
            // its descriptor matches, so an invalid history handle after the
            // first frame is what a version mismatch looks like from here. It is
            // reported as its own reason rather than as "no history" because the
            // two need different fixes.
            .HistoryLayoutMatches = true,
            .Engagement = m_Stats.Engagement,
            .EngagementMargin = m_Settings.EngagementMargin,
        };
        const auto decision = SelectReSTIRDITechnique(inputs);
        m_Stats.Record(decision);

        if (!decision.IsReSTIR() && decision.Reason != ReSTIRDIFallbackReason::NotRequested)
        {
            if (decision.Reason != m_LastReportedFallback)
            {
                m_LastReportedFallback = decision.Reason;
                OLO_CORE_WARN("ReSTIRDIPass: direct lighting fell back to the clustered tier — {} "
                              "(candidate emitters={}, per-pixel candidate budget={}, margin={})",
                              ToString(decision.Reason), m_Stats.Engagement.CandidateLightCount,
                              m_Stats.Engagement.CandidateBudget, m_Settings.EngagementMargin);
            }
        }
        else
        {
            // Nothing fell back this frame, so the next one that does is news
            // again. Without this, a user who fixes the cause and later
            // reintroduces it would get silence the second time.
            m_LastReportedFallback = ReSTIRDIFallbackReason::None;
        }

        return decision.IsReSTIR();
    }

    void ReSTIRDIPass::Execute(RGCommandContext& context)
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
        const RHI::ResourceHandle sampleHistoryID = resolveTexture(m_SelectedReservoirSampleHistory);
        const RHI::ResourceHandle radianceHistoryID = resolveTexture(m_SelectedReservoirRadianceHistory);
        const RHI::ResourceHandle stateHistoryID = resolveTexture(m_SelectedReservoirStateHistory);
        const RHI::ResourceHandle surfaceHistoryID = resolveTexture(m_SelectedSurfaceHistory);
        const RHI::ResourceHandle momentsHistoryID = resolveTexture(m_SelectedMomentsHistory);

        const bool graphResourcesResolved = radianceFramebuffer && initialFramebuffer && temporalFramebuffer &&
                                            spatialFramebuffers[0] && spatialFramebuffers[1] &&
                                            sceneDepthID.IsValid() && albedoID.IsValid() && normalID.IsValid() &&
                                            emissiveID.IsValid();

        // Decide FIRST, and unconditionally. Every exit below has already
        // counted its reason.
        const bool active = ResolveTechniqueForFrame(graphResourcesResolved);
        if (!active)
        {
            // The radiance target keeps whatever the transient pool left in it,
            // and the deferred lighting shader must NOT read that. Clearing it
            // to alpha 0 is what makes "the tier stood down" a value the
            // consumer can see rather than a leftover frame it would happily
            // multiply into the image — the failure this whole seam exists to
            // prevent.
            if (radianceFramebuffer)
            {
                radianceFramebuffer->Bind();
                const auto& spec = radianceFramebuffer->GetSpecification();
                context.SetViewport(0, 0, spec.Width, spec.Height);
                constexpr std::array<u32, 2> attachments{ 0u, 1u };
                RenderCommand::SetDrawBuffers(attachments);
                RenderCommand::SetColorMask(true, true, true, true);
                context.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
                context.Clear();
                radianceFramebuffer->Unbind();
            }
            return;
        }

        m_Target = radianceFramebuffer;

        // ------------------------------------------------------------------
        // The one UBO, shared verbatim by all four draws.
        // ------------------------------------------------------------------
        const auto& outSpec = radianceFramebuffer->GetSpecification();
        const auto width = static_cast<f32>(std::max(outSpec.Width, 1u));
        const auto height = static_cast<f32>(std::max(outSpec.Height, 1u));

        UBOStructures::ReSTIRDIUBO params{};
        // RENDER-RELATIVE, not world (issue #429) — see the header. The same
        // helper the ray-traced shadow tier uses, so the two cannot disagree
        // about the space their rays live in.
        const glm::mat4 relativeView = MakeViewRelative(m_View, m_RenderOrigin);
        params.InvView = glm::inverse(relativeView);
        // THE SHADER-RECONSTRUCTION SEAM, not a plain inverse. These shaders do
        // the `ndc = vec3(uv*2-1, depth*2-1)` reconstruction, the family
        // RHIProjectionSeam.h says must carry the Vulkan row flip — sampled uv
        // v=0 is the TOP row there. A plain glm::inverse renders correctly on GL
        // and reconstructs every shading point vertically mirrored on Vulkan,
        // which puts a hard diagonal band of wrong lighting across the frame.
        params.InvProjection = RHI::AdjustedInverseForShaderReconstruction(m_Projection);
        params.View = relativeView;
        params.PrevViewProjection = m_PreviousViewProjection;

        const u64 tlasAddress = m_RayTracingScene != nullptr ? m_RayTracingScene->GetTlasDeviceAddress() : 0u;
        params.TlasAddressAndFrame = glm::uvec4(static_cast<u32>(tlasAddress & 0xFFFFFFFFull),
                                                static_cast<u32>(tlasAddress >> 32u),
                                                RayTracing::kInstanceMaskAll, m_FrameIndex);

        const u32 lightSlots = std::min(m_GPUScene->GetLightSlotCount(), kReSTIRDIMaxLightSlots);
        params.SlotCounts = glm::uvec4(m_GPUScene->GetInstanceSlotCount(), m_GPUScene->GetGeometrySlotCount(),
                                       m_GPUScene->GetMaterialSlotCount(), lightSlots);

        const u64 emissiveAddress = m_EmissiveTable != nullptr ? m_EmissiveTable->GetDeviceAddress() : 0u;
        const u32 emissiveCount = (emissiveAddress != 0u) ? m_EmissiveTable->GetTriangleCount() : 0u;

        const bool texturesAvailable = m_MaterialTextures != nullptr &&
                                       m_MaterialTextures->GetDeviceAddress() != 0u &&
                                       m_MaterialTextures->GetSamplerHeapOffset() != RHI::HeapOffset::Invalid;
        // The MOMENTS plane is in this conjunction too, and that is not
        // belt-and-braces. The resolve reads the HISTORY_VALID flag before
        // trusting unit 5, and when the moments history is absent that unit is
        // bound to draw A's raw-candidate target instead (a dangling sampler is
        // undefined behaviour, not a zero read). Without the moments here, a
        // frame with valid reservoir histories and no moments history would read
        // the raw candidate's blue channel as an accumulated history length —
        // which only corrupts the Variance debug view, but corrupts it in a way
        // that looks like real variance.
        //
        // All five are acquired under one gate, so in practice they are valid
        // together; saying so explicitly is what stops that from becoming an
        // assumption a later change can break silently.
        m_Stats.HistoryPlanesAvailable =
            (sampleHistoryID.IsValid() ? 1u : 0u) + (radianceHistoryID.IsValid() ? 1u : 0u) +
            (stateHistoryID.IsValid() ? 1u : 0u) + (surfaceHistoryID.IsValid() ? 1u : 0u) +
            (momentsHistoryID.IsValid() ? 1u : 0u);
        const bool historyUsable = m_Settings.TemporalReuse && sampleHistoryID.IsValid() &&
                                   radianceHistoryID.IsValid() && stateHistoryID.IsValid() &&
                                   surfaceHistoryID.IsValid() && momentsHistoryID.IsValid();

        u32 flags = 0;
        if (historyUsable)
            flags |= 1u; // OLO_RESTIR_FLAG_HISTORY_VALID
        if (texturesAvailable)
            flags |= 2u; // OLO_RESTIR_FLAG_TEXTURES
        if (m_Settings.VisibilityReuse)
            flags |= 4u; // OLO_RESTIR_FLAG_VISIBILITY_REUSE
        if (m_Settings.TemporalReuse)
            flags |= 8u; // OLO_RESTIR_FLAG_TEMPORAL_REUSE
        if (m_Settings.SpatialReuse)
            flags |= 16u; // OLO_RESTIR_FLAG_SPATIAL_REUSE
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
                                       // keeps a sample the oracle calls visible visible here too.
                                       1.0e-3f, m_Settings.RayOriginNormalBias);
        // The emissive AREA pdf is READ FROM THE SAME TABLE the path tracer's NEE
        // reads it from, not recomputed. That is the "shared with the PT's NEE
        // rather than re-derived" requirement at the data level, and it is why a
        // parity run between the two is meaningful.
        const f32 pdfArea = (emissiveCount != 0u) ? m_EmissiveTable->GetPdfArea() : 0.0f;
        params.EstimatorParams =
            glm::vec4(pdfArea, 1.0e5f, m_Settings.MaxRadianceClamp,
                      static_cast<f32>(std::to_underlying(m_Settings.DebugView)));
        params.ScreenParams = glm::vec4(width, height, 1.0f / width, 1.0f / height);
        // Filled BEFORE the first upload, not only inside the spatial loop:
        // draws A, B and D read this lane too, and a zero here would mean one
        // candidate per pixel and the biased normalisation whatever the settings
        // said. The loop below overwrites only the pass index.
        params.ResamplingCounts = glm::uvec4(m_Settings.InitialCandidates, m_Settings.SpatialNeighbours, 0u,
                                             static_cast<u32>(std::to_underlying(m_Settings.BiasMode)));

        const u32 spatialPasses = m_Settings.SpatialReuse ? std::max(m_Settings.SpatialPasses, 1u) : 0u;
        m_Stats.InitialCandidatesPerPixel = m_Settings.InitialCandidates;
        m_Stats.SpatialNeighboursPerPixel = m_Settings.SpatialReuse ? m_Settings.SpatialNeighbours : 0u;
        m_Stats.SpatialPasses = spatialPasses;
        m_Stats.TemporalReuseRan = historyUsable;
        m_Stats.VisibilityReuseRan = m_Settings.VisibilityReuse;

        // The issue's ray-count telemetry. An UPPER BOUND, derived rather than
        // measured — the honest form the shadow, reflection and path-tracing
        // tiers all use. Per pixel: the initial draw's one visibility ray plus
        // the resolve's one. Sky pixels and pixels whose reservoir is empty
        // trace none, and the shader cannot report how many did. The spatial
        // draw traces NO rays at all: the target function is deliberately
        // unshadowed, which is the whole reason reuse is cheap.
        const u64 pixels = static_cast<u64>(outSpec.Width) * static_cast<u64>(outSpec.Height);
        m_Stats.RaysDispatchedUpperBound = pixels * (m_Settings.VisibilityReuse ? 2ull : 1ull);

        // Rebind binding 65 before writing: other passes may displace this
        // indexed binding, and the path tracer and shadow tier declare their own
        // blocks at the same number.
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, UBOStructures::ReSTIRDIUBO::GetSize());

        // The instance / geometry / material / LIGHT tables at their canonical
        // SSBO bindings. Bound HERE rather than relied on from an earlier pass:
        // an indexed buffer binding is global state any draw may displace.
        //
        // Omitting this does not fail loudly. Every light record reads back
        // inactive, so OloReSTIRSampleCandidate rejects every draw; M still
        // counts the rejections, so the reservoir looks populated while its
        // weight sum stays zero, W is zero, and the resolve writes a BLACK
        // radiance target. The deferred shader then drops its own light loop in
        // favour of that black — so the frame goes dark and every reservoir
        // setting stops mattering, which reads as "the tier does nothing" rather
        // than "a buffer was not bound". Measured exactly that way before this
        // call existed.
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
            // Clear to ZERO, which for a reservoir is the EMPTY reservoir
            // (kind 0 = None) and for the radiance target is alpha 0 = "no
            // value". So a draw that fails after the clear produces a frame lit
            // by the clustered tier rather than a black one — the same
            // asymmetry the shadow tier's clear-to-lit encodes.
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

        // Every ReSTIR draw reads the same G-Buffer set, so the four bindings
        // are one helper rather than four copies that can drift apart.
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
        // Draw A — RIS over the emitter set, one visibility ray on the survivor.
        // ------------------------------------------------------------------
        gpuTimers.BeginSubPass("ReSTIRDIInitialSample");
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
        drawFullscreen();
        initialFramebuffer->Unbind();
        gpuTimers.EndSubPass();

        // ------------------------------------------------------------------
        // Draw B — temporal reuse, gated on the #976 validity layer.
        // ------------------------------------------------------------------
        const RHI::ResourceHandle initial0 = initialFramebuffer->GetColorAttachmentHandle(0);
        const RHI::ResourceHandle initial1 = initialFramebuffer->GetColorAttachmentHandle(1);
        const RHI::ResourceHandle initial2 = initialFramebuffer->GetColorAttachmentHandle(2);
        const RHI::ResourceHandle initialSurface = initialFramebuffer->GetColorAttachmentHandle(3);
        const RHI::ResourceHandle initialRaw = initialFramebuffer->GetColorAttachmentHandle(4);

        gpuTimers.BeginSubPass("ReSTIRDITemporalReuse");
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
        // With no history the shader ignores units 4..7 (the HISTORY_VALID flag
        // is clear), but they must still be bound to something valid or the
        // samplers dangle — a dangling sampler is undefined behaviour, not a
        // zero read.
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
            // resample the same neighbours and only re-normalise — more cost,
            // no more independent samples.
            params.ResamplingCounts =
                glm::uvec4(m_Settings.InitialCandidates, m_Settings.SpatialNeighbours, pass,
                           static_cast<u32>(std::to_underlying(m_Settings.BiasMode)));
            m_ParamsUBO->Bind();
            m_ParamsUBO->SetData(&params, UBOStructures::ReSTIRDIUBO::GetSize());

            gpuTimers.BeginSubPass("ReSTIRDISpatialReuse");
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
        // Draw D — resolve: one visibility ray, the AOVs, the moments.
        // ------------------------------------------------------------------
        gpuTimers.BeginSubPass("ReSTIRDIResolve");
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
    }

    void ReSTIRDIPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ReSTIRDIPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ReSTIRDIPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepth = {};
        m_SelectedGBufferAlbedo = {};
        m_SelectedGBufferNormal = {};
        m_SelectedGBufferEmissive = {};
        m_SelectedVelocity = {};
        m_SelectedInitial = {};
        m_SelectedTemporal = {};
        m_SelectedSpatial = {};
        m_SelectedReservoirSampleHistory = {};
        m_SelectedReservoirRadianceHistory = {};
        m_SelectedReservoirStateHistory = {};
        m_SelectedSurfaceHistory = {};
        m_SelectedMomentsHistory = {};
    }
} // namespace OloEngine
