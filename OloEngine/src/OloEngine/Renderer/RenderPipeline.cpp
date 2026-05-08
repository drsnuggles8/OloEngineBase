#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/RenderPipelineBuilder.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <variant>

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

        // Halton low-discrepancy sequence used for TAA sub-pixel jitter. Index is
        // 1-based (index 0 is undefined for Halton); the sequence repeats every
        // kHaltonSequenceLength samples which is long enough to de-correlate the
        // jitter pattern from any typical framerate / scene loop.
        constexpr u32 kHaltonSequenceLength = 8;

        f32 HaltonSample(u32 index, u32 base)
        {
            f32 f = 1.0f;
            f32 r = 0.0f;
            while (index > 0)
            {
                f /= static_cast<f32>(base);
                r += f * static_cast<f32>(index % base);
                index /= base;
            }
            return r;
        }
    } // namespace

    void Renderer3D::RenderPipeline::Setup(Renderer3DData& data,
                                           ShaderLibrary& shaderLibrary,
                                           const FramebufferSpecification& shadowPassSpec,
                                           const FramebufferSpecification& scenePassSpec,
                                           const FramebufferSpecification& finalPassSpec)
    {
        if (data.RGraph)
        {
            data.RGraph->ResetTopology();
        }

        Reset();
        CreateFramePasses(data, shaderLibrary, shadowPassSpec, scenePassSpec, finalPassSpec);
        CreateRenderStreamNodes(data);
        CreatePostProcessPasses(finalPassSpec);
    }

    void Renderer3D::RenderPipeline::PrepareFrame(Renderer3DData& data, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();

        // Process any pending GPU resource creation commands from async loaders.
        GPUResourceQueue::ProcessAll();

        // Poll shaders that are still being linked asynchronously by the driver
        // (GL_ARB_parallel_shader_compile). Finalize any that are done.
        if (shaderLibrary.HasPendingShaders())
        {
            const u32 completed = shaderLibrary.PollPendingShaders();
            if (completed > 0)
            {
                OLO_CORE_TRACE("{} shader(s) finished async linking ({} still pending)",
                               completed, shaderLibrary.GetPendingCount());
            }
        }

        // Begin new frame for double-buffered resources.
        FrameResourceManager::Get().BeginFrame();

        RendererProfiler::GetInstance().BeginFrame();

        if (!FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::BeginScene: ScenePass is null!");
            return;
        }

        // Reset frame data buffer for new frame.
        FrameDataBufferManager::Get().Reset();

        // Rotate the per-entity transform history so DrawMesh/DrawAnimated
        // submission can look up "previous frame" transforms. In Forward /
        // Forward+ these maps stay empty because those paths never call
        // GetAndRecordPrevTransform.
        data.PrevEntityTransforms = std::move(data.CurrEntityTransforms);
        data.CurrEntityTransforms.clear();
        data.PrevInstanceTransforms = std::move(data.CurrInstanceTransforms);
        data.CurrInstanceTransforms.clear();

        // Get main-thread allocator for this frame (already reset by BeginFrame).
        CommandAllocator* frameAllocator = FrameResourceManager::Get().GetMainAllocator();
        const auto setRenderStreamAllocator = [frameAllocator](CommandBufferRenderPass* node)
        {
            if (node)
                node->SetCommandAllocator(frameAllocator);
        };
        StreamNodes.ForEach(setRenderStreamAllocator);

        // TAA projection jitter. We bake a sub-pixel Halton offset into the
        // projection matrix so the same pixel samples a slightly different
        // geometric position each frame; the TAA accumulator then averages
        // across frames for sub-pixel anti-aliasing. The jitter is applied
        // uniformly to ProjectionMatrix and therefore ViewProjectionMatrix,
        // so every downstream pass (G-Buffer, lighting, decals, water,
        // SSAO/GTAO, post-process) observes the same jittered camera. Both
        // the current and previous ViewProjection carry their respective
        // jitters so depth-based reprojection in TAA remains self-consistent
        // without requiring an explicit unjitter uniform.
        data.PrevJitterUV = data.CurrJitterUV;
        data.CurrJitterUV = glm::vec2(0.0f);
        if (data.PostProcess.TAAEnabled && FrameCorePasses.Scene)
        {
            const auto& spec = FrameCorePasses.Scene->GetFramebufferSpecification();
            if (spec.Width > 0 && spec.Height > 0)
            {
                // 1-based Halton index; Halton(0) is undefined. Loop modulo
                // kHaltonSequenceLength keeps the pattern short and stable.
                const u32 idx = (data.TAAJitterFrameIndex % kHaltonSequenceLength) + 1;
                // Halton samples land in [0, 1]; remap to [-0.5, 0.5] so the
                // jitter is centred around the unperturbed pixel.
                const f32 jx = HaltonSample(idx, 2) - 0.5f;
                const f32 jy = HaltonSample(idx, 3) - 0.5f;

                // Convert pixel offset to NDC — 2 NDC units span the screen,
                // so one pixel in NDC = 2 / resolution.
                const f32 jitterNdcX = jx * (2.0f / static_cast<f32>(spec.Width));
                const f32 jitterNdcY = jy * (2.0f / static_cast<f32>(spec.Height));

                // For perspective projections (P[3][3] == 0, P[2][3] == -1),
                // inject jitter via the z-column of the projection matrix.
                // After the perspective divide this becomes a constant NDC
                // offset (x_ndc = P[2][0] * z / w_clip = P[2][0] * z / -z = -P[2][0])
                // which is exactly the sub-pixel shift we want.
                //
                // For orthographic projections (P[3][3] == 1, P[2][3] == 0),
                // writing to P[2][0/1] produces a *depth-dependent* shear:
                // x_ndc = P[0][0]*x + P[2][0]*z + P[3][0]. Instead, add the
                // jitter to the translation row so every fragment gets the
                // same sub-pixel shift independent of depth.
                const bool isOrthographic = glm::abs(data.ProjectionMatrix[3][3] - 1.0f) < 1e-5f;
                if (isOrthographic)
                {
                    data.ProjectionMatrix[3][0] += jitterNdcX;
                    data.ProjectionMatrix[3][1] += jitterNdcY;
                }
                else
                {
                    data.ProjectionMatrix[2][0] += jitterNdcX;
                    data.ProjectionMatrix[2][1] += jitterNdcY;
                }
                data.ViewProjectionMatrix = data.ProjectionMatrix * data.ViewMatrix;

                // Track jitter in UV-space so the TAA shader (or any future
                // consumer) can subtract it if needed. NDC -> UV is * 0.5.
                data.CurrJitterUV = glm::vec2(jitterNdcX * 0.5f, jitterNdcY * 0.5f);

                data.TAAJitterFrameIndex = (data.TAAJitterFrameIndex + 1) % kHaltonSequenceLength;
            }
        }
        else
        {
            data.TAAJitterFrameIndex = 0;
        }

        CommandDispatch::SetViewProjectionMatrix(data.ViewProjectionMatrix);
        CommandDispatch::SetViewMatrix(data.ViewMatrix);
        CommandDispatch::SetProjectionMatrix(data.ProjectionMatrix);
        // Mirror the previous-frame view-projection into CommandDispatch so
        // dispatch paths that upload the shared CameraUBO themselves
        // (Terrain / Voxel / Decal) can fill
        // `CameraUBO::PrevViewProjection` with the true history instead of
        // aliasing the current VP — the latter wipes the matrix any other
        // consumer (TAA velocity reconstruction, motion blur) reads this
        // frame.
        CommandDispatch::SetPrevViewProjectionMatrix(data.PrevViewProjectionMatrix);

        data.InverseViewProjectionMatrix = glm::inverse(data.ViewProjectionMatrix);
        data.ViewFrustum.Update(data.ViewProjectionMatrix);

        data.Stats.Reset();
        data.CommandCounter = 0;

        // Advance occlusion culling frame (reads back previous frame's query results).
        if (data.OcclusionCullingEnabled)
        {
            data.OcclusionResultsAvailable = OcclusionQueryPool::GetInstance().BeginFrame();
            OcclusionStateManager::GetInstance().BeginFrame();
        }
        else
        {
            data.OcclusionResultsAvailable = false;
        }

        {
            ShaderBindingLayout::CameraUBO cameraData;
            cameraData.ViewProjection = data.ProjectionMatrix * data.ViewMatrix;
            cameraData.View = data.ViewMatrix;
            cameraData.Projection = data.ProjectionMatrix;
            cameraData.Position = data.ViewPos;
            cameraData._padding0 = 0.0f;
            // Previous-frame view-projection is maintained in
            // `data.PrevViewProjectionMatrix`. Forward PBR shaders consume
            // this through the CameraMatrices UBO (binding 0) to emit screen-
            // space velocity into scene FB RT3 — mirroring what the deferred
            // G-Buffer PBR shader does through u_PrevViewProjection.
            cameraData.PrevViewProjection = data.PrevViewProjectionMatrix;

            constexpr auto expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch");

            data.SharedSceneUBOs.Camera->SetData(&cameraData, expectedSize);

            // Re-bind to ensure this UBO is active at binding point 0.
            // Other subsystems (e.g. ShadowMap) create their own camera UBOs at the
            // same binding point, which can overwrite the persistent binding.
            data.SharedSceneUBOs.Camera->Bind();
        }

        if (data.SharedSceneUBOs.LightProperties)
        {
            ShaderBindingLayout::LightUBO lightData;
            const auto lightType = std::to_underlying(data.SceneLight.Type);

            lightData.LightPosition = glm::vec4(data.SceneLight.Position, 1.0f);
            lightData.LightDirection = glm::vec4(data.SceneLight.Direction, 0.0f);
            lightData.LightAmbient = glm::vec4(data.SceneLight.Ambient, 0.0f);
            lightData.LightDiffuse = glm::vec4(data.SceneLight.Diffuse, 0.0f);
            lightData.LightSpecular = glm::vec4(data.SceneLight.Specular, 0.0f);
            lightData.LightAttParams = glm::vec4(
                data.SceneLight.Constant,
                data.SceneLight.Linear,
                data.SceneLight.Quadratic,
                0.0f);
            lightData.LightSpotParams = glm::vec4(
                data.SceneLight.CutOff,
                data.SceneLight.OuterCutOff,
                0.0f,
                0.0f);
            lightData.ViewPosAndLightType = glm::vec4(data.ViewPos, static_cast<f32>(lightType));

            data.SharedSceneUBOs.LightProperties->SetData(&lightData, sizeof(ShaderBindingLayout::LightUBO));
        }

        CommandDispatch::SetSceneLight(data.SceneLight);
        CommandDispatch::SetViewPosition(data.ViewPos);

        const auto resetRenderStreamBucket = [](CommandBufferRenderPass* node)
        {
            if (node)
                node->ResetCommandBucket();
        };
        StreamNodes.ForEach(resetRenderStreamBucket);

        CommandDispatch::ResetState();

        // Set shadow texture IDs AFTER ResetState() so they aren't zeroed out.
        CommandDispatch::SetShadowTextureIDs(
            data.Shadow.GetCSMRendererID(),
            data.Shadow.GetSpotRendererID());

        // Set point shadow cubemap texture IDs.
        {
            std::array<u32, ShadowMap::MAX_POINT_SHADOWS> pointIDs{};
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                pointIDs[i] = data.Shadow.GetPointRendererID(i);
            }
            CommandDispatch::SetPointShadowTextureIDs(pointIDs);
        }

        // Initialize parallel scene context with immutable frame data.
        data.ParallelContext.ViewMatrix = data.ViewMatrix;
        data.ParallelContext.ProjectionMatrix = data.ProjectionMatrix;
        data.ParallelContext.ViewProjectionMatrix = data.ViewProjectionMatrix;
        data.ParallelContext.ViewPosition = data.ViewPos;
        data.ParallelContext.ViewFrustum = data.ViewFrustum;
        data.ParallelContext.FrustumCullingEnabled = data.FrustumCullingEnabled;
        data.ParallelContext.DynamicCullingEnabled = data.DynamicCullingEnabled;

        // Cache shader references for parallel access.
        data.ParallelContext.LightingShader = data.LightingShader;
        data.ParallelContext.SkinnedLightingShader = data.SkinnedLightingShader;
        // Route PBR shader slot to the G-Buffer write variant in Deferred mode
        // so parallel-submission workers pick the correct program without
        // needing to query RendererSettings per draw.
        const bool deferredActive = (data.Settings.Path == RenderingPath::Deferred);
        data.ParallelContext.PBRShader = (deferredActive && data.PBRGBufferShader)
                                             ? data.PBRGBufferShader
                                             : data.PBRShader;
        data.ParallelContext.PBRSkinnedShader = (deferredActive && data.PBRGBufferSkinnedShader)
                                                    ? data.PBRGBufferSkinnedShader
                                                    : data.PBRSkinnedShader;
        data.ParallelContext.LightCubeShader = data.LightCubeShader;
        data.ParallelContext.SkyboxShader = data.SkyboxShader;
        data.ParallelContext.QuadShader = data.QuadShader;

        data.ParallelSubmissionActive = false;
    }

    void Renderer3D::RenderPipeline::ConfigurePassesForFrame(Renderer3DData& data)
    {
        // OITResolvePass and SSSPass are self-resolving:
        // their Execute(RGCommandContext&) calls context.GetBlackboard() to look
        // up SceneColor directly, eliminating the per-frame side-channel.
        // (A previous typed-handle side-channel block was removed;
        // removed since those two passes resolve via the blackboard.)

        // ForwardOverlayPass, FoliagePass, WaterPass,
        // DecalPass and ParticlePass now self-resolve SceneColor (and
        // SceneDepth for Decal) directly from the render-graph blackboard
        // inside their Execute() implementations. No per-frame side-channel
        // setter calls are needed here.
        {
            // DeferredLightingPass now self-resolves all
            // G-Buffer and scene depth handles from the render-graph
            // blackboard. No SetXxx calls needed.
        }

        if (FrameCorePasses.Scene && data.Settings.Path == RenderingPath::Deferred)
        {
            FrameCorePasses.Scene->PrepareDeferredResources(data.Settings.Deferred.MSAASampleCount);
        }

        if (SceneCompositePasses.Particle)
        {
            SceneCompositePasses.Particle->SetRenderCallback(std::move(data.PendingParticleRenderCallback));
        }

        if (SceneCompositePasses.SSAO)
        {
            SceneCompositePasses.SSAO->SetSettings(data.PostProcess);
            // SSAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // Upload projection matrices for SSAO position reconstruction
            data.PostProcessGPU.SSAOData.Projection = data.ProjectionMatrix;
            data.PostProcessGPU.SSAOData.InverseProjection = glm::inverse(data.ProjectionMatrix);
            data.PostProcessGPU.SSAOData.DebugView = data.PostProcess.SSAODebugView ? 1 : 0;
        }
        if (SceneCompositePasses.GTAO)
        {
            SceneCompositePasses.GTAO->SetSettings(data.PostProcess);
            SceneCompositePasses.GTAO->SetProjectionMatrix(data.ProjectionMatrix);
            SceneCompositePasses.GTAO->SetViewMatrix(data.ViewMatrix);
            // GTAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // When GTAO is active, override the SSAO UBO debug/intensity fields
            // so the PostProcess_SSAOApply shader reads correct values for GTAO,
            // and upload the UBO since SSAORenderPass::Execute() is skipped.
            if (data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && data.PostProcess.GTAOEnabled)
            {
                data.PostProcessGPU.SSAOData.DebugView = data.PostProcess.GTAODebugView ? 1 : 0;
                // GTAO power is baked in compute; intensity=1 for the apply pass
                data.PostProcessGPU.SSAOData.Intensity = 1.0f;
                data.PostProcessGPU.SSAO->SetData(&data.PostProcessGPU.SSAOData, SSAOUBOData::GetSize());
                data.PostProcessGPU.SSAO->Bind();
            }
        }
        if (PostProcessPasses.SSS)
        {
            PostProcessPasses.SSS->SetSettings(data.Snow);
        }
        // Wire AOApplyPass before the dynamic post chain.
        if (PostProcessPasses.AOApply)
        {
            const bool ssaoEnabled = data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && data.PostProcess.SSAOEnabled;
            const bool gtaoEnabled = data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && data.PostProcess.GTAOEnabled;
            const bool aoApplyEnabled = (ssaoEnabled || gtaoEnabled) && PostProcessPasses.AOApply->IsReadyForExecution();
            PostProcessPasses.AOApply->SetEnabled(aoApplyEnabled);
            // SetAOTextureID was removed; AOApplyPass::Execute()
            // self-resolves AO texture via the render-graph blackboard.
            PostProcessPasses.AOApply->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
            PostProcessPasses.AOApply->SetSSAOUBO(data.PostProcessGPU.SSAO);
        }
        // Standalone dynamic post-chain configuration.
        if (PostProcessPasses.Bloom)
        {
            PostProcessPasses.Bloom->SetEnabled(data.PostProcess.BloomEnabled && PostProcessPasses.Bloom->IsReadyForExecution());
            PostProcessPasses.Bloom->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
            PostProcessPasses.Bloom->SetPostProcessGPUData(&data.PostProcessGPU.PostProcessData);
        }

        if (PostProcessPasses.DOF)
        {
            PostProcessPasses.DOF->SetEnabled(data.PostProcess.DOFEnabled);
            PostProcessPasses.DOF->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }

        if (PostProcessPasses.MotionBlur)
        {
            PostProcessPasses.MotionBlur->SetEnabled(data.PostProcess.MotionBlurEnabled);
            PostProcessPasses.MotionBlur->SetMotionBlurUBO(data.PostProcessGPU.MotionBlur);
        }

        if (PostProcessPasses.TAA)
        {
            PostProcessPasses.TAA->SetEnabled(data.PostProcess.TAAEnabled);
            PostProcessPasses.TAA->SetSettings(data.PostProcess);
        }

        if (PostProcessPasses.Precipitation)
        {
            const bool precipEnabled = data.Precipitation.Enabled &&
                                       (data.Precipitation.ScreenStreaksEnabled ||
                                        data.Precipitation.LensImpactsEnabled);
            PostProcessPasses.Precipitation->SetEnabled(precipEnabled);
        }

        if (PostProcessPasses.Fog)
        {
            PostProcessPasses.Fog->SetEnabled(data.Fog.Enabled);
            PostProcessPasses.Fog->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }

        if (PostProcessPasses.ChromAberration)
        {
            PostProcessPasses.ChromAberration->SetEnabled(data.PostProcess.ChromaticAberrationEnabled);
            PostProcessPasses.ChromAberration->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }
        if (PostProcessPasses.ColorGrading)
        {
            PostProcessPasses.ColorGrading->SetEnabled(data.PostProcess.ColorGradingEnabled);
            PostProcessPasses.ColorGrading->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }
        if (PostProcessPasses.ToneMap)
        {
            // ToneMap always runs; m_Enabled stays true.
            PostProcessPasses.ToneMap->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }
        if (PostProcessPasses.Vignette)
        {
            PostProcessPasses.Vignette->SetEnabled(data.PostProcess.VignetteEnabled);
            PostProcessPasses.Vignette->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }

        if (PostProcessPasses.FXAA)
        {
            PostProcessPasses.FXAA->SetEnabled(data.PostProcess.FXAAEnabled);
            PostProcessPasses.FXAA->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
        }

        if (PostProcessPasses.SelectionOutline)
        {
            PostProcessPasses.SelectionOutline->SetSelectedEntityIDs(data.SelectionOutlineEntityIDs);
            const bool selectionOutlineEnabled = data.EnableSelectionOutline &&
                                                !data.SelectionOutlineEntityIDs.empty() &&
                                                PostProcessPasses.SelectionOutline->IsReadyForExecution();
            PostProcessPasses.SelectionOutline->SetEnabled(selectionOutlineEnabled);
        }

        if (PostProcessPasses.UIComposite)
        {
            PostProcessPasses.UIComposite->SetRenderCallback(std::move(data.PendingUICompositeRenderCallback));
        }

        // Wire deferred lighting pass inputs each frame so it reflects the
        // current G-Buffer / debug-channel selection. The pass no-ops when
        // the path is Forward / Forward+ (GBuffer is never created).
        if (SceneCompositePasses.DeferredLighting && FrameCorePasses.Scene)
        {
            const bool deferred = (data.Settings.Path == RenderingPath::Deferred);
            SceneCompositePasses.DeferredLighting->SetGBuffer(deferred ? FrameCorePasses.Scene->GetGBuffer() : nullptr);
            SceneCompositePasses.DeferredLighting->SetDebugChannel(deferred ? data.Settings.Deferred.DebugChannel : 0);
            SceneCompositePasses.DeferredLighting->SetPerSampleLighting(deferred && data.Settings.Deferred.PerSampleLighting);
        }

        // Wire the opaque-decal graph shim: in Deferred mode it drains the
        // DecalRenderPass bucket into the G-Buffer between ScenePass and
        // DeferredLightingPass. Safe to update unconditionally — the pass
        // no-ops when it isn't registered in the graph (Forward paths).
        if (SceneCompositePasses.DeferredOpaqueDecal && RenderStreamPasses.Decal && FrameCorePasses.Scene)
        {
            const bool deferred = (data.Settings.Path == RenderingPath::Deferred);
            SceneCompositePasses.DeferredOpaqueDecal->SetDecalPass(RenderStreamPasses.Decal);
            SceneCompositePasses.DeferredOpaqueDecal->SetGBuffer(deferred ? FrameCorePasses.Scene->GetGBuffer() : nullptr);
            SceneCompositePasses.DeferredOpaqueDecal->SetPerSampleLighting(deferred && data.Settings.Deferred.PerSampleLighting);
        }

        // Phase 6: propagate OIT toggle to transparent passes that still
        // participate in the WB-OIT path, plus the prepare/resolve passes,
        // every frame so UI changes take effect immediately.
        {
            const bool oitEnabled = (data.Settings.Path == RenderingPath::Deferred) && data.Settings.Deferred.OITEnabled;
            if (SceneCompositePasses.Particle)
                SceneCompositePasses.Particle->SetOITEnabled(oitEnabled);
            if (RenderStreamPasses.Decal)
                RenderStreamPasses.Decal->SetOITEnabled(oitEnabled);
            if (SceneCompositePasses.OITPrepare)
                SceneCompositePasses.OITPrepare->SetEnabled(oitEnabled);
            if (SceneCompositePasses.OITResolve)
                SceneCompositePasses.OITResolve->SetEnabled(oitEnabled);
        }
    }

    void Renderer3D::RenderPipeline::ApplyGlobalResources(Renderer3DData& data)
    {
        OLO_PROFILE_FUNCTION();

        const auto& shaderRegistries = ShaderResourceRegistry::GetRegisteredRegistries();
        const auto& globalResources = data.GlobalResourceRegistry.GetBoundResources();

        for (const auto& shaderRegistryEntry : shaderRegistries)
        {
            auto* registry = shaderRegistryEntry.second;
            if (!registry)
            {
                continue;
            }

            for (const auto& [resourceName, resource] : globalResources)
            {
                if (registry->GetBindingInfo(resourceName) == nullptr)
                {
                    continue;
                }

                ShaderResourceInput input;
                if (std::holds_alternative<Ref<UniformBuffer>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<UniformBuffer>>(resource));
                }
                else if (std::holds_alternative<Ref<Texture2D>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<Texture2D>>(resource));
                }
                else if (std::holds_alternative<Ref<TextureCubemap>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<TextureCubemap>>(resource));
                }

                if (input.Type != ShaderResourceType::None)
                {
                    registry->SetResource(resourceName, input);
                }
            }
        }
    }

    void Renderer3D::RenderPipeline::UploadExecutionState(Renderer3DData& data)
    {
        auto& profiler = RendererProfiler::GetInstance();
        if (FrameCorePasses.Scene)
        {
            const auto& commandBucket = FrameCorePasses.Scene->GetCommandBucket();
            profiler.IncrementCounter(RendererProfiler::MetricType::CommandPackets, static_cast<u32>(commandBucket.GetCommandCount()));
        }

        ApplyGlobalResources(data);

        // Upload post-process settings to GPU
        {
            auto& pp = data.PostProcess;
            auto& gpu = data.PostProcessGPU.PostProcessData;
            gpu.TonemapOperator = static_cast<i32>(pp.Tonemap);
            gpu.Exposure = pp.Exposure;
            gpu.Gamma = pp.Gamma;
            gpu.BloomThreshold = pp.BloomThreshold;
            gpu.BloomIntensity = pp.BloomIntensity;
            gpu.VignetteIntensity = pp.VignetteIntensity;
            gpu.VignetteSmoothness = pp.VignetteSmoothness;
            gpu.ChromaticAberrationIntensity = pp.ChromaticAberrationIntensity;
            gpu.DOFFocusDistance = pp.DOFFocusDistance;
            gpu.DOFFocusRange = pp.DOFFocusRange;
            gpu.DOFBokehRadius = pp.DOFBokehRadius;
            gpu.MotionBlurStrength = pp.MotionBlurStrength;
            gpu.MotionBlurSamples = pp.MotionBlurSamples;
            gpu.CameraNear = data.CameraNearClip;
            gpu.CameraFar = data.CameraFarClip;
            if (FrameCorePasses.Scene)
            {
                const auto& spec = FrameCorePasses.Scene->GetFramebufferSpecification();
                gpu.InverseScreenWidth = 1.0f / static_cast<f32>(spec.Width);
                gpu.InverseScreenHeight = 1.0f / static_cast<f32>(spec.Height);
            }
            data.PostProcessGPU.PostProcess->SetData(&gpu, PostProcessUBOData::GetSize());
        }

        // Upload DRS bounds so screen-space shaders can clamp UVs to the rendered region.
        {
            const glm::vec2 bounds = data.RGraph ? data.RGraph->GetRenderScaleBounds() : glm::vec2(1.0f);
            data.SceneEffectsGPU.DRSData.RenderScaleBounds = bounds;
            data.SceneEffectsGPU.DRS->SetData(&data.SceneEffectsGPU.DRSData, DRSUBOData::GetSize());
        }

        // Upload snow settings to GPU
        if (data.Snow.Enabled)
        {
            auto& snow = data.Snow;
            auto& gpu = data.SceneEffectsGPU.SnowData;
            gpu.CoverageParams = glm::vec4(snow.HeightStart, snow.HeightFull, snow.SlopeStart, snow.SlopeFull);
            gpu.AlbedoAndRoughness = glm::vec4(snow.Albedo, snow.Roughness);
            gpu.SSSColorAndIntensity = glm::vec4(snow.SSSColor, snow.SSSIntensity);
            gpu.SparkleParams = glm::vec4(snow.SparkleIntensity, snow.SparkleDensity, snow.SparkleScale, snow.NormalPerturbStrength);
            gpu.Flags = glm::vec4(1.0f, snow.WindDriftFactor, 0.0f, 0.0f);
            data.SceneEffectsGPU.Snow->SetData(&gpu, SnowUBOData::GetSize());

            // SSS blur parameters
            auto& sssGpu = data.SceneEffectsGPU.SSSData;
            sssGpu.BlurParams = glm::vec4(snow.SSSBlurRadius, snow.SSSBlurFalloff, 0.0f, 0.0f);
            sssGpu.Flags = glm::vec4(snow.SSSBlurEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            if (FrameCorePasses.Scene)
            {
                const auto& spec = FrameCorePasses.Scene->GetFramebufferSpecification();
                sssGpu.BlurParams.z = static_cast<f32>(spec.Width);
                sssGpu.BlurParams.w = static_cast<f32>(spec.Height);
            }
            data.SceneEffectsGPU.SSS->SetData(&sssGpu, SSSUBOData::GetSize());
        }
        else
        {
            // Upload disabled state so shaders skip snow
            auto& gpu = data.SceneEffectsGPU.SnowData;
            gpu.Flags = glm::vec4(0.0f);
            data.SceneEffectsGPU.Snow->SetData(&gpu, SnowUBOData::GetSize());
        }

        // Upload fog & atmospheric scattering settings to GPU
        if (data.Fog.Enabled)
        {
            auto& fog = data.Fog;
            auto& gpu = data.SceneEffectsGPU.FogData;
            gpu.ColorAndDensity = glm::vec4(fog.Color, fog.Density);
            gpu.DistanceParams = glm::vec4(fog.Start, fog.End, fog.HeightFalloff, fog.HeightOffset);
            gpu.ScatterParams = glm::vec4(fog.RayleighStrength, fog.MieStrength, fog.MieDirectionality, fog.SunIntensity);
            gpu.RayleighColorAndMaxOpacity = glm::vec4(fog.RayleighColor, fog.MaxOpacity);
            // Derive sun direction from the scene's primary directional light
            // Guard against zero-length direction to prevent NaN from normalize
            glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
            const f32 dirLen2 = glm::dot(data.SceneLight.Direction, data.SceneLight.Direction);
            if (std::isfinite(dirLen2) && dirLen2 > 1e-8f)
            {
                sunDir = glm::normalize(data.SceneLight.Direction);
            }
            // Pack fog frame index into SunDirection.w (bare uniforms fail SPIR-V)
            // Wrap at 1024 to stay well within float32 integer-exact range
            gpu.SunDirection = glm::vec4(sunDir, static_cast<f32>(data.FogFrameIndex));
            data.FogFrameIndex = (data.FogFrameIndex + 1u) & 0x3FFu;
            gpu.Flags = glm::vec4(1.0f, static_cast<f32>(static_cast<i32>(fog.Mode)),
                                  fog.EnableScattering ? 1.0f : 0.0f,
                                  fog.EnableVolumetric ? 1.0f : 0.0f);

            // Accumulate time for noise animation
            const auto fogNow = std::chrono::steady_clock::now();
            const f32 fogDt = std::clamp(std::chrono::duration<f32>(fogNow - data.FogLastTime).count(), 0.0f, 0.1f);
            data.FogLastTime = fogNow;
            data.FogTime += fogDt;

            const f32 effectiveNoiseIntensity = fog.EnableNoise ? fog.NoiseIntensity : 0.0f;
            gpu.NoiseParams = glm::vec4(fog.NoiseScale, fog.NoiseSpeed, effectiveNoiseIntensity, data.FogTime);
            gpu.VolumetricParams = glm::vec4(static_cast<f32>(fog.VolumetricSamples), fog.AbsorptionCoefficient,
                                             fog.LightShaftIntensity, fog.EnableLightShafts ? 1.0f : 0.0f);
            data.SceneEffectsGPU.Fog->SetData(&gpu, FogUBOData::GetSize());
        }
        else
        {
            auto& gpu = data.SceneEffectsGPU.FogData;
            gpu.Flags = glm::vec4(0.0f);
            data.SceneEffectsGPU.Fog->SetData(&gpu, FogUBOData::GetSize());
        }

        // Upload fog volumes (collected by the scene)
        data.SceneEffectsGPU.FogVolumes->SetData(&data.SceneEffectsGPU.FogVolumesData, FogVolumesUBOData::GetSize());

        // Update wind system (regenerate 3D wind field, upload wind UBO)
        {
            // TODO: Pass actual frame dt once Timestep is threaded through BeginScene
            static auto lastTime = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            f32 dt = std::chrono::duration<f32>(now - lastTime).count();
            dt = std::clamp(dt, 0.0f, 0.1f);
            lastTime = now;

            WindSystem::Update(data.Wind, data.ViewPos, Timestep(dt));
            WindSystem::BindWindTexture();

            // Update snow accumulation system
            if (data.SnowAccumulation.Enabled)
            {
                SnowAccumulationSystem::Update(data.SnowAccumulation, data.ViewPos, Timestep(dt));
                SnowAccumulationSystem::BindSnowDepthTexture();
                CommandDispatch::SetSnowDepthTextureID(SnowAccumulationSystem::GetSnowDepthTextureID());
            }

            // Update snow ejecta particle simulation
            if (data.SnowEjecta.Enabled)
            {
                SnowEjectaSystem::Update(data.SnowEjecta, Timestep(dt));
            }

            // Update precipitation system (always run so disabled particles can drain)
            {
                glm::vec3 windXZ = glm::vec3(data.Wind.Direction.x, 0.0f, data.Wind.Direction.z);
                f32 windXZLen = glm::length(windXZ);
                glm::vec3 windDir = (windXZLen > 1e-6f) ? (windXZ / windXZLen) : glm::vec3(1.0f, 0.0f, 0.0f);
                f32 windSpeed = data.Wind.Speed;
                PrecipitationSystem::Update(data.Precipitation, data.ViewPos, windDir, windSpeed, Timestep(dt));

                if (data.Precipitation.Enabled)
                {
                    glm::vec2 windDirScreen = glm::vec2(windDir.x, -windDir.z);
                    ScreenSpacePrecipitation::Update(data.Precipitation, PrecipitationSystem::GetCurrentIntensity(), windDirScreen, windSpeed, dt);
                    PrecipitationSystem::UpdateScreenEffectsUBO(data.Precipitation, windDirScreen, data.FogTime);
                }
            }
        }

        // Upload motion blur / inverse VP matrices (needed by motion blur AND fog depth reconstruction
        // AND the deferred lighting pass, which reconstructs world-space position from G-Buffer depth
        // AND TAA for camera-only velocity reprojection in Forward / Forward+).
        if (data.PostProcess.MotionBlurEnabled || data.PostProcess.TAAEnabled || data.Fog.Enabled || data.Settings.Path == RenderingPath::Deferred)
        {
            auto& mb = data.PostProcessGPU.MotionBlurData;
            mb.InverseViewProjection = data.InverseViewProjectionMatrix;
            mb.PrevViewProjection = data.PrevViewProjectionMatrix;
            data.PostProcessGPU.MotionBlur->SetData(&mb, MotionBlurUBOData::GetSize());
        }
    }

    void Renderer3D::RenderPipeline::PopulateBlackboard(Renderer3DData& data)
    {
        OLO_PROFILE_FUNCTION();

        if (!data.RGraph)
            return;

        auto& graph = *data.RGraph;
        auto& pipeline = *this;

        // Clear prior-frame handles so stale handles are never accidentally resolved.
        graph.ClearBlackboard();
        graph.ClearImportedResources();

        auto& board = graph.GetBlackboard();

        // ------------------------------------------------------------------
        // Scene outputs
        // ------------------------------------------------------------------
        if (pipeline.FrameCorePasses.Scene && pipeline.FrameCorePasses.Scene->GetTarget())
        {
            board.SceneColor = graph.ImportFramebuffer(
                ResourceNames::SceneColor, pipeline.FrameCorePasses.Scene->GetTarget());

            // Sanity-check: importing must immediately resolve to the same
            // framebuffer. If not, the RenderGraph handle layer is broken.
            // Logged ONCE per change so we notice regressions without
            // spamming the log every frame.
            {
                static u32 s_PrevFbGL = 0;
                static u32 s_PrevTex0 = 0;
                const auto importedFB = pipeline.FrameCorePasses.Scene->GetTarget();
                const auto resolveNow = graph.ResolveFramebuffer(board.SceneColor);
                const u32 importedFbGL = importedFB->GetRendererID();
                const u32 importedTex0 = importedFB->GetColorAttachmentRendererID(0);
                const u32 resolveFbGL = resolveNow ? resolveNow->GetRendererID() : 0u;
                const u32 resolveTex0 = resolveNow ? resolveNow->GetColorAttachmentRendererID(0) : 0u;
                if (importedFbGL != resolveFbGL || importedTex0 != resolveTex0)
                {
                    OLO_CORE_ERROR("Renderer3D: SceneColor IMPORT/RESOLVE MISMATCH: handle=(idx={}, gen={}) importedFbGL={} importedTex0={} resolveFbGL={} resolveTex0={}",
                                   board.SceneColor.Index, board.SceneColor.Generation,
                                   importedFbGL, importedTex0, resolveFbGL, resolveTex0);
                }
                else if (importedFbGL != s_PrevFbGL || importedTex0 != s_PrevTex0)
                {
                    if (IsRenderGraphDiagnosticsEnabled())
                    {
                        OLO_CORE_TRACE("Renderer3D: SceneColor IMPORT OK: handle=(idx={}, gen={}) fbGL={} tex0={}",
                                       board.SceneColor.Index, board.SceneColor.Generation, importedFbGL, importedTex0);
                    }
                    s_PrevFbGL = importedFbGL;
                    s_PrevTex0 = importedTex0;
                }
            }

            // AO/Deferred consumers need true geometric depth + view-space
            // normals. In Deferred mode these come from the prepared
            // G-Buffer resolved attachments (not from ScenePass target
            // attachments, which are cleared for overlay consumers).
            const bool deferredActive = (data.Settings.Path == RenderingPath::Deferred);
            const auto gbuffer = deferredActive ? pipeline.FrameCorePasses.Scene->GetGBuffer() : Ref<GBuffer>{};
            OLO_CORE_ASSERT(!deferredActive || gbuffer,
                            "Renderer3D: Deferred path requires a prepared GBuffer before blackboard population");

            const u32 depthID = deferredActive
                                    ? gbuffer->GetDepthAttachmentID()
                                    : pipeline.FrameCorePasses.Scene->GetTarget()->GetDepthAttachmentRendererID();
            board.SceneDepth = graph.ImportTexture(
                ResourceNames::SceneDepth,
                depthID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth));

            const u32 sceneNormalsID = deferredActive
                                           ? gbuffer->GetColorAttachmentID(GBuffer::Normal)
                                           : pipeline.FrameCorePasses.Scene->GetTarget()->GetColorAttachmentRendererID(2);
            board.SceneNormals = graph.ImportTexture(
                ResourceNames::SceneNormals,
                sceneNormalsID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::SceneNormals));
        }

        // ------------------------------------------------------------------
        // G-Buffer (deferred path only)
        // ------------------------------------------------------------------
        const bool deferredActive = (data.Settings.Path == RenderingPath::Deferred);
        if (deferredActive && pipeline.FrameCorePasses.Scene)
            {
            const auto& gbuffer = pipeline.FrameCorePasses.Scene->GetGBuffer();
            OLO_CORE_ASSERT(gbuffer,
                            "Renderer3D: Deferred path requires a prepared GBuffer before GBuffer import");

            // G-Buffer attachments come from the dedicated GBuffer object,
            // NOT from ScenePass->GetTarget() (which is the lit-output FB in
            // deferred mode and only has a single color attachment).
            //
            // Layout matches GBuffer::AttachmentIndex:
            //   RT0 Albedo   — albedo.rgb + metallic.a
            //   RT1 Normal   — octahedral normal + roughness + AO
            //   RT2 Emissive — emissive HDR
            //   RT3 Velocity — exposed via the dedicated `Velocity` import
            //                  block below; not duplicated here.
            auto importGBuf = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
            {
                const u32 id = gbuffer->GetColorAttachmentID(slot);
                return graph.ImportTexture(name, id,
                                           RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, name));
            };

            board.GBufferAlbedo = importGBuf(ResourceNames::GBufferAlbedo, GBuffer::Albedo);
            board.GBufferNormal = importGBuf(ResourceNames::GBufferNormal, GBuffer::Normal);
            board.GBufferEmissive = importGBuf(ResourceNames::GBufferEmissive, GBuffer::Emissive);

            // Multisample companion handles. Imported
            // only when MSAA is active so the typed-handle path can drive
            // per-sample shading without going through the raw GBuffer
            // accessor. SceneDepthMS is also exposed here (rather than
            // alongside SceneDepth) because the multisample depth lives on
            // the G-Buffer, not on the lit scene framebuffer.
            if (gbuffer->GetSampleCount() > 1u)
            {
                auto importGBufMS = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
                {
                    const u32 id = gbuffer->GetMSColorAttachmentID(slot);
                    return graph.ImportTexture(name, id,
                                               RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, name));
                };

                board.GBufferAlbedoMS = importGBufMS(ResourceNames::GBufferAlbedoMS, GBuffer::Albedo);
                board.GBufferNormalMS = importGBufMS(ResourceNames::GBufferNormalMS, GBuffer::Normal);
                board.GBufferEmissiveMS = importGBufMS(ResourceNames::GBufferEmissiveMS, GBuffer::Emissive);
                board.VelocityMS = importGBufMS(ResourceNames::VelocityMS, GBuffer::Velocity);

                if (const u32 depthMSID = gbuffer->GetMSDepthAttachmentID(); depthMSID != 0)
                {
                    board.SceneDepthMS = graph.ImportTexture(
                        ResourceNames::SceneDepthMS, depthMSID,
                        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepthMS));
                }
            }
        }

        // ------------------------------------------------------------------
        // Velocity buffer
        // ------------------------------------------------------------------
        {
            u32 velocityID = 0;
            if (data.Settings.Path == RenderingPath::Deferred && pipeline.FrameCorePasses.Scene)
            {
                const auto& gbuffer = pipeline.FrameCorePasses.Scene->GetGBuffer();
                OLO_CORE_ASSERT(gbuffer,
                                "Renderer3D: Deferred path requires a prepared GBuffer before velocity import");
                velocityID = gbuffer->GetColorAttachmentID(GBuffer::Velocity);
            }
            else if (pipeline.FrameCorePasses.Scene && pipeline.FrameCorePasses.Scene->GetTarget())
            {
                velocityID = pipeline.FrameCorePasses.Scene->GetTarget()->GetColorAttachmentRendererID(3);
            }
            if (velocityID != 0)
            {
                board.Velocity = graph.ImportTexture(
                    ResourceNames::Velocity, velocityID,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::Velocity));
            }
        }

        // ------------------------------------------------------------------
        // AO buffer
        // ------------------------------------------------------------------
        {
            u32 aoID = 0;
            if (pipeline.SceneCompositePasses.SSAO && data.PostProcess.SSAOEnabled &&
                data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO)
            {
                aoID = pipeline.SceneCompositePasses.SSAO->GetSSAOTextureID();
            }
            else if (pipeline.SceneCompositePasses.GTAO && data.PostProcess.GTAOEnabled &&
                     data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO)
            {
                aoID = pipeline.SceneCompositePasses.GTAO->GetGTAOTextureID();
            }
            if (aoID != 0)
            {
                board.AOBuffer = graph.ImportTexture(
                    ResourceNames::AOBuffer, aoID,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::AOBuffer));
            }

            static u32 s_PrevAOID = 0;
            static i32 s_PrevAOTechnique = -1;
            static bool s_PrevSSAOEnabled = false;
            static bool s_PrevGTAOEnabled = false;
            const i32 activeTechnique = static_cast<i32>(data.PostProcess.ActiveAOTechnique);
            if (aoID != s_PrevAOID ||
                activeTechnique != s_PrevAOTechnique ||
                data.PostProcess.SSAOEnabled != s_PrevSSAOEnabled ||
                data.PostProcess.GTAOEnabled != s_PrevGTAOEnabled)
            {
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_TRACE("Renderer3D: AO import state: technique={}, ssaoEnabled={}, gtaoEnabled={}, aoTexID={}, aoHandleValid={}",
                                   activeTechnique,
                                   data.PostProcess.SSAOEnabled,
                                   data.PostProcess.GTAOEnabled,
                                   aoID,
                                   board.AOBuffer.IsValid());
                }
                s_PrevAOID = aoID;
                s_PrevAOTechnique = activeTechnique;
                s_PrevSSAOEnabled = data.PostProcess.SSAOEnabled;
                s_PrevGTAOEnabled = data.PostProcess.GTAOEnabled;
            }
        }

        // ------------------------------------------------------------------
        // Shadow maps
        // ------------------------------------------------------------------
        {
            const u32 csmID = data.Shadow.GetCSMRendererID();
            const u32 spotID = data.Shadow.GetSpotRendererID();
            if (csmID != 0)
            {
                board.ShadowMapCSM = graph.ImportTexture(
                    ResourceNames::ShadowMapCSM, csmID,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, ResourceNames::ShadowMapCSM));
            }
            if (spotID != 0)
            {
                board.ShadowMapSpot = graph.ImportTexture(
                    ResourceNames::ShadowMapSpot, spotID,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, ResourceNames::ShadowMapSpot));
            }
            // Point-light shadow cubemaps — import each active light slot separately.
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                const u32 pointID = data.Shadow.GetPointRendererID(i);
                if (pointID != 0)
                {
                    board.ShadowMapPoint[i] = graph.ImportTexture(
                        ResourceNames::ShadowMapPoint[i], pointID,
                        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, ResourceNames::ShadowMapPoint[i]));
                }
            }
        }

        // ------------------------------------------------------------------
        // Post-process chain outputs
        // Full-resolution single-attachment post outputs are now declared as
        // graph-owned transient framebuffers from scene dimensions, so execute
        // paths can bind graph materialization directly instead of borrowing
        // pass-owned output framebuffers at declaration time. Internal
        // histories and scratch resources (for example TAA / fog history)
        // remain owned by the passes that manage them.
        // ------------------------------------------------------------------
        u32 postProcessWidth = 1u;
        u32 postProcessHeight = 1u;
        if (pipeline.FrameCorePasses.Scene)
        {
            const auto& sceneSpec = pipeline.FrameCorePasses.Scene->GetFramebufferSpecification();
            postProcessWidth = sceneSpec.Width > 0 ? sceneSpec.Width : 1u;
            postProcessHeight = sceneSpec.Height > 0 ? sceneSpec.Height : 1u;
        }

        auto declareGraphOnlyPostProcessFB =
            [&](std::string_view name, RGResourceFormat fmt) -> RGFramebufferHandle
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Width = postProcessWidth;
            desc.Height = postProcessHeight;
            desc.Format = fmt;
            desc.DebugName = std::string(name);
            return graph.DeclareTransientFramebuffer(name, desc);
        };

        if (pipeline.PostProcessPasses.SSS &&
            data.Snow.Enabled &&
            data.Snow.SSSBlurEnabled &&
            pipeline.PostProcessPasses.SSS->IsReadyForExecution())
        {
            board.SSSColor = declareGraphOnlyPostProcessFB(ResourceNames::SSSColor, RGResourceFormat::RGBA16Float);
        }

        // AOApplyColor exists only when AO apply is actually executable for
        // this frame: the pass is enabled, the AO producer imported a valid
        // AOBuffer, and SceneDepth is available for bilateral upsampling.
        if (pipeline.PostProcessPasses.AOApply)
        {
            if (pipeline.PostProcessPasses.AOApply->IsEnabled() &&
                pipeline.PostProcessPasses.AOApply->IsReadyForExecution() &&
                board.AOBuffer.IsValid() &&
                board.SceneDepth.IsValid())
            {
                board.AOApplyColor = declareGraphOnlyPostProcessFB(ResourceNames::AOApplyColor, RGResourceFormat::RGBA16Float);
            }
        }

        // PostProcessColor is an alias handle to the latest upstream graph
        // resource in the dynamic chain, NOT a separate imported resource.
        // This preserves declaration-derived reachability:
        //   AOApplyColor -> Bloom, SSSColor -> Bloom, or SceneColor -> Bloom.
        // Importing a fresh `PostProcessColor` framebuffer here severs that
        // producer/consumer chain, which lets AO/SSS get culled and can feed
        // stale/black data into the post stack.
        if (board.AOApplyColor.IsValid())
            board.PostProcessColor = board.AOApplyColor;
        else if (board.SSSColor.IsValid())
            board.PostProcessColor = board.SSSColor;
        else
            board.PostProcessColor = board.SceneColor;

        // BloomColor exists only when bloom is executable for this frame and
        // the scene dimensions are large enough for the graph-owned mip chain
        // the pass consumes internally.
        if (pipeline.PostProcessPasses.Bloom)
        {
            auto computeBloomMipCount = [postProcessWidth, postProcessHeight]() -> u32
            {
                u32 mipCount = 0u;
                u32 mipW = postProcessWidth / 2u;
                u32 mipH = postProcessHeight / 2u;
                for (u32 i = 0; i < 5u; ++i)
                {
                    if (mipW < 2u || mipH < 2u)
                        break;
                    ++mipCount;
                    mipW /= 2u;
                    mipH /= 2u;
                }
                return mipCount;
            };

            if (pipeline.PostProcessPasses.Bloom->IsEnabled() &&
                pipeline.PostProcessPasses.Bloom->IsReadyForExecution() &&
                computeBloomMipCount() > 0u)
            {
                board.BloomColor = declareGraphOnlyPostProcessFB(ResourceNames::BloomColor, RGResourceFormat::RGBA16Float);
            }
        }

        // DOFColor is declared only when DOF is enabled.
        if (pipeline.PostProcessPasses.DOF && data.PostProcess.DOFEnabled)
            board.DOFColor = declareGraphOnlyPostProcessFB(ResourceNames::DOFColor, RGResourceFormat::RGBA16Float);

        // MotionBlurColor is declared only when motion blur is enabled.
        if (pipeline.PostProcessPasses.MotionBlur && data.PostProcess.MotionBlurEnabled)
            board.MotionBlurColor = declareGraphOnlyPostProcessFB(ResourceNames::MotionBlurColor, RGResourceFormat::RGBA16Float);

        // TAAColor is declared only when TAA is enabled.
        if (pipeline.PostProcessPasses.TAA && data.PostProcess.TAAEnabled)
            board.TAAColor = declareGraphOnlyPostProcessFB(ResourceNames::TAAColor, RGResourceFormat::RGBA16Float);

        // PrecipitationColor is declared only when screen FX are active.
        const bool precipScreenEnabled = data.Precipitation.Enabled &&
                                         (data.Precipitation.ScreenStreaksEnabled ||
                                          data.Precipitation.LensImpactsEnabled);
        if (pipeline.PostProcessPasses.Precipitation && precipScreenEnabled)
            board.PrecipitationColor = declareGraphOnlyPostProcessFB(ResourceNames::PrecipitationColor, RGResourceFormat::RGBA16Float);

        // FogColor is declared only when fog is enabled.
        if (pipeline.PostProcessPasses.Fog && data.Fog.Enabled)
            board.FogColor = declareGraphOnlyPostProcessFB(ResourceNames::FogColor, RGResourceFormat::RGBA16Float);

        // Extracted effect sub-chain. Each handle is
        // declared only when its effect is enabled so downstream consumers
        // can rely on IsValid() as the canonical "effect ran" signal.
        // ToneMap is declared unconditionally (no settings gate).
        if (pipeline.PostProcessPasses.ChromAberration && data.PostProcess.ChromaticAberrationEnabled)
            board.ChromAbColor = declareGraphOnlyPostProcessFB(ResourceNames::ChromAbColor, RGResourceFormat::RGBA16Float);
        if (pipeline.PostProcessPasses.ColorGrading && data.PostProcess.ColorGradingEnabled)
            board.ColorGradingColor = declareGraphOnlyPostProcessFB(ResourceNames::ColorGradingColor, RGResourceFormat::RGBA16Float);
        if (pipeline.PostProcessPasses.ToneMap)
            board.ToneMapColor = declareGraphOnlyPostProcessFB(ResourceNames::ToneMapColor, RGResourceFormat::RGBA16Float);
        if (pipeline.PostProcessPasses.Vignette && data.PostProcess.VignetteEnabled)
            board.VignetteColor = declareGraphOnlyPostProcessFB(ResourceNames::VignetteColor, RGResourceFormat::RGBA8UNorm);

        // Only declare FXAAColor when FXAA is active so
        // downstream consumers can rely on `board.FXAAColor.IsValid()` as
        // the canonical "anti-aliased post-process available" signal.
        if (pipeline.PostProcessPasses.FXAA && data.PostProcess.FXAAEnabled)
            board.FXAAColor = declareGraphOnlyPostProcessFB(ResourceNames::FXAAColor, RGResourceFormat::RGBA8UNorm);

        if (pipeline.PostProcessPasses.SelectionOutline &&
            pipeline.PostProcessPasses.SelectionOutline->IsEnabled() &&
            pipeline.PostProcessPasses.SelectionOutline->IsReadyForExecution())
            board.SelectionOutlineColor = declareGraphOnlyPostProcessFB(ResourceNames::SelectionOutlineColor, RGResourceFormat::RGBA8UNorm);

        if (pipeline.PostProcessPasses.UIComposite)
        {
            RGResourceDesc uiCompositeDesc;
            uiCompositeDesc.Kind = ResourceHandle::Kind::Framebuffer;
            uiCompositeDesc.Width = postProcessWidth;
            uiCompositeDesc.Height = postProcessHeight;
            uiCompositeDesc.Attachments = { RGResourceFormat::RGBA8UNorm, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
            uiCompositeDesc.DebugName = std::string(ResourceNames::UIComposite);
            board.UIComposite = graph.DeclareTransientFramebuffer(ResourceNames::UIComposite, uiCompositeDesc);
        }

        // Default framebuffer / swapchain target represented as an imported
        // external output resource. Backing framebuffer is null by design;
        // FinalPass presents via RGCommandContext::BindDefaultFramebuffer().
        board.Backbuffer = graph.ImportFramebuffer(
            ResourceNames::Backbuffer, nullptr,
            RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, ResourceNames::Backbuffer));

        // ------------------------------------------------------------------
        // OIT buffers
        // ------------------------------------------------------------------
        // OIT graph resources are declared only when weighted-blended OIT is
        // *actually* active for this frame. Skipping the declaration when OIT
        // is disabled means transparent contributor
        // passes (Particle / Water / Decal) bail out of their
        // `builder.Write(board.OITAccum, ...)` declarations
        // (`if (board.OITAccum.IsValid())` is already guarded), so the
        // graph never sees write edges into a buffer that nothing reads.
        // OITPreparePass and OITResolvePass also self-skip via `m_Enabled`.
        const bool oitActive = (data.Settings.Path == RenderingPath::Deferred) &&
                               data.Settings.Deferred.OITEnabled &&
                               pipeline.SceneCompositePasses.OITResolve;
        if (oitActive)
        {
            // Declare as a shared transient MRT framebuffer (RT0 = RGBA16F
            // accumulation, RT1 = RG16F revealage, depth = DEPTH24_STENCIL8).
            // Both blackboard handles point to the same physical transient FB;
            // passes distinguish the two colour attachments by index (0 and 1).
            RGResourceDesc oitDesc;
            oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
            oitDesc.Width = postProcessWidth;
            oitDesc.Height = postProcessHeight;
            oitDesc.Attachments = {
                RGResourceFormat::RGBA16Float,
                RGResourceFormat::RG16Float,
                RGResourceFormat::Depth24Stencil8
            };
            oitDesc.DebugName = std::string(ResourceNames::OITBuffer);

            const auto oitHandle = graph.DeclareTransientFramebuffer(ResourceNames::OITBuffer, oitDesc);
            board.OITAccum = oitHandle;
            board.OITRevealage = oitHandle;
        }

        // ------------------------------------------------------------------
        // Temporal histories (imported from prior frame)
        // ------------------------------------------------------------------
        // TAAHistory is owned by TAARenderPass.
        if (pipeline.PostProcessPasses.TAA)
        {
            board.TAAHistory = graph.ImportHistory(
                ResourceNames::TAAHistory, pipeline.PostProcessPasses.TAA->GetTAAHistoryTextureID());
        }

        // FogHistory is owned by FogRenderPass.
        if (pipeline.PostProcessPasses.Fog)
        {
            board.FogHistory = graph.ImportHistory(
                ResourceNames::FogHistory, pipeline.PostProcessPasses.Fog->GetFogHistoryTextureID());
        }

        // ------------------------------------------------------------------
        // IBL resources
        // ------------------------------------------------------------------
        if (data.GlobalIrradianceMapID != 0)
        {
            board.IrradianceMap = graph.ImportTexture(
                ResourceNames::IrradianceMap, data.GlobalIrradianceMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, ResourceNames::IrradianceMap));
        }
        if (data.GlobalPrefilterMapID != 0)
        {
            board.PrefilterMap = graph.ImportTexture(
                ResourceNames::PrefilterMap, data.GlobalPrefilterMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, ResourceNames::PrefilterMap));
        }
        if (data.GlobalBRDFLutMapID != 0)
        {
            board.BrdfLut = graph.ImportTexture(
                ResourceNames::BrdfLut, data.GlobalBRDFLutMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::BrdfLut));
        }
    }

    void Renderer3D::RenderPipeline::RefreshBlackboardHandles(Renderer3DData& data)
    {
        auto& board = data.RGraph->GetBlackboard();
        board.SSAORaw = data.RGraph->GetFramebufferHandle("SSAORaw");
        // Phase D Slice 2: JFA ping-pong scratch framebuffers for SelectionOutlinePass.
        board.JFAPing = data.RGraph->GetFramebufferHandle("JFAPing");
        board.JFAPong = data.RGraph->GetFramebufferHandle("JFAPong");
        // Refresh imported core handles as well. Handle generations can advance
        // when the resource registry is rebuilt; using stale generations causes
        // ResolveFramebuffer/ResolveTexture to fail and leaves downstream passes
        // rendering into/reading from uninitialized outputs.
        board.SceneColor = data.RGraph->GetFramebufferHandle(ResourceNames::SceneColor);
        board.SceneDepth = data.RGraph->GetTextureHandle(ResourceNames::SceneDepth);
        board.SceneNormals = data.RGraph->GetTextureHandle(ResourceNames::SceneNormals);
        board.Velocity = data.RGraph->GetTextureHandle(ResourceNames::Velocity);
        board.AOBuffer = data.RGraph->GetTextureHandle(ResourceNames::AOBuffer);
        board.OITAccum = data.RGraph->GetFramebufferHandle(ResourceNames::OITBuffer);
        board.OITRevealage = board.OITAccum;
        // Refresh post-process transient framebuffer handles after BuildFrameGraph.
        // BuildFrameGraph finalizes per-frame resource generations; using handles
        // captured before build can resolve stale framebuffer slots (e.g. ping-pong
        // IDs), which manifests as black/transparent post chain outputs.
        board.SSSColor = data.RGraph->GetFramebufferHandle(ResourceNames::SSSColor);
        board.AOApplyColor = data.RGraph->GetFramebufferHandle(ResourceNames::AOApplyColor);
        board.BloomColor = data.RGraph->GetFramebufferHandle(ResourceNames::BloomColor);
        board.DOFColor = data.RGraph->GetFramebufferHandle(ResourceNames::DOFColor);
        board.MotionBlurColor = data.RGraph->GetFramebufferHandle(ResourceNames::MotionBlurColor);
        board.TAAColor = data.RGraph->GetFramebufferHandle(ResourceNames::TAAColor);
        board.PrecipitationColor = data.RGraph->GetFramebufferHandle(ResourceNames::PrecipitationColor);
        board.FogColor = data.RGraph->GetFramebufferHandle(ResourceNames::FogColor);
        board.FogHalfRes = data.RGraph->GetFramebufferHandle(ResourceNames::FogHalfRes);
        board.ChromAbColor = data.RGraph->GetFramebufferHandle(ResourceNames::ChromAbColor);
        board.ColorGradingColor = data.RGraph->GetFramebufferHandle(ResourceNames::ColorGradingColor);
        board.ToneMapColor = data.RGraph->GetFramebufferHandle(ResourceNames::ToneMapColor);
        board.VignetteColor = data.RGraph->GetFramebufferHandle(ResourceNames::VignetteColor);
        board.FXAAColor = data.RGraph->GetFramebufferHandle(ResourceNames::FXAAColor);
        board.SelectionOutlineColor = data.RGraph->GetFramebufferHandle(ResourceNames::SelectionOutlineColor);
        board.UIComposite = data.RGraph->GetFramebufferHandle(ResourceNames::UIComposite);
        board.TAAHistory = data.RGraph->GetTextureHandle(ResourceNames::TAAHistory);
        board.FogHistory = data.RGraph->GetTextureHandle(ResourceNames::FogHistory);

        // Rebuild dynamic post-chain alias from refreshed handles.
        if (board.AOApplyColor.IsValid())
            board.PostProcessColor = board.AOApplyColor;
        else if (board.SSSColor.IsValid())
            board.PostProcessColor = board.SSSColor;
        else
            board.PostProcessColor = board.SceneColor;

        // Phase D Slice 3: Bloom mip-chain scratch framebuffers.
        for (u32 i = 0; i < 5u; ++i)
        {
            const std::string mipName = "BloomMip" + std::to_string(i);
            board.BloomMips[i] = data.RGraph->GetFramebufferHandle(mipName);
        }
        // Phase D Slice 4: GTAO edge scratch texture.
        board.GTAOEdge = data.RGraph->GetTextureHandle("GTAOEdge");
        // Phase D Slice 6: HZB depth pyramid scratch texture.
        board.HZBDepth = data.RGraph->GetTextureHandle(ResourceNames::HZBDepth);
        // Phase D Slice 5: Water refraction scratch texture.
        board.WaterRefraction = data.RGraph->GetTextureHandle("WaterRefraction");
    }

    auto Renderer3D::RenderPipeline::BuildInputs(Renderer3DData& data) -> RenderPipelineInputs
    {
        RenderPipelineInputs inputs{};
        inputs.Graph = data.RGraph.Raw();

        inputs.Nodes.Geometry = StreamNodes.Geometry.Raw();
        inputs.Nodes.ForwardOverlay = StreamNodes.ForwardOverlay.Raw();
        inputs.Nodes.Foliage = StreamNodes.Foliage.Raw();
        inputs.Nodes.Decal = StreamNodes.Decal.Raw();
        inputs.Nodes.Water = StreamNodes.Water.Raw();

        inputs.Passes.Scene = FrameCorePasses.Scene.Raw();
        inputs.Passes.Shadow = FrameCorePasses.Shadow.Raw();
        inputs.Passes.DeferredLighting = SceneCompositePasses.DeferredLighting.Raw();
        inputs.Passes.DeferredOpaqueDecal = SceneCompositePasses.DeferredOpaqueDecal.Raw();
        inputs.Passes.Water = RenderStreamPasses.Water.Raw();
        inputs.Passes.Decal = RenderStreamPasses.Decal.Raw();
        inputs.Passes.SSAO = SceneCompositePasses.SSAO.Raw();
        inputs.Passes.GTAO = SceneCompositePasses.GTAO.Raw();
        inputs.Passes.Particle = SceneCompositePasses.Particle.Raw();
        inputs.Passes.OITPrepare = SceneCompositePasses.OITPrepare.Raw();
        inputs.Passes.OITResolve = SceneCompositePasses.OITResolve.Raw();
        inputs.Passes.SSS = PostProcessPasses.SSS.Raw();
        inputs.Passes.AOApply = PostProcessPasses.AOApply.Raw();
        inputs.Passes.Bloom = PostProcessPasses.Bloom.Raw();
        inputs.Passes.DOF = PostProcessPasses.DOF.Raw();
        inputs.Passes.MotionBlur = PostProcessPasses.MotionBlur.Raw();
        inputs.Passes.TAA = PostProcessPasses.TAA.Raw();
        inputs.Passes.Precipitation = PostProcessPasses.Precipitation.Raw();
        inputs.Passes.Fog = PostProcessPasses.Fog.Raw();
        inputs.Passes.ChromAberration = PostProcessPasses.ChromAberration.Raw();
        inputs.Passes.ColorGrading = PostProcessPasses.ColorGrading.Raw();
        inputs.Passes.ToneMap = PostProcessPasses.ToneMap.Raw();
        inputs.Passes.Vignette = PostProcessPasses.Vignette.Raw();
        inputs.Passes.FXAA = PostProcessPasses.FXAA.Raw();
        inputs.Passes.SelectionOutline = PostProcessPasses.SelectionOutline.Raw();
        inputs.Passes.UIComposite = PostProcessPasses.UIComposite.Raw();
        inputs.Passes.Final = PostProcessPasses.Final.Raw();

        inputs.Runtime.Renderer = &data.Settings;
        inputs.Runtime.PostProcess = &data.PostProcess;
        inputs.Runtime.Snow = &data.Snow;
        inputs.Runtime.Fog = &data.Fog;
        inputs.Runtime.Precipitation = &data.Precipitation;

        return inputs;
    }

    void Renderer3D::RenderPipeline::CreateFramePasses(Renderer3DData& data,
                                                       ShaderLibrary& shaderLibrary,
                                                       const FramebufferSpecification& shadowPassSpec,
                                                       const FramebufferSpecification& scenePassSpec,
                                                       const FramebufferSpecification& finalPassSpec)
    {
        // Shadow pass (renders before scene, doesn't need scene framebuffer dimensions)
        FrameCorePasses.Shadow = Ref<ShadowRenderPass>::Create();
        FrameCorePasses.Shadow->SetName("ShadowPass");
        FrameCorePasses.Shadow->Init(shadowPassSpec);
        FrameCorePasses.Shadow->SetShadowMap(&data.Shadow);

        FrameCorePasses.Scene = Ref<SceneRenderPass>::Create();
        FrameCorePasses.Scene->SetName("ScenePass");
        FrameCorePasses.Scene->Init(scenePassSpec);

        // Deferred lighting composition — no-op when Settings.Path is
        // Forward / Forward+ (no G-Buffer supplied). Writes into
        // ScenePass's colour[0] so downstream passes stay path-agnostic.
        SceneCompositePasses.DeferredLighting = Ref<DeferredLightingPass>::Create();
        SceneCompositePasses.DeferredLighting->SetName("DeferredLightingPass");
        SceneCompositePasses.DeferredLighting->Init(scenePassSpec);

        // Graph-scheduled opaque-decal shim. Pulls the decal bucket into
        // the G-Buffer between ScenePass and DeferredLightingPass (was
        // previously a synchronous call inside SceneRenderPass::Execute,
        // now a proper graph node with declared resource edges).
        SceneCompositePasses.DeferredOpaqueDecal = Ref<DeferredOpaqueDecalPass>::Create();
        SceneCompositePasses.DeferredOpaqueDecal->SetName("DeferredOpaqueDecalPass");
        SceneCompositePasses.DeferredOpaqueDecal->Init(scenePassSpec);

        // Forward overlay pass — runs after DeferredLightingPass in Deferred
        // mode to render skybox / terrain / voxel terrain / infinite grid /
        // light-cube geometry that cannot participate in the G-Buffer MRT
        // write. No-ops in Forward / Forward+.
        RenderStreamPasses.ForwardOverlay = Ref<ForwardOverlayRenderPass>::Create();
        RenderStreamPasses.ForwardOverlay->SetName("ForwardOverlayPass");
        RenderStreamPasses.ForwardOverlay->Init(finalPassSpec);

        SceneCompositePasses.Particle = Ref<ParticleRenderPass>::Create();
        SceneCompositePasses.Particle->SetName("ParticlePass");
        SceneCompositePasses.Particle->Init(finalPassSpec);

        RenderStreamPasses.Foliage = Ref<FoliageRenderPass>::Create();
        RenderStreamPasses.Foliage->SetName("FoliagePass");
        RenderStreamPasses.Foliage->Init(finalPassSpec);

        RenderStreamPasses.Water = Ref<WaterRenderPass>::Create();
        RenderStreamPasses.Water->SetName("WaterPass");
        RenderStreamPasses.Water->Init(finalPassSpec);

        RenderStreamPasses.Decal = Ref<DecalRenderPass>::Create();
        RenderStreamPasses.Decal->SetName("DecalPass");
        RenderStreamPasses.Decal->Init(finalPassSpec);
        RenderStreamPasses.Decal->SetOITShader(shaderLibrary.Get("Decal_OIT"));

        SceneCompositePasses.SSAO = Ref<SSAORenderPass>::Create();
        SceneCompositePasses.SSAO->SetName("SSAOPass");
        SceneCompositePasses.SSAO->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        SceneCompositePasses.SSAO->SetSSAOUBO(data.PostProcessGPU.SSAO, &data.PostProcessGPU.SSAOData);

        SceneCompositePasses.GTAO = Ref<GTAORenderPass>::Create();
        SceneCompositePasses.GTAO->SetName("GTAOPass");
        SceneCompositePasses.GTAO->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        SceneCompositePasses.GTAO->SetGTAOUBO(data.PostProcessGPU.GTAO, &data.PostProcessGPU.GTAOData);

        SceneCompositePasses.OITPrepare = Ref<OITPrepareRenderPass>::Create();
        SceneCompositePasses.OITPrepare->SetName("OITPreparePass");
        SceneCompositePasses.OITPrepare->Init(finalPassSpec);

        PostProcessPasses.SSS = Ref<SSSRenderPass>::Create();
        PostProcessPasses.SSS->SetName("SSSPass");
        PostProcessPasses.SSS->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
        PostProcessPasses.SSS->SetSSSUBO(data.SceneEffectsGPU.SSS, &data.SceneEffectsGPU.SSSData);

        // OIT resolve pass. Composites weighted-blended transparent
        // accumulation (produced by ParticlePass when OITEnabled) over the
        // scene FB, then acts as a passthrough for downstream piping.
        SceneCompositePasses.OITResolve = Ref<OITResolveRenderPass>::Create();
        SceneCompositePasses.OITResolve->SetName("OITResolvePass");
        SceneCompositePasses.OITResolve->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene().
    }

    void Renderer3D::RenderPipeline::CreateRenderStreamNodes(Renderer3DData& data)
    {
        auto* const rendererData = &data;
        auto* const pipeline = this;

        StreamNodes.Geometry = FrameCorePasses.Scene.As<CommandBufferRenderPass>();
        OLO_CORE_ASSERT(StreamNodes.Geometry, "RenderPipeline::CreateRenderStreamNodes requires ScenePass to be bucket-backed");
        FrameCorePasses.Scene->SetSetupCallback(
            [rendererData](RGBuilder& builder, FrameBlackboard& board)
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

                if (rendererData->Settings.Path == RenderingPath::Deferred)
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
            });

        StreamNodes.ForwardOverlay = RenderStreamPasses.ForwardOverlay.As<CommandBufferRenderPass>();
        OLO_CORE_ASSERT(StreamNodes.ForwardOverlay, "RenderPipeline::CreateRenderStreamNodes requires ForwardOverlayPass to be bucket-backed");
        RenderStreamPasses.ForwardOverlay->SetSetupCallback(
            [rendererData, pipeline](RGBuilder& builder, FrameBlackboard& board)
            {
                if (rendererData->Settings.Path != RenderingPath::Deferred)
                    return;
                if (!pipeline->RenderStreamPasses.ForwardOverlay ||
                    pipeline->RenderStreamPasses.ForwardOverlay->GetCommandBucket().GetCommandCount() == 0)
                    return;

                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            });

        StreamNodes.Foliage = RenderStreamPasses.Foliage.As<CommandBufferRenderPass>();
        OLO_CORE_ASSERT(StreamNodes.Foliage, "RenderPipeline::CreateRenderStreamNodes requires FoliagePass to be bucket-backed");
        RenderStreamPasses.Foliage->SetSetupCallback(
            [pipeline](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!pipeline->RenderStreamPasses.Foliage ||
                    pipeline->RenderStreamPasses.Foliage->GetCommandBucket().GetCommandCount() == 0)
                    return;

                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            });

        StreamNodes.Water = RenderStreamPasses.Water.As<CommandBufferRenderPass>();
        OLO_CORE_ASSERT(StreamNodes.Water, "RenderPipeline::CreateRenderStreamNodes requires WaterPass to be bucket-backed");
        RenderStreamPasses.Water->SetSetupCallback(
            [pipeline](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!pipeline->RenderStreamPasses.Water ||
                    pipeline->RenderStreamPasses.Water->GetCommandBucket().GetCommandCount() == 0)
                    return;

                if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }

                if (const auto sceneTarget = pipeline->FrameCorePasses.Scene ? pipeline->FrameCorePasses.Scene->GetTarget() : nullptr;
                    sceneTarget)
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
            });

        StreamNodes.Decal = RenderStreamPasses.Decal.As<CommandBufferRenderPass>();
        OLO_CORE_ASSERT(StreamNodes.Decal, "RenderPipeline::CreateRenderStreamNodes requires DecalPass to be bucket-backed");
        RenderStreamPasses.Decal->SetSetupCallback(
            [rendererData, pipeline](RGBuilder& builder, FrameBlackboard& board)
            {
                if (!pipeline->RenderStreamPasses.Decal ||
                    pipeline->RenderStreamPasses.Decal->GetCommandBucket().GetCommandCount() == 0)
                    return;

                const bool oitEnabled = (rendererData->Settings.Path == RenderingPath::Deferred) &&
                                        rendererData->Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                    {
                        [[maybe_unused]] const auto oitAccumRead = builder.Read(board.OITAccum, RGReadUsage::RenderTargetRead);
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    }
                    if (board.OITRevealage.IsValid())
                    {
                        [[maybe_unused]] const auto oitRevealageRead = builder.Read(board.OITRevealage, RGReadUsage::RenderTargetRead);
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                    }
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            });
    }

    void Renderer3D::RenderPipeline::CreatePostProcessPasses(const FramebufferSpecification& finalPassSpec)
    {
        // AO apply standalone post stage.
        // Sits between SSSPass (or SceneColor) and BloomPass in dynamic mode.
        PostProcessPasses.AOApply = Ref<AOApplyRenderPass>::Create();
        PostProcessPasses.AOApply->SetName("AOApplyPass");
        PostProcessPasses.AOApply->Init(finalPassSpec);

        // Bloom standalone pass.
        // Sits between PostProcess and DOF.
        PostProcessPasses.Bloom = Ref<BloomRenderPass>::Create();
        PostProcessPasses.Bloom->SetName("BloomPass");
        PostProcessPasses.Bloom->Init(finalPassSpec);

        // DOF standalone pass.
        // Sits between Bloom and MotionBlur.
        PostProcessPasses.DOF = Ref<DOFRenderPass>::Create();
        PostProcessPasses.DOF->SetName("DOFPass");
        PostProcessPasses.DOF->Init(finalPassSpec);

        // MotionBlur standalone pass.
        // Sits between DOF and TAA.
        PostProcessPasses.MotionBlur = Ref<MotionBlurRenderPass>::Create();
        PostProcessPasses.MotionBlur->SetName("MotionBlurPass");
        PostProcessPasses.MotionBlur->Init(finalPassSpec);

        // TAA standalone pass.
        // Sits between PostProcess and Fog.
        PostProcessPasses.TAA = Ref<TAARenderPass>::Create();
        PostProcessPasses.TAA->SetName("TAAPass");
        PostProcessPasses.TAA->Init(finalPassSpec);

        // Screen-space precipitation standalone pass.
        // Sits between TAA and Fog.
        PostProcessPasses.Precipitation = Ref<PrecipitationRenderPass>::Create();
        PostProcessPasses.Precipitation->SetName("PrecipitationPass");
        PostProcessPasses.Precipitation->Init(finalPassSpec);

        // Volumetric fog standalone pass.
        // Sits between Precipitation and the late post-effect sub-chain.
        PostProcessPasses.Fog = Ref<FogRenderPass>::Create();
        PostProcessPasses.Fog->SetName("FogPass");
        PostProcessPasses.Fog->Init(finalPassSpec);

        // Four standalone effects in
        // in chain order. Each pass self-skips when its effect is disabled;
        // the graph topology stays constant regardless of settings.
        PostProcessPasses.ChromAberration = Ref<ChromaticAberrationRenderPass>::Create();
        PostProcessPasses.ChromAberration->SetName("ChromAberrationPass");
        PostProcessPasses.ChromAberration->Init(finalPassSpec);

        PostProcessPasses.ColorGrading = Ref<ColorGradingRenderPass>::Create();
        PostProcessPasses.ColorGrading->SetName("ColorGradingPass");
        PostProcessPasses.ColorGrading->Init(finalPassSpec);

        PostProcessPasses.ToneMap = Ref<ToneMapRenderPass>::Create();
        PostProcessPasses.ToneMap->SetName("ToneMapPass");
        PostProcessPasses.ToneMap->Init(finalPassSpec);

        PostProcessPasses.Vignette = Ref<VignetteRenderPass>::Create();
        PostProcessPasses.Vignette->SetName("VignettePass");
        PostProcessPasses.Vignette->Init(finalPassSpec);

        // FXAA extracted into its own graph pass.
        // Always created so the graph topology can stay constant; the
        // pass self-skips when `Settings.FXAAEnabled` is false and the
        // blackboard import is gated on the same flag.
        PostProcessPasses.FXAA = Ref<FXAARenderPass>::Create();
        PostProcessPasses.FXAA->SetName("FXAAPass");
        PostProcessPasses.FXAA->Init(finalPassSpec);

        // SelectionOutline is always created so the post/UI topology stays
        // stable. Per-frame blackboard declarations decide whether its graph
        // resources materialize, and ConfigurePassesForFrame drives
        // the runtime enabled flag + selected entity IDs.
        PostProcessPasses.SelectionOutline = Ref<SelectionOutlineRenderPass>::Create();
        PostProcessPasses.SelectionOutline->SetName("SelectionOutlinePass");
        PostProcessPasses.SelectionOutline->Init(finalPassSpec);

        PostProcessPasses.UIComposite = Ref<UICompositeRenderPass>::Create();
        PostProcessPasses.UIComposite->SetName("UICompositePass");
        PostProcessPasses.UIComposite->Init(finalPassSpec);

        PostProcessPasses.Final = Ref<FinalRenderPass>::Create();
        PostProcessPasses.Final->SetName("FinalPass");
        PostProcessPasses.Final->Init(finalPassSpec);
    }
} // namespace OloEngine
