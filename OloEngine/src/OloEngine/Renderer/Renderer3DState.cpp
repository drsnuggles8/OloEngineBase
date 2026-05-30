#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine
{
    namespace
    {
        bool s_ForceDisableCulling = false;
    } // namespace

    bool Renderer3D::IsShadowPassAvailable()
    {
        return s_Data.Pipeline->FrameCorePasses.Shadow != nullptr;
    }

    void Renderer3D::AddMeshShadowCaster(RendererID vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                         RendererID shadowVaoID, const BoundingBox& worldBounds)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddMeshCaster(vaoID, indexCount, baseIndex, transform, shadowVaoID, worldBounds);
        }
    }

    void Renderer3D::AddSkinnedShadowCaster(RendererID vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                            u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddSkinnedCaster(vaoID, indexCount, baseIndex, transform, boneBufferOffset, boneCount, worldBounds);
        }
    }

    void Renderer3D::AddTerrainShadowCaster(RendererID vaoID, u32 indexCount, u32 patchVertexCount,
                                            const glm::mat4& transform, RendererID heightmapTextureID,
                                            const ShaderBindingLayout::TerrainUBO& terrainUBO)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddTerrainCaster(vaoID, indexCount, patchVertexCount, transform, heightmapTextureID, terrainUBO);
        }
    }

    void Renderer3D::AddVoxelShadowCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddVoxelCaster(vaoID, indexCount, transform);
        }
    }

    void Renderer3D::AddFoliageShadowCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddFoliageCaster(renderer, depthShader, time);
        }
    }

    void Renderer3D::SetLight(const Light& light)
    {
        s_Data.SceneLight = light;
    }

    void Renderer3D::SetViewPosition(const glm::vec3& position)
    {
        s_Data.ViewPos = position;
    }

    void Renderer3D::SetCameraClipPlanes(f32 nearClip, f32 farClip)
    {
        s_Data.CameraNearClip = nearClip;
        s_Data.CameraFarClip = farClip;
    }

    void Renderer3D::UploadMultiLightUBO(const UBOStructures::MultiLightUBO& data, i32 activeLightCount)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.MultiLightBuffer)
        {
            // Clamp to valid range to prevent buffer overrun
            activeLightCount = std::clamp(activeLightCount, 0, static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS));

            // Only upload header (16 bytes) + active lights to minimize CPU→GPU transfer
            constexpr u32 headerSize = 4 * sizeof(i32); // LightCount, MaxLights, ShadowCasterCount, DirectionalLightCount
            const u32 uploadSize = headerSize + static_cast<u32>(activeLightCount) * static_cast<u32>(sizeof(UBOStructures::MultiLightData));

            // Ensure the GPU header reflects the clamped count (the caller may
            // have set data.LightCount to a value exceeding MAX_LIGHTS).
            if (data.LightCount != activeLightCount)
            {
                UBOStructures::MultiLightUBO temp = data;
                temp.LightCount = activeLightCount;
                s_Data.MultiLightBuffer->SetData(&temp, uploadSize);
            }
            else
            {
                s_Data.MultiLightBuffer->SetData(&data, uploadSize);
            }
        }
    }

    void Renderer3D::UploadLightProbeData(const ShaderBindingLayout::LightProbeVolumeUBO& uboData,
                                          const void* shData, u32 shDataSize)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.LightProbeVolumeUBO)
        {
            s_Data.LightProbeVolumeUBO->SetData(&uboData, ShaderBindingLayout::LightProbeVolumeUBO::GetSize());
        }

        if (shData && shDataSize > 0)
        {
            if (!s_Data.LightProbeSHBuffer || s_Data.LightProbeSHBuffer->GetSize() < shDataSize)
            {
                s_Data.LightProbeSHBuffer = StorageBuffer::Create(shDataSize, ShaderBindingLayout::SSBO_LIGHT_PROBES);
            }
            s_Data.LightProbeSHBuffer->SetData(shData, shDataSize);
        }
        // When no SH data is provided, the UBO's Enabled field should already be 0,
        // causing the shader to early-out. The SSBO remains bound from init (zeroed).
    }

    void Renderer3D::SetGlobalIBL(RendererID irradianceMapID, RendererID prefilterMapID,
                                  RendererID brdfLutMapID, RendererID environmentMapID,
                                  f32 iblIntensity)
    {
        s_Data.GlobalIrradianceMapID = irradianceMapID;
        s_Data.GlobalPrefilterMapID = prefilterMapID;
        s_Data.GlobalBRDFLutMapID = brdfLutMapID;
        s_Data.GlobalEnvironmentMapID = environmentMapID;
        s_Data.GlobalIBLIntensity = iblIntensity;
    }

    void Renderer3D::ClearGlobalIBL()
    {
        s_Data.GlobalIrradianceMapID = 0;
        s_Data.GlobalPrefilterMapID = 0;
        s_Data.GlobalBRDFLutMapID = 0;
        s_Data.GlobalEnvironmentMapID = 0;
        s_Data.GlobalIBLIntensity = 1.0f;
    }

    void Renderer3D::SetSceneLights(const Ref<Scene>& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !s_Data.MultiLightBuffer)
        {
            return;
        }

        // Collect lights from the scene
        constexpr u32 MAX_POINT_LIGHTS = 16;
        constexpr u32 MAX_SPOT_LIGHTS = 8;

        u32 pointLightCount = 0;
        u32 spotLightCount = 0;

        // TODO: Create proper multi-light UBO structure and populate it
        // For now, we'll just gather the count and warn if limits exceeded

        // Count directional lights
        auto dirLightView = scene->GetAllEntitiesWith<DirectionalLightComponent>();
        u32 dirLightCount = 0;
        for ([[maybe_unused]] auto entity : dirLightView)
        {
            ++dirLightCount;
        }

        // Count and collect point lights
        auto pointLightView = scene->GetAllEntitiesWith<PointLightComponent>();
        for ([[maybe_unused]] auto entity : pointLightView)
        {
            ++pointLightCount;
        }

        // Count and collect spot lights
        auto spotLightView = scene->GetAllEntitiesWith<SpotLightComponent>();
        for ([[maybe_unused]] auto entity : spotLightView)
        {
            ++spotLightCount;
        }

        // Warn if limits exceeded
        if (pointLightCount > MAX_POINT_LIGHTS)
        {
            OLO_CORE_WARN("Scene contains {} point lights, but max is {}. Only first {} will be rendered.",
                          pointLightCount, MAX_POINT_LIGHTS, MAX_POINT_LIGHTS);
        }

        if (spotLightCount > MAX_SPOT_LIGHTS)
        {
            OLO_CORE_WARN("Scene contains {} spot lights, but max is {}. Only first {} will be rendered.",
                          spotLightCount, MAX_SPOT_LIGHTS, MAX_SPOT_LIGHTS);
        }

        // TODO: Populate multi-light UBO with actual light data
        // This requires matching the shader's light structure
    }

    void Renderer3D::EnableFrustumCulling(bool enable)
    {
        s_Data.FrustumCullingEnabled = enable;
    }

    bool Renderer3D::IsFrustumCullingEnabled()
    {
        if (s_ForceDisableCulling)
            return false;
        return s_Data.FrustumCullingEnabled;
    }

    void Renderer3D::EnableDynamicCulling(bool enable)
    {
        s_Data.DynamicCullingEnabled = enable;
    }

    bool Renderer3D::IsDynamicCullingEnabled()
    {
        if (s_ForceDisableCulling)
            return false;
        return s_Data.DynamicCullingEnabled;
    }

    const Frustum& Renderer3D::GetViewFrustum()
    {
        return s_Data.ViewFrustum;
    }

    void Renderer3D::SetForceDisableCulling(bool disable)
    {
        s_ForceDisableCulling = disable;
        if (disable)
        {
            EnableFrustumCulling(false);
            EnableDynamicCulling(false);
            OLO_CORE_WARN("Renderer3D: All culling forcibly disabled for debugging!");
        }
    }

    bool Renderer3D::IsForceDisableCulling()
    {
        return s_ForceDisableCulling;
    }

    void Renderer3D::EnableDepthPrepass(bool enable)
    {
        OLO_PROFILE_FUNCTION();
        s_Data.DepthPrepassEnabled = enable;
    }

    bool Renderer3D::IsDepthPrepassEnabled()
    {
        return s_Data.DepthPrepassEnabled;
    }

    void Renderer3D::EnableOcclusionCulling(bool enable)
    {
        OLO_PROFILE_FUNCTION();
        s_Data.OcclusionCullingEnabled = enable;
        if (enable)
        {
            auto& queryPool = OcclusionQueryPool::GetInstance();
            if (!queryPool.IsInitialized())
            {
                queryPool.Initialize(1024);
            }
            if (auto& culler = OcclusionCuller::GetInstance(); !culler.IsInitialized())
            {
                culler.Initialize();
            }
            OcclusionStateManager::GetInstance().SetMaxQueries(queryPool.GetMaxQueries());
        }
    }

    bool Renderer3D::IsOcclusionCullingEnabled()
    {
        return s_Data.OcclusionCullingEnabled;
    }

    Renderer3D::Statistics& Renderer3D::GetStats()
    {
        return s_Data.Stats;
    }

    void Renderer3D::ResetStats()
    {
        s_Data.Stats.Reset();
    }

    void Renderer3D::ApplyRendererSettings()
    {
        OLO_PROFILE_FUNCTION();
        auto& settings = s_Data.Settings;

        // Clamp MSAA sample count to what the driver advertises. The combo
        // box exposes 1/2/4/8 but older or mobile GPUs may cap at 4. Logs
        // on clamp so users notice rather than silently dropping samples.
        if (const u32 maxSamples = GetMaxMSAASamples(); maxSamples > 0)
        {
            const u32 requested = settings.Deferred.MSAASampleCount;
            if (requested > maxSamples)
            {
                OLO_CORE_WARN("Renderer3D: MSAASampleCount={} exceeds driver cap {}. Clamping.",
                              requested, maxSamples);
                settings.Deferred.MSAASampleCount = maxSamples;
            }
        }

        // Detect a RenderingPath switch and rebuild the graph topology
        // BEFORE touching the Forward+ mode / culling toggles below so that
        // downstream code always observes a graph whose registered pass
        // list matches the active path. RGraph must exist — if we're called
        // pre-Init (defensive), skip the rebuild and let SetupRenderGraph
        // do the first configure.
        // Also detect ActiveAOTechnique changes so that
        // the conditional AO-pass registration in ConfigureRenderGraph
        // reflects the newly selected technique without waiting for a path
        // switch.
        const bool pathChanged = settings.Path != s_Data.ActiveGraphPath;
        const bool aoTechniqueChanged =
            s_Data.PostProcess.ActiveAOTechnique != s_Data.ActiveGraphAOTechnique;
        if (s_Data.RGraph && (pathChanged || aoTechniqueChanged))
        {
            ConfigureRenderGraph(settings.Path);
        }

        // Sync culling toggles
        EnableFrustumCulling(settings.FrustumCullingEnabled);
        EnableOcclusionCulling(settings.OcclusionCullingEnabled);

        // Sync Forward+ settings
        auto& fplus = s_Data.ForwardPlus;
        switch (settings.Path)
        {
            case RenderingPath::Forward:
                if (settings.ForwardPlusAutoSwitch)
                {
                    fplus.SetMode(ForwardPlusMode::Auto);
                    fplus.SetLightCountThreshold(settings.ForwardPlusLightThreshold);
                    fplus.SetLightCountThresholdDown(settings.ForwardPlusLightThresholdDown);
                }
                else
                {
                    fplus.SetMode(ForwardPlusMode::Never);
                }
                break;
            case RenderingPath::ForwardPlus:
                fplus.SetMode(ForwardPlusMode::Always);
                break;
            case RenderingPath::Deferred:
                // Deferred reuses the Forward+ tile-culling compute to build
                // per-tile light lists; the G-Buffer lighting shader samples
                // those same SSBOs. Forcing ForwardPlusMode::Always here
                // guarantees the tile classification runs every frame while
                // the scene pipeline is operating in Deferred mode.
                fplus.SetMode(ForwardPlusMode::Always);
                break;
        }

        // Forward+ compute culling requires the depth pre-pass.
        // Include the Auto case: when Forward+ can dynamically activate,
        // the depth buffer must already be available for the culling dispatch.
        // Deferred likewise needs the depth buffer before the lighting pass —
        // the G-Buffer depth attachment is populated by the scene pass MRT
        // writes, and the depth-prepass additionally supports Forward+ tile
        // culling reused by DeferredLightingPass.
        const bool effectiveDepthPrepass = settings.DepthPrepassEnabled || (settings.Path == RenderingPath::ForwardPlus) || (settings.Path == RenderingPath::Deferred) || (settings.Path == RenderingPath::Forward && settings.ForwardPlusAutoSwitch);
        EnableDepthPrepass(effectiveDepthPrepass);

        fplus.SetTileSize(settings.ForwardPlusTileSize);
        fplus.SetDebugVisualization(settings.ForwardPlusDebugHeatmap);
    }

    bool Renderer3D::IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        BoundingSphere sphere = mesh->GetTransformedBoundingSphere(transform);
        sphere.Radius *= 1.3f;

        return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
    }

    bool Renderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        BoundingSphere expandedSphere = sphere;
        expandedSphere.Radius *= 1.3f;

        return s_Data.ViewFrustum.IsBoundingSphereVisible(expandedSphere);
    }

    bool Renderer3D::IsVisibleInFrustum(const BoundingBox& box)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        return s_Data.ViewFrustum.IsBoundingBoxVisible(box);
    }
} // namespace OloEngine
