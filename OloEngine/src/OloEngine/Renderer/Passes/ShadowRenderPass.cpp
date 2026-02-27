#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"

namespace OloEngine
{
    ShadowRenderPass::ShadowRenderPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("ShadowRenderPass");
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

    void ShadowRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

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
                m_ShadowFramebuffer->AttachDepthTextureArrayLayer(csmArray->GetRendererID(), cascade);
                RenderCommand::ClearDepthOnly();

                const glm::mat4& lightVP = m_ShadowMap->GetCSMMatrix(cascade);
                RenderCascadeOrFace(lightVP, ShadowPassType::CSM, cascade);
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

    void ShadowRenderPass::RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight)
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
        auto& modelUBO = shadowMap.GetShadowModelUBO();
        modelUBO->Bind();

        // Helper to populate and upload the shadow ModelUBO for a given transform
        auto uploadShadowModelUBO = [&modelUBO](const glm::mat4& worldTransform)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = worldTransform;
            modelData.Normal = glm::mat4(1.0f); // Shadow depth shaders don't use normals
            modelData.EntityID = -1;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            modelUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
        };

        // ── Static meshes ──
        {
            const char* shaderName = (type == ShadowPassType::Point) ? "ShadowDepthPoint" : "ShadowDepth";
            auto shadowShader = Renderer3D::GetShaderLibrary().Get(shaderName);
            if (shadowShader)
            {
                shadowShader->Bind();
                for (const auto& caster : m_MeshCasters)
                {
                    uploadShadowModelUBO(caster.transform);
                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount);
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
            if (!skinnedShadowShader)
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

                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount);
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
                caster.renderer->SetTime(caster.time);
                caster.renderer->RenderShadows(caster.depthShader);
            }
        }
    }

    // Shadow caster submission methods
    void ShadowRenderPass::AddMeshCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform)
    {
        m_MeshCasters.push_back({ vaoID, indexCount, transform });
    }

    void ShadowRenderPass::AddSkinnedCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform,
                                            u32 boneBufferOffset, u32 boneCount)
    {
        m_SkinnedCasters.push_back({ vaoID, indexCount, transform, boneBufferOffset, boneCount });
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
