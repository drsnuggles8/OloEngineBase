#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"

#include <cstdio>

namespace OloEngine
{
    ShadowRenderPass::ShadowRenderPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("ShadowRenderPass");
    }

    void ShadowRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // Skip declaring writes when shadow rendering is off — otherwise the
        // graph records dependency edges that never materialise and consumers
        // may end up reading uncleared shadow maps. Execute() already gates
        // on the same condition.
        if (!m_ShadowMap || !m_ShadowMap->IsEnabled())
            return;

        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
            {
                if (const auto cascadeView = blackboard.Shadows.ShadowMapCSMCascades[cascade]; cascadeView.IsValid())
                {
                    builder.Write(cascadeView, RGWriteUsage::DepthStencil);
                }
                else
                {
                    builder.Write(blackboard.Shadows.ShadowMapCSM, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(cascade));
                }
            }
        }

        if (blackboard.Shadows.ShadowMapSpot.IsValid())
        {
            for (u32 light = 0; light < ShadowMap::MAX_SPOT_SHADOWS; ++light)
            {
                if (const auto spotLayerView = blackboard.Shadows.ShadowMapSpotLayers[light]; spotLayerView.IsValid())
                {
                    builder.Write(spotLayerView, RGWriteUsage::DepthStencil);
                }
                else
                {
                    builder.Write(blackboard.Shadows.ShadowMapSpot, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(light));
                }
            }
        }

        for (u32 light = 0; light < ShadowMap::MAX_POINT_SHADOWS; ++light)
        {
            const auto& pointHandle = blackboard.Shadows.ShadowMapPoint[light];
            if (!pointHandle.IsValid())
                continue;

            for (u32 face = 0; face < FrameBlackboard::MaxShadowMapCubeFaces; ++face)
            {
                if (const auto faceView = blackboard.Shadows.ShadowMapPointFaces[light][face]; faceView.IsValid())
                {
                    builder.Write(faceView, RGWriteUsage::DepthStencil);
                }
                else
                {
                    RGSubresourceRange faceRange{};
                    faceRange.BaseSlice = face;
                    faceRange.SliceCount = 1u;
                    builder.Write(pointHandle, RGWriteUsage::DepthStencil, faceRange);
                }
            }
        }
    }

    ShadowRenderPass::~ShadowRenderPass() = default;

    void ShadowRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create a depth-only framebuffer. The internal depth texture created by
        // Invalidate() will be replaced per-cascade via AttachDepthTextureArrayLayer.
        FramebufferSpecification shadowSpec;
        shadowSpec.Width = spec.Width;
        shadowSpec.Height = spec.Height;
        shadowSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
        m_ShadowFramebuffer = Framebuffer::Create(shadowSpec);
    }

    void ShadowRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        (void)context;

        const bool hasCasters = !m_MeshCasters.empty() || !m_SkinnedCasters.empty() ||
                                !m_TerrainCasters.empty() || !m_VoxelCasters.empty() ||
                                !m_FoliageCasters.empty();

        if (!m_ShadowMap || !m_ShadowMap->IsEnabled() || !hasCasters)
        {
            if (!m_WarnedOnce && !hasCasters && m_ShadowMap && m_ShadowMap->IsEnabled())
            {
                OLO_CORE_WARN("ShadowRenderPass::Execute skipped: no shadow casters submitted");
                m_WarnedOnce = true;
            }
            // Clear caster lists for next frame
            m_MeshCasters.clear();
            m_SkinnedCasters.clear();
            m_TerrainCasters.clear();
            m_VoxelCasters.clear();
            m_FoliageCasters.clear();
            return;
        }

        if (!m_ShadowFramebuffer)
        {
            OLO_CORE_ERROR("ShadowRenderPass::Execute: Shadow framebuffer not initialized!");
            return;
        }

        const u32 resolution = m_ShadowMap->GetResolution();

        // Save current viewport
        const auto prevViewport = RenderCommand::GetViewport();

        // Bind shadow framebuffer and set viewport to shadow resolution
        m_ShadowFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, resolution, resolution);

        // Render state for shadow rendering: depth test on, depth write on, no color
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetColorMask(false, false, false, false);

        // Use front-face culling during shadow pass to reduce peter-panning
        RenderCommand::EnableCulling();
        RenderCommand::FrontCull();

        // Render CSM cascades
        if (const auto& csmArray = m_ShadowMap->GetCSMTextureArray(); csmArray)
        {
            if (!m_LoggedOnce)
            {
                OLO_CORE_INFO("ShadowRenderPass: Rendering {} CSM cascades, resolution={}, FBO={}, textureID={}",
                              ShadowMap::MAX_CSM_CASCADES, resolution, m_ShadowFramebuffer->GetRendererID(), csmArray->GetRendererID());
                m_LoggedOnce = true;
            }
            for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
            {
                const glm::mat4& lightVP = m_ShadowMap->GetCSMMatrix(cascade);
                const Frustum cascadeFrustum(lightVP);

                // Skip cascade if no bounded casters pass the frustum test and
                // no unbounded casters (terrain, foliage, voxel) exist.
                const bool hasUnbounded = !m_TerrainCasters.empty() ||
                                          !m_FoliageCasters.empty() ||
                                          !m_VoxelCasters.empty();
                if (!hasUnbounded)
                {
                    const bool anyMesh = std::any_of(m_MeshCasters.begin(), m_MeshCasters.end(),
                                                     [&](const ShadowMeshCaster& c)
                                                     { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    const bool anySkinned = !anyMesh && std::any_of(m_SkinnedCasters.begin(), m_SkinnedCasters.end(),
                                                                    [&](const ShadowSkinnedCaster& c)
                                                                    { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    if (!anyMesh && !anySkinned)
                        continue; // No work for this cascade — skip all GL state changes
                }

                m_ShadowFramebuffer->AttachDepthTextureArrayLayer(csmArray->GetRendererID(), cascade);
                RenderCommand::ClearDepthOnly();

                RenderCascadeOrFace(lightVP, ShadowPassType::CSM, cascade, &cascadeFrustum);
            }
        }

        // Render spot light shadows
        if (const auto& spotArray = m_ShadowMap->GetSpotTextureArray(); spotArray)
        {
            const auto spotCount = static_cast<u32>(m_ShadowMap->GetSpotShadowCount());
            for (u32 i = 0; i < spotCount; ++i)
            {
                m_ShadowFramebuffer->AttachDepthTextureArrayLayer(spotArray->GetRendererID(), i);
                RenderCommand::ClearDepthOnly();

                const glm::mat4& lightVP = m_ShadowMap->GetSpotMatrix(i);
                RenderCascadeOrFace(lightVP, ShadowPassType::Spot, i);
            }
        }

        // Render point light shadow cubemaps (6 faces per light)
        if (const auto pointCount = static_cast<u32>(m_ShadowMap->GetPointShadowCount()); pointCount > 0)
        {
            for (u32 light = 0; light < pointCount; ++light)
            {
                const u32 cubemapID = m_ShadowMap->GetPointRendererID(light);
                if (cubemapID == 0)
                {
                    continue;
                }

                for (u32 face = 0; face < 6; ++face)
                {
                    m_ShadowFramebuffer->AttachDepthTextureArrayLayer(cubemapID, face);
                    RenderCommand::ClearDepthOnly();

                    const glm::mat4& faceVP = m_ShadowMap->GetPointFaceMatrix(light, face);
                    RenderCascadeOrFace(faceVP, ShadowPassType::Point, light);
                }
            }
        }

        // Restore state
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::BackCull();
        m_ShadowFramebuffer->Unbind();
        RenderCommand::SetViewport(prevViewport.x, prevViewport.y, prevViewport.width, prevViewport.height);

        // Clear caster lists for next frame (vectors keep their allocation)
        m_MeshCasters.clear();
        m_SkinnedCasters.clear();
        m_TerrainCasters.clear();
        m_VoxelCasters.clear();
        m_FoliageCasters.clear();
    }

    void ShadowRenderPass::RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight,
                                               const Frustum* cullFrustum)
    {
        OLO_PROFILE_FUNCTION();

        auto& shadowMap = Renderer3D::GetShadowMap();

        // Upload light VP to the shadow camera UBO (binding 0)
        auto cameraUBOData = ShaderBindingLayout::CameraUBO{};
        cameraUBOData.ViewProjection = lightVP;
        cameraUBOData.View = glm::mat4(1.0f);
        cameraUBOData.Projection = lightVP;

        if (type == ShadowPassType::Point)
        {
            const auto& params = shadowMap.GetPointShadowParams(layerOrLight);
            cameraUBOData.Position = glm::vec3(params);
            cameraUBOData._padding0 = params.w; // far plane
        }
        else
        {
            cameraUBOData.Position = glm::vec3(0.0f);
            cameraUBOData._padding0 = 0.0f;
        }

        auto& cameraUBO = shadowMap.GetShadowCameraUBO();
        cameraUBO->SetData(&cameraUBOData, ShaderBindingLayout::CameraUBO::GetSize());
        cameraUBO->Bind();

        // Shadow shaders read transforms from the engine-wide InstanceBuffer
        // at SSBO_INSTANCE_DATA = 15 (no more shadow-specific UBO at binding 3).
        // Static mesh casters use the auto-batched path below; the helper lambda
        // covers skinned / terrain / voxel paths where per-caster state (bones,
        // heightmap, terrain UBO) blocks batching.
        auto instanceBuffer = Renderer3D::GetModelInstanceBuffer();
        auto uploadShadowModelUBO = [&instanceBuffer](const glm::mat4& worldTransform)
        {
            if (!instanceBuffer)
                return;
            InstanceData inst;
            inst.Transform = worldTransform;
            inst.Normal = glm::mat4(1.0f);       // Shadow depth shaders don't use normals
            inst.PrevTransform = worldTransform; // shadow casters have no motion-vector use today
            inst.EntityID = -1;
            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();
        };

        // ── Static meshes (auto-batched by shared VAO + index range) ──
        {
            const char* shaderName = (type == ShadowPassType::Point) ? "ShadowDepthPoint" : "ShadowDepth";
            auto shadowShader = Renderer3D::GetShaderLibrary().Get(shaderName);
            if (shadowShader && !m_MeshCasters.empty())
            {
                // Casters sharing (drawVao, indexCount, baseIndex) all read the
                // same submesh range, so they can collapse into a single
                // glDrawElementsInstanced. The shadow VS reads
                // instances[gl_InstanceIndex].Transform from the SSBO.
                struct ShadowMeshBatch
                {
                    RendererID drawVao;
                    u32 indexCount;
                    u32 baseIndex;
                    std::vector<InstanceData> instances;
                };
                thread_local std::vector<ShadowMeshBatch> batches;
                batches.clear();

                for (const auto& caster : m_MeshCasters)
                {
                    if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                        continue;

                    RendererID const drawVao = (caster.shadowVaoID != 0) ? caster.shadowVaoID : caster.vaoID;
                    InstanceData inst;
                    inst.Transform = caster.transform;
                    inst.Normal = glm::mat4(1.0f);
                    inst.PrevTransform = caster.transform;
                    inst.EntityID = -1;

                    auto it = std::find_if(batches.begin(), batches.end(),
                                           [&](const ShadowMeshBatch& b)
                                           { return b.drawVao == drawVao && b.indexCount == caster.indexCount &&
                                                    b.baseIndex == caster.baseIndex; });
                    if (it == batches.end())
                    {
                        batches.push_back({ drawVao, caster.indexCount, caster.baseIndex, { inst } });
                    }
                    else
                    {
                        it->instances.push_back(inst);
                    }
                }

                if (!batches.empty())
                {
                    shadowShader->Bind();
                    // Build the source label once per pass. We tag every
                    // shadow batch with the cascade / light index so the
                    // profiler's "Instanced Draws" tab can show e.g.
                    // "Shadow CSM cascade 1" — making it obvious which
                    // shadow target a given batched draw is filling in.
                    auto& profiler = RendererProfiler::GetInstance();
                    const bool recording = profiler.IsRecordingInstancedDraws();
                    char sourceLabel[64];
                    if (recording)
                    {
                        const char* kind = (type == ShadowPassType::CSM)    ? "CSM cascade"
                                           : (type == ShadowPassType::Spot) ? "Spot light"
                                                                            : "Point light";
                        std::snprintf(sourceLabel, sizeof(sourceLabel), "Shadow %s %u", kind, layerOrLight);
                    }
                    for (const auto& batch : batches)
                    {
                        if (instanceBuffer)
                        {
                            instanceBuffer->Upload(std::span<const InstanceData>(batch.instances.data(),
                                                                                 batch.instances.size()));
                            instanceBuffer->Bind();
                        }
                        // Single-instance groups still go through the instanced
                        // call — gl_InstanceIndex is 0 either way and the
                        // driver handles count==1 cheaply.
                        RenderCommand::DrawIndexedInstancedRaw(batch.drawVao, batch.indexCount, batch.baseIndex,
                                                               static_cast<u32>(batch.instances.size()));
                        if (recording)
                        {
                            // EntityIDs intentionally null — shadow casters
                            // carry raw VAOs + transforms, not entity refs,
                            // so per-instance picking isn't meaningful here.
                            profiler.RecordInstancedDraw(
                                /*meshHandle=*/0,
                                batch.drawVao,
                                batch.indexCount,
                                static_cast<u32>(batch.instances.size()),
                                /*entityIDs=*/nullptr,
                                /*fromAutoBatching=*/true,
                                sourceLabel);
                        }
                    }
                }
            }
        }

        // ── Skinned meshes ──
        if (!m_SkinnedCasters.empty())
        {
            Ref<Shader> skinnedShadowShader;
            if (type == ShadowPassType::Point)
            {
                skinnedShadowShader = Renderer3D::GetShaderLibrary().Get("ShadowDepthPointSkinned");
            }
            else
            {
                skinnedShadowShader = Renderer3D::GetShaderLibrary().Get("ShadowDepthSkinned");
            }
            if (skinnedShadowShader)
            {
                skinnedShadowShader->Bind();
                auto& animUBO = shadowMap.GetShadowAnimationUBO();
                animUBO->Bind();

                for (const auto& caster : m_SkinnedCasters)
                {
                    if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                        continue;
                    uploadShadowModelUBO(caster.transform);

                    if (caster.boneCount > 0)
                    {
                        const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixPtr(caster.boneBufferOffset);
                        if (boneMatrices)
                        {
                            auto count = std::min(caster.boneCount, static_cast<u32>(ShaderBindingLayout::AnimationUBO::MAX_BONES));
                            animUBO->SetData(boneMatrices, count * sizeof(glm::mat4));
                        }
                    }

                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount, caster.baseIndex);
                }
            }
        }

        // ── Terrain patches ──
        if (!m_TerrainCasters.empty())
        {
            const char* terrainDepthName = (type == ShadowPassType::Point) ? "ShadowDepthPoint" : "Terrain_Depth";
            auto terrainDepthShader = Renderer3D::GetShaderLibrary().Get(terrainDepthName);
            if (!terrainDepthShader)
            {
                terrainDepthShader = Renderer3D::GetTerrainDepthShader();
            }
            if (terrainDepthShader)
            {
                terrainDepthShader->Bind();
                auto terrainUBO = Renderer3D::GetTerrainUBO();

                for (const auto& caster : m_TerrainCasters)
                {
                    uploadShadowModelUBO(caster.transform);

                    if (caster.heightmapTextureID != 0)
                    {
                        RenderCommand::BindTexture(ShaderBindingLayout::TEX_TERRAIN_HEIGHTMAP, caster.heightmapTextureID);
                    }

                    if (terrainUBO)
                    {
                        terrainUBO->SetData(&caster.terrainUBO, ShaderBindingLayout::TerrainUBO::GetSize());
                        terrainUBO->Bind();
                    }

                    RenderCommand::DrawIndexedPatchesRaw(caster.vaoID, caster.indexCount, caster.patchVertexCount);
                }
            }
        }

        // ── Voxel meshes ──
        if (!m_VoxelCasters.empty())
        {
            auto voxelDepthShader = Renderer3D::GetVoxelDepthShader();
            if (voxelDepthShader)
            {
                voxelDepthShader->Bind();
                for (const auto& caster : m_VoxelCasters)
                {
                    uploadShadowModelUBO(caster.transform);
                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount);
                }
            }
        }

        // ── Foliage ──
        for (const auto& caster : m_FoliageCasters)
        {
            if (caster.renderer && caster.depthShader)
            {
                caster.depthShader->Bind();
                // Shadow path is depth-only; prev time is irrelevant so mirror current time.
                caster.renderer->SetTime(caster.time, caster.time);
                caster.renderer->RenderShadows(caster.depthShader);
            }
        }
    }

    // Returns true when the caster has valid world bounds AND those bounds lie
    // entirely outside the frustum, meaning it can safely be skipped.
    // Casters with NoBounds (Min.x == FLT_MAX) are never culled.
    bool ShadowRenderPass::ShouldCull(const BoundingBox& worldBounds, const Frustum& frustum)
    {
        if (worldBounds.Min.x >= std::numeric_limits<f32>::max())
            return false; // No bounds provided — always include
        return !frustum.IsBoxVisible(worldBounds.Min, worldBounds.Max);
    }

    // Shadow caster submission methods
    void ShadowRenderPass::AddMeshCaster(RendererID vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                         RendererID shadowVaoID, const BoundingBox& worldBounds)
    {
        m_MeshCasters.push_back({ vaoID, indexCount, baseIndex, transform, shadowVaoID, worldBounds });
    }

    void ShadowRenderPass::AddSkinnedCaster(RendererID vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                            u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds)
    {
        m_SkinnedCasters.push_back({ vaoID, indexCount, baseIndex, transform, boneBufferOffset, boneCount, worldBounds });
    }

    void ShadowRenderPass::AddTerrainCaster(RendererID vaoID, u32 indexCount, u32 patchVertexCount,
                                            const glm::mat4& transform, RendererID heightmapTextureID,
                                            const ShaderBindingLayout::TerrainUBO& terrainUBO)
    {
        m_TerrainCasters.push_back({ vaoID, indexCount, patchVertexCount, transform, heightmapTextureID, terrainUBO });
    }

    void ShadowRenderPass::AddVoxelCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform)
    {
        m_VoxelCasters.push_back({ vaoID, indexCount, transform });
    }

    void ShadowRenderPass::AddFoliageCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time)
    {
        m_FoliageCasters.push_back({ renderer, depthShader, time });
    }

    Ref<Framebuffer> ShadowRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        return m_ShadowFramebuffer;
    }

    void ShadowRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        // Shadow pass resolution is managed by ShadowMap::m_Settings, not the framebuffer spec
        ResizeFramebuffer(width, height);
    }

    void ShadowRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ShadowRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        m_WarnedOnce = false;
        m_LoggedOnce = false;
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            Init(m_FramebufferSpec);
        }
    }
} // namespace OloEngine
