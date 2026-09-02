#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

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

    void Renderer3D::AddMeshShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                         RHI::ResourceHandle shadowVaoID, const BoundingBox& worldBounds, bool twoSided)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddMeshCaster(vaoID, indexCount, baseIndex, transform, shadowVaoID, worldBounds, twoSided);
        }
    }

    void Renderer3D::AddSkinnedShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                            u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddSkinnedCaster(vaoID, indexCount, baseIndex, transform, boneBufferOffset, boneCount, worldBounds);
        }
    }

    void Renderer3D::AddTerrainShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 patchVertexCount,
                                            const glm::mat4& transform, RHI::ResourceHandle heightmapTextureID,
                                            const ShaderBindingLayout::TerrainUBO& terrainUBO)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            // The depth pass has no virtual-texture branch and no VT resources
            // bound, so the enable flag is cleared here rather than at each of
            // the three submit sites. Clearing it in ONE place is what keeps a
            // future depth-shader change from finding a stale "VT is on" in the
            // block it shares with the lit shaders (issue #715).
            ShaderBindingLayout::TerrainUBO depthUBO = terrainUBO;
            depthUBO.VTParams2 = glm::vec4(0.0f);
            shadowPass->AddTerrainCaster(vaoID, indexCount, patchVertexCount, transform, heightmapTextureID, depthUBO);
        }
    }

    void Renderer3D::AddVoxelShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, const glm::mat4& transform,
                                          u32 instanceCount)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddVoxelCaster(vaoID, indexCount, transform, instanceCount);
        }
    }

    void Renderer3D::AddFoliageShadowCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time)
    {
        if (auto shadowPass = s_Data.Pipeline->FrameCorePasses.Shadow; shadowPass)
        {
            shadowPass->AddFoliageCaster(renderer, depthShader, time);
        }
    }

    void Renderer3D::SubmitDDGIVolume(const DDGIVolumeDesc& desc)
    {
        if (auto ddgiPass = s_Data.Pipeline->FrameCorePasses.DDGIProbeUpdate; ddgiPass)
        {
            ddgiPass->SubmitVolume(desc);
        }
    }

    bool Renderer3D::IsDDGICollectingCasters()
    {
        if (s_Data.AuxCasterSink != nullptr)
        {
            return true;
        }
        auto ddgiPass = s_Data.Pipeline->FrameCorePasses.DDGIProbeUpdate;
        return ddgiPass && ddgiPass->WantsCasters();
    }

    void Renderer3D::AddDDGICaster(const DDGIMeshCaster& caster)
    {
        if (s_Data.AuxCasterSink != nullptr)
        {
            s_Data.AuxCasterSink->push_back(caster);
        }
        // Only feed the DDGI pass when IT asked for casters this frame — the
        // aux sink alone must not push bake-time geometry into the pass's
        // per-frame list (it would double up with the frame's own traversal).
        if (auto ddgiPass = s_Data.Pipeline->FrameCorePasses.DDGIProbeUpdate;
            ddgiPass && ddgiPass->WantsCasters())
        {
            ddgiPass->AddMeshCaster(caster);
        }
    }

    void Renderer3D::SetAuxCasterSink(std::vector<DDGIMeshCaster>* sink)
    {
        s_Data.AuxCasterSink = sink;
    }

    DDGIProbeUpdatePass* Renderer3D::GetDDGIPass()
    {
        return s_Data.Pipeline ? s_Data.Pipeline->FrameCorePasses.DDGIProbeUpdate.Raw() : nullptr;
    }

    void Renderer3D::SetViewPosition(const glm::vec3& position)
    {
        s_Data.ViewPos = position;
        // Keep the culling camera following unless it is frozen (issue #726).
        // Scene pushes the position here AFTER BeginScene, so the mirror in
        // RefreshCullingCamera() alone would leave the cull position one call
        // behind on the runtime-camera path.
        if (!s_Data.CullingCameraFrozen)
            s_Data.CullViewPos = position;
    }

    void Renderer3D::SetPrimaryDirectionalLightDirection(const glm::vec3& direction)
    {
        s_Data.PrimaryDirectionalLightDir = direction;
    }

    void Renderer3D::SetCameraClipPlanes(f32 nearClip, f32 farClip)
    {
        s_Data.CameraNearClip = nearClip;
        s_Data.CameraFarClip = farClip;
        if (!s_Data.CullingCameraFrozen)
        {
            s_Data.CullNearClip = nearClip;
            s_Data.CullFarClip = farClip;
        }
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

            // Camera-relative (issue #429): light positions are world-space, but
            // the lit pass evaluates lightDir = lightPos - worldPos with a
            // render-relative worldPos, so shift the active lights' positions by
            // the render origin. Directional lights (Position.w == 0) do not use
            // Position, so shifting their xyz is harmless. Always route through a
            // temp copy (the shift and/or the count fixup both need one); at the
            // origin the subtraction is a no-op and the bytes are identical to
            // before. Point/spot Position.w (the type tag) is preserved.
            const glm::vec3 origin = s_Data.RenderOrigin;
            UBOStructures::MultiLightUBO temp = data;
            temp.LightCount = activeLightCount;
            for (i32 i = 0; i < activeLightCount; ++i)
            {
                temp.Lights[i].Position.x -= origin.x;
                temp.Lights[i].Position.y -= origin.y;
                temp.Lights[i].Position.z -= origin.z;
            }
            s_Data.MultiLightBuffer->SetData(&temp, uploadSize);
        }
    }

    void Renderer3D::UploadLightProbeData(const ShaderBindingLayout::LightProbeVolumeUBO& uboData,
                                          const void* shData, u32 shDataSize)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.LightProbeVolumeUBO)
        {
            // Camera-relative (issue #429): the probe-grid lookup indexes by
            // (worldPos - BoundsMin)/spacing with a render-relative worldPos, so
            // shift the world-space AABB corners by the render origin. The grid
            // spacing/dimensions are unchanged, so every probe's implicit sample
            // position shifts consistently and the indexing is invariant. No-op
            // at the origin.
            ShaderBindingLayout::LightProbeVolumeUBO shifted = uboData;
            shifted.BoundsMin -= glm::vec4(s_Data.RenderOrigin, 0.0f);
            shifted.BoundsMax -= glm::vec4(s_Data.RenderOrigin, 0.0f);
            s_Data.LightProbeVolumeUBO->SetData(&shifted, ShaderBindingLayout::LightProbeVolumeUBO::GetSize());
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

    void Renderer3D::UploadLightmapData(const ShaderBindingLayout::LightmapUBO& uboData,
                                        const Ref<Texture2DArray>& atlas)
    {
        OLO_PROFILE_FUNCTION();

        // Dirty guard: the caller uploads every frame, the values change only
        // on resolve/toggle. All LightmapUBO bytes are named members (explicit
        // pad), so memcmp sees no indeterminate padding.
        if (s_Data.LightmapUBO &&
            (!s_Data.LightmapUBOUploaded || std::memcmp(&s_Data.LastLightmapUBO, &uboData, sizeof(uboData)) != 0))
        {
            s_Data.LightmapUBO->SetData(&uboData, ShaderBindingLayout::LightmapUBO::GetSize());
            s_Data.LastLightmapUBO = uboData;
            s_Data.LightmapUBOUploaded = true;
        }

        // Publish the atlas at TEX_LIGHTMAP for slot-based AND heap-bindless
        // readers alike (LightmapSampling.glsl is a shared include, so its
        // sampler stays slot-based and this call is what keeps both routes fed —
        // the same mechanism the DDGI atlases use). The white placeholder keeps
        // the sampler valid when no bake exists; uboData.Enabled == 0 gates any
        // actual sampling then. Deliberately NOT dirty-guarded: the publish is
        // a CPU table update + bind, and re-publishing every frame self-heals
        // across heap/topology resets (see render-pipeline-caches).
        // The placeholder is an ARRAY, not s_Data.WhiteTexture: TEX_LIGHTMAP's
        // sampler is a sampler2DArray since issue #868, and binding a
        // GL_TEXTURE_2D there is undefined rather than merely wrong-looking.
        const Ref<Texture2DArray>& bound = atlas ? atlas : s_Data.LightmapPlaceholderAtlas;
        if (bound)
        {
            // NullSamplerKind::Texture2DArray, not the Texture2D default: the
            // fallback descriptor minted for an invalid handle has to match the
            // shader's sampler2DArray, or a Vulkan bindless read of an
            // unpublished slot samples a 2D null view (issue #868).
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_LIGHTMAP, bound->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent, {},
                                                     RHI::NullSamplerKind::Texture2DArray);
        }
    }

    void Renderer3D::SetGlobalIBL(RHI::ResourceHandle irradianceMap, RHI::ResourceHandle prefilterMap,
                                  RHI::ResourceHandle brdfLutMap, RHI::ResourceHandle environmentMap,
                                  f32 iblIntensity)
    {
        s_Data.GlobalIrradianceMapID = irradianceMap;
        s_Data.GlobalPrefilterMapID = prefilterMap;
        s_Data.GlobalBRDFLutMapID = brdfLutMap;
        s_Data.GlobalEnvironmentMapID = environmentMap;
        s_Data.GlobalIBLIntensity = iblIntensity;
    }

    void Renderer3D::ClearGlobalIBL()
    {
        s_Data.GlobalIrradianceMapID = {};
        s_Data.GlobalPrefilterMapID = {};
        s_Data.GlobalBRDFLutMapID = {};
        s_Data.GlobalEnvironmentMapID = {};
        s_Data.GlobalIBLIntensity = 1.0f;
    }

    void Renderer3D::OverrideGlobalIrradiance(RHI::ResourceHandle irradianceMap, f32 iblIntensity)
    {
        s_Data.GlobalIrradianceMapID = irradianceMap;
        s_Data.GlobalIBLIntensity = iblIntensity;
    }

    void Renderer3D::UploadUnderwaterFogUBO(const UnderwaterFogUBOData& data)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.UnderwaterFogBuffer)
        {
            // Camera-relative (issue #429): the underwater-fog / caustic pass
            // reconstructs a world position from the scene depth via its OWN
            // world-space InverseViewProjection. Because the depth was written by
            // the render-relative geometry (ndc = VP_rel * worldPos_rel),
            //   inverse(VP_world) * ndc == translate(O) * worldPos_rel == worldPos_ABSOLUTE,
            // so the reconstruction lands in absolute world space *for free* — the
            // caustic/god-ray patterns and the water-surface-height compare are
            // therefore correct with NO shift here (keep CameraPos / WaterSurfaceY
            // / InverseViewProjection in world space). Depth-based fog is a
            // difference, so it is invariant either way.
            s_Data.UnderwaterFogBuffer->SetData(&data, UnderwaterFogUBOData::GetSize());
        }
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

    // ---- Observer camera / frozen culling camera (issue #726) --------------

    void Renderer3D::SetCullingCameraFrozen(bool frozen)
    {
        if (frozen == s_Data.CullingCameraFrozen)
            return;

        if (frozen)
        {
            // Snapshot the CURRENT render camera as the culling camera. Doing
            // it here rather than letting the next RefreshCullingCamera() skip
            // its copy means the freeze captures the frame the user was looking
            // at when they pressed the button, not whatever the observer has
            // already drifted to by the next BeginScene.
            s_Data.CullViewMatrix = s_Data.ViewMatrix;
            s_Data.CullProjectionMatrix = s_Data.ProjectionMatrix;
            s_Data.CullViewProjectionMatrix = s_Data.ViewProjectionMatrix;
            s_Data.CullViewPos = s_Data.ViewPos;
            s_Data.CullNearClip = s_Data.CameraNearClip;
            s_Data.CullFarClip = s_Data.CameraFarClip;
            // The retained Hi-Z pyramid stops being regenerated from this point
            // (GenerateOcclusionHZB early-outs while frozen), so the VP that
            // describes it has to be pinned to the same instant. Without this
            // the pyramid is the frozen camera's depth while the reprojection
            // matrix is the observer's, and the occlusion cull rejects a
            // plausible-but-wrong set — the exact failure mode this whole tool
            // exists to make impossible to miss.
            s_Data.CullPrevViewProjectionMatrix = s_Data.PrevViewProjectionMatrix;
        }

        s_Data.CullingCameraFrozen = frozen;
        // Keep the settings bool in step so the checkboxes that read it show the
        // truth after an MCP or code-driven toggle, and so the reconcile at the
        // top of RefreshCullingCamera() does not immediately undo this.
        s_Data.Settings.ObserverCameraEnabled = frozen;

        if (!frozen)
        {
            // Unfreezing: re-mirror immediately so a consumer that reads the
            // culling camera between here and the next BeginScene (an MCP query,
            // an editor panel) never sees the stale frozen values.
            RefreshCullingCamera();
            // The pyramid still holds the frozen camera's depth and
            // CullPrevViewProjectionMatrix still describes it, so the pair stays
            // consistent; EndScene's regeneration resumes this frame and the
            // next frame's cull is live again.
        }

        OLO_CORE_INFO("Renderer3D: culling camera {}", frozen ? "FROZEN (observer camera active)" : "unfrozen");
    }

    bool Renderer3D::IsCullingCameraFrozen()
    {
        return s_Data.CullingCameraFrozen;
    }

    const glm::mat4& Renderer3D::GetCullViewMatrix()
    {
        return s_Data.CullViewMatrix;
    }

    const glm::mat4& Renderer3D::GetCullProjectionMatrix()
    {
        return s_Data.CullProjectionMatrix;
    }

    const glm::mat4& Renderer3D::GetCullViewProjectionMatrix()
    {
        return s_Data.CullViewProjectionMatrix;
    }

    const glm::vec3& Renderer3D::GetCullViewPosition()
    {
        return s_Data.CullViewPos;
    }

    f32 Renderer3D::GetCullNearClip()
    {
        return s_Data.CullNearClip;
    }

    f32 Renderer3D::GetCullFarClip()
    {
        return s_Data.CullFarClip;
    }

    const glm::mat4& Renderer3D::GetCullViewProjectionRelative()
    {
        return s_Data.CullViewProjectionRelative;
    }

    const glm::vec3& Renderer3D::GetCullViewPositionRelative()
    {
        return s_Data.CullViewPosRelative;
    }

    const glm::vec2& Renderer3D::GetCullProjParams()
    {
        return s_Data.CullProjParams;
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

    void Renderer3D::EnableDepthAwareClusterCulling(bool enable)
    {
        s_Data.DepthAwareClusterCullingEnabled = enable;
    }

    bool Renderer3D::IsDepthAwareClusterCullingEnabled()
    {
        return s_Data.DepthAwareClusterCullingEnabled;
    }

    bool Renderer3D::ComputeSettingsDerivedDepthPrepass()
    {
        // Forward+ compute culling requires the depth pre-pass.
        // Include the Auto case: when Forward+ can dynamically activate,
        // the depth buffer must already be available for the culling dispatch.
        // Deferred likewise needs the depth buffer before the lighting pass —
        // the G-Buffer depth attachment is populated by the scene pass MRT
        // writes, and the depth-prepass additionally supports Forward+ tile
        // culling reused by DeferredLightingPass.
        const auto& settings = s_Data.Settings;
        return settings.DepthPrepassEnabled || (settings.Path == RenderingPath::ForwardPlus) || (settings.Path == RenderingPath::Deferred) || (settings.Path == RenderingPath::Forward && settings.ForwardPlusAutoSwitch);
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

    void Renderer3D::EnableHZBOcclusionCulling(bool enable)
    {
        // GPU Hi-Z occlusion cull for instanced static geometry (#431). When
        // toggled off, the persistent pyramid is invalidated so a later
        // re-enable starts cleanly from this frame's depth rather than stale
        // data — and the GPU cull immediately falls back to frustum-only.
        s_Data.HZBOcclusionCullingEnabled = enable;
        if (!enable)
            s_Data.OcclusionHZBValid = false;
    }

    bool Renderer3D::IsHZBOcclusionCullingEnabled()
    {
        // Honour the global debug override, same as IsFrustumCullingEnabled() /
        // IsDynamicCullingEnabled() — "all culling forcibly disabled" must also
        // turn off the HZB occlusion path.
        if (s_ForceDisableCulling)
            return false;
        return s_Data.HZBOcclusionCullingEnabled;
    }

    void Renderer3D::SetHZBOcclusionDepthBias(f32 bias)
    {
        s_Data.HZBOcclusionDepthBias = bias;
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
        // Observer camera (#726) first: SetCullingCameraFrozen snapshots the
        // camera on the false->true edge, so it has to see the transition rather
        // than a value that has already been mirrored somewhere else.
        SetCullingCameraFrozen(settings.ObserverCameraEnabled);
        EnableFrustumCulling(settings.FrustumCullingEnabled);
        EnableOcclusionCulling(settings.OcclusionCullingEnabled);
        EnableHZBOcclusionCulling(settings.HZBOcclusionCullingEnabled);

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

        // Depth prepass: settings-derived (see ComputeSettingsDerivedDepthPrepass
        // for why Forward+/Deferred force it on). Shared with the MCP
        // depthprepass lever's 'auto' token, so the two can't drift.
        EnableDepthPrepass(ComputeSettingsDerivedDepthPrepass());

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
