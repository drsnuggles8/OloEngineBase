#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
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
    // Helper to apply POD render state to the renderer API
    static void ApplyPODRenderState(const PODRenderState& state, RendererAPI& api)
    {
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
        std::array<u32, 32> BoundTextureIDs = { 0 };

        // Shadow texture renderer IDs (set per-frame)
        u32 CSMShadowTextureID = 0;
        u32 SpotShadowTextureID = 0;
        std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS> PointShadowTextureIDs = { 0 };

        CommandDispatch::Statistics Stats;
    };

    static CommandDispatchData s_Data;

    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<sizet>(CommandType::SetMultisampling) + 1];

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

        s_Data.CurrentBoundShaderID = 0;
        std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
        s_Data.Stats.Reset();

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
        s_Data.BoundTextureIDs.fill(0);
        s_Data.CSMShadowTextureID = 0;
        s_Data.SpotShadowTextureID = 0;
        s_Data.PointShadowTextureIDs.fill(0);
        s_Data.Stats.Reset();
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
        OLO_PROFILE_FUNCTION();
        s_Data.CSMShadowTextureID = csmTextureID;
        s_Data.SpotShadowTextureID = spotTextureID;
    }

    void CommandDispatch::SetPointShadowTextureIDs(const std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS>& pointTextureIDs)
    {
        OLO_PROFILE_FUNCTION();
        s_Data.PointShadowTextureIDs = pointTextureIDs;
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
        if (type == CommandType::Invalid || static_cast<sizet>(type) >= sizeof(s_DispatchTable) / sizeof(CommandDispatchFn))
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

        // Validate POD renderer IDs
        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array ID or shader ID");
            return;
        }

        // Apply POD render state directly
        ApplyPODRenderState(cmd->renderState, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            s_Data.Stats.ShaderBinds++;
        }

        ShaderBindingLayout::CameraUBO cameraData;
        cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
        cameraData.View = s_Data.ViewMatrix;
        // Calculate projection matrix from ViewProjection and View: Projection = ViewProjection * inverse(View)
        cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
        cameraData.Position = s_Data.ViewPos; // Add camera position
        cameraData._padding0 = 0.0f;          // Initialize padding

        if (s_Data.CameraUBO)
        {
            constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch in DrawMesh");
            s_Data.CameraUBO->SetData(&cameraData, expectedSize);
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
        }

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

        // Update material UBO - use PBR if enabled, otherwise use legacy
        if (cmd->enablePBR)
        {
            ShaderBindingLayout::PBRMaterialUBO pbrMaterialData;
            pbrMaterialData.BaseColorFactor = cmd->baseColorFactor;
            pbrMaterialData.EmissiveFactor = cmd->emissiveFactor;
            pbrMaterialData.MetallicFactor = cmd->metallicFactor;
            pbrMaterialData.RoughnessFactor = cmd->roughnessFactor;
            pbrMaterialData.NormalScale = cmd->normalScale;
            pbrMaterialData.OcclusionStrength = cmd->occlusionStrength;
            pbrMaterialData.UseAlbedoMap = cmd->albedoMapID != 0 ? 1 : 0;
            pbrMaterialData.UseNormalMap = cmd->normalMapID != 0 ? 1 : 0;
            pbrMaterialData.UseMetallicRoughnessMap = cmd->metallicRoughnessMapID != 0 ? 1 : 0;
            pbrMaterialData.UseAOMap = cmd->aoMapID != 0 ? 1 : 0;
            pbrMaterialData.UseEmissiveMap = cmd->emissiveMapID != 0 ? 1 : 0;
            pbrMaterialData.EnableIBL = cmd->enableIBL ? 1 : 0;
            pbrMaterialData.ApplyGammaCorrection = 1; // Enable gamma correction by default
            pbrMaterialData.AlphaCutoff = 0;          // Default alpha cutoff

            if (s_Data.MaterialUBO)
            {
                constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
                static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch in DrawMesh");
                s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
                glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
            }
        }
        else
        {
            ShaderBindingLayout::MaterialUBO materialData;
            materialData.Ambient = glm::vec4(cmd->ambient, 1.0f);
            materialData.Diffuse = glm::vec4(cmd->diffuse, 1.0f);
            materialData.Specular = glm::vec4(cmd->specular, cmd->shininess);
            materialData.Emissive = glm::vec4(0.0f);
            materialData.UseTextureMaps = cmd->useTextureMaps ? 1 : 0;
            materialData.AlphaMode = 0;   // Default alpha mode
            materialData.DoubleSided = 0; // Default double-sided
            materialData._padding = 0;    // Clear remaining padding

            if (s_Data.MaterialUBO)
            {
                constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
                static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch in DrawMesh");
                s_Data.MaterialUBO->SetData(&materialData, expectedSize);
                glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
            }
        }

        const Light& light = s_Data.SceneLight;
        auto lightType = std::to_underlying(light.Type);

        ShaderBindingLayout::LightUBO lightData;
        lightData.LightPosition = glm::vec4(light.Position, 1.0f);
        lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
        lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
        lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
        lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
        lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
        lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
        lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

        if (s_Data.LightUBO)
        {
            constexpr u32 expectedSize = ShaderBindingLayout::LightUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::LightUBO) == expectedSize, "LightUBO size mismatch");
            s_Data.LightUBO->SetData(&lightData, expectedSize);
            glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_LIGHTS, s_Data.LightUBO->GetRendererID());
        }

        // Bind textures based on material type using renderer IDs directly
        if (cmd->enablePBR)
        {
            // PBR texture binding using renderer IDs
            if (cmd->albedoMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->albedoMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_DIFFUSE);
                    glBindTexture(GL_TEXTURE_2D, cmd->albedoMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->albedoMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->metallicRoughnessMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != cmd->metallicRoughnessMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_SPECULAR);
                    glBindTexture(GL_TEXTURE_2D, cmd->metallicRoughnessMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = cmd->metallicRoughnessMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->normalMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] != cmd->normalMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_NORMAL);
                    glBindTexture(GL_TEXTURE_2D, cmd->normalMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] = cmd->normalMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->aoMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] != cmd->aoMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_AMBIENT);
                    glBindTexture(GL_TEXTURE_2D, cmd->aoMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] = cmd->aoMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->emissiveMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] != cmd->emissiveMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_EMISSIVE);
                    glBindTexture(GL_TEXTURE_2D, cmd->emissiveMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] = cmd->emissiveMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->environmentMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->environmentMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_ENVIRONMENT);
                    glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->environmentMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = cmd->environmentMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->irradianceMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] != cmd->irradianceMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_USER_0);
                    glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->irradianceMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] = cmd->irradianceMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->prefilterMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] != cmd->prefilterMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_USER_1);
                    glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->prefilterMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] = cmd->prefilterMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->brdfLutMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] != cmd->brdfLutMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_USER_2);
                    glBindTexture(GL_TEXTURE_2D, cmd->brdfLutMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] = cmd->brdfLutMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            // Bind shadow map textures (CSM at slot 8, spot at slot 13)
            if (s_Data.CSMShadowTextureID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] != s_Data.CSMShadowTextureID)
                {
                    glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW, s_Data.CSMShadowTextureID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW] = s_Data.CSMShadowTextureID;
                    s_Data.Stats.TextureBinds++;
                }
            }
            if (s_Data.SpotShadowTextureID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] != s_Data.SpotShadowTextureID)
                {
                    glBindTextureUnit(ShaderBindingLayout::TEX_SHADOW_SPOT, s_Data.SpotShadowTextureID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SHADOW_SPOT] = s_Data.SpotShadowTextureID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            // Bind point light shadow cubemaps (slots 14-17)
            static constexpr u32 pointSlots[UBOStructures::ShadowUBO::MAX_POINT_SHADOWS] = {
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
                        s_Data.Stats.TextureBinds++;
                    }
                }
            }
        }
        else if (cmd->useTextureMaps)
        {
            // Legacy texture binding using renderer IDs
            if (cmd->diffuseMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->diffuseMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_DIFFUSE);
                    glBindTexture(GL_TEXTURE_2D, cmd->diffuseMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->diffuseMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->specularMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != cmd->specularMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_SPECULAR);
                    glBindTexture(GL_TEXTURE_2D, cmd->specularMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = cmd->specularMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }
        }

        // Handle bone matrices for animated meshes using FrameDataBuffer
        if (cmd->isAnimatedMesh && s_Data.BoneMatricesUBO && cmd->boneCount > 0)
        {
            using namespace UBOStructures;
            constexpr sizet MAX_BONES = AnimationConstants::MAX_BONES;
            sizet boneCount = glm::min(static_cast<sizet>(cmd->boneCount), MAX_BONES);

            // Runtime check for bone limit exceeded
            if (cmd->boneCount > MAX_BONES)
            {
                OLO_CORE_WARN("Animated mesh has {} bones, exceeding limit of {}. Bone matrices will be truncated.",
                              cmd->boneCount, MAX_BONES);
            }

            // Get bone matrices from FrameDataBuffer
            const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixPtr(cmd->boneBufferOffset);
            if (boneMatrices)
            {
                s_Data.BoneMatricesUBO->SetData(boneMatrices, static_cast<u32>(boneCount * sizeof(glm::mat4)));
                glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_ANIMATION, s_Data.BoneMatricesUBO->GetRendererID());
            }
        }

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
            return;
        }

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
        s_Data.Stats.DrawCalls++;
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);
    }

    void CommandDispatch::DrawMeshInstanced(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        auto const* cmd = static_cast<const DrawMeshInstancedCommand*>(data);

        // Validate POD renderer IDs
        if (cmd->vertexArrayID == 0 || cmd->shaderRendererID == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: Invalid vertex array ID or shader ID");
            return;
        }

        // Apply POD render state directly
        ApplyPODRenderState(cmd->renderState, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            s_Data.Stats.ShaderBinds++;
        }

        // Update material UBO
        ShaderBindingLayout::MaterialUBO materialData;
        materialData.Ambient = glm::vec4(cmd->ambient, 1.0f);
        materialData.Diffuse = glm::vec4(cmd->diffuse, 1.0f);
        materialData.Specular = glm::vec4(cmd->specular, cmd->shininess);
        materialData.Emissive = glm::vec4(0.0f);
        materialData.UseTextureMaps = cmd->useTextureMaps ? 1 : 0;
        materialData.AlphaMode = 0;
        materialData.DoubleSided = 0;
        materialData._padding = 0;

        if (s_Data.MaterialUBO)
        {
            constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
            static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch");
            s_Data.MaterialUBO->SetData(&materialData, expectedSize);
        }

        UpdateMaterialTextureFlag(cmd->useTextureMaps);

        // Get transforms from FrameDataBuffer
        const sizet maxInstances = 100;
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
            GLint baseLocation = glGetUniformLocation(cmd->shaderRendererID, "u_ModelMatrices[0]");
            if (baseLocation != -1)
            {
                // Upload all instance matrices in a single GL call
                glUniformMatrix4fv(baseLocation, static_cast<GLsizei>(instanceCount), GL_FALSE, glm::value_ptr(transforms[0]));
            }
            GLint instanceCountLoc = glGetUniformLocation(cmd->shaderRendererID, "u_InstanceCount");
            if (instanceCountLoc != -1)
            {
                glUniform1i(instanceCountLoc, static_cast<GLint>(instanceCount));
            }
        }

        // Bind textures using renderer IDs
        if (cmd->useTextureMaps)
        {
            if (cmd->diffuseMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != cmd->diffuseMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_DIFFUSE);
                    glBindTexture(GL_TEXTURE_2D, cmd->diffuseMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->diffuseMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }

            if (cmd->specularMapID != 0)
            {
                if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != cmd->specularMapID)
                {
                    glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_SPECULAR);
                    glBindTexture(GL_TEXTURE_2D, cmd->specularMapID);
                    s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = cmd->specularMapID;
                    s_Data.Stats.TextureBinds++;
                }
            }
        }

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: No indices to draw");
            return;
        }

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
        s_Data.Stats.DrawCalls++;
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

        // Apply POD render state (skybox-specific settings already in renderState)
        ApplyPODRenderState(cmd->renderState, api);

        // Bind skybox shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            s_Data.Stats.ShaderBinds++;
        }

        // Bind skybox cubemap texture using renderer ID directly
        if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->skyboxTextureID)
        {
            glActiveTexture(GL_TEXTURE0 + ShaderBindingLayout::TEX_ENVIRONMENT);
            glBindTexture(GL_TEXTURE_CUBE_MAP, cmd->skyboxTextureID);
            s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = cmd->skyboxTextureID;
            s_Data.Stats.TextureBinds++;
        }

        // Bind vertex array and draw using renderer ID directly
        glBindVertexArray(cmd->vertexArrayID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->indexCount), GL_UNSIGNED_INT, nullptr);

        // Update statistics
        s_Data.Stats.DrawCalls++;
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

        // Apply POD render state directly
        ApplyPODRenderState(cmd->renderState, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            s_Data.Stats.ShaderBinds++;
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
            s_Data.Stats.TextureBinds++;
        }

        // Bind VAO and draw using renderer ID directly
        glBindVertexArray(cmd->quadVAID);
        s_Data.Stats.DrawCalls++;
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

        // Apply POD render state (grid-specific: blending enabled, depth test enabled)
        ApplyPODRenderState(cmd->renderState, api);

        // Bind grid shader using renderer ID directly
        if (s_Data.CurrentBoundShaderID != cmd->shaderRendererID)
        {
            glUseProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShaderID = cmd->shaderRendererID;
            s_Data.Stats.ShaderBinds++;
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

        s_Data.Stats.DrawCalls++;
    }
} // namespace OloEngine
