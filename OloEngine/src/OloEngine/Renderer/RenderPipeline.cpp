#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GBuffer.h"
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
        constexpr ImageFormat kTemporalHistoryFormat = ImageFormat::RGBA16F;

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

        void ResetHistoryStorage(Ref<Texture2D>& historyTexture, bool& historyValid)
        {
            historyTexture.Reset();
            historyValid = false;
        }

        void EnsureHistoryStorage(Ref<Texture2D>& historyTexture,
                                  bool& historyValid,
                                  const u32 width,
                                  const u32 height)
        {
            if (width == 0 || height == 0)
            {
                ResetHistoryStorage(historyTexture, historyValid);
                return;
            }

            TextureSpecification historySpec;
            historySpec.Width = width;
            historySpec.Height = height;
            historySpec.Format = kTemporalHistoryFormat;
            historySpec.GenerateMips = false;
            historySpec.MipLevels = 1u;

            if (!historyTexture)
            {
                historyTexture = Texture2D::Create(historySpec);
                historyValid = false;
                return;
            }

            if (const auto& currentSpec = historyTexture->GetSpecification(); currentSpec.Format != kTemporalHistoryFormat)
            {
                historyTexture = Texture2D::Create(historySpec);
                historyValid = false;
                return;
            }

            if (historyTexture->GetWidth() != width || historyTexture->GetHeight() != height)
            {
                historyTexture->Resize(width, height);
                historyValid = false;
            }
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

        // GPU frustum-cull pool reset — slot cursor recycles from 0 each
        // frame. Buffers stay allocated (lifetime = engine, not frame) so
        // steady-state scatter scenes don't re-allocate on every frame.
        if (data.GPUFrustumCuller)
            data.GPUFrustumCuller->BeginFrame();

        // Get main-thread allocator for this frame (already reset by BeginFrame).
        CommandAllocator* frameAllocator = FrameResourceManager::Get().GetMainAllocator();
        const auto setRenderStreamAllocator = [frameAllocator](CommandBufferRenderPass* node)
        {
            if (node)
                node->SetCommandAllocator(frameAllocator);
        };
        ForEachRenderStreamNode(setRenderStreamAllocator);

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
        ForEachRenderStreamNode(resetRenderStreamBucket);

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
        // The production pass layer no longer needs per-frame blackboard
        // handle setter plumbing for canonical scene/post resources.
        // Setup() selects and stores the relevant graph handles on each pass,
        // so Execute() resolves that setup-owned state directly instead of
        // repeating blackboard lookup ladders here.
        {
            // DeferredLightingPass follows the same pattern: setup selects the
            // canonical G-buffer/depth/velocity family and scene target, while
            // this frame hook only pushes dynamic settings like debug channel
            // and per-sample lighting.
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
            // SSAOPass resolves its setup-selected depth/normal handles at
            // execution time; only technique settings and UBO contents vary here.

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
            // GTAOPass likewise keeps its canonical graph handles in setup-owned
            // state; this hook only updates dynamic technique parameters.

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
            // AO texture selection is setup-owned too; the per-frame hook only
            // updates the enable state and bound UBOs.
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
            PostProcessPasses.MotionBlur->SetPostProcessUBO(data.PostProcessGPU.PostProcess);
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
            // OIT is path-agnostic — the shaders work in Forward, Forward+,
            // and Deferred. Previously locked to Deferred which prevented
            // enabling OIT at all from the Forward UI; relaxed so the toggle
            // is the single source of truth.
            const bool oitEnabled = data.Settings.OITEnabled;
            const bool hasOITContributors =
                (SceneCompositePasses.Particle && SceneCompositePasses.Particle->HasRenderCallback()) ||
                (RenderStreamPasses.Decal && RenderStreamPasses.Decal->GetCommandBucket().GetCommandCount() > 0);
            if (SceneCompositePasses.Particle)
                SceneCompositePasses.Particle->SetOITEnabled(oitEnabled);
            if (RenderStreamPasses.Decal)
                RenderStreamPasses.Decal->SetOITEnabled(oitEnabled);
            if (SceneCompositePasses.OITPrepare)
            {
                SceneCompositePasses.OITPrepare->SetEnabled(oitEnabled);
                SceneCompositePasses.OITPrepare->SetHasContributors(hasOITContributors);
            }
            if (SceneCompositePasses.OITResolve)
            {
                SceneCompositePasses.OITResolve->SetEnabled(oitEnabled);
                SceneCompositePasses.OITResolve->SetHasContributors(hasOITContributors);
            }
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

    namespace
    {
        constexpr u64 kFnv1aOffset = 0xcbf29ce484222325ull;
        constexpr u64 kFnv1aPrime = 0x100000001b3ull;

        inline void HashByte(u64& h, u8 v) noexcept
        {
            h = (h ^ v) * kFnv1aPrime;
        }
        inline void HashU32(u64& h, u32 v) noexcept
        {
            HashByte(h, static_cast<u8>(v & 0xffu));
            HashByte(h, static_cast<u8>((v >> 8) & 0xffu));
            HashByte(h, static_cast<u8>((v >> 16) & 0xffu));
            HashByte(h, static_cast<u8>((v >> 24) & 0xffu));
        }
        inline void HashBool(u64& h, bool v) noexcept
        {
            HashByte(h, v ? 1u : 0u);
        }

        template<typename PassPtr>
        inline void HashPassState(u64& h, const PassPtr& pass) noexcept
        {
            // Hash the underlying pointer so rebuilding a pass with the same
            // readiness still invalidates the cache. Per-pass enabled state
            // is captured separately via the data.PostProcess.* flags below.
            const auto addr = reinterpret_cast<uintptr_t>(pass.Raw());
            HashU32(h, static_cast<u32>(addr));
            HashU32(h, static_cast<u32>(addr >> 32u));
            if (!pass)
                return;
            // Not every pass type exposes IsReadyForExecution(); fold it in
            // when available so passes whose Setup() branches on readiness
            // (Bloom, DOF, AOApply, etc.) still invalidate the cache when the
            // flip happens.
            if constexpr (requires { pass->IsReadyForExecution(); })
                HashBool(h, pass->IsReadyForExecution());
        }
    } // anonymous namespace

    u64 Renderer3D::RenderPipeline::ComputeBlackboardFingerprint(const Renderer3DData& data) const
    {
        u64 h = kFnv1aOffset;

        // Scene framebuffer dimensions drive most transient resource sizes; a
        // resize must trigger a full repopulate.
        if (FrameCorePasses.Scene)
        {
            const auto& spec = FrameCorePasses.Scene->GetFramebufferSpecification();
            HashU32(h, spec.Width);
            HashU32(h, spec.Height);
        }
        else
        {
            HashU32(h, 0u);
            HashU32(h, 0u);
        }

        // Rendering path / deferred sub-state
        HashU32(h, static_cast<u32>(data.Settings.Path));
        HashU32(h, data.Settings.Deferred.MSAASampleCount);
        HashBool(h, data.Settings.OITEnabled);
        HashBool(h, data.Settings.Deferred.PerSampleLighting);

        // Shadow renderer IDs change when shadow textures are (re)created; the
        // blackboard imports them by raw GL ID so a change must invalidate.
        HashU32(h, data.Shadow.GetResolution());
        HashU32(h, data.Shadow.GetCSMRendererID());
        HashU32(h, data.Shadow.GetSpotRendererID());
        for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            HashU32(h, data.Shadow.GetPointRendererID(i));

        // Post-process technique selection + per-effect toggles
        HashU32(h, static_cast<u32>(data.PostProcess.ActiveAOTechnique));
        HashBool(h, data.PostProcess.SSAOEnabled);
        HashBool(h, data.PostProcess.GTAOEnabled);
        HashBool(h, data.PostProcess.BloomEnabled);
        HashBool(h, data.PostProcess.DOFEnabled);
        HashBool(h, data.PostProcess.MotionBlurEnabled);
        HashBool(h, data.PostProcess.TAAEnabled);
        HashBool(h, data.PostProcess.ChromaticAberrationEnabled);
        HashBool(h, data.PostProcess.ColorGradingEnabled);
        HashBool(h, data.PostProcess.VignetteEnabled);
        HashBool(h, data.PostProcess.FXAAEnabled);

        // Other systems that gate blackboard branches
        HashBool(h, data.Fog.Enabled);
        HashBool(h, data.Snow.Enabled);
        HashBool(h, data.Snow.SSSBlurEnabled);

        // Selection outline gate inputs — PopulateBlackboard declares
        // SelectionOutlineColor / JFAPing / JFAPong only when the editor
        // has at least one selected entity AND the toggle is on. Both
        // values must invalidate the cache or selecting an entity after
        // a frame with no selection would silently skip declaration.
        HashBool(h, data.EnableSelectionOutline);
        HashBool(h, !data.SelectionOutlineEntityIDs.empty());

        // Temporal-history gate inputs — `if (TAAHistoryValid && Texture)`
        // / `if (FogHistoryValid && Texture)` decide whether the prior
        // frame's history is imported into the blackboard for reprojection.
        // These flags flip false→true after the FIRST successful frame
        // produces history, so the cache MUST invalidate on that transition
        // — otherwise PopulateBlackboard never re-runs, the import never
        // happens, and TAA / fog reprojection sample the current frame as
        // history (TAA degenerates to a pass-through and the jitter shows
        // through as a screen-space shake).
        HashBool(h, TAAHistoryValid);
        HashBool(h, FogHistoryValid);

        // Pass-set readiness (covers branches like
        //   `if (pipeline.PostProcessPasses.X && X->IsReadyForExecution())`)
        // The same fingerprint is reused as the RenderGraph::BuildFrameGraph
        // cache key, so it must cover every pass whose Setup() declarations
        // may change between frames — not just the post-process chain.
        HashPassState(h, FrameCorePasses.Shadow);
        HashPassState(h, FrameCorePasses.Scene);
        HashPassState(h, SceneCompositePasses.DeferredLighting);
        HashPassState(h, SceneCompositePasses.DeferredOpaqueDecal);
        HashPassState(h, SceneCompositePasses.SSAO);
        HashPassState(h, SceneCompositePasses.GTAO);
        HashPassState(h, SceneCompositePasses.Particle);
        HashPassState(h, SceneCompositePasses.OITPrepare);
        HashPassState(h, SceneCompositePasses.OITResolve);
        HashPassState(h, RenderStreamPasses.ForwardOverlay);
        HashPassState(h, RenderStreamPasses.Foliage);
        HashPassState(h, RenderStreamPasses.Water);
        HashPassState(h, RenderStreamPasses.Decal);
        HashPassState(h, PostProcessPasses.SSS);
        HashPassState(h, PostProcessPasses.AOApply);
        HashPassState(h, PostProcessPasses.Bloom);
        HashPassState(h, PostProcessPasses.DOF);
        HashPassState(h, PostProcessPasses.MotionBlur);
        HashPassState(h, PostProcessPasses.TAA);
        HashPassState(h, PostProcessPasses.Precipitation);
        HashPassState(h, PostProcessPasses.Fog);
        HashPassState(h, PostProcessPasses.ChromAberration);
        HashPassState(h, PostProcessPasses.ColorGrading);
        HashPassState(h, PostProcessPasses.ToneMap);
        HashPassState(h, PostProcessPasses.Vignette);
        HashPassState(h, PostProcessPasses.FXAA);
        HashPassState(h, PostProcessPasses.SelectionOutline);
        HashPassState(h, PostProcessPasses.UIComposite);
        HashPassState(h, PostProcessPasses.Final);

        // Water needs a refraction texture only when it has draws this frame.
        HashBool(h, RenderStreamPasses.Water &&
                        RenderStreamPasses.Water->GetCommandBucket().GetCommandCount() > 0u);

        // Non-zero sentinel so callers can use 0 to mean "no cache".
        if (h == 0u)
            h = 1u;
        return h;
    }

    void Renderer3D::RenderPipeline::PopulateBlackboard(Renderer3DData& data)
    {
        OLO_PROFILE_FUNCTION();

        if (!data.RGraph)
            return;

        auto& graph = *data.RGraph;
        auto& pipeline = *this;

        // ------------------------------------------------------------------
        // Cache short-circuit
        // ------------------------------------------------------------------
        // PopulateBlackboard declares ~80 transient resources per frame, and
        // every declaration touches multiple unordered_map<std::string, X>
        // entries (find/insert/erase). In MSVC Debug each map op is dominated
        // by iterator-debug overhead, so the whole function runs ~65ms per
        // frame on a stable scene. Hash the inputs the function branches on;
        // if nothing has changed since last frame, the existing handles in
        // FrameBlackboard + the imported-resource maps inside RenderGraph are
        // still valid and we can skip the entire body.
        //
        // Stable handles (review item 5) keep this cache correct — the slot
        // generations no longer churn across frames, so the handles held in
        // FrameBlackboard remain valid. The cache is still load-bearing as a
        // fast-path: skipping the ~80 declarations + 11 string-keyed map
        // resets is the win, not handle stability per se. The deeper item-19
        // fix (interned name → handle slot table) would speed up the work
        // we *do* pay when the fingerprint actually changes.
        {
            const u64 currentFingerprint = ComputeBlackboardFingerprint(data);
            if (m_HasValidBlackboardCache && currentFingerprint == m_BlackboardFingerprint)
                return;
            m_BlackboardFingerprint = currentFingerprint;
            m_HasValidBlackboardCache = true;
        }

        // Clear prior-frame handles so stale handles are never accidentally resolved.
        graph.ClearBlackboard();
        graph.ClearImportedResources();

        auto& board = graph.GetBlackboard();

        // ------------------------------------------------------------------
        // Scene outputs
        // ------------------------------------------------------------------
        if (pipeline.FrameCorePasses.Scene)
        {
            const auto& sceneSpec = pipeline.FrameCorePasses.Scene->GetFramebufferSpecification();
            if (sceneSpec.Width > 0u && sceneSpec.Height > 0u)
            {
                RGResourceDesc sceneDesc;
                sceneDesc.Kind = ResourceHandle::Kind::Framebuffer;
                sceneDesc.Width = sceneSpec.Width;
                sceneDesc.Height = sceneSpec.Height;
                sceneDesc.Attachments = {
                    RGResourceFormat::RGBA16Float,
                    RGResourceFormat::R32Int,
                    RGResourceFormat::RG16Float,
                    RGResourceFormat::RG16Float,
                    RGResourceFormat::Depth24Stencil8,
                };
                sceneDesc.DebugName = std::string(ResourceNames::SceneColor);
                board.Scene.SceneColor = graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, sceneDesc);
                board.Scene.SceneColorTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, board.Scene.SceneColor, 0u);
                board.Scene.SceneEntityID = graph.CreateFramebufferAttachmentView(ResourceNames::SceneEntityID, board.Scene.SceneColor, 1u);
                board.Scene.SceneViewNormals = graph.CreateFramebufferAttachmentView(ResourceNames::SceneViewNormals, board.Scene.SceneColor, 2u);
                board.Scene.SceneDepthAttachment = graph.CreateFramebufferDepthAttachmentView(ResourceNames::SceneDepthAttachment, board.Scene.SceneColor);
            }

            // AO/Deferred consumers need true geometric depth + view-space
            // normals. In Deferred mode these come from the prepared
            // G-Buffer resolved attachments (not from ScenePass target
            // attachments, which are cleared for overlay consumers).
            const bool deferredActive = (data.Settings.Path == RenderingPath::Deferred);
            const auto gbuffer = deferredActive ? pipeline.FrameCorePasses.Scene->GetGBuffer() : Ref<GBuffer>{};
            OLO_CORE_ASSERT(!deferredActive || gbuffer,
                            "Renderer3D: Deferred path requires a prepared GBuffer before blackboard population");

            if (sceneSpec.Width > 0u && sceneSpec.Height > 0u && !deferredActive)
            {
                RGResourceDesc depthDesc;
                depthDesc.Kind = ResourceHandle::Kind::Texture2D;
                depthDesc.Format = RGResourceFormat::Depth24Stencil8;
                depthDesc.Width = sceneSpec.Width;
                depthDesc.Height = sceneSpec.Height;
                depthDesc.DebugName = std::string(ResourceNames::SceneDepth);
                board.Scene.SceneDepth = graph.AllocateTransientTextureHandle(ResourceNames::SceneDepth, depthDesc);

                RGResourceDesc normalsDesc;
                normalsDesc.Kind = ResourceHandle::Kind::Texture2D;
                normalsDesc.Format = RGResourceFormat::RG16Float;
                normalsDesc.Width = sceneSpec.Width;
                normalsDesc.Height = sceneSpec.Height;
                normalsDesc.DebugName = std::string(ResourceNames::SceneNormals);
                board.Scene.SceneNormals = graph.AllocateTransientTextureHandle(ResourceNames::SceneNormals, normalsDesc);
            }
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
            //   RT3 Velocity — screen-space motion vectors
            auto buildGBufferFramebufferDesc = [&gbuffer](const u32 sampleCount, std::string_view debugName) -> RGResourceDesc
            {
                RGResourceDesc desc;
                desc.Kind = ResourceHandle::Kind::Framebuffer;
                desc.Width = gbuffer->GetWidth();
                desc.Height = gbuffer->GetHeight();
                desc.Samples = sampleCount;
                // RT layout must match `GBuffer::AttachmentIndex` so the
                // transient-pool aliasing key matches the physical G-Buffer.
                // Index 4 (R32Int) is the per-pixel entity-ID slot blitted
                // into Scene FB RT1 by `DeferredLightingPass` so picking +
                // selection-outline see real IDs in Deferred just like
                // Forward.
                desc.Attachments = {
                    RGResourceFormat::RGBA8UNorm,
                    RGResourceFormat::RGBA16Float,
                    RGResourceFormat::RGBA16Float,
                    RGResourceFormat::RG16Float,
                    RGResourceFormat::R32Int,
                    RGResourceFormat::Depth24Stencil8,
                };
                desc.DebugName = std::string(debugName);
                return desc;
            };

            const auto resolvedGBuffer = graph.DeclareTransientFramebuffer(
                ResourceNames::GBufferResolved,
                buildGBufferFramebufferDesc(1u, ResourceNames::GBufferResolved),
                gbuffer->GetSamplingFramebuffer());

            // Multisample companion handles. Declared only when MSAA is active
            // so the typed-handle path can drive per-sample shading without
            // going through the raw GBuffer accessor. DeferredOpaqueDecalPass
            // publishes these post-decal to preserve the same semantics as the
            // single-sample deferred exports. SceneDepthMS is also exposed here
            // (rather than alongside SceneDepth) because the multisample depth
            // lives on the G-Buffer, not on the lit scene framebuffer.
            if (gbuffer->GetSampleCount() > 1u)
            {
                const auto buildResolvedBackingName = [](std::string_view resourceName)
                {
                    return std::string(resourceName) + "__ResolvedBacking";
                };

                const auto multisampleGBuffer = graph.DeclareTransientFramebuffer(
                    ResourceNames::GBufferMS,
                    buildGBufferFramebufferDesc(gbuffer->GetSampleCount(), ResourceNames::GBufferMS),
                    gbuffer->GetFramebuffer());

                board.GBuffer.GBufferAlbedoMS = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferAlbedoMS, multisampleGBuffer, 0u);
                board.GBuffer.GBufferNormalMS = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferNormalMS, multisampleGBuffer, 1u);
                board.GBuffer.GBufferEmissiveMS = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferEmissiveMS, multisampleGBuffer, 2u);
                board.GBuffer.VelocityMS = graph.CreateFramebufferAttachmentView(ResourceNames::VelocityMS, multisampleGBuffer, 3u);
                board.GBuffer.SceneDepthMS = graph.CreateFramebufferDepthAttachmentView(ResourceNames::SceneDepthMS, multisampleGBuffer);

                const auto sceneDepthResolvedBacking =
                    graph.CreateFramebufferDepthAttachmentView(buildResolvedBackingName(ResourceNames::SceneDepth), resolvedGBuffer);
                const auto sceneNormalsResolvedBacking =
                    graph.CreateFramebufferAttachmentView(buildResolvedBackingName(ResourceNames::SceneNormals), resolvedGBuffer, 1u);
                const auto gbufferAlbedoResolvedBacking =
                    graph.CreateFramebufferAttachmentView(buildResolvedBackingName(ResourceNames::GBufferAlbedo), resolvedGBuffer, 0u);
                const auto gbufferNormalResolvedBacking =
                    graph.CreateFramebufferAttachmentView(buildResolvedBackingName(ResourceNames::GBufferNormal), resolvedGBuffer, 1u);
                const auto gbufferEmissiveResolvedBacking =
                    graph.CreateFramebufferAttachmentView(buildResolvedBackingName(ResourceNames::GBufferEmissive), resolvedGBuffer, 2u);
                const auto velocityResolvedBacking =
                    graph.CreateFramebufferAttachmentView(buildResolvedBackingName(ResourceNames::Velocity), resolvedGBuffer, 3u);

                // Canonical deferred single-sample handles remain the semantic
                // read surface for downstream passes, but when MSAA is active
                // model them explicitly as resolve views over the multisample
                // attachments rather than as unrelated sibling names.
                board.Scene.SceneDepth = graph.CreateTextureMultisampleResolveView(ResourceNames::SceneDepth,
                                                                                   board.GBuffer.SceneDepthMS,
                                                                                   sceneDepthResolvedBacking);
                board.Scene.SceneNormals = graph.CreateTextureMultisampleResolveView(ResourceNames::SceneNormals,
                                                                                     board.GBuffer.GBufferNormalMS,
                                                                                     sceneNormalsResolvedBacking);
                board.GBuffer.GBufferAlbedo = graph.CreateTextureMultisampleResolveView(ResourceNames::GBufferAlbedo,
                                                                                        board.GBuffer.GBufferAlbedoMS,
                                                                                        gbufferAlbedoResolvedBacking);
                board.GBuffer.GBufferNormal = graph.CreateTextureMultisampleResolveView(ResourceNames::GBufferNormal,
                                                                                        board.GBuffer.GBufferNormalMS,
                                                                                        gbufferNormalResolvedBacking);
                board.GBuffer.GBufferEmissive = graph.CreateTextureMultisampleResolveView(ResourceNames::GBufferEmissive,
                                                                                          board.GBuffer.GBufferEmissiveMS,
                                                                                          gbufferEmissiveResolvedBacking);
                board.GBuffer.Velocity = graph.CreateTextureMultisampleResolveView(ResourceNames::Velocity,
                                                                                   board.GBuffer.VelocityMS,
                                                                                   velocityResolvedBacking);
            }
            else
            {
                board.Scene.SceneDepth = graph.CreateFramebufferDepthAttachmentView(ResourceNames::SceneDepth, resolvedGBuffer);
                board.Scene.SceneNormals = graph.CreateFramebufferAttachmentView(ResourceNames::SceneNormals, resolvedGBuffer, 1u);
                board.GBuffer.GBufferAlbedo = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferAlbedo, resolvedGBuffer, 0u);
                board.GBuffer.GBufferNormal = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferNormal, resolvedGBuffer, 1u);
                board.GBuffer.GBufferEmissive = graph.CreateFramebufferAttachmentView(ResourceNames::GBufferEmissive, resolvedGBuffer, 2u);
                board.GBuffer.Velocity = graph.CreateFramebufferAttachmentView(ResourceNames::Velocity, resolvedGBuffer, 3u);
            }
        }

        // ------------------------------------------------------------------
        // Velocity buffer
        // ------------------------------------------------------------------
        if (!deferredActive && pipeline.FrameCorePasses.Scene)
        {
            const auto& sceneSpec = pipeline.FrameCorePasses.Scene->GetFramebufferSpecification();
            if (sceneSpec.Width > 0u && sceneSpec.Height > 0u)
            {
                RGResourceDesc velocityDesc;
                velocityDesc.Kind = ResourceHandle::Kind::Texture2D;
                velocityDesc.Format = RGResourceFormat::RG16Float;
                velocityDesc.Width = sceneSpec.Width;
                velocityDesc.Height = sceneSpec.Height;
                velocityDesc.DebugName = std::string(ResourceNames::Velocity);
                board.GBuffer.Velocity = graph.AllocateTransientTextureHandle(ResourceNames::Velocity, velocityDesc);
            }
        }

        // ------------------------------------------------------------------
        // AO buffer
        // ------------------------------------------------------------------
        {
            const bool ssaoReady = pipeline.SceneCompositePasses.SSAO &&
                                   data.PostProcess.SSAOEnabled &&
                                   data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO &&
                                   pipeline.SceneCompositePasses.SSAO->IsReadyForExecution();
            const bool gtaoReady = pipeline.SceneCompositePasses.GTAO &&
                                   data.PostProcess.GTAOEnabled &&
                                   data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO &&
                                   pipeline.SceneCompositePasses.GTAO->IsReadyForExecution();

            if (ssaoReady)
            {
                u32 aoWidth = 1u;
                u32 aoHeight = 1u;
                if (pipeline.FrameCorePasses.Scene)
                {
                    const auto& sceneSpec = pipeline.FrameCorePasses.Scene->GetFramebufferSpecification();
                    aoWidth = std::max(1u, sceneSpec.Width / 2u);
                    aoHeight = std::max(1u, sceneSpec.Height / 2u);
                }

                RGResourceDesc aoDesc;
                aoDesc.Kind = ResourceHandle::Kind::Texture2D;
                aoDesc.Format = RGResourceFormat::RG16Float;
                aoDesc.Width = aoWidth;
                aoDesc.Height = aoHeight;
                aoDesc.DebugName = std::string(ResourceNames::AOBuffer);
                board.AO.AOBuffer = graph.AllocateTransientTextureHandle(ResourceNames::AOBuffer, aoDesc);
            }
            else if (gtaoReady)
            {
                u32 aoWidth = 1u;
                u32 aoHeight = 1u;
                if (pipeline.FrameCorePasses.Scene)
                {
                    const auto& sceneSpec = pipeline.FrameCorePasses.Scene->GetFramebufferSpecification();
                    aoWidth = sceneSpec.Width > 0u ? sceneSpec.Width : 1u;
                    aoHeight = sceneSpec.Height > 0u ? sceneSpec.Height : 1u;
                }

                RGResourceDesc aoDesc;
                aoDesc.Kind = ResourceHandle::Kind::Texture2D;
                aoDesc.Format = RGResourceFormat::R8UNorm;
                aoDesc.Width = aoWidth;
                aoDesc.Height = aoHeight;
                aoDesc.DebugName = std::string(ResourceNames::AOBuffer);
                board.AO.AOBuffer = graph.AllocateTransientTextureHandle(ResourceNames::AOBuffer, aoDesc);
            }

            static i32 s_PrevAOTechnique = -1;
            static bool s_PrevSSAOEnabled = false;
            static bool s_PrevGTAOEnabled = false;
            static bool s_PrevSSAOReady = false;
            static bool s_PrevGTAOReady = false;
            static bool s_PrevAOHandleValid = false;
            const i32 activeTechnique = static_cast<i32>(data.PostProcess.ActiveAOTechnique);
            const bool aoHandleValid = board.AO.AOBuffer.IsValid();
            if (activeTechnique != s_PrevAOTechnique ||
                data.PostProcess.SSAOEnabled != s_PrevSSAOEnabled ||
                data.PostProcess.GTAOEnabled != s_PrevGTAOEnabled ||
                ssaoReady != s_PrevSSAOReady ||
                gtaoReady != s_PrevGTAOReady ||
                aoHandleValid != s_PrevAOHandleValid)
            {
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_TRACE("Renderer3D: AO output state: technique={}, ssaoEnabled={}, gtaoEnabled={}, ssaoReady={}, gtaoReady={}, aoHandleValid={}",
                                   activeTechnique,
                                   data.PostProcess.SSAOEnabled,
                                   data.PostProcess.GTAOEnabled,
                                   ssaoReady,
                                   gtaoReady,
                                   aoHandleValid);
                }
                s_PrevAOTechnique = activeTechnique;
                s_PrevSSAOEnabled = data.PostProcess.SSAOEnabled;
                s_PrevGTAOEnabled = data.PostProcess.GTAOEnabled;
                s_PrevSSAOReady = ssaoReady;
                s_PrevGTAOReady = gtaoReady;
                s_PrevAOHandleValid = aoHandleValid;
            }
        }

        // ------------------------------------------------------------------
        // Shadow maps
        // ------------------------------------------------------------------
        {
            const auto shadowResolution = std::max(data.Shadow.GetResolution(), 1u);
            const auto buildShadowTextureDesc = [shadowResolution](const ResourceHandle::Kind kind,
                                                                   std::string_view debugName,
                                                                   const u32 depthOrLayers)
            {
                auto desc = RGResourceDesc::FromHandleKind(kind, debugName);
                desc.Format = RGResourceFormat::Depth32Float;
                desc.Width = shadowResolution;
                desc.Height = shadowResolution;
                desc.DepthOrLayers = depthOrLayers;
                return desc;
            };

            const u32 csmID = data.Shadow.GetCSMRendererID();
            const u32 spotID = data.Shadow.GetSpotRendererID();
            if (csmID != 0)
            {
                board.Shadows.ShadowMapCSM = graph.DeclareTransientTexture(
                    ResourceNames::ShadowMapCSM,
                    buildShadowTextureDesc(ResourceHandle::Kind::Texture2DArray,
                                           ResourceNames::ShadowMapCSM,
                                           FrameBlackboard::MaxShadowMapCascades),
                    csmID);

                for (u32 cascade = 0; cascade < FrameBlackboard::MaxShadowMapCascades; ++cascade)
                {
                    board.Shadows.ShadowMapCSMCascades[cascade] = graph.CreateTextureArrayLayerView(
                        ResourceNames::ShadowMapCSMCascade[cascade], board.Shadows.ShadowMapCSM, cascade);
                }
            }
            if (spotID != 0)
            {
                board.Shadows.ShadowMapSpot = graph.DeclareTransientTexture(
                    ResourceNames::ShadowMapSpot,
                    buildShadowTextureDesc(ResourceHandle::Kind::Texture2DArray,
                                           ResourceNames::ShadowMapSpot,
                                           FrameBlackboard::MaxShadowMapSpotLights),
                    spotID);

                for (u32 light = 0; light < FrameBlackboard::MaxShadowMapSpotLights; ++light)
                {
                    board.Shadows.ShadowMapSpotLayers[light] = graph.CreateTextureArrayLayerView(
                        ResourceNames::ShadowMapSpotLayer[light], board.Shadows.ShadowMapSpot, light);
                }
            }
            // Point-light shadow cubemaps — declare each active light slot as a
            // frame-local transient root with explicit external backing.
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                const u32 pointID = data.Shadow.GetPointRendererID(i);
                if (pointID != 0)
                {
                    board.Shadows.ShadowMapPoint[i] = graph.DeclareTransientTexture(
                        ResourceNames::ShadowMapPoint[i],
                        buildShadowTextureDesc(ResourceHandle::Kind::TextureCube,
                                               ResourceNames::ShadowMapPoint[i],
                                               FrameBlackboard::MaxShadowMapCubeFaces),
                        pointID);

                    for (u32 face = 0; face < FrameBlackboard::MaxShadowMapCubeFaces; ++face)
                    {
                        const auto faceViewName = std::string(ResourceNames::ShadowMapPoint[i]) + "Face" + std::to_string(face);
                        board.Shadows.ShadowMapPointFaces[i][face] = graph.CreateTextureCubeFaceView(
                            faceViewName, board.Shadows.ShadowMapPoint[i], face);
                    }
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

        auto declareGraphOnlyFramebuffer =
            [&graph](std::string_view name, const RGResourceDesc& desc) -> RGFramebufferHandle
        {
            return graph.DeclareTransientFramebuffer(name, desc);
        };

        auto declareGraphOnlyTexture =
            [&graph](std::string_view name, const RGResourceDesc& desc) -> RGTextureHandle
        {
            return graph.AllocateTransientTextureHandle(name, desc);
        };

        auto declareGraphOnlyPostProcessFB =
            [&postProcessWidth, &postProcessHeight, &declareGraphOnlyFramebuffer](std::string_view name, const RGResourceFormat fmt) -> RGFramebufferHandle
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Width = postProcessWidth;
            desc.Height = postProcessHeight;
            desc.Format = fmt;
            desc.DebugName = std::string(name);
            return declareGraphOnlyFramebuffer(name, desc);
        };

        struct GraphOnlyPostProcessOutput
        {
            RGFramebufferHandle Framebuffer;
            RGTextureHandle Texture;
        };

        auto declareGraphOnlyPostProcessOutput =
            [&declareGraphOnlyPostProcessFB, &graph](std::string_view framebufferName,
                                                     std::string_view textureName,
                                                     const RGResourceFormat fmt) -> GraphOnlyPostProcessOutput
        {
            const auto framebuffer = declareGraphOnlyPostProcessFB(framebufferName, fmt);
            const auto texture = framebuffer.IsValid()
                                     ? graph.CreateFramebufferAttachmentView(textureName, framebuffer, 0u)
                                     : RGTextureHandle{};
            return GraphOnlyPostProcessOutput{ .Framebuffer = framebuffer, .Texture = texture };
        };

        // ------------------------------------------------------------------
        // Graph-owned scratch resources
        // ------------------------------------------------------------------
        if (pipeline.SceneCompositePasses.SSAO &&
            data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO &&
            data.PostProcess.SSAOEnabled &&
            board.AO.AOBuffer.IsValid())
        {
            // SSAORaw / SSAOBlur must match the SSAO pass's half-res viewport.
            // Using the full-res FramebufferSpecification here was the cause of
            // "ghost copies" with SSAO enabled: the blur sampled SSAO data only
            // in the FB's top-left quarter and spatially-shifted AO across the
            // rest of the screen.
            const u32 ssaoWidth = pipeline.SceneCompositePasses.SSAO->GetHalfWidth();
            const u32 ssaoHeight = pipeline.SceneCompositePasses.SSAO->GetHalfHeight();
            if (ssaoWidth > 0u && ssaoHeight > 0u)
            {
                RGResourceDesc rawDesc;
                rawDesc.Kind = ResourceHandle::Kind::Framebuffer;
                rawDesc.Format = RGResourceFormat::RG16Float;
                rawDesc.Width = ssaoWidth;
                rawDesc.Height = ssaoHeight;
                rawDesc.DebugName = "SSAORaw";
                board.Scratch.SSAORaw = declareGraphOnlyFramebuffer("SSAORaw", rawDesc);

                RGResourceDesc blurDesc = rawDesc;
                blurDesc.DebugName = std::string(ResourceNames::SSAOBlur);
                board.Scratch.SSAOBlur = declareGraphOnlyFramebuffer(ResourceNames::SSAOBlur, blurDesc);
            }
        }

        if (pipeline.SceneCompositePasses.GTAO &&
            data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO &&
            data.PostProcess.GTAOEnabled &&
            board.AO.AOBuffer.IsValid() &&
            board.Scene.SceneDepth.IsValid() &&
            board.Scene.SceneNormals.IsValid())
        {
            const auto nextPow2 = [](u32 value)
            {
                u32 result = 1u;
                while (result < value)
                    result <<= 1u;
                return result;
            };

            const u32 hzbW = nextPow2(postProcessWidth);
            const u32 hzbH = nextPow2(postProcessHeight);
            u32 mipCount = 1u;
            for (u32 mipW = hzbW, mipH = hzbH; mipW > 1u || mipH > 1u; ++mipCount)
            {
                mipW = mipW > 1u ? (mipW / 2u) : 1u;
                mipH = mipH > 1u ? (mipH / 2u) : 1u;
            }

            RGResourceDesc hzbDesc;
            hzbDesc.Kind = ResourceHandle::Kind::Texture2D;
            hzbDesc.Format = RGResourceFormat::R32Float;
            hzbDesc.Width = hzbW;
            hzbDesc.Height = hzbH;
            hzbDesc.MipLevels = mipCount;
            hzbDesc.DebugName = std::string(ResourceNames::HZBDepth);
            board.Scratch.HZBDepth = declareGraphOnlyTexture(ResourceNames::HZBDepth, hzbDesc);

            if (board.Scratch.HZBDepth.IsValid())
            {
                const auto declaredMipViewCount = std::min<u32>(mipCount, FrameBlackboard::MaxHZBMipViews);
                for (u32 mip = 0u; mip < declaredMipViewCount; ++mip)
                {
                    const auto mipViewName = std::string(ResourceNames::HZBDepth) + "Mip" + std::to_string(mip);
                    board.Scratch.HZBDepthMipViews[mip] = graph.CreateTextureMipView(mipViewName, board.Scratch.HZBDepth, mip);
                }
            }

            RGResourceDesc edgeDesc;
            edgeDesc.Kind = ResourceHandle::Kind::Texture2D;
            edgeDesc.Format = RGResourceFormat::R8UNorm;
            edgeDesc.Width = postProcessWidth;
            edgeDesc.Height = postProcessHeight;
            edgeDesc.DebugName = "GTAOEdge";
            board.Scratch.GTAOEdge = declareGraphOnlyTexture("GTAOEdge", edgeDesc);

            RGResourceDesc denoiseDesc;
            denoiseDesc.Kind = ResourceHandle::Kind::Texture2D;
            denoiseDesc.Format = RGResourceFormat::R8UNorm;
            denoiseDesc.Width = postProcessWidth;
            denoiseDesc.Height = postProcessHeight;
            denoiseDesc.DebugName = std::string(ResourceNames::GTAODenoisePing);
            board.Scratch.GTAODenoisePing = declareGraphOnlyTexture(ResourceNames::GTAODenoisePing, denoiseDesc);

            denoiseDesc.DebugName = std::string(ResourceNames::GTAODenoisePong);
            board.Scratch.GTAODenoisePong = declareGraphOnlyTexture(ResourceNames::GTAODenoisePong, denoiseDesc);
        }

        if (pipeline.RenderStreamPasses.Water &&
            pipeline.RenderStreamPasses.Water->GetCommandBucket().GetCommandCount() > 0 &&
            board.Scene.SceneColor.IsValid())
        {
            RGResourceDesc refrDesc;
            refrDesc.Kind = ResourceHandle::Kind::Texture2D;
            refrDesc.Format = RGResourceFormat::RGBA16Float;
            refrDesc.Width = postProcessWidth;
            refrDesc.Height = postProcessHeight;
            refrDesc.DebugName = "WaterRefraction";
            board.Scratch.WaterRefraction = declareGraphOnlyTexture("WaterRefraction", refrDesc);
        }

        if (pipeline.PostProcessPasses.SSS &&
            data.Snow.Enabled &&
            data.Snow.SSSBlurEnabled &&
            pipeline.PostProcessPasses.SSS->IsReadyForExecution())
        {
            const auto sssOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::SSSColor,
                ResourceNames::SSSColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.SSSColor = sssOutput.Framebuffer;
            board.Post.SSSColorTexture = sssOutput.Texture;
        }

        // AOApplyColor exists only when AO apply is actually executable for
        // this frame: the pass is enabled, the AO producer imported a valid
        // AOBuffer, and SceneDepth is available for bilateral upsampling.
        if (pipeline.PostProcessPasses.AOApply)
        {
            if (pipeline.PostProcessPasses.AOApply->IsEnabled() &&
                pipeline.PostProcessPasses.AOApply->IsReadyForExecution() &&
                board.AO.AOBuffer.IsValid() &&
                board.Scene.SceneDepth.IsValid())
            {
                const auto aoApplyOutput = declareGraphOnlyPostProcessOutput(
                    ResourceNames::AOApplyColor,
                    ResourceNames::AOApplyColorTexture,
                    RGResourceFormat::RGBA16Float);
                board.Post.AOApplyColor = aoApplyOutput.Framebuffer;
                board.Post.AOApplyColorTexture = aoApplyOutput.Texture;
            }
        }

        // PostProcessColor is an alias handle to the latest upstream graph
        // resource in the dynamic chain, NOT a separate imported resource.
        // This preserves declaration-derived reachability:
        //   AOApplyColor -> Bloom, SSSColor -> Bloom, or SceneColor -> Bloom.
        // Importing a fresh `PostProcessColor` framebuffer here severs that
        // producer/consumer chain, which lets AO/SSS get culled and can feed
        // stale/black data into the post stack.
        //
        // The handle copies below keep board.Post.PostProcessColor usable by code
        // paths that already hold the typed handle. RegisterFramebufferAlias /
        // RegisterTextureAlias also make the base-name "PostProcessColor"
        // resolvable via graph.GetFramebufferHandle / GetTextureHandle, so
        // post-process passes that read by name (DOF, MotionBlur, TAA, ...)
        // pick up whichever upstream is active without each having to list
        // every possible chain source explicitly in their candidate arrays.
        std::string_view postProcessTargetFramebuffer;
        std::string_view postProcessTargetTexture;
        if (board.Post.AOApplyColor.IsValid())
        {
            board.Post.PostProcessColor = board.Post.AOApplyColor;
            board.Post.PostProcessColorTexture = board.Post.AOApplyColorTexture;
            postProcessTargetFramebuffer = ResourceNames::AOApplyColor;
            postProcessTargetTexture = ResourceNames::AOApplyColorTexture;
        }
        else if (board.Post.SSSColor.IsValid())
        {
            board.Post.PostProcessColor = board.Post.SSSColor;
            board.Post.PostProcessColorTexture = board.Post.SSSColorTexture;
            postProcessTargetFramebuffer = ResourceNames::SSSColor;
            postProcessTargetTexture = ResourceNames::SSSColorTexture;
        }
        else
        {
            board.Post.PostProcessColor = board.Scene.SceneColor;
            board.Post.PostProcessColorTexture = board.Scene.SceneColorTexture;
            postProcessTargetFramebuffer = ResourceNames::SceneColor;
            postProcessTargetTexture = ResourceNames::SceneColorTexture;
        }
        graph.RegisterFramebufferAlias(ResourceNames::PostProcessColor, postProcessTargetFramebuffer);
        graph.RegisterTextureAlias(ResourceNames::PostProcessColorTexture, postProcessTargetTexture);

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
                const auto bloomOutput = declareGraphOnlyPostProcessOutput(
                    ResourceNames::BloomColor,
                    ResourceNames::BloomColorTexture,
                    RGResourceFormat::RGBA16Float);
                board.Post.BloomColor = bloomOutput.Framebuffer;
                board.Post.BloomColorTexture = bloomOutput.Texture;

                u32 mipW = postProcessWidth / 2u;
                u32 mipH = postProcessHeight / 2u;
                for (u32 i = 0; i < 5u; ++i)
                {
                    if (mipW < 2u || mipH < 2u)
                        break;

                    RGResourceDesc mipDesc;
                    mipDesc.Kind = ResourceHandle::Kind::Framebuffer;
                    mipDesc.Format = RGResourceFormat::RGBA16Float;
                    mipDesc.Width = mipW;
                    mipDesc.Height = mipH;
                    mipDesc.DebugName = "BloomMip" + std::to_string(i);
                    board.Scratch.BloomMips[i] = declareGraphOnlyFramebuffer(mipDesc.DebugName, mipDesc);

                    mipW /= 2u;
                    mipH /= 2u;
                }
            }
        }

        // DOFColor is declared only when DOF is enabled.
        if (pipeline.PostProcessPasses.DOF && data.PostProcess.DOFEnabled &&
            pipeline.PostProcessPasses.DOF->IsReadyForExecution())
        {
            const auto dofOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::DOFColor,
                ResourceNames::DOFColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.DOFColor = dofOutput.Framebuffer;
            board.Post.DOFColorTexture = dofOutput.Texture;
        }

        // MotionBlurColor is declared only when motion blur is enabled.
        if (pipeline.PostProcessPasses.MotionBlur && data.PostProcess.MotionBlurEnabled &&
            pipeline.PostProcessPasses.MotionBlur->IsReadyForExecution())
        {
            const auto motionBlurOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::MotionBlurColor,
                ResourceNames::MotionBlurColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.MotionBlurColor = motionBlurOutput.Framebuffer;
            board.Post.MotionBlurColorTexture = motionBlurOutput.Texture;
        }

        // TAAColor is declared only when TAA is enabled.
        if (pipeline.PostProcessPasses.TAA && data.PostProcess.TAAEnabled &&
            pipeline.PostProcessPasses.TAA->IsReadyForExecution())
        {
            const auto taaOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::TAAColor,
                ResourceNames::TAAColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.TAAColor = taaOutput.Framebuffer;
            board.Post.TAAColorTexture = taaOutput.Texture;
        }

        // PrecipitationColor is declared only when screen FX are active.
        const bool precipScreenEnabled = data.Precipitation.Enabled &&
                                         (data.Precipitation.ScreenStreaksEnabled ||
                                          data.Precipitation.LensImpactsEnabled);
        if (pipeline.PostProcessPasses.Precipitation && precipScreenEnabled &&
            pipeline.PostProcessPasses.Precipitation->IsReadyForExecution())
        {
            const auto precipitationOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::PrecipitationColor,
                ResourceNames::PrecipitationColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.PrecipitationColor = precipitationOutput.Framebuffer;
            board.Post.PrecipitationColorTexture = precipitationOutput.Texture;
        }

        // FogColor is declared only when fog is enabled.
        if (pipeline.PostProcessPasses.Fog && data.Fog.Enabled &&
            pipeline.PostProcessPasses.Fog->IsReadyForExecution())
        {
            const auto fogOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::FogColor,
                ResourceNames::FogColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.FogColor = fogOutput.Framebuffer;
            board.Post.FogColorTexture = fogOutput.Texture;

            const auto& fogSpec = pipeline.PostProcessPasses.Fog->GetFramebufferSpecification();
            if (fogSpec.Width > 0u && fogSpec.Height > 0u)
            {
                RGResourceDesc fogHalfDesc;
                fogHalfDesc.Kind = ResourceHandle::Kind::Framebuffer;
                fogHalfDesc.Format = RGResourceFormat::RGBA16Float;
                fogHalfDesc.Width = (fogSpec.Width + 1u) / 2u;
                fogHalfDesc.Height = (fogSpec.Height + 1u) / 2u;
                fogHalfDesc.DebugName = std::string(ResourceNames::FogHalfRes);
                board.Scratch.FogHalfRes = declareGraphOnlyFramebuffer(ResourceNames::FogHalfRes, fogHalfDesc);
            }
        }

        // Extracted effect sub-chain. Each handle is
        // declared only when its effect is enabled so downstream consumers
        // can rely on IsValid() as the canonical "effect ran" signal.
        // ToneMap is declared unconditionally (no settings gate).
        if (pipeline.PostProcessPasses.ChromAberration && data.PostProcess.ChromaticAberrationEnabled &&
            pipeline.PostProcessPasses.ChromAberration->IsReadyForExecution())
        {
            const auto chromAbOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::ChromAbColor,
                ResourceNames::ChromAbColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.ChromAbColor = chromAbOutput.Framebuffer;
            board.Post.ChromAbColorTexture = chromAbOutput.Texture;
        }
        if (pipeline.PostProcessPasses.ColorGrading && data.PostProcess.ColorGradingEnabled &&
            pipeline.PostProcessPasses.ColorGrading->IsReadyForExecution())
        {
            const auto colorGradingOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::ColorGradingColor,
                ResourceNames::ColorGradingColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.ColorGradingColor = colorGradingOutput.Framebuffer;
            board.Post.ColorGradingColorTexture = colorGradingOutput.Texture;
        }
        if (pipeline.PostProcessPasses.ToneMap && pipeline.PostProcessPasses.ToneMap->IsReadyForExecution())
        {
            const auto toneMapOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::ToneMapColor,
                ResourceNames::ToneMapColorTexture,
                RGResourceFormat::RGBA16Float);
            board.Post.ToneMapColor = toneMapOutput.Framebuffer;
            board.Post.ToneMapColorTexture = toneMapOutput.Texture;
        }
        if (pipeline.PostProcessPasses.Vignette && data.PostProcess.VignetteEnabled &&
            pipeline.PostProcessPasses.Vignette->IsReadyForExecution())
        {
            const auto vignetteOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::VignetteColor,
                ResourceNames::VignetteColorTexture,
                RGResourceFormat::RGBA8UNorm);
            board.Post.VignetteColor = vignetteOutput.Framebuffer;
            board.Post.VignetteColorTexture = vignetteOutput.Texture;
        }

        // Only declare FXAAColor when FXAA is active so
        // downstream consumers can rely on `board.Post.FXAAColor.IsValid()` as
        // the canonical "anti-aliased post-process available" signal.
        if (pipeline.PostProcessPasses.FXAA && data.PostProcess.FXAAEnabled &&
            pipeline.PostProcessPasses.FXAA->IsReadyForExecution())
        {
            const auto fxaaOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::FXAAColor,
                ResourceNames::FXAAColorTexture,
                RGResourceFormat::RGBA8UNorm);
            board.Post.FXAAColor = fxaaOutput.Framebuffer;
            board.Post.FXAAColorTexture = fxaaOutput.Texture;
        }

        // Gate on the data flags that drive UploadExecutionState's SetEnabled
        // computation (which runs AFTER PopulateBlackboard, so checking the
        // pass's m_Enabled here would always reflect the previous frame).
        // The fingerprint cache (ComputeBlackboardFingerprint) hashes the
        // same two values so a selection change forces a rebuild.
        if (pipeline.PostProcessPasses.SelectionOutline &&
            data.EnableSelectionOutline &&
            !data.SelectionOutlineEntityIDs.empty() &&
            pipeline.PostProcessPasses.SelectionOutline->IsReadyForExecution())
        {
            const auto selectionOutlineOutput = declareGraphOnlyPostProcessOutput(
                ResourceNames::SelectionOutlineColor,
                ResourceNames::SelectionOutlineColorTexture,
                RGResourceFormat::RGBA8UNorm);
            board.Post.SelectionOutlineColor = selectionOutlineOutput.Framebuffer;
            board.Post.SelectionOutlineColorTexture = selectionOutlineOutput.Texture;

            const auto& outlineSpec = pipeline.PostProcessPasses.SelectionOutline->GetFramebufferSpecification();
            if (outlineSpec.Width > 0u && outlineSpec.Height > 0u)
            {
                RGResourceDesc jfaDesc;
                jfaDesc.Kind = ResourceHandle::Kind::Framebuffer;
                jfaDesc.Format = RGResourceFormat::RGBA32Float;
                jfaDesc.Width = outlineSpec.Width;
                jfaDesc.Height = outlineSpec.Height;

                jfaDesc.DebugName = "JFAPing";
                board.Scratch.JFAPing = declareGraphOnlyFramebuffer("JFAPing", jfaDesc);

                jfaDesc.DebugName = "JFAPong";
                board.Scratch.JFAPong = declareGraphOnlyFramebuffer("JFAPong", jfaDesc);
            }
        }

        if (pipeline.PostProcessPasses.UIComposite)
        {
            RGResourceDesc uiCompositeDesc;
            uiCompositeDesc.Kind = ResourceHandle::Kind::Framebuffer;
            uiCompositeDesc.Width = postProcessWidth;
            uiCompositeDesc.Height = postProcessHeight;
            uiCompositeDesc.Attachments = { RGResourceFormat::RGBA8UNorm, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
            uiCompositeDesc.DebugName = std::string(ResourceNames::UIComposite);
            board.Post.UIComposite = graph.DeclareTransientFramebuffer(ResourceNames::UIComposite, uiCompositeDesc);
            board.Post.UICompositeTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, board.Post.UIComposite, 0u);
        }

        // Default framebuffer / swapchain target represented as an imported
        // external output resource. Backing framebuffer is null by design;
        // FinalPass presents via RGCommandContext::BindDefaultFramebuffer().
        board.Post.Backbuffer = graph.ImportFramebuffer(
            ResourceNames::Backbuffer, nullptr,
            RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, ResourceNames::Backbuffer));

        // ------------------------------------------------------------------
        // OIT buffers
        // ------------------------------------------------------------------
        // OIT graph resources are declared only when weighted-blended OIT is
        // *actually* active for this frame. Skipping the declaration when OIT
        // is disabled means transparent contributor
        // passes (Particle / Water / Decal) bail out of their
        // `builder.Write(board.OIT.OITAccum, ...)` declarations
        // (`if (board.OIT.OITAccum.IsValid())` is already guarded), so the
        // graph never sees write edges into a buffer that nothing reads.
        // OITPreparePass and OITResolvePass also self-skip via `m_Enabled`.
        const bool oitActive = data.Settings.OITEnabled &&
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
            board.OIT.OITBuffer = oitHandle;
            board.OIT.OITAccum = graph.CreateFramebufferAttachmentView(ResourceNames::OITAccum, oitHandle, 0u);
            board.OIT.OITRevealage = graph.CreateFramebufferAttachmentView(ResourceNames::OITRevealage, oitHandle, 1u);
            board.OIT.OITDepthAttachment = graph.CreateFramebufferDepthAttachmentView(ResourceNames::OITDepthAttachment, oitHandle);
        }

        // ------------------------------------------------------------------
        // Temporal histories (imported from prior frame)
        // ------------------------------------------------------------------
        // TAAHistory persists in renderer-owned storage, is registered as a
        // graph-managed sink every frame, and is imported only when the
        // previous frame produced a valid history.
        if (pipeline.PostProcessPasses.TAA)
        {
            const auto& taaSpec = pipeline.PostProcessPasses.TAA->GetFramebufferSpecification();
            EnsureHistoryStorage(pipeline.TAAHistoryTexture, pipeline.TAAHistoryValid, taaSpec.Width, taaSpec.Height);
            graph.RegisterHistoryTextureSink(
                ResourceNames::TAAHistory,
                pipeline.TAAHistoryTexture ? pipeline.TAAHistoryTexture->GetRendererID() : 0u,
                pipeline.TAAHistoryTexture ? pipeline.TAAHistoryTexture->GetWidth() : 0u,
                pipeline.TAAHistoryTexture ? pipeline.TAAHistoryTexture->GetHeight() : 0u,
                &pipeline.TAAHistoryValid);
        }
        if (pipeline.TAAHistoryValid && pipeline.TAAHistoryTexture)
        {
            board.Temporal.TAAHistory = graph.ImportHistory(
                ResourceNames::TAAHistory, pipeline.TAAHistoryTexture->GetRendererID());
        }

        // FogHistory persists in renderer-owned storage, is registered as a
        // graph-managed sink every frame, and is imported only when the
        // previous frame produced a valid history.
        if (pipeline.PostProcessPasses.Fog)
        {
            const auto& fogSpec = pipeline.PostProcessPasses.Fog->GetFramebufferSpecification();
            const auto fogHalfWidth = (fogSpec.Width + 1u) / 2u;
            const auto fogHalfHeight = (fogSpec.Height + 1u) / 2u;
            EnsureHistoryStorage(pipeline.FogHistoryTexture, pipeline.FogHistoryValid, fogHalfWidth, fogHalfHeight);
            graph.RegisterHistoryTextureSink(
                ResourceNames::FogHistory,
                pipeline.FogHistoryTexture ? pipeline.FogHistoryTexture->GetRendererID() : 0u,
                pipeline.FogHistoryTexture ? pipeline.FogHistoryTexture->GetWidth() : 0u,
                pipeline.FogHistoryTexture ? pipeline.FogHistoryTexture->GetHeight() : 0u,
                &pipeline.FogHistoryValid);
        }
        if (pipeline.FogHistoryValid && pipeline.FogHistoryTexture)
        {
            board.Temporal.FogHistory = graph.ImportHistory(
                ResourceNames::FogHistory, pipeline.FogHistoryTexture->GetRendererID());
        }

        // ------------------------------------------------------------------
        // IBL resources
        // ------------------------------------------------------------------
        if (data.GlobalIrradianceMapID != 0)
        {
            board.IBL.IrradianceMap = graph.ImportTexture(
                ResourceNames::IrradianceMap, data.GlobalIrradianceMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, ResourceNames::IrradianceMap));
        }
        if (data.GlobalPrefilterMapID != 0)
        {
            board.IBL.PrefilterMap = graph.ImportTexture(
                ResourceNames::PrefilterMap, data.GlobalPrefilterMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, ResourceNames::PrefilterMap));
        }
        if (data.GlobalBRDFLutMapID != 0)
        {
            board.IBL.BrdfLut = graph.ImportTexture(
                ResourceNames::BrdfLut, data.GlobalBRDFLutMapID,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::BrdfLut));
        }
    }

    auto Renderer3D::RenderPipeline::BuildInputs(Renderer3DData& data) -> RenderPipelineInputs
    {
        RenderPipelineInputs inputs{};
        inputs.Graph = data.RGraph.Raw();
        inputs.ActiveAOTechnique = data.PostProcess.ActiveAOTechnique;

        inputs.Passes.Scene = FrameCorePasses.Scene.Raw();
        inputs.Passes.Shadow = FrameCorePasses.Shadow.Raw();
        inputs.Passes.DeferredLighting = SceneCompositePasses.DeferredLighting.Raw();
        inputs.Passes.DeferredOpaqueDecal = SceneCompositePasses.DeferredOpaqueDecal.Raw();
        inputs.Passes.ForwardOverlay = RenderStreamPasses.ForwardOverlay.Raw();
        inputs.Passes.Foliage = RenderStreamPasses.Foliage.Raw();
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
