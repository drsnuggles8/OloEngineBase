#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"

#include <array>

namespace OloEngine
{
    namespace
    {
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

    void Renderer3D::BeginSceneCommon()
    {
        OLO_PROFILE_FUNCTION();

        // Process any pending GPU resource creation commands from async loaders
        GPUResourceQueue::ProcessAll();

        // Poll shaders that are still being linked asynchronously by the driver
        // (GL_ARB_parallel_shader_compile). Finalize any that are done.
        if (m_ShaderLibrary.HasPendingShaders())
        {
            const u32 completed = m_ShaderLibrary.PollPendingShaders();
            if (completed > 0)
            {
                OLO_CORE_TRACE("{} shader(s) finished async linking ({} still pending)",
                               completed, m_ShaderLibrary.GetPendingCount());
            }
        }

        // Begin new frame for double-buffered resources
        FrameResourceManager::Get().BeginFrame();

        RendererProfiler::GetInstance().BeginFrame();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::BeginScene: ScenePass is null!");
            return;
        }

        // Reset frame data buffer for new frame
        FrameDataBufferManager::Get().Reset();

        // Rotate the per-entity transform history so DrawMesh/DrawAnimated
        // submission can look up "previous frame" transforms. In Forward /
        // Forward+ these maps stay empty because those paths never call
        // GetAndRecordPrevTransform.
        s_Data.PrevEntityTransforms = std::move(s_Data.CurrEntityTransforms);
        s_Data.CurrEntityTransforms.clear();
        s_Data.PrevInstanceTransforms = std::move(s_Data.CurrInstanceTransforms);
        s_Data.CurrInstanceTransforms.clear();

        // Get main-thread allocator for this frame (already reset by BeginFrame)
        CommandAllocator* frameAllocator = FrameResourceManager::Get().GetMainAllocator();
        const auto setRenderStreamAllocator = [frameAllocator](PassGraphNode* node)
        {
            if (node)
                node->SetCommandAllocator(frameAllocator);
        };
        s_Data.StreamNodes.ForEach(setRenderStreamAllocator);

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
        s_Data.PrevJitterUV = s_Data.CurrJitterUV;
        s_Data.CurrJitterUV = glm::vec2(0.0f);
        if (s_Data.PostProcess.TAAEnabled && s_Data.ScenePass && s_Data.ScenePass->GetTarget())
        {
            const auto& spec = s_Data.ScenePass->GetTarget()->GetSpecification();
            if (spec.Width > 0 && spec.Height > 0)
            {
                // 1-based Halton index; Halton(0) is undefined. Loop modulo
                // kHaltonSequenceLength keeps the pattern short and stable.
                const u32 idx = (s_Data.TAAJitterFrameIndex % kHaltonSequenceLength) + 1;
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
                const bool isOrthographic = glm::abs(s_Data.ProjectionMatrix[3][3] - 1.0f) < 1e-5f;
                if (isOrthographic)
                {
                    s_Data.ProjectionMatrix[3][0] += jitterNdcX;
                    s_Data.ProjectionMatrix[3][1] += jitterNdcY;
                }
                else
                {
                    s_Data.ProjectionMatrix[2][0] += jitterNdcX;
                    s_Data.ProjectionMatrix[2][1] += jitterNdcY;
                }
                s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;

                // Track jitter in UV-space so the TAA shader (or any future
                // consumer) can subtract it if needed. NDC -> UV is * 0.5.
                s_Data.CurrJitterUV = glm::vec2(jitterNdcX * 0.5f, jitterNdcY * 0.5f);

                s_Data.TAAJitterFrameIndex = (s_Data.TAAJitterFrameIndex + 1) % kHaltonSequenceLength;
            }
        }
        else
        {
            s_Data.TAAJitterFrameIndex = 0;
        }

        CommandDispatch::SetViewProjectionMatrix(s_Data.ViewProjectionMatrix);
        CommandDispatch::SetViewMatrix(s_Data.ViewMatrix);
        CommandDispatch::SetProjectionMatrix(s_Data.ProjectionMatrix);
        // Mirror the previous-frame view-projection into CommandDispatch so
        // dispatch paths that upload the shared CameraUBO themselves
        // (Terrain / Voxel / Decal) can fill
        // `CameraUBO::PrevViewProjection` with the true history instead of
        // aliasing the current VP — the latter wipes the matrix any other
        // consumer (TAA velocity reconstruction, motion blur) reads this
        // frame.
        CommandDispatch::SetPrevViewProjectionMatrix(s_Data.PrevViewProjectionMatrix);

        s_Data.InverseViewProjectionMatrix = glm::inverse(s_Data.ViewProjectionMatrix);
        s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);

        s_Data.Stats.Reset();
        s_Data.CommandCounter = 0;

        // Advance occlusion culling frame (reads back previous frame's query results)
        if (s_Data.OcclusionCullingEnabled)
        {
            s_Data.OcclusionResultsAvailable = OcclusionQueryPool::GetInstance().BeginFrame();
            OcclusionStateManager::GetInstance().BeginFrame();
        }
        else
        {
            s_Data.OcclusionResultsAvailable = false;
        }

        UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
        UpdateLightPropertiesUBO();

        CommandDispatch::SetSceneLight(s_Data.SceneLight);
        CommandDispatch::SetViewPosition(s_Data.ViewPos);

        const auto resetRenderStreamBucket = [](PassGraphNode* node)
        {
            if (node)
                node->ResetCommandBucket();
        };
        s_Data.StreamNodes.ForEach(resetRenderStreamBucket);

        CommandDispatch::ResetState();

        // Set shadow texture IDs AFTER ResetState() so they aren't zeroed out
        CommandDispatch::SetShadowTextureIDs(
            s_Data.Shadow.GetCSMRendererID(),
            s_Data.Shadow.GetSpotRendererID());

        // Set point shadow cubemap texture IDs
        {
            std::array<u32, ShadowMap::MAX_POINT_SHADOWS> pointIDs{};
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                pointIDs[i] = s_Data.Shadow.GetPointRendererID(i);
            }
            CommandDispatch::SetPointShadowTextureIDs(pointIDs);
        }

        // Initialize parallel scene context with immutable frame data
        s_Data.ParallelContext.ViewMatrix = s_Data.ViewMatrix;
        s_Data.ParallelContext.ProjectionMatrix = s_Data.ProjectionMatrix;
        s_Data.ParallelContext.ViewProjectionMatrix = s_Data.ViewProjectionMatrix;
        s_Data.ParallelContext.ViewPosition = s_Data.ViewPos;
        s_Data.ParallelContext.ViewFrustum = s_Data.ViewFrustum;
        s_Data.ParallelContext.FrustumCullingEnabled = s_Data.FrustumCullingEnabled;
        s_Data.ParallelContext.DynamicCullingEnabled = s_Data.DynamicCullingEnabled;

        // Cache shader references for parallel access
        s_Data.ParallelContext.LightingShader = s_Data.LightingShader;
        s_Data.ParallelContext.SkinnedLightingShader = s_Data.SkinnedLightingShader;
        // Route PBR shader slot to the G-Buffer write variant in Deferred mode
        // so parallel-submission workers pick the correct program without
        // needing to query RendererSettings per draw.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        s_Data.ParallelContext.PBRShader = (deferredActive && s_Data.PBRGBufferShader)
                                               ? s_Data.PBRGBufferShader
                                               : s_Data.PBRShader;
        s_Data.ParallelContext.PBRSkinnedShader = (deferredActive && s_Data.PBRGBufferSkinnedShader)
                                                      ? s_Data.PBRGBufferSkinnedShader
                                                      : s_Data.PBRSkinnedShader;
        s_Data.ParallelContext.LightCubeShader = s_Data.LightCubeShader;
        s_Data.ParallelContext.SkyboxShader = s_Data.SkyboxShader;
        s_Data.ParallelContext.QuadShader = s_Data.QuadShader;

        s_Data.ParallelSubmissionActive = false;
    }

    void Renderer3D::BeginScene(const PerspectiveCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetView();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = camera.GetViewProjection();
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        BeginSceneCommon();
    }

    void Renderer3D::BeginScene(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetViewMatrix();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        BeginSceneCommon();
    }

    void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = glm::inverse(transform);
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = glm::vec3(transform[3]);
        // Camera base class has no near/far — keep previous values

        BeginSceneCommon();
    }

    void Renderer3D::UploadFogVolumes(const FogVolumesUBOData& data)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.SceneEffectsGPU.FogVolumesData = data;
    }
} // namespace OloEngine
