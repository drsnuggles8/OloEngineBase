#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Asset/AssetManager.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <atomic>

/*
 * POD Command Dispatch System
 *
 * This file resolves POD command data (asset handles, renderer IDs) at dispatch time.
 *
 * Key changes from Ref<T>-based commands:
 * - Asset handles are resolved via AssetManager::GetAsset<T>(handle) if needed
 * - Renderer IDs are used directly for GL resource binding (textures, VAOs)
 * - Bone matrices and transforms are retrieved from FrameDataBuffer using offset+count
 * - POD render state is applied directly (no Ref<RenderState> dereference)
 *
 * Performance considerations:
 * - Shader binding uses cached renderer IDs to avoid redundant binds
 * - Texture binding uses per-slot tracking to minimize bind calls
 * - Asset resolution from handle is only needed when Ref<T> methods are required
 */

namespace OloEngine
{
    struct CommandDispatchData
    {
        Ref<UniformBuffer> CameraUBO = nullptr;
        Ref<UniformBuffer> MaterialUBO = nullptr;
        Ref<UniformBuffer> BoneMatricesUBO = nullptr;
        Ref<UniformBuffer> PrevBoneMatricesUBO = nullptr;
        Ref<InstanceBuffer> ModelInstanceBuffer = nullptr;
        TiledForwardPlus* ForwardPlus = nullptr;
        glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
        glm::mat4 ViewMatrix = glm::mat4(1.0f);
        glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
        // Previous-frame view-projection mirrored from `Renderer3D::s_Data
        // .PrevViewProjectionMatrix` once per `BeginScene`. Used by the
        // Terrain / Voxel / Decal dispatchers that upload the shared
        // CameraUBO themselves (they cannot reach into Renderer3D's private
        // `s_Data`) — previous revisions aliased this slot to the current
        // `ViewProjectionMatrix`, which silently clobbered the true history
        // for any later shader reading the full CameraUBO (TAA velocity
        // reconstruction, motion blur).
        glm::mat4 PrevViewProjectionMatrix = glm::mat4(1.0f);
        glm::vec3 ViewPos = glm::vec3(0.0f);

        u32 CurrentBoundShaderID = 0;
        u32 CurrentBoundVAO = 0;
        u16 LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        u16 LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        std::array<u32, ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS> BoundTextureIDs = { 0 };
        u32 CurrentViewportWidth = 0;
        u32 CurrentViewportHeight = 0;

        // Track currently bound UBO renderer IDs per binding point to avoid
        // redundant glBindBufferBase calls. Indexed by ShaderBindingLayout::UBO_*.
        static constexpr u32 MAX_TRACKED_UBO_BINDINGS = 8;
        std::array<u32, MAX_TRACKED_UBO_BINDINGS> BoundUBOIDs = { 0 };

        // Shadow texture renderer IDs (set per-frame)
        u32 CSMShadowTextureID = 0;
        u32 SpotShadowTextureID = 0;
        // Comparison-OFF raw-depth views of the CSM / spot arrays (PCSS blocker search)
        u32 CSMRawShadowTextureID = 0;
        u32 SpotRawShadowTextureID = 0;
        std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS> PointShadowTextureIDs = { 0 };

        // Snow accumulation depth texture (set per-frame)
        u32 SnowDepthTextureID = 0;

        // Depth prepass override: when true, ApplyPODRenderState forces depth-only state
        bool DepthPrepassActive = false;
        // Color pass of depth prepass: override depth func to GL_LEQUAL + depth mask false
        bool DepthPrepassColorPassActive = false;
        // Water surface-depth capture: forces depth-only state even for the blended
        // water draw so the nearest water surface is written to its own depth target.
        bool WaterDepthCaptureActive = false;

        CommandDispatch::Statistics Stats;
    };

    static CommandDispatchData s_Data;

    void CommandDispatch::InvalidateUBOCache(u32 bindingPoint)
    {
        if (bindingPoint < CommandDispatchData::MAX_TRACKED_UBO_BINDINGS)
        {
            s_Data.BoundUBOIDs[bindingPoint] = 0;
        }
    }

    void CommandDispatch::InvalidateTextureBinding(u32 textureID)
    {
        if (textureID == 0)
            return;
        // Clear every slot that still claims this GL ID. After glDeleteTextures
        // the driver unbinds the texture, but our cached BoundTextureIDs would
        // otherwise still say it's bound — causing the next BindTrackedTexture
        // call (with a recycled GL ID) to skip the actual glBindTextureUnit.
        for (auto& slot : s_Data.BoundTextureIDs)
        {
            if (slot == textureID)
                slot = 0;
        }
    }

    // Conditionally bind a UBO only when the binding point has changed,
    // avoiding redundant glBindBufferBase calls each draw.
    static void BindUBOIfNeeded(u32 bindingPoint, u32 rendererID)
    {
        if (bindingPoint < CommandDispatchData::MAX_TRACKED_UBO_BINDINGS)
        {
            if (s_Data.BoundUBOIDs[bindingPoint] == rendererID)
                return;
            s_Data.BoundUBOIDs[bindingPoint] = rendererID;
        }
        glBindBufferBase(GL_UNIFORM_BUFFER, bindingPoint, rendererID);
    }

    // Conditionally bind a VAO only when it differs from the currently bound one.
    static void BindVAOIfNeeded(u32 vaoID)
    {
        if (s_Data.CurrentBoundVAO != vaoID)
        {
            glBindVertexArray(vaoID);
            s_Data.CurrentBoundVAO = vaoID;
        }
    }

    // Helper to apply POD render state to the renderer API (skips if same index as last)
    static void ApplyPODRenderState(u16 renderStateIndex, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        if (renderStateIndex == INVALID_RENDER_STATE_INDEX)
        {
            // Apply safe defaults so no stale GL state persists
            s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
            static const PODRenderState s_Default{};
            api.SetBlendState(s_Default.blendEnabled);
            api.SetDepthTest(s_Default.depthTestEnabled);
            if (s_Default.depthTestEnabled)
            {
                api.SetDepthFunc(s_Default.depthFunction);
            }
            api.SetDepthMask(s_Default.depthWriteMask);
            api.DisableStencilTest();
            api.DisableCulling();
            api.SetLineWidth(s_Default.lineWidth);
            api.SetPolygonMode(s_Default.polygonFace, s_Default.polygonMode);
            api.DisableScissorTest();
            api.SetColorMask(s_Default.colorMaskR, s_Default.colorMaskG, s_Default.colorMaskB, s_Default.colorMaskA);
            api.SetPolygonOffset(0.0f, 0.0f);
            if (s_Default.multisamplingEnabled)
                api.EnableMultisampling();
            else
                api.DisableMultisampling();

            // During depth prepass, enforce depth-only state even for default render state
            if (s_Data.DepthPrepassActive)
            {
                api.SetColorMask(false, false, false, false);
                api.SetDepthTest(true);
                api.SetDepthMask(true);
                api.SetDepthFunc(GL_LESS);
                api.SetBlendState(false);
            }
            // During color pass of depth prepass, override depth to GL_LEQUAL + no writes
            else if (s_Data.DepthPrepassColorPassActive)
            {
                api.SetDepthFunc(GL_LEQUAL);
                api.SetDepthMask(false);
            }
            else
            {
                // No additional handling required.
            }
            return;
        }

        if (renderStateIndex == s_Data.LastRenderStateIndex)
            return;
        s_Data.LastRenderStateIndex = renderStateIndex;

        const auto& state = FrameDataBufferManager::Get().GetRenderState(renderStateIndex);
        api.SetBlendState(state.blendEnabled);
        if (state.blendEnabled)
        {
            api.SetBlendFunc(state.blendSrcFactor, state.blendDstFactor);
            api.SetBlendEquation(state.blendEquation);
        }

        api.SetDepthTest(state.depthTestEnabled);
        if (state.depthTestEnabled)
        {
            api.SetDepthFunc(state.depthFunction);
        }
        api.SetDepthMask(state.depthWriteMask);

        if (state.stencilEnabled)
            api.EnableStencilTest();
        else
            api.DisableStencilTest();

        if (state.stencilEnabled)
        {
            api.SetStencilFunc(state.stencilFunction, state.stencilReference, state.stencilReadMask);
            api.SetStencilMask(state.stencilWriteMask);
            api.SetStencilOp(state.stencilFail, state.stencilDepthFail, state.stencilDepthPass);
        }

        if (state.cullingEnabled)
            api.EnableCulling();
        else
            api.DisableCulling();

        if (state.cullingEnabled)
        {
            api.SetCullFace(state.cullFace);
        }

        api.SetLineWidth(state.lineWidth);
        api.SetPolygonMode(state.polygonFace, state.polygonMode);

        if (state.scissorEnabled)
            api.EnableScissorTest();
        else
            api.DisableScissorTest();

        if (state.scissorEnabled)
        {
            api.SetScissorBox(state.scissorX, state.scissorY, state.scissorWidth, state.scissorHeight);
        }

        api.SetColorMask(state.colorMaskR, state.colorMaskG, state.colorMaskB, state.colorMaskA);

        // Apply per-attachment color write mask (for MRT: e.g. disable writes to entity-ID/normal attachments)
        // glColorMask above resets all buffers, then glColorMaski selectively disables masked-out ones
        if (state.colorAttachmentWriteMask != 0xFF)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                if (!(state.colorAttachmentWriteMask & (1u << i)))
                {
                    api.SetColorMaskForAttachment(i, false, false, false, false);
                }
            }
        }

        if (state.polygonOffsetEnabled)
            api.SetPolygonOffset(state.polygonOffsetFactor, state.polygonOffsetUnits);
        else
            api.SetPolygonOffset(0.0f, 0.0f);

        if (state.multisamplingEnabled)
            api.EnableMultisampling();
        else
            api.DisableMultisampling();

        // During depth prepass, override to depth-only state after applying
        // the command's full state (so culling, stencil, etc. are still correct).
        // EXCEPTION: transparent objects (blendEnabled) MUST NOT participate
        // in the depth prepass — if they do, they write the prepass depth for
        // their own surface which then occludes later transparent passes.
        // Concretely: the InfiniteGrid (alpha-blended) would write depth at
        // the ground plane, and the WaterRenderPass (running after the scene
        // pass) would then fail its GL_LEQUAL depth test wherever a water
        // trough sits below the grid plane — holes through which the grid
        // became visible in Forward+/Deferred modes (depth prepass on).
        // For transparent commands we disable both color and depth writes so
        // the draw becomes a no-op during the depth prepass; the full command
        // still runs normally in the following color pass.
        if (s_Data.DepthPrepassActive)
        {
            if (state.blendEnabled)
            {
                api.SetColorMask(false, false, false, false);
                api.SetDepthTest(false);
                api.SetDepthMask(false);
                api.SetBlendState(false);
                api.SetStencilMask(0); // Transparent draws must not touch the stencil buffer during the depth prepass.
            }
            else
            {
                api.SetColorMask(false, false, false, false);
                api.SetDepthTest(true);
                api.SetDepthMask(true);
                api.SetDepthFunc(GL_LESS);
                api.SetBlendState(false);
                api.SetStencilMask(0); // Depth-prepass opaques emit depth only — leave stencil alone.
            }
        }
        // During color pass of depth prepass, override depth to GL_LEQUAL + no writes
        else if (s_Data.DepthPrepassColorPassActive)
        {
            api.SetDepthFunc(GL_LEQUAL);
            api.SetDepthMask(false);
        }
        else
        {
            // No additional handling required.
        }

        // Water surface-depth capture: force depth-only even though water is
        // blended, so the nearest water surface lands in the dedicated depth target.
        if (s_Data.WaterDepthCaptureActive)
        {
            api.SetColorMask(false, false, false, false);
            api.SetDepthTest(true);
            api.SetDepthMask(true);
            api.SetDepthFunc(GL_LESS);
            api.SetBlendState(false);
        }
    }

    // Helper: Upload material UBO and bind material textures.
    // Skips entirely when materialDataIndex matches the last-used index.
    // Helper: Conditionally bind a texture only when the slot isn't already
    // bound to the same ID, updating tracking and stats.
    static void BindTrackedTexture(RendererID textureID, u32 slot, GLenum target)
    {
        if (textureID != 0 && s_Data.BoundTextureIDs[slot] != textureID)
        {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(target, textureID);
            s_Data.BoundTextureIDs[slot] = textureID;
            ++s_Data.Stats.TextureBinds;
        }
    }

    // Helper: Bind all PBR material textures (albedo, metallic-roughness, normal,
    // AO, emissive, environment cubemap, irradiance, prefilter, BRDF LUT).
    static void BindPBRTextures(const PODMaterialData& mat)
    {
        BindTrackedTexture(mat.albedoMapID, ShaderBindingLayout::TEX_DIFFUSE, GL_TEXTURE_2D);
        BindTrackedTexture(mat.metallicRoughnessMapID, ShaderBindingLayout::TEX_SPECULAR, GL_TEXTURE_2D);
        BindTrackedTexture(mat.normalMapID, ShaderBindingLayout::TEX_NORMAL, GL_TEXTURE_2D);
        BindTrackedTexture(mat.aoMapID, ShaderBindingLayout::TEX_AMBIENT, GL_TEXTURE_2D);
        BindTrackedTexture(mat.emissiveMapID, ShaderBindingLayout::TEX_EMISSIVE, GL_TEXTURE_2D);
        BindTrackedTexture(mat.environmentMapID, ShaderBindingLayout::TEX_ENVIRONMENT, GL_TEXTURE_CUBE_MAP);
        BindTrackedTexture(mat.irradianceMapID, ShaderBindingLayout::TEX_USER_0, GL_TEXTURE_CUBE_MAP);
        BindTrackedTexture(mat.prefilterMapID, ShaderBindingLayout::TEX_USER_1, GL_TEXTURE_CUBE_MAP);
        BindTrackedTexture(mat.brdfLutMapID, ShaderBindingLayout::TEX_USER_2, GL_TEXTURE_2D);
    }

    // Helper: Bind legacy material textures (diffuse, specular).
    static void BindLegacyTextures(const PODMaterialData& mat)
    {
        BindTrackedTexture(mat.diffuseMapID, ShaderBindingLayout::TEX_DIFFUSE, GL_TEXTURE_2D);
        BindTrackedTexture(mat.specularMapID, ShaderBindingLayout::TEX_SPECULAR, GL_TEXTURE_2D);
    }

    static void UploadMaterialState(const PODMaterialData& mat, u16 materialDataIndex)
    {
        OLO_PROFILE_FUNCTION();

        const bool sameIndex = (materialDataIndex == s_Data.LastMaterialDataIndex);
        s_Data.LastMaterialDataIndex = materialDataIndex;

        if (mat.enablePBR)
        {
            if (!sameIndex)
            {
                ShaderBindingLayout::PBRMaterialUBO pbrMaterialData{};
                pbrMaterialData.BaseColorFactor = mat.baseColorFactor;
                pbrMaterialData.EmissiveFactor = mat.emissiveFactor;
                pbrMaterialData.MetallicFactor = mat.metallicFactor;
                pbrMaterialData.RoughnessFactor = mat.roughnessFactor;
                pbrMaterialData.NormalScale = mat.normalScale;
                pbrMaterialData.OcclusionStrength = mat.occlusionStrength;
                pbrMaterialData.UseAlbedoMap = mat.albedoMapID != 0 ? 1 : 0;
                pbrMaterialData.UseNormalMap = mat.normalMapID != 0 ? 1 : 0;
                pbrMaterialData.UseMetallicRoughnessMap = mat.metallicRoughnessMapID != 0 ? 1 : 0;
                pbrMaterialData.UseAOMap = mat.aoMapID != 0 ? 1 : 0;
                pbrMaterialData.UseEmissiveMap = mat.emissiveMapID != 0 ? 1 : 0;
                pbrMaterialData.EnableIBL = mat.enableIBL ? 1 : 0;
                pbrMaterialData.ApplyGammaCorrection = 1;
                pbrMaterialData.AlphaCutoff = mat.alphaCutoff;
                pbrMaterialData.AlphaMode = mat.alphaMode;
                pbrMaterialData.EnableLightProbes = 0;
                pbrMaterialData.IBLIntensity = mat.iblIntensity;

                if (s_Data.MaterialUBO)
                {
                    constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
                    static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch");
                    s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
                    BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
                }
            }
            else if (s_Data.MaterialUBO)
            {
                // Even when material data hasn't changed, re-establish the binding
                // point (other subsystems may have overwritten it).
                BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
            }
            else
            {
                // No additional handling required.
            }

            // Always rebind textures — an intervening pass (e.g. DecalPass)
            // may have changed texture slots since the last material upload.
            BindPBRTextures(mat);
        }
        else
        {
            if (!sameIndex)
            {
                ShaderBindingLayout::MaterialUBO materialData;
                materialData.Ambient = glm::vec4(mat.ambient, 1.0f);
                materialData.Diffuse = glm::vec4(mat.diffuse, 1.0f);
                materialData.Specular = glm::vec4(mat.specular, mat.shininess);
                materialData.Emissive = glm::vec4(0.0f);
                materialData.UseTextureMaps = mat.useTextureMaps ? 1 : 0;
                materialData.AlphaMode = 0;
                materialData.DoubleSided = 0;
                materialData._padding = 0;

                if (s_Data.MaterialUBO)
                {
                    constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
                    static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch");
                    s_Data.MaterialUBO->SetData(&materialData, expectedSize);
                    BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
                }
            }
            else if (s_Data.MaterialUBO)
            {
                // Even when material data hasn't changed, re-establish the
                // binding point — other subsystems (e.g. ParticleBatchRenderer)
                // may have overwritten UBO_MATERIAL.
                BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
            }
            else
            {
                // No additional handling required.
            }

            if (mat.useTextureMaps)
            {
                BindLegacyTextures(mat);
            }
        }
    }

    // Helper: Bind per-frame shadow and snow depth textures (only relevant for PBR paths).
    // Relies on BoundTextureIDs tracking to avoid redundant binds.
    static void BindShadowTextures()
    {
        if (s_Data.CSMShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] != s_Data.CSMShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW, s_Data.CSMShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] = s_Data.CSMShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
        if (s_Data.SpotShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] != s_Data.SpotShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_SPOT, s_Data.SpotShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] = s_Data.SpotShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Comparison-OFF raw-depth views for the PCSS blocker search (plain
        // sampler2DArray at TEX_SHADOW_CSM_RAW / TEX_SHADOW_SPOT_RAW).
        if (s_Data.CSMRawShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_CSM_RAW] != s_Data.CSMRawShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, s_Data.CSMRawShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_CSM_RAW] = s_Data.CSMRawShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
        if (s_Data.SpotRawShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT_RAW] != s_Data.SpotRawShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_SPOT_RAW, s_Data.SpotRawShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT_RAW] = s_Data.SpotRawShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        static constexpr std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS> pointSlots = {
            ShaderBindingLayout::TEX_SHADOW_POINT_0,
            ShaderBindingLayout::TEX_SHADOW_POINT_1,
            ShaderBindingLayout::TEX_SHADOW_POINT_2,
            ShaderBindingLayout::TEX_SHADOW_POINT_3
        };
        for (u32 i = 0; i < UBOStructures::ShadowUBO::MAX_POINT_SHADOWS; ++i)
        {
            if (s_Data.PointShadowTextureIDs[i] != 0)
            {
                if (s_Data.BoundTextureIDs[pointSlots[i]] != s_Data.PointShadowTextureIDs[i])
                {
                    glBindTextureUnit(pointSlots[i], s_Data.PointShadowTextureIDs[i]);
                    s_Data.BoundTextureIDs[pointSlots[i]] = s_Data.PointShadowTextureIDs[i];
                    ++s_Data.Stats.TextureBinds;
                }
            }
        }

        if (s_Data.SnowDepthTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SNOW_DEPTH] != s_Data.SnowDepthTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SNOW_DEPTH, s_Data.SnowDepthTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SNOW_DEPTH] = s_Data.SnowDepthTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
    }

    // Helper: Upload bone matrices from FrameDataBuffer.
    static void UploadBoneMatrices(bool isAnimated, u32 boneBufferOffset, u32 boneCount, u32 prevBoneBufferOffset = UINT32_MAX)
    {
        if (!isAnimated || !s_Data.BoneMatricesUBO || boneCount == 0)
            return;

        using namespace UBOStructures;
        constexpr sizet MAX_BONES = AnimationConstants::MAX_BONES;
        sizet count = glm::min(static_cast<sizet>(boneCount), MAX_BONES);

        if (boneCount > MAX_BONES)
        {
            OLO_CORE_WARN("Animated mesh has {} bones, exceeding limit of {}. Bone matrices will be truncated.",
                          boneCount, MAX_BONES);
        }

        const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixPtr(boneBufferOffset);
        if (boneMatrices)
        {
            s_Data.BoneMatricesUBO->SetData(boneMatrices, static_cast<u32>(count * sizeof(glm::mat4)));
            BindUBOIfNeeded(ShaderBindingLayout::UBO_ANIMATION, s_Data.BoneMatricesUBO->GetRendererID());
        }

        // Previous-frame bone matrices for per-bone velocity. Both the forward
        // PBR_MultiLight_Skinned (scene FB RT3) and deferred PBR_GBuffer_Skinned
        // (G-Buffer RT3) variants bind this UBO at binding 31. Upload only when
        // the caller provided a distinct offset (UINT32_MAX sentinel means
        // "reuse current", which matches static / first-frame animated meshes).
        if (s_Data.PrevBoneMatricesUBO)
        {
            const glm::mat4* prevBoneMatrices = nullptr;
            if (prevBoneBufferOffset != UINT32_MAX)
                prevBoneMatrices = FrameDataBufferManager::Get().GetBoneMatrixPtr(prevBoneBufferOffset);

            // Fall back to current bones whenever the prev stream is missing
            // (sentinel offset OR allocator pointer lookup returned null) so
            // the prev UBO never carries stale bytes from a previous draw —
            // skinned shaders then compute zero bone-motion instead of
            // garbage / leftover entity data.
            const glm::mat4* sourceData = prevBoneMatrices ? prevBoneMatrices : boneMatrices;
            if (sourceData)
            {
                s_Data.PrevBoneMatricesUBO->SetData(sourceData, static_cast<u32>(count * sizeof(glm::mat4)));
                BindUBOIfNeeded(ShaderBindingLayout::UBO_ANIMATION_PREV, s_Data.PrevBoneMatricesUBO->GetRendererID());
            }
        }
    }

    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::COUNT))];

    void CommandDispatch::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        // Initialize dispatch table
        std::ranges::fill(s_DispatchTable, nullptr);

        // State management dispatch functions
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetViewport))] = CommandDispatch::SetViewport;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetClearColor))] = CommandDispatch::SetClearColor;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::Clear))] = CommandDispatch::Clear;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::ClearStencil))] = CommandDispatch::ClearStencil;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetBlendState))] = CommandDispatch::SetBlendState;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetBlendFunc))] = CommandDispatch::SetBlendFunc;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetBlendEquation))] = CommandDispatch::SetBlendEquation;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetDepthTest))] = CommandDispatch::SetDepthTest;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetDepthMask))] = CommandDispatch::SetDepthMask;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetDepthFunc))] = CommandDispatch::SetDepthFunc;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetStencilTest))] = CommandDispatch::SetStencilTest;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetStencilFunc))] = CommandDispatch::SetStencilFunc;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetStencilMask))] = CommandDispatch::SetStencilMask;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetStencilOp))] = CommandDispatch::SetStencilOp;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetCulling))] = CommandDispatch::SetCulling;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetCullFace))] = CommandDispatch::SetCullFace;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetLineWidth))] = CommandDispatch::SetLineWidth;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetPolygonMode))] = CommandDispatch::SetPolygonMode;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetPolygonOffset))] = CommandDispatch::SetPolygonOffset;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetScissorTest))] = CommandDispatch::SetScissorTest;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetScissorBox))] = CommandDispatch::SetScissorBox;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetColorMask))] = CommandDispatch::SetColorMask;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetMultisampling))] = CommandDispatch::SetMultisampling;

        // Draw commands dispatch functions
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::BindDefaultFramebuffer))] = CommandDispatch::BindDefaultFramebuffer;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::BindTexture))] = CommandDispatch::BindTexture;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::SetShaderResource))] = CommandDispatch::SetShaderResource;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawIndexed))] = CommandDispatch::DrawIndexed;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawIndexedInstanced))] = CommandDispatch::DrawIndexedInstanced;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawArrays))] = CommandDispatch::DrawArrays;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawLines))] = CommandDispatch::DrawLines;
        // Higher-level commands
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawMesh))] = CommandDispatch::DrawMesh;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawMeshInstanced))] = CommandDispatch::DrawMeshInstanced;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawSkybox))] = CommandDispatch::DrawSkybox;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawInfiniteGrid))] = CommandDispatch::DrawInfiniteGrid;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawQuad))] = CommandDispatch::DrawQuad;

        // Terrain/Voxel commands
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawTerrainPatch))] = CommandDispatch::DrawTerrainPatch;
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawVoxelMesh))] = CommandDispatch::DrawVoxelMesh;

        // Decal commands
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawDecal))] = CommandDispatch::DrawDecal;

        // Foliage commands
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawFoliageLayer))] = CommandDispatch::DrawFoliageLayer;

        // Water commands
        s_DispatchTable[static_cast<sizet>(std::to_underlying(CommandType::DrawWater))] = CommandDispatch::DrawWater;

        ResetState();

        // Register the dispatch resolver so CommandPacket::Execute() can look
        // up dispatch functions without a compile-time dependency on this TU.
        CommandPacket::SetDispatchResolver(CommandDispatch::GetDispatchFunction);

        // Register view state callbacks so CommandBucket::Execute() can
        // save/bind/restore view state without depending on this TU.
        CommandBucket::SetViewStateCallbacks(
            // Read: capture current global view state
            [](BucketViewState& out)
            {
                out.ViewMatrix = s_Data.ViewMatrix;
                out.ProjectionMatrix = s_Data.ProjectionMatrix;
                out.ViewProjectionMatrix = s_Data.ViewProjectionMatrix;
                out.ViewPosition = s_Data.ViewPos;
            },
            // Write: apply a view state to global state
            [](const BucketViewState& in)
            {
                CommandDispatch::SetViewMatrix(in.ViewMatrix);
                CommandDispatch::SetProjectionMatrix(in.ProjectionMatrix);
                CommandDispatch::SetViewProjectionMatrix(in.ViewProjectionMatrix);
                CommandDispatch::SetViewPosition(in.ViewPosition);
            });

        OLO_CORE_INFO("CommandDispatch: Initialized (UBOs managed by Renderer3D)");
    }

    void CommandDispatch::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        s_Data.CameraUBO.Reset();
        s_Data.MaterialUBO.Reset();
        s_Data.BoneMatricesUBO.Reset();
        s_Data.PrevBoneMatricesUBO.Reset();
        s_Data.ModelInstanceBuffer.Reset();
        s_Data.ForwardPlus = nullptr;
    }

    void CommandDispatch::SetUBOReferences(
        const Ref<UniformBuffer>& cameraUBO,
        const Ref<UniformBuffer>& materialUBO,
        const Ref<UniformBuffer>& boneMatricesUBO,
        const Ref<InstanceBuffer>& modelInstanceBuffer,
        const Ref<UniformBuffer>& prevBoneMatricesUBO,
        TiledForwardPlus* forwardPlus)
    {
        s_Data.CameraUBO = cameraUBO;
        s_Data.MaterialUBO = materialUBO;
        s_Data.BoneMatricesUBO = boneMatricesUBO;
        s_Data.ModelInstanceBuffer = modelInstanceBuffer;
        s_Data.PrevBoneMatricesUBO = prevBoneMatricesUBO;
        s_Data.ForwardPlus = forwardPlus;
    }

    namespace
    {
        // Upload a single InstanceData built from a per-draw ModelUBO struct
        // into the InstanceBuffer at SSBO_INSTANCE_DATA = 15. This is the
        // only model-matrix upload path now that the legacy ModelMatrixUBO
        // at binding 3 has been retired — every mesh / shadow / decal /
        // foliage / water shader reads `instances[gl_InstanceIndex].Transform`
        // (or `instances[v_InstanceIndex]` in fragment) via InstanceBlock.glsl.
        //
        // `instanceBuffer` is taken by non-const reference because OloEngine's
        // Ref<T> propagates const through operator->; a `const Ref<T>&` would
        // make Upload/Bind unreachable here (they mutate the GPU buffer).
        void UploadModelInstance(const ShaderBindingLayout::ModelUBO& modelData,
                                 Ref<InstanceBuffer>& instanceBuffer)
        {
            if (!instanceBuffer)
                return;

            InstanceData inst;
            inst.Transform = modelData.Model;
            inst.Normal = modelData.Normal;
            inst.PrevTransform = modelData.PrevModel;
            inst.EntityID = modelData.EntityID;
            // Color / Custom keep their defaults (white tint, 0) — the explicit
            // instancing path populates them in Phase 3.

            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();
        }
    } // namespace

    void CommandDispatch::BindSceneResources()
    {
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        if (s_Data.ForwardPlus)
        {
            s_Data.ForwardPlus->UploadDisabledUBO();
        }
    }

    void CommandDispatch::ResetState()
    {
        s_Data.CurrentBoundShaderID = 0;
        s_Data.CurrentBoundVAO = 0;
        s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        s_Data.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        s_Data.BoundTextureIDs.fill(0);
        s_Data.CurrentViewportWidth = 0;
        s_Data.CurrentViewportHeight = 0;
        s_Data.BoundUBOIDs.fill(0);
        s_Data.CSMShadowTextureID = 0;
        s_Data.SpotShadowTextureID = 0;
        s_Data.CSMRawShadowTextureID = 0;
        s_Data.SpotRawShadowTextureID = 0;
        s_Data.PointShadowTextureIDs.fill(0);
        s_Data.SnowDepthTextureID = 0;
        s_Data.DepthPrepassActive = false;
        s_Data.DepthPrepassColorPassActive = false;
        s_Data.Stats.Reset();
    }

    void CommandDispatch::InvalidateRenderStateCache()
    {
        s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
    }

    void CommandDispatch::SetDepthPrepassActive(bool active)
    {
        s_Data.DepthPrepassActive = active;
        if (active)
        {
            s_Data.DepthPrepassColorPassActive = false;
        }
        // Invalidate cache so the next command re-applies state
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetDepthPrepassColorPassActive(bool active)
    {
        s_Data.DepthPrepassColorPassActive = active;
        if (active)
        {
            s_Data.DepthPrepassActive = false;
        }
        // Invalidate cache so the next command re-applies state
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetWaterDepthCaptureActive(bool active)
    {
        s_Data.WaterDepthCaptureActive = active;
        // Invalidate so the next command — and the post-capture color command —
        // re-applies state; otherwise a same-render-state water command would
        // early-out and leak the depth-only override into the color pass.
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetViewProjectionMatrix(const glm::mat4& vp)
    {
        s_Data.ViewProjectionMatrix = vp;
    }

    void CommandDispatch::SetViewMatrix(const glm::mat4& view)
    {
        s_Data.ViewMatrix = view;
    }

    void CommandDispatch::SetProjectionMatrix(const glm::mat4& projection)
    {
        s_Data.ProjectionMatrix = projection;
    }

    void CommandDispatch::SetPrevViewProjectionMatrix(const glm::mat4& prevVP)
    {
        s_Data.PrevViewProjectionMatrix = prevVP;
    }

    const glm::mat4& CommandDispatch::GetViewMatrix()
    {
        return s_Data.ViewMatrix;
    }

    const glm::mat4& CommandDispatch::GetProjectionMatrix()
    {
        return s_Data.ProjectionMatrix;
    }

    const glm::mat4& CommandDispatch::GetViewProjectionMatrix()
    {
        return s_Data.ViewProjectionMatrix;
    }

    const glm::vec3& CommandDispatch::GetViewPosition()
    {
        return s_Data.ViewPos;
    }

    void CommandDispatch::SetViewPosition(const glm::vec3& viewPos)
    {
        s_Data.ViewPos = viewPos;
    }

    void CommandDispatch::SetShadowTextureIDs(u32 csmTextureID, u32 spotTextureID,
                                              u32 csmRawTextureID, u32 spotRawTextureID)
    {
        s_Data.CSMShadowTextureID = csmTextureID;
        s_Data.SpotShadowTextureID = spotTextureID;
        s_Data.CSMRawShadowTextureID = csmRawTextureID;
        s_Data.SpotRawShadowTextureID = spotRawTextureID;
    }

    void CommandDispatch::SetPointShadowTextureIDs(const std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS>& pointTextureIDs)
    {
        s_Data.PointShadowTextureIDs = pointTextureIDs;
    }

    void CommandDispatch::SetSnowDepthTextureID(u32 textureID)
    {
        s_Data.SnowDepthTextureID = textureID;
    }

    CommandDispatch::Statistics& CommandDispatch::GetStatistics()
    {
        return s_Data.Stats;
    }

    void CommandDispatch::UpdateMaterialTextureFlag(bool useTextures)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.MaterialUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateMaterialTextureFlag: MaterialUBO not initialized");
            return;
        }

        // Update only the UseTextureMaps field in the material UBO
        i32 flag = useTextures ? 1 : 0;
        u32 offset = static_cast<u32>(offsetof(ShaderBindingLayout::MaterialUBO, UseTextureMaps));

        s_Data.MaterialUBO->SetData(&flag, sizeof(i32), offset);
    }

    CommandDispatchFn CommandDispatch::GetDispatchFunction(CommandType type)
    {
        if (type == CommandType::Invalid || static_cast<sizet>(std::to_underlying(type)) >= static_cast<sizet>(std::to_underlying(CommandType::COUNT)))
        {
            OLO_CORE_ERROR("CommandDispatch::GetDispatchFunction: Invalid command type {}", static_cast<int>(type));
            return nullptr;
        }

        return s_DispatchTable[static_cast<sizet>(std::to_underlying(type))];
    }

    void CommandDispatch::SetViewport(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetViewportCommand*>(data);
        s_Data.CurrentViewportWidth = cmd->width;
        s_Data.CurrentViewportHeight = cmd->height;
        api.SetViewport(cmd->x, cmd->y, cmd->width, cmd->height);
    }

    void CommandDispatch::SetClearColor(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetClearColorCommand*>(data);
        api.SetClearColor(cmd->color);
    }

    void CommandDispatch::Clear(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const ClearCommand*>(data);
        // TODO(olbu): Have separate methods for partial clears
        if (cmd->clearColor || cmd->clearDepth)
            api.Clear();
    }

    void CommandDispatch::ClearStencil(const void* /*data*/, RendererAPI& api)
    {
        api.ClearStencil();
    }

    void CommandDispatch::SetBlendState(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendStateCommand*>(data);
        api.SetBlendState(cmd->enabled);
    }

    void CommandDispatch::SetBlendFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendFuncCommand*>(data);
        api.SetBlendFunc(cmd->sourceFactor, cmd->destFactor);
    }

    void CommandDispatch::SetBlendEquation(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendEquationCommand*>(data);
        api.SetBlendEquation(cmd->mode);
    }

    void CommandDispatch::SetDepthTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthTestCommand*>(data);
        api.SetDepthTest(cmd->enabled);
    }

    void CommandDispatch::SetDepthMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthMaskCommand*>(data);
        api.SetDepthMask(cmd->writeMask);
    }

    void CommandDispatch::SetDepthFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthFuncCommand*>(data);
        api.SetDepthFunc(cmd->function);
    }

    void CommandDispatch::SetStencilTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilTestCommand*>(data);
        if (cmd->enabled)
            api.EnableStencilTest();
        else
            api.DisableStencilTest();
    }

    void CommandDispatch::SetStencilFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilFuncCommand*>(data);
        api.SetStencilFunc(cmd->function, cmd->reference, cmd->mask);
    }

    void CommandDispatch::SetStencilMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilMaskCommand*>(data);
        api.SetStencilMask(cmd->mask);
    }

    void CommandDispatch::SetStencilOp(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilOpCommand*>(data);
        api.SetStencilOp(cmd->stencilFail, cmd->depthFail, cmd->depthPass);
    }

    void CommandDispatch::SetCulling(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetCullingCommand*>(data);
        if (cmd->enabled)
            api.EnableCulling();
        else
            api.DisableCulling();
    }

    void CommandDispatch::SetCullFace(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetCullFaceCommand*>(data);
        api.SetCullFace(cmd->face);
    }

    void CommandDispatch::SetLineWidth(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetLineWidthCommand*>(data);
        api.SetLineWidth(cmd->width);
    }

    void CommandDispatch::SetPolygonMode(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetPolygonModeCommand*>(data);
        api.SetPolygonMode(cmd->face, cmd->mode);
    }

    void CommandDispatch::SetPolygonOffset(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetPolygonOffsetCommand*>(data);
        if (cmd->enabled)
            api.SetPolygonOffset(cmd->factor, cmd->units);
        else
            api.SetPolygonOffset(0.0f, 0.0f);
    }

    void CommandDispatch::SetScissorTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetScissorTestCommand*>(data);
        if (cmd->enabled)
            api.EnableScissorTest();
        else
            api.DisableScissorTest();
    }

    void CommandDispatch::SetScissorBox(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetScissorBoxCommand*>(data);
        api.SetScissorBox(cmd->x, cmd->y, cmd->width, cmd->height);
    }

    void CommandDispatch::SetColorMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetColorMaskCommand*>(data);
        api.SetColorMask(cmd->red, cmd->green, cmd->blue, cmd->alpha);
    }

    void CommandDispatch::SetMultisampling(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetMultisamplingCommand*>(data);
        if (cmd->enabled)
            api.EnableMultisampling();
        else
            api.DisableMultisampling();
    }

    void CommandDispatch::BindDefaultFramebuffer(const void* /*data*/, RendererAPI& api)
    {
        api.BindDefaultFramebuffer();
    }

    void CommandDispatch::BindTexture(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const BindTextureCommand*>(data);
        api.BindTexture(cmd->slot, cmd->textureID);
    }

    void CommandDispatch::SetShaderResource(const void* data, RendererAPI& /*api*/)
    {
        auto const* cmd = static_cast<const SetShaderResourceCommand*>(data);

        auto* registry = ShaderResourceRegistry::Find(cmd->shaderID);
        if (registry)
        {
            bool success = registry->SetResource(cmd->resourceName, cmd->resourceInput);
            if (!success)
            {
                OLO_CORE_WARN("Failed to set shader resource '{0}' for shader ID {1}",
                              cmd->resourceName, cmd->shaderID);
            }
        }
        else
        {
            OLO_CORE_WARN("No registry found for shader ID {0} when setting resource '{1}'",
                          cmd->shaderID, cmd->resourceName);
        }
    }

    void CommandDispatch::DrawIndexed(const void* data, [[maybe_unused]] RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedCommand*>(data);

        if (cmd->vertexArrayID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexed: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), cmd->indexType, nullptr);
    }

    void CommandDispatch::DrawIndexedInstanced(const void* data, [[maybe_unused]] RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedInstancedCommand*>(data);

        if (cmd->vertexArrayID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexedInstanced: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw instanced
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), cmd->indexType, nullptr, static_cast<GLsizei>(cmd->instanceCount));
    }

    void CommandDispatch::DrawArrays(const void* data, [[maybe_unused]] RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawArraysCommand*>(data);

        if (cmd->vertexArrayID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawArrays: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw arrays
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawArrays(cmd->primitiveType, 0, static_cast<GLsizei>(cmd->vertexCount));
    }

    void CommandDispatch::DrawLines(const void* data, [[maybe_unused]] RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawLinesCommand*>(data);

        if (cmd->vertexArrayID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawLines: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw lines
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(cmd->vertexCount));
    }

    void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        auto const* cmd = static_cast<const DrawMeshCommand*>(data);

        // Resolve material data from table
        const auto& mat = FrameDataBufferManager::Get().GetMaterialData(cmd->materialDataIndex);

        // Validate POD renderer IDs
        if (cmd->vertexArrayID == 0 || mat.shaderRendererID == 0)
        {
            if (static std::atomic<u64> s_InvalidDrawMeshLogCount{ 0 }; s_InvalidDrawMeshLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
            {
                OLO_CORE_WARN("CommandDispatch::DrawMesh: Skipping draw with invalid IDs (VAO={}, Shader={})",
                              cmd->vertexArrayID, mat.shaderRendererID);
            }
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != mat.shaderRendererID)
        {
            glUseProgram(mat.shaderRendererID);
            s_Data.CurrentBoundShaderID = mat.shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // During depth prepass, only the model matrix and bones are needed
        // (vertex transform). Skip material, textures, normal matrix, and light UBOs.
        if (s_Data.DepthPrepassActive)
        {
            // Camera UBO is still needed for vertex transform (u_ViewProjection)
            if (s_Data.CameraUBO)
            {
                BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
            }

            if (s_Data.ModelInstanceBuffer)
            {
                ShaderBindingLayout::ModelUBO modelData;
                modelData.Model = cmd->transform;
                modelData.Normal = glm::mat4(1.0f); // Not used in depth-only pass
                modelData.EntityID = cmd->entityID;
                modelData._paddingEntity[0] = 0;
                modelData._paddingEntity[1] = 0;
                modelData._paddingEntity[2] = 0;
                modelData.PrevModel = cmd->prevTransform;

                UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
                // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
            }

            // Bone matrices are still needed for skinned mesh vertex positions.
            // prevBoneBufferOffset uses UINT32_MAX as a sentinel meaning "alias current"
            // (static / first-frame / non-Deferred path) — the helper then skips the second
            // upload and the skinned shader reads the same data for both current and prev.
            UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCount, cmd->prevBoneBufferOffset);
        }
        else
        {
            // Camera and Light UBO data is uploaded once per frame during
            // `RenderPipeline::PrepareFrame(...)`, but their
            // binding points may be overwritten by other subsystem UBOs (e.g.
            // ShadowMap creates its own Camera UBO at the same binding point).
            // Re-establish the binding so shaders read the correct scene-camera buffer.
            if (s_Data.CameraUBO)
            {
                BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
            }

            // Update model matrix UBO
            if (s_Data.ModelInstanceBuffer)
            {
                ShaderBindingLayout::ModelUBO modelData;
                modelData.Model = cmd->transform;
                modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
                modelData.EntityID = cmd->entityID;
                modelData._paddingEntity[0] = 0;
                modelData._paddingEntity[1] = 0;
                modelData._paddingEntity[2] = 0;
                modelData.PrevModel = cmd->prevTransform;

                constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
                static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");

                UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
                // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
            }

            // Material UBO + texture bindings (skipped when material unchanged)
            UploadMaterialState(mat, cmd->materialDataIndex);

            // Shadow/snow textures (per-frame, outside material diffing)
            if (mat.enablePBR)
                BindShadowTextures();

            // Bone matrices
            UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCount, cmd->prevBoneBufferOffset);
        }

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
            return;
        }

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);

        // Conditional rendering: GPU skips draw if occlusion query indicates fully occluded
        bool startedConditionalRender = false;
        if (cmd->occlusionQueryIndex != UINT32_MAX)
        {
            u32 queryID = OcclusionQueryPool::GetInstance().GetQueryID(cmd->occlusionQueryIndex);
            if (queryID != 0)
            {
                api.BeginConditionalRender(queryID);
                startedConditionalRender = true;
            }
        }

        // Use baseIndex offset for multi-submesh MeshSources sharing a single IBO
        const void* indexOffset = reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd->baseIndex) * sizeof(u32));

        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, indexOffset);
        ++s_Data.Stats.DrawCalls;

        if (startedConditionalRender)
        {
            ++s_Data.Stats.ConditionalDraws;
            api.EndConditionalRender();
        }
    }

    void CommandDispatch::DrawMeshInstanced(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        auto const* cmd = static_cast<const DrawMeshInstancedCommand*>(data);

        // Resolve material data from table
        const auto& mat = FrameDataBufferManager::Get().GetMaterialData(cmd->materialDataIndex);

        // Validate POD renderer IDs
        if (cmd->vertexArrayID == 0 || mat.shaderRendererID == 0)
        {
            if (static std::atomic<u64> s_InvalidDrawMeshInstancedLogCount{ 0 }; s_InvalidDrawMeshInstancedLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
            {
                OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Skipping draw with invalid IDs (VAO={}, Shader={})",
                              cmd->vertexArrayID, mat.shaderRendererID);
            }
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != mat.shaderRendererID)
        {
            glUseProgram(mat.shaderRendererID);
            s_Data.CurrentBoundShaderID = mat.shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Camera UBO: re-bind in case a prior pass (e.g. ShadowMap)
        // overwrote the binding point.  Mirrors the logic in DrawMesh's color path.
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Material UBO + texture bindings (skipped when material unchanged)
        UploadMaterialState(mat, cmd->materialDataIndex);

        // GPU-frustum-cull fast path: the cull compute already wrote
        // compacted survivors to `cullOutputInstanceBufferID` and the
        // surviving count into `cullIndirectBufferID`. Skip the FrameDataBuffer
        // -> InstanceData scratch loop and the upload; bind the pre-populated
        // output buffer at SSBO_INSTANCE_DATA and draw indirect.
        if (const bool useGPUCull = cmd->cullIndirectBufferID != 0 && cmd->cullOutputInstanceBufferID != 0; useGPUCull)
        {
            // Rebind slot 15 to the per-submission output buffer. The engine-
            // wide `s_Data.ModelInstanceBuffer` is unchanged so it can be
            // reused by subsequent CPU-path draws in the same frame.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_INSTANCE_DATA,
                             cmd->cullOutputInstanceBufferID);

            // Shadow/snow textures (per-frame, outside material diffing)
            if (mat.enablePBR)
                BindShadowTextures();

            // Bone matrices (no-op for non-animated GPU-cull submissions)
            UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCountPerInstance);

            if (cmd->indexCount == 0)
            {
                OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced (GPU cull): No indices to draw");
                return;
            }

            BindVAOIfNeeded(cmd->vertexArrayID);
            ++s_Data.Stats.DrawCalls;
            api.DrawElementsIndirectRaw(cmd->vertexArrayID, cmd->cullIndirectBufferID);

            // Profiler stats — we DON'T know the surviving instance count
            // without a CPU readback (which would stall the GPU pipeline),
            // so we record `transformCount` (the pre-cull count) as both
            // `InstancesRendered` and as the "Instanced Draws" tab payload.
            // The over-report is bounded by the cull's input size and keeps
            // the counters stable across frames where cull ratios vary.
            auto& profiler = RendererProfiler::GetInstance();
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancedDrawCalls, 1);
            const u32 preCullCount = cmd->transformCount;
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancesRendered, preCullCount);
            if (preCullCount > 1)
                profiler.IncrementCounter(RendererProfiler::MetricType::InstancesBatched, preCullCount - 1);
            profiler.IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, (cmd->indexCount / 3u) * preCullCount);
            profiler.IncrementCounter(RendererProfiler::MetricType::VerticesRendered, cmd->indexCount * preCullCount);

            // Surface this draw in the "Instanced Draws" tab so the user
            // can see GPU-culled submissions alongside CPU-batched ones.
            // EntityID stream is intentionally null — the GPU cull doesn't
            // know which input instances survived without a readback, so
            // the tab shows the pre-cull count with "(no entity-ID stream)"
            // rather than a bogus per-instance breakdown.
            if (profiler.IsRecordingInstancedDraws())
            {
                profiler.RecordInstancedDraw(
                    static_cast<u64>(cmd->meshHandle),
                    cmd->vertexArrayID,
                    cmd->indexCount,
                    preCullCount,
                    /*entityIDs=*/nullptr,
                    /*fromAutoBatching=*/false,
                    "Scene (GPU cull)");
            }
            return;
        }

        // Pack the per-instance transforms (and prev-frame transforms) into the
        // engine's ModelInstanceBuffer SSBO at binding 15. Shaders read each
        // instance via `instances[gl_InstanceIndex].Transform` etc. — see
        // include/InstanceBlock_Vertex.glsl. The legacy `u_ModelMatrices[]`
        // uniform-array path is dead since the migration off ModelMatrices UBO;
        // no production shader declares those uniforms anymore.
        constexpr sizet maxInstances = CommandBucketConfig{}.MaxMeshInstances;
        sizet instanceCount = static_cast<sizet>(cmd->transformCount);
        if (instanceCount > maxInstances)
        {
            OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Too many instances ({}). Only first {} will be rendered.",
                          instanceCount, maxInstances);
            instanceCount = maxInstances;
        }

        auto& frameBuffer = FrameDataBufferManager::Get();
        const glm::mat4* transforms = frameBuffer.GetTransformPtr(cmd->transformBufferOffset);
        const glm::mat4* prevTransforms = nullptr;
        if (cmd->prevTransformBufferOffset != UINT32_MAX)
            prevTransforms = frameBuffer.GetTransformPtr(cmd->prevTransformBufferOffset);
        if (!prevTransforms)
            prevTransforms = transforms; // Aliasing matches the non-instanced path's zero-velocity convention.
        const i32* entityIDs = nullptr;
        if (cmd->entityIDBufferOffset != UINT32_MAX)
            entityIDs = frameBuffer.GetEntityIDPtr(cmd->entityIDBufferOffset);
        const glm::vec4* colors = nullptr;
        if (cmd->colorBufferOffset != UINT32_MAX)
            colors = frameBuffer.GetColorPtr(cmd->colorBufferOffset);
        const f32* customs = nullptr;
        if (cmd->customBufferOffset != UINT32_MAX)
            customs = frameBuffer.GetCustomPtr(cmd->customBufferOffset);

        if (transforms && s_Data.ModelInstanceBuffer)
        {
            // Thread-local scratch — heap-backed so MaxMeshInstances can scale
            // to thousands without blowing the stack (16384 * 224 B = 3.5 MB
            // would be a hard stack overflow on Windows's default 1 MB).
            // `vector::resize` only grows; subsequent calls in the same thread
            // reuse the existing allocation.
            thread_local std::vector<InstanceData> scratch;
            if (scratch.size() < instanceCount)
                scratch.resize(instanceCount);

            for (sizet i = 0; i < instanceCount; ++i)
            {
                InstanceData& inst = scratch[i];
                inst.Transform = transforms[i];
                inst.Normal = glm::transpose(glm::inverse(transforms[i]));
                inst.PrevTransform = prevTransforms[i];
                // Per-source EntityID survives the N-into-1 batch collapse via
                // FrameDataBuffer's EntityID stream — CommandBucket::BatchCommands
                // writes one entry per source DrawMeshCommand, and the fragment
                // shader's flat `v_InstanceIndex` varying selects the right
                // entry (see InstanceBlock.glsl). Fallback to -1 when the
                // stream wasn't allocated (alloc-failure path) keeps picking
                // deterministic.
                inst.EntityID = entityIDs ? entityIDs[i] : -1;
                inst.Color = colors ? colors[i] : glm::vec4(1.0f);
                inst.Custom = customs ? customs[i] : 0.0f;
            }
            const std::span<const InstanceData> instances(scratch.data(), instanceCount);
            s_Data.ModelInstanceBuffer->Upload(instances);
            s_Data.ModelInstanceBuffer->Bind();
        }

        // Shadow/snow textures (per-frame, outside material diffing)
        if (mat.enablePBR)
            BindShadowTextures();

        // Bone matrices
        UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCountPerInstance);

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: No indices to draw");
            return;
        }

        // Bind VAO (cached) and draw instanced
        BindVAOIfNeeded(cmd->vertexArrayID);
        ++s_Data.Stats.DrawCalls;
        const void* indexOffset = reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd->baseIndex) * sizeof(u32));
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, indexOffset, static_cast<GLsizei>(instanceCount));

        // RendererProfiler: surface the batching savings. One instanced draw
        // covers `instanceCount` entities; `InstancesBatched` reports the
        // savings vs naive submission so a scene with 100 trees shows up as
        // "1 instanced draw, 100 instances, 99 batched" instead of being
        // invisible in the regular DrawCalls counter.
        //
        // Triangle / vertex counters get the *post-instance-multiplication*
        // totals — without this, "Triangles per draw call" in the perf
        // overlay reads as ~3 for any instanced draw regardless of count,
        // making the "Low triangles per draw call" warning fire on
        // perfectly-batched scenes. Mirrors what OpenGLRendererAPI::Draw-
        // IndexedInstanced does for the non-Command dispatch path.
        auto& profiler = RendererProfiler::GetInstance();
        profiler.IncrementCounter(RendererProfiler::MetricType::InstancedDrawCalls, 1);
        profiler.IncrementCounter(RendererProfiler::MetricType::InstancesRendered, static_cast<u32>(instanceCount));
        if (instanceCount > 1)
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancesBatched, static_cast<u32>(instanceCount - 1));
        const u32 instCount32 = static_cast<u32>(instanceCount);
        profiler.IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, (cmd->indexCount / 3u) * instCount32);
        profiler.IncrementCounter(RendererProfiler::MetricType::VerticesRendered, cmd->indexCount * instCount32);

        // Per-call breakdown for the "which entities collapsed together?" view
        // in the profiler UI. Recording is opt-in (toggle in the panel) so
        // most frames pay only the bool check. `fromAutoBatching` is true when
        // CommandBucket collapsed N source DrawMeshCommands into this packet
        // (entityID stream populated by BatchCommands) and false for explicit
        // InstancedMeshComponent submissions (entityID stream populated by
        // Renderer3D::DrawMeshInstanced(span<InstanceData>)).
        if (profiler.IsRecordingInstancedDraws())
        {
            const bool fromAutoBatching = (cmd->entityIDBufferOffset != UINT32_MAX) && (instanceCount > 1);
            profiler.RecordInstancedDraw(
                static_cast<u64>(cmd->meshHandle),
                cmd->vertexArrayID,
                cmd->indexCount,
                static_cast<u32>(instanceCount),
                entityIDs,
                fromAutoBatching);
        }
    }

    void CommandDispatch::DrawSkybox(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawSkyboxCommand*>(data);

        // Validate POD renderer IDs
        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0 || cmd->skyboxTextureID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawSkybox: Invalid vertex array ID, shader ID, or skybox texture ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind skybox shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Re-establish camera UBO binding (may be overwritten by shadow pass)
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Bind skybox cubemap texture using renderer ID directly
        if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->skyboxTextureID)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_ENVIRONMENT);
            glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->skyboxTextureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = cmd->skyboxTextureID;
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);

        // Update statistics
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawQuad(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawQuadCommand*>(data);

        // Validate POD renderer IDs
        if (cmd->quadVAID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawQuad: Invalid vertex array ID or shader ID");
            return;
        }

        if (cmd->textureID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawQuad: Missing texture for quad");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Update model matrix UBO
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = -1;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // static quad: no motion contribution

            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
        }

        // Bind texture using renderer ID directly
        if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->textureID)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_DIFFUSE);
            glBindTexture(GL_TEXTURE_2D, cmd->textureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->textureID;
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw quad
        BindVAOIfNeeded(cmd->quadVAID);
        ++s_Data.Stats.DrawCalls;
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    }

    void CommandDispatch::DrawInfiniteGrid(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawInfiniteGridCommand*>(data);

        // Validate POD renderer IDs
        if (cmd->quadVAOID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawInfiniteGrid: Invalid VAO ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind grid shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Note: Grid shader reads view/projection from Camera UBO (binding 0)
        // Re-establish the binding (may be overwritten by shadow pass)
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Set grid scale uniform if the shader supports it
        if (GLint gridScaleLoc = glGetUniformLocation(cmd->shaderRendererID, "u_GridScale"); gridScaleLoc != -1)
        {
            glUniform1f(gridScaleLoc, cmd->gridScale);
        }

        // Bind fullscreen quad VAO (cached) and draw
        BindVAOIfNeeded(cmd->quadVAOID);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawTerrainPatch(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawTerrainPatchCommand*>(data);

        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawTerrainPatch: Invalid vertex array ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload camera UBO
        if (s_Data.CameraUBO)
        {
            ShaderBindingLayout::CameraUBO cameraData{};
            cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
            cameraData.View = s_Data.ViewMatrix;
            cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
            cameraData.Position = s_Data.ViewPos;
            cameraData._padding0 = 0.0f;
            // Use the true previous-frame VP propagated from
            // `Renderer3D::BeginScene` — earlier revisions aliased the
            // current-frame VP here, which silently clobbered history for
            // every subsequent consumer of CameraUBO (TAA velocity
            // reconstruction, motion blur). Terrain itself doesn't emit
            // object motion; per-draw "no rigid motion" is handled via
            // `ModelUBO::PrevModel` below, not the shared CameraUBO.
            cameraData.PrevViewProjection = s_Data.PrevViewProjectionMatrix;
            s_Data.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Upload model matrix UBO
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // terrain: routed through ForwardOverlayPass, no motion tracking
            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
            // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
        }

        // Upload terrain UBO (per-chunk data with tess factors)
        if (auto terrainUBO = Renderer3D::GetTerrainUBO(); terrainUBO)
        {
            terrainUBO->SetData(&cmd->terrainUBOData, ShaderBindingLayout::TerrainUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_TERRAIN, terrainUBO->GetRendererID());
        }

        // Bind terrain textures
        if (cmd->heightmapTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_HEIGHTMAP, cmd->heightmapTextureID);
        }
        if (cmd->splatmapTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_SPLATMAP, cmd->splatmapTextureID);
        }
        if (cmd->splatmap1TextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_SPLATMAP_1, cmd->splatmap1TextureID);
        }
        if (cmd->albedoArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_ALBEDO_ARRAY, cmd->albedoArrayTextureID);
        }
        if (cmd->normalArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_NORMAL_ARRAY, cmd->normalArrayTextureID);
        }
        if (cmd->armArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_ARM_ARRAY, cmd->armArrayTextureID);
        }

        // Bind shadow textures (terrain PBR needs shadows too)
        if (s_Data.CSMShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] != s_Data.CSMShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW, s_Data.CSMShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] = s_Data.CSMShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
        if (s_Data.SpotShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] != s_Data.SpotShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_SPOT, s_Data.SpotShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] = s_Data.SpotShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
        // PCSS raw-depth views (terrain PBR / voxel use soft shadows too).
        if (s_Data.CSMRawShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_CSM_RAW] != s_Data.CSMRawShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, s_Data.CSMRawShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_CSM_RAW] = s_Data.CSMRawShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }
        if (s_Data.SpotRawShadowTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT_RAW] != s_Data.SpotRawShadowTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_SPOT_RAW, s_Data.SpotRawShadowTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT_RAW] = s_Data.SpotRawShadowTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind snow depth texture for accumulation displacement
        if (s_Data.SnowDepthTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SNOW_DEPTH] != s_Data.SnowDepthTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_SNOW_DEPTH, s_Data.SnowDepthTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SNOW_DEPTH] = s_Data.SnowDepthTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind VAO (cached) and draw with GL_PATCHES
        BindVAOIfNeeded(cmd->vertexArrayID);
        glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(cmd->patchVertexCount));
        glDrawElements(GL_PATCHES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawVoxelMesh(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawVoxelMeshCommand*>(data);

        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawVoxelMesh: Invalid vertex array ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload camera UBO
        if (s_Data.CameraUBO)
        {
            ShaderBindingLayout::CameraUBO cameraData{};
            cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
            cameraData.View = s_Data.ViewMatrix;
            cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
            cameraData.Position = s_Data.ViewPos;
            cameraData._padding0 = 0.0f;
            // True previous-frame VP from Renderer3D::BeginScene — never
            // alias the current VP into this slot (breaks TAA / motion
            // blur for every consumer of the shared CameraUBO).
            cameraData.PrevViewProjection = s_Data.PrevViewProjectionMatrix;
            s_Data.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Upload model matrix UBO
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // voxel: routed through ForwardOverlayPass, no motion tracking
            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
            // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
        }

        // Bind textures for triplanar sampling
        if (cmd->albedoArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_ALBEDO_ARRAY, cmd->albedoArrayTextureID);
        }
        if (cmd->normalArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_NORMAL_ARRAY, cmd->normalArrayTextureID);
        }
        if (cmd->armArrayTextureID != 0)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_TERRAIN_ARM_ARRAY, cmd->armArrayTextureID);
        }

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawDecal(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawDecalCommand*>(data);

        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawDecal: Invalid vertex array ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader (cached). DecalRenderPass may have installed an OIT
        // override on the packet itself (oitProgramOverride) -- substitute
        // the Decal_OIT program when set so forward decal commands target the
        // graph-owned OIT MRT layout without requiring resubmission of the
        // bucket. Reading the override from the command keeps the queue
        // stateless and replay-safe.
        u32 decalProgramID = (cmd->oitProgramOverride != 0)
                                 ? cmd->oitProgramOverride
                                 : cmd->shaderRendererID;
        if (s_Data.CurrentBoundShaderID != decalProgramID)
        {
            glUseProgram(decalProgramID);
            s_Data.CurrentBoundShaderID = decalProgramID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload model UBO
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->decalTransform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->decalTransform));
            modelData.EntityID = cmd->entityID;
            // Decals don't currently track per-frame transform history — alias
            // current into PrevModel so motion-vector outputs see zero rigid
            // motion instead of reading the zero-initialised identity that
            // `modelData{}` would otherwise leave, which produces bogus
            // per-fragment velocity for every decal under TAA/motion blur.
            modelData.PrevModel = cmd->decalTransform;
            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
            // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
        }

        // Upload decal UBO
        if (auto decalUBO = Renderer3D::GetDecalUBO(); decalUBO)
        {
            ShaderBindingLayout::DecalUBO decalData{};
            decalData.InverseDecalTransform = cmd->inverseDecalTransform;
            decalData.InverseViewProjection = cmd->inverseViewProjection;
            decalData.DecalColor = cmd->decalColor;
            decalData.DecalParams = cmd->decalParams;
            decalUBO->SetData(&decalData, ShaderBindingLayout::DecalUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_DECAL, decalUBO->GetRendererID());
        }

        // Bind albedo texture (with redundancy check)
        if (cmd->albedoTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] != cmd->albedoTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_USER_0, cmd->albedoTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] = cmd->albedoTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind optional decal normal + RMA textures (used by Decal_GBuffer_Normal
        // and Decal_GBuffer_RMA variants). Unused modes pass 0 and the slot is
        // left alone — the variant shader only samples the slot it needs.
        if (cmd->normalTextureID != 0 &&
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] != cmd->normalTextureID)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_USER_1, cmd->normalTextureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] = cmd->normalTextureID;
            ++s_Data.Stats.TextureBinds;
        }
        if (cmd->rmaTextureID != 0 &&
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] != cmd->rmaTextureID)
        {
            glBindTextureUnit(ShaderBindingLayout::TEX_USER_2, cmd->rmaTextureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] = cmd->rmaTextureID;
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw decal cube
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawFoliageLayer(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        const auto* cmd = static_cast<const DrawFoliageLayerCommand*>(data);

        if (!cmd || cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0 || cmd->instanceCount == 0 || cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawFoliageLayer: Invalid foliage command (VAO={}, shader={}, instances={}, indices={})",
                           cmd ? cmd->vertexArrayID : 0, cmd ? cmd->shaderRendererID : 0,
                           cmd ? cmd->instanceCount : 0, cmd ? cmd->indexCount : 0);
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader (cached)
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload model UBO (parent terrain transform)
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->modelTransform;
            modelData.Normal = cmd->normalMatrix;
            modelData.EntityID = cmd->entityID;
            modelData.PrevModel = cmd->modelTransform; // foliage: no per-instance prev history — alias current for zero motion
            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
            // Legacy ModelMatrixUBO binding retired — all shaders now read transforms from the InstanceBuffer SSBO at binding 15.
        }

        // Upload foliage UBO (per-layer parameters)
        if (auto foliageUBO = Renderer3D::GetFoliageUBO(); foliageUBO)
        {
            ShaderBindingLayout::FoliageUBO foliageData{};
            foliageData.Time = cmd->time;
            foliageData.WindStrength = cmd->windStrength;
            foliageData.WindSpeed = cmd->windSpeed;
            foliageData.ViewDistance = cmd->viewDistance;
            foliageData.FadeStart = cmd->fadeStart;
            foliageData.AlphaCutoff = cmd->alphaCutoff;
            foliageData.PrevTime = cmd->prevTime;
            foliageData.BaseColor = cmd->baseColor;
            foliageUBO->SetData(&foliageData, ShaderBindingLayout::FoliageUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_FOLIAGE, foliageUBO->GetRendererID());
        }

        // Bind albedo texture (with redundancy check)
        if (cmd->albedoTextureID != 0)
        {
            if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->albedoTextureID)
            {
                glBindTextureUnit(ShaderBindingLayout::TEX_DIFFUSE, cmd->albedoTextureID);
                s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->albedoTextureID;
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind VAO (cached) and draw instanced foliage
        BindVAOIfNeeded(cmd->vertexArrayID);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(cmd->instanceCount));
        ++s_Data.Stats.DrawCalls;
    }
    void CommandDispatch::DrawWater(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        const auto* cmd = static_cast<const DrawWaterCommand*>(data);

        if (!cmd || cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0 || cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawWater: Invalid water command (VAO={}, shader={}, indices={})",
                           cmd ? cmd->vertexArrayID : 0, cmd ? cmd->shaderRendererID : 0,
                           cmd ? cmd->indexCount : 0);
            return;
        }
        // Resolve and apply render state from table (cull, depth, blend enable).
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader (cached).
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload model UBO
        if (s_Data.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->modelTransform;
            modelData.Normal = cmd->normalMatrix;
            modelData.EntityID = cmd->entityID;
            modelData.PrevModel = cmd->modelTransform; // water: surface is animated in-shader; mesh transform stable — alias for zero rigid motion
            UploadModelInstance(modelData, s_Data.ModelInstanceBuffer);
            // Legacy ModelMatrixUBO bind removed — water shader reads transforms from the InstanceBuffer SSBO at binding 15.
        }

        // Upload water UBO
        if (auto waterUBO = Renderer3D::GetWaterUBO(); waterUBO)
        {
            const f32 viewportWidth = static_cast<f32>(s_Data.CurrentViewportWidth);
            const f32 viewportHeight = static_cast<f32>(s_Data.CurrentViewportHeight);

            ShaderBindingLayout::WaterUBO waterData{};
            waterData.WaveParams = cmd->waveParams;
            waterData.WaveDir0 = cmd->waveDir0;
            waterData.WaveDir1 = cmd->waveDir1;
            waterData.WaterColor = cmd->waterColor;
            waterData.WaterDeepColor = cmd->waterDeepColor;
            waterData.VisualParams = cmd->visualParams;
            waterData.NormalMapScroll = cmd->normalMapScroll;
            waterData.NormalMapSpeed = cmd->normalMapSpeed;
            waterData.LightDirection = cmd->lightDirection;
            waterData.ScreenParams = glm::vec4(viewportWidth, viewportHeight,
                                               viewportWidth > 0.0f ? 1.0f / viewportWidth : 0.0f,
                                               viewportHeight > 0.0f ? 1.0f / viewportHeight : 0.0f);
            waterData.DepthRefractionParams = cmd->depthRefractionParams;
            waterData.RefractionColor = cmd->refractionColor;
            waterData.FoamParams = cmd->foamParams;
            waterData.FoamParams2 = cmd->foamParams2;
            waterData.SSSColor = cmd->sssColor;
            waterData.SSRParams = cmd->ssrParams;
            waterData.TessParams = cmd->tessParams;
            waterUBO->SetData(&waterData, ShaderBindingLayout::WaterUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_WATER, waterUBO->GetRendererID());
        }

        // Bind normal map and noise textures (tracked for redundancy elimination and stats)
        BindTrackedTexture(cmd->normalMap0ID, ShaderBindingLayout::TEX_WATER_NORMAL_0, GL_TEXTURE_2D);
        BindTrackedTexture(cmd->normalMap1ID, ShaderBindingLayout::TEX_WATER_NORMAL_1, GL_TEXTURE_2D);
        BindTrackedTexture(cmd->noiseTextureID, ShaderBindingLayout::TEX_WATER_NOISE, GL_TEXTURE_2D);
        BindTrackedTexture(cmd->foamTextureID, ShaderBindingLayout::TEX_WATER_FOAM, GL_TEXTURE_2D);

        // Bind the global environment cubemap for water reflections (binding 9).
        // The water pass doesn't otherwise touch this slot, so set it explicitly
        // instead of relying on a prior pass having left the skybox bound there —
        // without this, grazing-angle water reflects an unbound (black/grey)
        // cubemap and looks see-through rather than reflective. When there's no
        // environment map, deterministically clear the slot rather than leaving a
        // stale cubemap from a previous frame/scene (BindTrackedTexture skips id 0,
        // so clear it directly and update the tracking).
        if (const auto envMapID = Renderer3D::GetGlobalEnvironmentMapID(); envMapID != 0)
        {
            BindTrackedTexture(envMapID, ShaderBindingLayout::TEX_ENVIRONMENT, GL_TEXTURE_CUBE_MAP);
        }
        else if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != 0)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_ENVIRONMENT);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = 0;
        }

        // Bind VAO (cached) and draw water.
        // Water.glsl includes tessellation control / evaluation stages. With
        // TES active, OpenGL requires GL_PATCHES
        // input primitives; issuing GL_TRIANGLES triggers
        // GL_INVALID_OPERATION ("primitive mode mismatch").
        //
        // The user-facing tessellation toggle still works: u_TessParams.x is
        // consumed by TCS to collapse tess factors toward 1.0 when disabled,
        // so we can keep a single, valid primitive mode at draw time.
        BindVAOIfNeeded(cmd->vertexArrayID);
        glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(GL_PATCHES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
        ++s_Data.Stats.DrawCalls;
    }
} // namespace OloEngine
