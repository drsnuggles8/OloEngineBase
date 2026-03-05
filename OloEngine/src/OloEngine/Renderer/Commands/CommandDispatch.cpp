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
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Asset/AssetManager.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

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
        Ref<UniformBuffer> LightUBO = nullptr;
        Ref<UniformBuffer> BoneMatricesUBO = nullptr;
        Ref<UniformBuffer> ModelMatrixUBO = nullptr;
        glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
        glm::mat4 ViewMatrix = glm::mat4(1.0f);
        glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
        Light SceneLight;
        glm::vec3 ViewPos = glm::vec3(0.0f);

        u32 CurrentBoundShaderID = 0;
        u16 LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        u16 LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        std::array<u32, 32> BoundTextureIDs = { 0 };

        // Shadow texture renderer IDs (set per-frame)
        u32 CSMShadowTextureID = 0;
        u32 SpotShadowTextureID = 0;
        std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS> PointShadowTextureIDs = { 0 };

        // Snow accumulation depth texture (set per-frame)
        u32 SnowDepthTextureID = 0;

        CommandDispatch::Statistics Stats;
    };

    static CommandDispatchData s_Data;

    // Helper to apply POD render state to the renderer API (skips if same index as last)
    static void ApplyPODRenderState(u16 renderStateIndex, RendererAPI& api)
    {
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

        if (state.polygonOffsetEnabled)
            api.SetPolygonOffset(state.polygonOffsetFactor, state.polygonOffsetUnits);
        else
            api.SetPolygonOffset(0.0f, 0.0f);

        if (state.multisamplingEnabled)
            api.EnableMultisampling();
        else
            api.DisableMultisampling();
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
        const bool sameIndex = (materialDataIndex == s_Data.LastMaterialDataIndex);
        s_Data.LastMaterialDataIndex = materialDataIndex;

        if (mat.enablePBR)
        {
            if (!sameIndex)
            {
                ShaderBindingLayout::PBRMaterialUBO pbrMaterialData;
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
                pbrMaterialData.AlphaCutoff = 0;

                if (s_Data.MaterialUBO)
                {
                    constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
                    static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch");
                    s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
                }
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
                }
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
    static void UploadBoneMatrices(bool isAnimated, u32 boneBufferOffset, u32 boneCount)
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
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_ANIMATION, s_Data.BoneMatricesUBO->GetRendererID());
        }
    }

    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<sizet>(CommandType::COUNT)];

    void CommandDispatch::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        // Initialize dispatch table
        std::ranges::fill(s_DispatchTable, nullptr);

        // State management dispatch functions
        s_DispatchTable[static_cast<sizet>(CommandType::SetViewport)] = CommandDispatch::SetViewport;
        s_DispatchTable[static_cast<sizet>(CommandType::SetClearColor)] = CommandDispatch::SetClearColor;
        s_DispatchTable[static_cast<sizet>(CommandType::Clear)] = CommandDispatch::Clear;
        s_DispatchTable[static_cast<sizet>(CommandType::ClearStencil)] = CommandDispatch::ClearStencil;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendState)] = CommandDispatch::SetBlendState;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendFunc)] = CommandDispatch::SetBlendFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendEquation)] = CommandDispatch::SetBlendEquation;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthTest)] = CommandDispatch::SetDepthTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthMask)] = CommandDispatch::SetDepthMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthFunc)] = CommandDispatch::SetDepthFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilTest)] = CommandDispatch::SetStencilTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilFunc)] = CommandDispatch::SetStencilFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilMask)] = CommandDispatch::SetStencilMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilOp)] = CommandDispatch::SetStencilOp;
        s_DispatchTable[static_cast<sizet>(CommandType::SetCulling)] = CommandDispatch::SetCulling;
        s_DispatchTable[static_cast<sizet>(CommandType::SetCullFace)] = CommandDispatch::SetCullFace;
        s_DispatchTable[static_cast<sizet>(CommandType::SetLineWidth)] = CommandDispatch::SetLineWidth;
        s_DispatchTable[static_cast<sizet>(CommandType::SetPolygonMode)] = CommandDispatch::SetPolygonMode;
        s_DispatchTable[static_cast<sizet>(CommandType::SetPolygonOffset)] = CommandDispatch::SetPolygonOffset;
        s_DispatchTable[static_cast<sizet>(CommandType::SetScissorTest)] = CommandDispatch::SetScissorTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetScissorBox)] = CommandDispatch::SetScissorBox;
        s_DispatchTable[static_cast<sizet>(CommandType::SetColorMask)] = CommandDispatch::SetColorMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetMultisampling)] = CommandDispatch::SetMultisampling;

        // Draw commands dispatch functions
        s_DispatchTable[static_cast<sizet>(CommandType::BindDefaultFramebuffer)] = CommandDispatch::BindDefaultFramebuffer;
        s_DispatchTable[static_cast<sizet>(CommandType::BindTexture)] = CommandDispatch::BindTexture;
        s_DispatchTable[static_cast<sizet>(CommandType::SetShaderResource)] = CommandDispatch::SetShaderResource;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawIndexed)] = CommandDispatch::DrawIndexed;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawIndexedInstanced)] = CommandDispatch::DrawIndexedInstanced;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawArrays)] = CommandDispatch::DrawArrays;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawLines)] = CommandDispatch::DrawLines;
        // Higher-level commands
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMesh)] = CommandDispatch::DrawMesh;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMeshInstanced)] = CommandDispatch::DrawMeshInstanced;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawSkybox)] = CommandDispatch::DrawSkybox;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawInfiniteGrid)] = CommandDispatch::DrawInfiniteGrid;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawQuad)] = CommandDispatch::DrawQuad;

        // Terrain/Voxel commands
        s_DispatchTable[static_cast<sizet>(CommandType::DrawTerrainPatch)] = CommandDispatch::DrawTerrainPatch;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawVoxelMesh)] = CommandDispatch::DrawVoxelMesh;

        // Decal commands
        s_DispatchTable[static_cast<sizet>(CommandType::DrawDecal)] = CommandDispatch::DrawDecal;

        // Foliage commands
        s_DispatchTable[static_cast<sizet>(CommandType::DrawFoliageLayer)] = CommandDispatch::DrawFoliageLayer;

        s_Data.CurrentBoundShaderID = 0;
        std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
        s_Data.Stats.Reset();

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
        s_Data.LightUBO.Reset();
        s_Data.BoneMatricesUBO.Reset();
        s_Data.ModelMatrixUBO.Reset();
    }

    void CommandDispatch::SetUBOReferences(
        const Ref<UniformBuffer>& cameraUBO,
        const Ref<UniformBuffer>& materialUBO,
        const Ref<UniformBuffer>& lightUBO,
        const Ref<UniformBuffer>& boneMatricesUBO,
        const Ref<UniformBuffer>& modelMatrixUBO)
    {
        s_Data.CameraUBO = cameraUBO;
        s_Data.MaterialUBO = materialUBO;
        s_Data.LightUBO = lightUBO;
        s_Data.BoneMatricesUBO = boneMatricesUBO;
        s_Data.ModelMatrixUBO = modelMatrixUBO;
    }

    void CommandDispatch::ResetState()
    {
        s_Data.CurrentBoundShaderID = 0;
        s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        s_Data.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        s_Data.BoundTextureIDs.fill(0);
        s_Data.CSMShadowTextureID = 0;
        s_Data.SpotShadowTextureID = 0;
        s_Data.PointShadowTextureIDs.fill(0);
        s_Data.SnowDepthTextureID = 0;
        s_Data.Stats.Reset();
    }

    void CommandDispatch::InvalidateRenderStateCache()
    {
        s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
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

    void CommandDispatch::SetSceneLight(const Light& light)
    {
        s_Data.SceneLight = light;
    }

    void CommandDispatch::SetViewPosition(const glm::vec3& viewPos)
    {
        s_Data.ViewPos = viewPos;
    }

    void CommandDispatch::SetShadowTextureIDs(u32 csmTextureID, u32 spotTextureID)
    {
        s_Data.CSMShadowTextureID = csmTextureID;
        s_Data.SpotShadowTextureID = spotTextureID;
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
        if (type == CommandType::Invalid || static_cast<sizet>(type) >= static_cast<sizet>(CommandType::COUNT))
        {
            OLO_CORE_ERROR("CommandDispatch::GetDispatchFunction: Invalid command type {}", static_cast<int>(type));
            return nullptr;
        }

        return s_DispatchTable[static_cast<sizet>(type)];
    }

    void CommandDispatch::SetViewport(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetViewportCommand*>(data);
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

        auto* registry = Renderer3D::GetShaderRegistry(cmd->shaderID);
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

        // Bind VAO directly using renderer ID
        glBindVertexArray(cmd->vertexArrayID);
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

        // Bind VAO directly using renderer ID
        glBindVertexArray(cmd->vertexArrayID);
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

        // Bind VAO directly using renderer ID
        glBindVertexArray(cmd->vertexArrayID);
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

        // Bind VAO directly using renderer ID
        glBindVertexArray(cmd->vertexArrayID);
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
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array ID or shader ID");
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

        // Camera and Light UBOs are uploaded once per frame in BeginSceneCommon
        // (via UpdateCameraMatricesUBO / UpdateLightPropertiesUBO) and their
        // binding points persist from UBO creation, so no per-draw upload needed.

        // Update model matrix UBO
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;

            constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");

            s_Data.ModelMatrixUBO->SetData(&modelData, expectedSize);
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Material UBO + texture bindings (skipped when material unchanged)
        UploadMaterialState(mat, cmd->materialDataIndex);

        // Shadow/snow textures (per-frame, outside material diffing)
        if (mat.enablePBR)
            BindShadowTextures();

        // Bone matrices
        UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCount);

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
            return;
        }

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
        ++s_Data.Stats.DrawCalls;
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
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
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: Invalid vertex array ID or shader ID");
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

        // Material UBO + texture bindings (skipped when material unchanged)
        UploadMaterialState(mat, cmd->materialDataIndex);

        // Get transforms from FrameDataBuffer
        constexpr sizet maxInstances = CommandBucketConfig{}.MaxMeshInstances;
        sizet instanceCount = static_cast<sizet>(cmd->transformCount);
        if (instanceCount > maxInstances)
        {
            OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Too many instances ({}). Only first {} will be rendered.",
                          instanceCount, maxInstances);
            instanceCount = maxInstances;
        }

        // Set instance transforms from FrameDataBuffer
        // Use single glUniformMatrix4fv call with count > 1 to upload all matrices at once
        // This avoids per-element glGetUniformLocation calls which are expensive
        const glm::mat4* transforms = FrameDataBufferManager::Get().GetTransformPtr(cmd->transformBufferOffset);
        if (transforms)
        {
            // Get base location once for u_ModelMatrices[0] - array elements are sequential
            GLint baseLocation = glGetUniformLocation(mat.shaderRendererID, "u_ModelMatrices[0]");
            if (baseLocation != -1)
            {
                // Upload all instance matrices in a single GL call
                glUniformMatrix4fv(baseLocation, static_cast<GLsizei>(instanceCount), GL_FALSE, glm::value_ptr(transforms[0]));
            }
            GLint instanceCountLoc = glGetUniformLocation(mat.shaderRendererID, "u_InstanceCount");
            if (instanceCountLoc != -1)
            {
                glUniform1i(instanceCountLoc, static_cast<GLint>(instanceCount));
            }
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

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
        ++s_Data.Stats.DrawCalls;
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(instanceCount));
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

        // Bind skybox cubemap texture using renderer ID directly
        if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->skyboxTextureID)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_ENVIRONMENT);
            glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->skyboxTextureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = cmd->skyboxTextureID;
            ++s_Data.Stats.TextureBinds;
        }

        // Bind vertex array and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
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
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = -1;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;

            constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");

            s_Data.ModelMatrixUBO->SetData(&modelData, expectedSize);
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Bind texture using renderer ID directly
        if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->textureID)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_DIFFUSE);
            glBindTexture(GL_TEXTURE_2D, cmd->textureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->textureID;
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->quadVAID);
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

        // Note: Grid shader typically reads view/projection from Camera UBO
        // and calculates grid lines in fragment shader using world position

        // Set grid scale uniform if the shader supports it
        GLint gridScaleLoc = glGetUniformLocation(cmd->shaderRendererID, "u_GridScale");
        if (gridScaleLoc != -1)
        {
            glUniform1f(gridScaleLoc, cmd->gridScale);
        }

        // Bind fullscreen quad VAO and draw
        glBindVertexArray(cmd->quadVAOID);
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
            ShaderBindingLayout::CameraUBO cameraData;
            cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
            cameraData.View = s_Data.ViewMatrix;
            cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
            cameraData.Position = s_Data.ViewPos;
            cameraData._padding0 = 0.0f;
            s_Data.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Upload model matrix UBO
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            s_Data.ModelMatrixUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Upload light UBO
        if (s_Data.LightUBO)
        {
            const Light& light = s_Data.SceneLight;
            ShaderBindingLayout::LightUBO lightData;
            lightData.LightPosition = glm::vec4(light.Position, 1.0f);
            lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
            lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
            lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
            lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
            lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
            lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
            lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(std::to_underlying(light.Type)));
            s_Data.LightUBO->SetData(&lightData, ShaderBindingLayout::LightUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_LIGHTS, s_Data.LightUBO->GetRendererID());
        }

        // Upload terrain UBO (per-chunk data with tess factors)
        auto terrainUBO = Renderer3D::GetTerrainUBO();
        if (terrainUBO)
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

        // Draw with GL_PATCHES for tessellation
        glBindVertexArray(cmd->vertexArrayID);
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
            ShaderBindingLayout::CameraUBO cameraData;
            cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
            cameraData.View = s_Data.ViewMatrix;
            cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
            cameraData.Position = s_Data.ViewPos;
            cameraData._padding0 = 0.0f;
            s_Data.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

        // Upload model matrix UBO
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData._paddingEntity[0] = 0;
            modelData._paddingEntity[1] = 0;
            modelData._paddingEntity[2] = 0;
            s_Data.ModelMatrixUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Upload light UBO
        if (s_Data.LightUBO)
        {
            const Light& light = s_Data.SceneLight;
            ShaderBindingLayout::LightUBO lightData;
            lightData.LightPosition = glm::vec4(light.Position, 1.0f);
            lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
            lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
            lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
            lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
            lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
            lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
            lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(std::to_underlying(light.Type)));
            s_Data.LightUBO->SetData(&lightData, ShaderBindingLayout::LightUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_LIGHTS, s_Data.LightUBO->GetRendererID());
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

        // Draw with GL_TRIANGLES
        glBindVertexArray(cmd->vertexArrayID);
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

        // Bind shader (cached)
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload model UBO
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->decalTransform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->decalTransform));
            modelData.EntityID = cmd->entityID;
            s_Data.ModelMatrixUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Upload decal UBO
        auto decalUBO = Renderer3D::GetDecalUBO();
        if (decalUBO)
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

        // Draw the decal projection cube
        glBindVertexArray(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawFoliageLayer(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        const auto* cmd = static_cast<const DrawFoliageLayerCommand*>(data);

        if (!cmd || cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0 || cmd->instanceCount == 0)
        {
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
        if (s_Data.ModelMatrixUBO)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->modelTransform;
            modelData.Normal = cmd->normalMatrix;
            modelData.EntityID = cmd->entityID;
            s_Data.ModelMatrixUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
        }

        // Upload foliage UBO (per-layer parameters)
        auto foliageUBO = Renderer3D::GetFoliageUBO();
        if (foliageUBO)
        {
            ShaderBindingLayout::FoliageUBO foliageData{};
            foliageData.Time = cmd->time;
            foliageData.WindStrength = cmd->windStrength;
            foliageData.WindSpeed = cmd->windSpeed;
            foliageData.ViewDistance = cmd->viewDistance;
            foliageData.FadeStart = cmd->fadeStart;
            foliageData.AlphaCutoff = cmd->alphaCutoff;
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

        // Draw instanced foliage quads
        glBindVertexArray(cmd->vertexArrayID);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(cmd->instanceCount));
        ++s_Data.Stats.DrawCalls;
    }
} // namespace OloEngine
