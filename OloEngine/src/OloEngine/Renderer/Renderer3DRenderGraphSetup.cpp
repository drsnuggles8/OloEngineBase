#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3DFrameGraphBuilder.h"
#include "OloEngine/Renderer/ShaderConstants.h"

#include <algorithm>
#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void Renderer3D::CreatePrimaryFrameGraphPasses(const FramebufferSpecification& shadowPassSpec,
                                                   const FramebufferSpecification& scenePassSpec,
                                                   const FramebufferSpecification& finalPassSpec)
    {
        // Shadow pass (renders before scene, doesn't need scene framebuffer dimensions)
        s_Data.ShadowPass = Ref<ShadowRenderPass>::Create();
        s_Data.ShadowPass->SetName("ShadowPass");
        s_Data.ShadowPass->Init(shadowPassSpec);
        s_Data.ShadowPass->SetShadowMap(&s_Data.Shadow);

        s_Data.ScenePass = Ref<SceneRenderPass>::Create();
        s_Data.ScenePass->SetName("ScenePass");
        s_Data.ScenePass->Init(scenePassSpec);

        // Deferred lighting composition — no-op when Settings.Path is
        // Forward / Forward+ (no G-Buffer supplied). Writes into
        // ScenePass's colour[0] so downstream passes stay path-agnostic.
        s_Data.SceneCompositePasses.DeferredLighting = Ref<DeferredLightingPass>::Create();
        s_Data.SceneCompositePasses.DeferredLighting->SetName("DeferredLightingPass");
        s_Data.SceneCompositePasses.DeferredLighting->Init(scenePassSpec);

        // Graph-scheduled opaque-decal shim. Pulls the decal bucket into
        // the G-Buffer between ScenePass and DeferredLightingPass (was
        // previously a synchronous call inside SceneRenderPass::Execute,
        // now a proper graph node with declared resource edges).
        s_Data.SceneCompositePasses.DeferredOpaqueDecal = Ref<DeferredOpaqueDecalPass>::Create();
        s_Data.SceneCompositePasses.DeferredOpaqueDecal->SetName("DeferredOpaqueDecalPass");
        s_Data.SceneCompositePasses.DeferredOpaqueDecal->Init(scenePassSpec);

        // Forward overlay pass — runs after DeferredLightingPass in Deferred
        // mode to render skybox / terrain / voxel terrain / infinite grid /
        // light-cube geometry that cannot participate in the G-Buffer MRT
        // write. No-ops in Forward / Forward+.
        s_Data.ForwardOverlayPass = Ref<ForwardOverlayRenderPass>::Create();
        s_Data.ForwardOverlayPass->SetName("ForwardOverlayPass");
        s_Data.ForwardOverlayPass->Init(finalPassSpec);

        s_Data.SceneCompositePasses.Particle = Ref<ParticleRenderPass>::Create();
        s_Data.SceneCompositePasses.Particle->SetName("ParticlePass");
        s_Data.SceneCompositePasses.Particle->Init(finalPassSpec);

        s_Data.FoliagePass = Ref<FoliageRenderPass>::Create();
        s_Data.FoliagePass->SetName("FoliagePass");
        s_Data.FoliagePass->Init(finalPassSpec);

        s_Data.WaterPass = Ref<WaterRenderPass>::Create();
        s_Data.WaterPass->SetName("WaterPass");
        s_Data.WaterPass->Init(finalPassSpec);

        s_Data.DecalPass = Ref<DecalRenderPass>::Create();
        s_Data.DecalPass->SetName("DecalPass");
        s_Data.DecalPass->Init(finalPassSpec);

        s_Data.SceneCompositePasses.SSAO = Ref<SSAORenderPass>::Create();
        s_Data.SceneCompositePasses.SSAO->SetName("SSAOPass");
        s_Data.SceneCompositePasses.SSAO->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        s_Data.SceneCompositePasses.SSAO->SetSSAOUBO(s_Data.PostProcessGPU.SSAO, &s_Data.PostProcessGPU.SSAOData);

        s_Data.SceneCompositePasses.GTAO = Ref<GTAORenderPass>::Create();
        s_Data.SceneCompositePasses.GTAO->SetName("GTAOPass");
        s_Data.SceneCompositePasses.GTAO->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        s_Data.SceneCompositePasses.GTAO->SetGTAOUBO(s_Data.PostProcessGPU.GTAO, &s_Data.PostProcessGPU.GTAOData);

        s_Data.PostProcessPasses.SSS = Ref<SSSRenderPass>::Create();
        s_Data.PostProcessPasses.SSS->SetName("SSSPass");
        s_Data.PostProcessPasses.SSS->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        s_Data.PostProcessPasses.SSS->SetSSSUBO(s_Data.SceneEffectsGPU.SSS, &s_Data.SceneEffectsGPU.SSSData);

        // OIT resolve pass. Composites weighted-blended transparent
        // accumulation (produced by ParticlePass when OITEnabled) over the
        // scene FB, then acts as a passthrough for downstream piping.
        s_Data.SceneCompositePasses.OITResolve = Ref<OITResolveRenderPass>::Create();
        s_Data.SceneCompositePasses.OITResolve->SetName("OITResolvePass");
        s_Data.SceneCompositePasses.OITResolve->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
    }

    void Renderer3D::CreateFrameGraphRenderStreamNodes()
    {
        const auto graphicsBucketNodeFlags = RenderGraphNodeFlags::Graphics | RenderGraphNodeFlags::UsesCommandBucket;
        s_Data.StreamNodes.Geometry = Ref<PassGraphNode>::Create(
            "ScenePass",
            s_Data.ScenePass.As<CommandBufferRenderPass>(),
            [](RGBuilder& builder, FrameBlackboard& board)
            {
                if (board.ShadowMapCSM.IsValid())
                {
                    [[maybe_unused]] const auto shadowCSMRead = builder.Read(board.ShadowMapCSM, RGReadUsage::ShaderSample);
                }
                if (board.ShadowMapSpot.IsValid())
                {
                    [[maybe_unused]] const auto shadowSpotRead = builder.Read(board.ShadowMapSpot, RGReadUsage::ShaderSample);
                }

                for (const auto& pointHandle : board.ShadowMapPoint)
                {
                    if (pointHandle.IsValid())
                    {
                        [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
                    }
                }

                if (board.SceneDepth.IsValid())
                    builder.Write(board.SceneDepth, RGWriteUsage::DepthStencil);
                if (board.Velocity.IsValid())
                    builder.Write(board.Velocity, RGWriteUsage::RenderTarget);

                if (s_Data.Settings.Path == RenderingPath::Deferred)
                {
                    if (board.GBufferAlbedo.IsValid())
                        builder.Write(board.GBufferAlbedo, RGWriteUsage::RenderTarget);
                    if (board.GBufferNormal.IsValid())
                        builder.Write(board.GBufferNormal, RGWriteUsage::RenderTarget);
                    if (board.GBufferEmissive.IsValid())
                        builder.Write(board.GBufferEmissive, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            graphicsBucketNodeFlags);

        s_Data.StreamNodes.ForwardOverlay = Ref<PassGraphNode>::Create(
            "ForwardOverlayPass",
            s_Data.ForwardOverlayPass.As<CommandBufferRenderPass>(),
            [](RGBuilder& builder, FrameBlackboard& board)
            {
                if (s_Data.Settings.Path != RenderingPath::Deferred)
                    return;
                if (!s_Data.ForwardOverlayPass || s_Data.ForwardOverlayPass->GetCommandBucket().GetCommandCount() == 0)
                    return;

                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            },
            graphicsBucketNodeFlags);

        s_Data.StreamNodes.Foliage = Ref<PassGraphNode>::Create(
            "FoliagePass",
            s_Data.FoliagePass.As<CommandBufferRenderPass>(),
            [](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!s_Data.FoliagePass || s_Data.FoliagePass->GetCommandBucket().GetCommandCount() == 0)
                    return;

                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            },
            graphicsBucketNodeFlags);

        s_Data.StreamNodes.Water = Ref<PassGraphNode>::Create(
            "WaterPass",
            s_Data.WaterPass.As<CommandBufferRenderPass>(),
            [](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!s_Data.WaterPass || s_Data.WaterPass->GetCommandBucket().GetCommandCount() == 0)
                    return;

                const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                        s_Data.Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    if (board.OITRevealage.IsValid())
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }

                if (auto sceneTarget = s_Data.ScenePass ? s_Data.ScenePass->GetTarget() : nullptr; sceneTarget)
                {
                    const auto& sceneTargetSpec = sceneTarget->GetSpecification();
                    if (sceneTargetSpec.Width > 0 && sceneTargetSpec.Height > 0)
                    {
                        RGResourceDesc refrDesc;
                        refrDesc.Kind = ResourceHandle::Kind::Texture2D;
                        refrDesc.Format = RGResourceFormat::RGBA16Float;
                        refrDesc.Width = sceneTargetSpec.Width;
                        refrDesc.Height = sceneTargetSpec.Height;
                        const auto refrHandle = builder.CreateTexture("WaterRefraction", refrDesc);
                        builder.Write(refrHandle, RGWriteUsage::ShaderImage);
                        [[maybe_unused]] const auto refrRead = builder.Read(refrHandle, RGReadUsage::ShaderSample);
                    }
                }
            },
            graphicsBucketNodeFlags);

        s_Data.StreamNodes.Decal = Ref<PassGraphNode>::Create(
            "DecalPass",
            s_Data.DecalPass.As<CommandBufferRenderPass>(),
            [](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!s_Data.DecalPass || s_Data.DecalPass->GetCommandBucket().GetCommandCount() == 0)
                    return;

                const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                        s_Data.Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    if (board.OITRevealage.IsValid())
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            graphicsBucketNodeFlags);
    }

    void Renderer3D::ConfigureFrameGraphOITInfrastructure()
    {
        // Wire OIT buffer provider + accumulation marker through to
        // ParticlePass. The buffer is allocated lazily
        // in OITResolveRenderPass; installing a provider callback (rather
        // than caching a Ref) lets paths that never enable OIT skip the
        // ~2x screen-sized RGBA16F+RG16F GPU memory cost.
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.SceneCompositePasses.OITResolve;
            s_Data.SceneCompositePasses.Particle->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                      { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.SceneCompositePasses.Particle->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                          {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }

        // WaterPass WB-OIT hookup: shader override + provider + marker. The
        // OIT toggle itself is flipped each frame in ApplyRendererSettings;
        // attach the infrastructure here so enabling it has immediate effect.
        if (s_Data.WaterPass)
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.SceneCompositePasses.OITResolve;
            s_Data.WaterPass->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                   { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.WaterPass->SetOITShader(m_ShaderLibrary.Get("Water_OIT"));
            s_Data.WaterPass->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                       {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }

        // DecalPass WB-OIT hookup (forward path only — deferred uses
        // G-Buffer variants via ExecuteOnGBuffer which is already order-
        // independent).
        if (s_Data.DecalPass)
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.SceneCompositePasses.OITResolve;
            s_Data.DecalPass->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                   { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.DecalPass->SetOITShader(m_ShaderLibrary.Get("Decal_OIT"));
            s_Data.DecalPass->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                       {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }
    }

    void Renderer3D::CreatePostProcessFrameGraphPasses(const FramebufferSpecification& finalPassSpec)
    {
        // AO Apply extracted from legacy PostProcess pass.
        // Sits between SSSPass (or SceneColor) and BloomPass in dynamic mode.
        s_Data.PostProcessPasses.AOApply = Ref<AOApplyRenderPass>::Create();
        s_Data.PostProcessPasses.AOApply->SetName("AOApplyPass");
        s_Data.PostProcessPasses.AOApply->Init(finalPassSpec);

        // Bloom standalone pass.
        // Sits between PostProcess and DOF.
        s_Data.PostProcessPasses.Bloom = Ref<BloomRenderPass>::Create();
        s_Data.PostProcessPasses.Bloom->SetName("BloomPass");
        s_Data.PostProcessPasses.Bloom->Init(finalPassSpec);

        // DOF standalone pass.
        // Sits between Bloom and MotionBlur.
        s_Data.PostProcessPasses.DOF = Ref<DOFRenderPass>::Create();
        s_Data.PostProcessPasses.DOF->SetName("DOFPass");
        s_Data.PostProcessPasses.DOF->Init(finalPassSpec);

        // MotionBlur standalone pass.
        // Sits between DOF and TAA.
        s_Data.PostProcessPasses.MotionBlur = Ref<MotionBlurRenderPass>::Create();
        s_Data.PostProcessPasses.MotionBlur->SetName("MotionBlurPass");
        s_Data.PostProcessPasses.MotionBlur->Init(finalPassSpec);

        // TAA standalone pass.
        // Sits between PostProcess and Fog.
        s_Data.PostProcessPasses.TAA = Ref<TAARenderPass>::Create();
        s_Data.PostProcessPasses.TAA->SetName("TAAPass");
        s_Data.PostProcessPasses.TAA->Init(finalPassSpec);

        // Screen-space precipitation standalone pass.
        // Sits between TAA and Fog.
        s_Data.PostProcessPasses.Precipitation = Ref<PrecipitationRenderPass>::Create();
        s_Data.PostProcessPasses.Precipitation->SetName("PrecipitationPass");
        s_Data.PostProcessPasses.Precipitation->Init(finalPassSpec);

        // Volumetric fog standalone pass.
        // Sits between Precipitation and the late post-effect sub-chain.
        s_Data.PostProcessPasses.Fog = Ref<FogRenderPass>::Create();
        s_Data.PostProcessPasses.Fog->SetName("FogPass");
        s_Data.PostProcessPasses.Fog->Init(finalPassSpec);

        // Four standalone effects in
        // in chain order. Each pass self-skips when its effect is disabled;
        // the graph topology stays constant regardless of settings.
        s_Data.PostProcessPasses.ChromAberration = Ref<ChromaticAberrationRenderPass>::Create();
        s_Data.PostProcessPasses.ChromAberration->SetName("ChromAberrationPass");
        s_Data.PostProcessPasses.ChromAberration->Init(finalPassSpec);

        s_Data.PostProcessPasses.ColorGrading = Ref<ColorGradingRenderPass>::Create();
        s_Data.PostProcessPasses.ColorGrading->SetName("ColorGradingPass");
        s_Data.PostProcessPasses.ColorGrading->Init(finalPassSpec);

        s_Data.PostProcessPasses.ToneMap = Ref<ToneMapRenderPass>::Create();
        s_Data.PostProcessPasses.ToneMap->SetName("ToneMapPass");
        s_Data.PostProcessPasses.ToneMap->Init(finalPassSpec);

        s_Data.PostProcessPasses.Vignette = Ref<VignetteRenderPass>::Create();
        s_Data.PostProcessPasses.Vignette->SetName("VignettePass");
        s_Data.PostProcessPasses.Vignette->Init(finalPassSpec);

        // FXAA extracted into its own graph pass.
        // Always created so the graph topology can stay constant; the
        // pass self-skips when `Settings.FXAAEnabled` is false and the
        // blackboard import is gated on the same flag.
        s_Data.PostProcessPasses.FXAA = Ref<FXAARenderPass>::Create();
        s_Data.PostProcessPasses.FXAA->SetName("FXAAPass");
        s_Data.PostProcessPasses.FXAA->Init(finalPassSpec);

        if (s_Data.EnableSelectionOutline)
        {
            s_Data.PostProcessPasses.SelectionOutline = Ref<SelectionOutlineRenderPass>::Create();
            s_Data.PostProcessPasses.SelectionOutline->SetName("SelectionOutlinePass");
            s_Data.PostProcessPasses.SelectionOutline->Init(finalPassSpec);
        }

        s_Data.PostProcessPasses.UIComposite = Ref<UICompositeRenderPass>::Create();
        s_Data.PostProcessPasses.UIComposite->SetName("UICompositePass");
        s_Data.PostProcessPasses.UIComposite->Init(finalPassSpec);

        s_Data.PostProcessPasses.Final = Ref<FinalRenderPass>::Create();
        s_Data.PostProcessPasses.Final->SetName("FinalPass");
        s_Data.PostProcessPasses.Final->Init(finalPassSpec);
    }

    void Renderer3D::SetupRenderGraph(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Setting up Renderer3D RenderGraph with dimensions: {}x{}", width, height);

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("Invalid dimensions for RenderGraph: {}x{}", width, height);
            return;
        }

        s_Data.RGraph->Init(width, height);

        // Phase D: enable runtime materialization of compiled transient plan
        // entries. Unit tests keep this path disabled by default; production
        // renderer setup opts in explicitly.
        s_Data.RGraph->SetTransientMaterializationEnabled(true);

        FramebufferSpecification shadowPassSpec;
        shadowPassSpec.Width = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);
        shadowPassSpec.Height = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);

        FramebufferSpecification scenePassSpec;
        scenePassSpec.Width = width;
        scenePassSpec.Height = height;
        scenePassSpec.Samples = 1;
        scenePassSpec.Attachments = {
            FramebufferTextureFormat::RGBA16F,     // [0] HDR color output
            FramebufferTextureFormat::RED_INTEGER, // [1] Entity ID attachment
            FramebufferTextureFormat::RG16F,       // [2] View-space normals (octahedral encoded for SSAO)
            FramebufferTextureFormat::RG16F,       // [3] Screen-space velocity (forward-path TAA input; unused in Deferred, which reads G-Buffer RT3)
            FramebufferTextureFormat::Depth
        };

        FramebufferSpecification finalPassSpec;
        finalPassSpec.Width = width;
        finalPassSpec.Height = height;

        CreatePrimaryFrameGraphPasses(shadowPassSpec, scenePassSpec, finalPassSpec);
        CreateFrameGraphRenderStreamNodes();
        ConfigureFrameGraphOITInfrastructure();
        CreatePostProcessFrameGraphPasses(finalPassSpec);

        // All passes are now constructed. Build the initial graph topology
        // for the currently-configured rendering path. Runtime switches
        // between Forward / Forward+ / Deferred re-run ConfigureRenderGraph
        // from ApplyRendererSettings so the graph only ever contains the
        // passes that are relevant for the active path.
        ConfigureRenderGraph(s_Data.Settings.Path);
    }

    auto Renderer3D::BuildFrameGraphInputs() -> Renderer3DFrameGraphInputs
    {
        Renderer3DFrameGraphInputs inputs{};
        inputs.Graph = s_Data.RGraph.Raw();
        inputs.GeometryNode = s_Data.StreamNodes.Geometry.Raw();
        inputs.ForwardOverlayNode = s_Data.StreamNodes.ForwardOverlay.Raw();
        inputs.FoliageNode = s_Data.StreamNodes.Foliage.Raw();
        inputs.DecalNode = s_Data.StreamNodes.Decal.Raw();
        inputs.WaterNode = s_Data.StreamNodes.Water.Raw();
        inputs.ScenePass = s_Data.ScenePass.Raw();
        inputs.ShadowPass = s_Data.ShadowPass.Raw();
        inputs.DeferredLightPass = s_Data.SceneCompositePasses.DeferredLighting.Raw();
        inputs.OpaqueDecalPass = s_Data.SceneCompositePasses.DeferredOpaqueDecal.Raw();
        inputs.WaterPass = s_Data.WaterPass.Raw();
        inputs.DecalPass = s_Data.DecalPass.Raw();
        inputs.SSAOPass = s_Data.SceneCompositePasses.SSAO.Raw();
        inputs.GTAOPass = s_Data.SceneCompositePasses.GTAO.Raw();
        inputs.ParticlePass = s_Data.SceneCompositePasses.Particle.Raw();
        inputs.OITResolvePass = s_Data.SceneCompositePasses.OITResolve.Raw();
        inputs.SSSPass = s_Data.PostProcessPasses.SSS.Raw();
        inputs.AOApplyPass = s_Data.PostProcessPasses.AOApply.Raw();
        inputs.BloomPass = s_Data.PostProcessPasses.Bloom.Raw();
        inputs.DOFPass = s_Data.PostProcessPasses.DOF.Raw();
        inputs.MotionBlurPass = s_Data.PostProcessPasses.MotionBlur.Raw();
        inputs.TAAPass = s_Data.PostProcessPasses.TAA.Raw();
        inputs.PrecipitationPass = s_Data.PostProcessPasses.Precipitation.Raw();
        inputs.FogPass = s_Data.PostProcessPasses.Fog.Raw();
        inputs.ChromAberrationPass = s_Data.PostProcessPasses.ChromAberration.Raw();
        inputs.ColorGradingPass = s_Data.PostProcessPasses.ColorGrading.Raw();
        inputs.ToneMapPass = s_Data.PostProcessPasses.ToneMap.Raw();
        inputs.VignettePass = s_Data.PostProcessPasses.Vignette.Raw();
        inputs.FXAAPass = s_Data.PostProcessPasses.FXAA.Raw();
        inputs.SelectionOutlinePass = s_Data.PostProcessPasses.SelectionOutline.Raw();
        inputs.UICompositePass = s_Data.PostProcessPasses.UIComposite.Raw();
        inputs.FinalPass = s_Data.PostProcessPasses.Final.Raw();
        inputs.Settings = &s_Data.Settings;
        inputs.PostProcess = &s_Data.PostProcess;
        inputs.Snow = &s_Data.Snow;
        inputs.Fog = &s_Data.Fog;
        inputs.Precipitation = &s_Data.Precipitation;
        inputs.EnableSelectionOutline = &s_Data.EnableSelectionOutline;
        return inputs;
    }

    void Renderer3D::FinalizeConfiguredRenderGraph(RenderingPath path)
    {
        const bool deferred = (path == RenderingPath::Deferred);

        // The topology is graph-native now: ordering is derived primarily from
        // per-pass resource declarations, while this helper is responsible for
        // reporting the configured chain, validating the resource contract, and
        // committing the active-path bookkeeping once the rebuild succeeds.
        if (IsRenderGraphDiagnosticsEnabled())
        {
            if (deferred)
            {
                OLO_CORE_TRACE("Renderer3D: Render graph (Deferred): Shadow -> Scene -> DeferredOpaqueDecal -> DeferredLighting -> ForwardOverlay -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> AOApply -> Bloom -> DOF -> MotionBlur -> TAA -> Precipitation -> Fog -> ChromAb -> ColorGrading -> ToneMap -> Vignette -> FXAA{} -> UIComposite -> Final",
                               s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
            }
            else
            {
                OLO_CORE_TRACE("Renderer3D: Render graph ({}): Shadow -> Scene -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> AOApply -> Bloom -> DOF -> MotionBlur -> TAA -> Precipitation -> Fog -> ChromAb -> ColorGrading -> ToneMap -> Vignette -> FXAA{} -> UIComposite -> Final",
                               path == RenderingPath::ForwardPlus ? "Forward+" : "Forward",
                               s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
            }
        }

        // Validate the resource-aware RDG contract: every pass's declared
        // reads must have a transitive execution dependency on their
        // producer. Hazards are logged to the engine logger; in debug
        // builds we additionally assert so regressions surface immediately.
        const auto hazards = s_Data.RGraph->ValidateResourceHazards();
        if (!hazards.empty())
        {
            // Any Cycle entry means validation couldn't even run;
            // surface that distinctly from genuine resource hazards.
            const bool cycle = std::any_of(hazards.begin(), hazards.end(),
                                           [](const auto& h)
                                           { return h.Kind == RenderGraph::HazardKind::Cycle; });
            if (cycle)
            {
                OLO_CORE_ERROR("Renderer3D: RenderGraph dependency cycle detected — resource hazard validation aborted.");
                OLO_CORE_ASSERT(!cycle, "RenderGraph dependency cycle detected. Break the cycle and retry.");
            }
            else
            {
                OLO_CORE_ERROR("Renderer3D: RenderGraph has {} resource hazards — see previous log entries for details.", hazards.size());
                OLO_CORE_ASSERT(hazards.empty(), "RenderGraph resource hazard detected (see log). Add ConnectPass / AddExecutionDependency for the reported producer -> consumer edge.");
            }
        }
        else if (IsRenderGraphDiagnosticsEnabled())
        {
            OLO_CORE_TRACE("Renderer3D: RenderGraph resource hazard validation passed.");
        }

        s_Data.ActiveGraphPath = path;
        s_Data.ActiveGraphAOTechnique = s_Data.PostProcess.ActiveAOTechnique;
    }

    void Renderer3D::ConfigureRenderGraph(RenderingPath path)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
        {
            OLO_CORE_TRACE("Renderer3D: Configuring RenderGraph for path = {}",
                           path == RenderingPath::Forward       ? "Forward"
                           : path == RenderingPath::ForwardPlus ? "Forward+"
                                                                : "Deferred");
        }

        const auto inputs = BuildFrameGraphInputs();

        // Delegate the production frame topology to the dedicated builder so
        // Renderer3D stays the frontend/facade that owns live pass instances,
        // diagnostics, and path-switch validation rather than the entire
        // node-registration recipe.
        BuildRenderer3DFrameGraph(inputs, path);

        FinalizeConfiguredRenderGraph(path);
    }
} // namespace OloEngine
