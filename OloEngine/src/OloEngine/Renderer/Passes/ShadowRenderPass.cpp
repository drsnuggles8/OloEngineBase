#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/CameraRelative.h"
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

        // The local-light shadow atlas is one depth target: every prioritised
        // spot / point-face tile renders into sub-rects of it via per-entry
        // viewports, so a single DepthStencil write declaration covers all of
        // them (issue #435).
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            builder.Write(blackboard.Shadows.ShadowMapAtlas, RGWriteUsage::DepthStencil);
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

        // Root-cause early-out for issue #522: when no light requested shadows this
        // frame the CSM/spot/point matrices are stale (identity), so rendering any
        // caster against them is pure waste (GPU + ×N cascade/face re-submission).
        // Scene already skips caster submission in this case, but gate here too so a
        // caster leaking through any other path can never paint a stale shadow map.
        const bool shadowsRequested = m_ShadowMap && m_ShadowMap->AnyShadowsRequested();

        if (!m_ShadowMap || !m_ShadowMap->IsEnabled() || !shadowsRequested || !hasCasters)
        {
            // Only warn about the genuinely suspicious case: shadows enabled AND
            // requested by a light, yet nothing was submitted to cast them.
            if (!m_WarnedOnce && shadowsRequested && !hasCasters && m_ShadowMap && m_ShadowMap->IsEnabled())
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
                    const bool anyMesh = std::ranges::any_of(m_MeshCasters,
                                                             [&](const ShadowMeshCaster& c)
                                                             { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    const bool anySkinned = !anyMesh && std::ranges::any_of(m_SkinnedCasters,
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

        // Render the local-light shadow atlas (issue #435): attach the atlas
        // once, clear it whole (glClear ignores the viewport), then render
        // each entry — a spot tile or one point-light cube face — with the
        // viewport set to its packed sub-rect. Rasterisation is clipped by
        // the viewport, so tiles can't bleed into each other.
        if (const auto& atlas = m_ShadowMap->GetAtlasTextureArray();
            atlas && m_ShadowMap->GetAtlasEntryCount() > 0)
        {
            m_ShadowFramebuffer->AttachDepthTextureArrayLayer(atlas->GetRendererID(), 0);
            // glClear honours the scissor box (not the viewport) — force it
            // off so the whole-atlas clear can't be clipped by leaked state.
            RenderCommand::DisableScissorTest();
            RenderCommand::ClearDepthOnly();

            const u32 entryCount = m_ShadowMap->GetAtlasEntryCount();
            for (u32 entry = 0; entry < entryCount; ++entry)
            {
                const auto& rect = m_ShadowMap->GetAtlasEntryRect(entry);
                if (rect.Size == 0)
                    continue;

                RenderCommand::SetViewport(rect.X, rect.Y, rect.Size, rect.Size);

                const glm::mat4& lightVP = m_ShadowMap->GetAtlasEntryMatrix(entry);
                const Frustum entryFrustum(lightVP);
                RenderCascadeOrFace(lightVP, ShadowPassType::Atlas, entry, &entryFrustum);
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
                                               const Frustum* cullFrustum) const
    {
        OLO_PROFILE_FUNCTION();

        auto& shadowMap = Renderer3D::GetShadowMap();

        // Camera-relative (issue #429): render the shadow map in the same
        // render-relative space as the main pass. lightVP is world-space; shift
        // it to map (worldPos - origin) -> light clip (matching the sampling
        // matrices ShadowMap::UploadUBO shifts by the same origin), and shift
        // the casters below by the same origin. No-op near origin.
        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();
        const glm::mat4 lightVPRel = MakeViewProjectionRelative(lightVP, renderOrigin);

        // Upload light VP to the shadow camera UBO (binding 0)
        auto cameraUBOData = ShaderBindingLayout::CameraUBO{};
        cameraUBOData.ViewProjection = lightVPRel;
        cameraUBOData.View = glm::mat4(1.0f);
        cameraUBOData.Projection = lightVPRel;
        // Caster shaders that reconstruct an absolute world position from the
        // relative one (terrain snow-height displacement in Terrain_Depth.glsl:
        // worldP = (u_Model*pos).xyz + u_RenderOrigin) need the real origin here;
        // left at its 0 default the snow clip-region UV falls outside the map far
        // from origin and the displaced caster geometry no longer matches the lit
        // surface, detaching the shadow. No-op near origin (issue #429).
        cameraUBOData.RenderOrigin = renderOrigin;
        // Every target — CSM cascades and atlas entries alike — renders
        // standard projective depth now (the old linear-distance point-cubemap
        // path died with the shadow atlas, issue #435), so no light position /
        // far plane needs to ride the camera UBO.
        cameraUBOData.Position = glm::vec3(0.0f);
        cameraUBOData._padding0 = 0.0f;

        auto& cameraUBO = shadowMap.GetShadowCameraUBO();
        cameraUBO->SetData(&cameraUBOData, ShaderBindingLayout::CameraUBO::GetSize());
        cameraUBO->Bind();

        // Shadow shaders read transforms from the engine-wide InstanceBuffer
        // at SSBO_INSTANCE_DATA = 15 (no more shadow-specific UBO at binding 3).
        // Static mesh casters use the auto-batched path below; the helper lambda
        // covers skinned / terrain / voxel paths where per-caster state (bones,
        // heightmap, terrain UBO) blocks batching.
        auto instanceBuffer = Renderer3D::GetModelInstanceBuffer();
        auto uploadShadowModelUBO = [&instanceBuffer, &renderOrigin](const glm::mat4& worldTransform)
        {
            if (!instanceBuffer)
                return;
            const glm::mat4 relTransform = MakeModelRelative(worldTransform, renderOrigin);
            InstanceData inst;
            inst.Transform = relTransform;
            inst.Normal = glm::mat4(1.0f);     // Shadow depth shaders don't use normals
            inst.PrevTransform = relTransform; // shadow casters have no motion-vector use today
            inst.EntityID = -1;
            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();
        };

        // ── Static meshes (auto-batched by shared VAO + index range) ──
        {
            auto shadowShader = Renderer3D::GetShaderLibrary().Get("ShadowDepth");
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
                    const glm::mat4 relTransform = MakeModelRelative(caster.transform, renderOrigin);
                    InstanceData inst;
                    inst.Transform = relTransform;
                    inst.Normal = glm::mat4(1.0f);
                    inst.PrevTransform = relTransform;
                    inst.EntityID = -1;

                    auto it = std::ranges::find_if(batches,
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
                        const char* kind = (type == ShadowPassType::CSM) ? "CSM cascade" : "Atlas entry";
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
            Ref<Shader> skinnedShadowShader = Renderer3D::GetShaderLibrary().Get("ShadowDepthSkinned");
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
            auto terrainDepthShader = Renderer3D::GetShaderLibrary().Get("Terrain_Depth");
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
