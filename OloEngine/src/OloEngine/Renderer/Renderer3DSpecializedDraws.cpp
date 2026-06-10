#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

namespace OloEngine
{
    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RendererID albedoTextureID,
        i32 entityID)
    {
        // Delegate to the extended variant with Albedo mode + zero extra textures.
        return DrawDecal(decalTransform, inverseDecalTransform, decalColor, decalParams,
                         albedoTextureID, /*normal*/ 0u, /*rma*/ 0u,
                         DrawDecalCommand::DecalMode::Albedo,
                         /*transparent*/ false, entityID);
    }

    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RendererID albedoTextureID,
        RendererID normalTextureID,
        RendererID rmaTextureID,
        DrawDecalCommand::DecalMode mode,
        bool transparent,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->RenderStreamPasses.Decal)
        {
            OLO_CORE_ERROR("Renderer3D::DrawDecal: DecalPass is null!");
            return nullptr;
        }

        if (!s_Data.DecalShader || !s_Data.DecalCubeMesh)
        {
            return nullptr;
        }

        auto va = s_Data.DecalCubeMesh->GetVertexArray();
        if (!va)
        {
            return nullptr;
        }

        CommandPacket* packet = CreateDecalDrawCall<DrawDecalCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawDecal: Failed to allocate decal command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawDecalCommand>();
        cmd->header.type = CommandType::DrawDecal;

        // Deferred-path decals write into the G-Buffer attachment that matches
        // the decal mode (Albedo → RT0, Normal → RT1, RMA → RT0.a+RT1.zw) BEFORE
        // the lighting pass, so they are re-lit by DeferredLightingPass. In
        // Forward/Forward+, every mode collapses to the existing transparent
        // overlay shader — there is no forward-path normal/RMA decal. Transparent
        // decals always route through the forward shader even in Deferred, so
        // they composite over the lit scene colour after DeferredLightingPass.
        const bool deferredPath = !transparent &&
                                  s_Data.Settings.Path == RenderingPath::Deferred &&
                                  s_Data.DecalGBufferShader != nullptr;
        Ref<Shader> decalShader = s_Data.DecalShader;
        if (deferredPath)
        {
            switch (mode)
            {
                case DrawDecalCommand::DecalMode::Normal:
                    if (s_Data.DecalGBufferNormalShader)
                        decalShader = s_Data.DecalGBufferNormalShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::RMA:
                    if (s_Data.DecalGBufferRMAShader)
                        decalShader = s_Data.DecalGBufferRMAShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::Emissive:
                    if (s_Data.DecalGBufferEmissiveShader)
                        decalShader = s_Data.DecalGBufferEmissiveShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::Albedo:
                default:
                    decalShader = s_Data.DecalGBufferShader;
                    break;
            }
        }

        cmd->vertexArrayID = va->GetRendererID();
        cmd->indexCount = s_Data.DecalCubeMesh->GetIndexCount();
        cmd->shaderRendererID = decalShader->GetRendererID();
        cmd->decalTransform = decalTransform;
        cmd->inverseDecalTransform = inverseDecalTransform;
        cmd->inverseViewProjection = s_Data.InverseViewProjectionMatrix;
        cmd->decalColor = decalColor;
        cmd->decalParams = decalParams;
        cmd->albedoTextureID = albedoTextureID;
        cmd->normalTextureID = normalTextureID;
        cmd->rmaTextureID = rmaTextureID;
        cmd->mode = deferredPath
                        ? mode
                        : DrawDecalCommand::DecalMode::Albedo; // Forward path always albedo.
        cmd->transparent = transparent ? u8{ 1 } : u8{ 0 };
        cmd->entityID = entityID;

        // Decal render state: blend on for albedo (soft edges), blend off for
        // normal/RMA (hard discard threshold — see shader comments). Depth
        // read-only, front-face culling in all cases.
        {
            PODRenderState decalState = CreateDefaultPODRenderState();
            const bool blendForThisMode = (cmd->mode == DrawDecalCommand::DecalMode::Albedo);
            decalState.blendEnabled = blendForThisMode;
            decalState.blendSrcFactor = GL_SRC_ALPHA;
            decalState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            decalState.depthTestEnabled = true;
            decalState.depthFunction = GL_LEQUAL;
            decalState.depthWriteMask = false;
            decalState.cullingEnabled = true;
            decalState.cullFace = GL_FRONT;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(decalState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: in Forward/Forward+ decals are transparent overlays
        // rendered after opaque geometry; in Deferred they write into the
        // G-Buffer pre-lighting so they are opaque from the sorter's POV.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = decalShader->GetRendererID() & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(decalTransform);
        metadata.m_SortKey = deferredPath
                                 ? DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth)
                                 : DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawFoliageLayer(
        RendererID vertexArrayID, u32 indexCount, u32 instanceCount,
        RendererID albedoTextureID,
        const glm::mat4& modelTransform,
        f32 time,
        f32 prevTime,
        f32 windStrength, f32 windSpeed,
        f32 viewDistance, f32 fadeStart, f32 alphaCutoff,
        const glm::vec4& baseColor,
        const BoundingBox& layerBounds,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->RenderStreamPasses.Foliage)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: FoliagePass is null!");
            return nullptr;
        }

        if (!s_Data.FoliageShader)
        {
            return nullptr;
        }

        // Deferred: route through ScenePass (the G-Buffer FB) with the
        // G-Buffer variant shader so foliage participates in the deferred
        // lighting composite. Falls back to the forward FoliagePass route
        // when the variant is missing.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.FoliageGBufferShader && s_Data.Pipeline->FrameCorePasses.Scene;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.FoliageGBufferShader : s_Data.FoliageShader;

        // Frustum cull the entire layer using the precomputed bounding box.
        if (s_Data.FrustumCullingEnabled)
        {
            const BoundingBox worldBounds = layerBounds.Transform(modelTransform);
            if (!s_Data.ViewFrustum.IsBoundingBoxVisible(worldBounds))
            {
                return nullptr;
            }
        }

        CommandPacket* packet = useGBufferVariant
                                    ? CreateDrawCall<DrawFoliageLayerCommand>()
                                    : CreateFoliageDrawCall<DrawFoliageLayerCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: Failed to allocate foliage command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawFoliageLayerCommand>();
        cmd->header.type = CommandType::DrawFoliageLayer;

        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = indexCount;
        cmd->instanceCount = instanceCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->modelTransform = modelTransform;
        cmd->normalMatrix = glm::transpose(glm::inverse(modelTransform));
        cmd->time = time;
        cmd->prevTime = prevTime;
        cmd->windStrength = windStrength;
        cmd->windSpeed = windSpeed;
        cmd->viewDistance = viewDistance;
        cmd->fadeStart = fadeStart;
        cmd->alphaCutoff = alphaCutoff;
        cmd->baseColor = baseColor;
        cmd->albedoTextureID = albedoTextureID;
        cmd->entityID = entityID;

        // Foliage render state: opaque alpha-tested, depth test + write, no blend.
        {
            PODRenderState foliageState = CreateDefaultPODRenderState();
            foliageState.depthTestEnabled = true;
            foliageState.depthFunction = GL_LEQUAL;
            foliageState.depthWriteMask = true;
            foliageState.blendEnabled = false;
            foliageState.cullingEnabled = true;
            foliageState.cullFace = GL_BACK;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(foliageState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: opaque, sorted by shader then depth (front-to-back).
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawWaterSurface(
        RendererID vertexArrayID, u32 indexCount,
        const glm::mat4& modelTransform,
        f32 time,
        f32 prevTime,
        const WaterDrawParams& params,
        const BoundingBox& bounds,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->RenderStreamPasses.Water)
        {
            OLO_CORE_ERROR("Renderer3D::DrawWaterSurface: WaterPass is null!");
            return nullptr;
        }

        if (!s_Data.WaterShader)
        {
            return nullptr;
        }

        // Frustum cull the water surface.
        if (s_Data.FrustumCullingEnabled)
        {
            const BoundingBox worldBounds = bounds.Transform(modelTransform);
            if (!s_Data.ViewFrustum.IsBoundingBoxVisible(worldBounds))
            {
                return nullptr;
            }
        }

        CommandPacket* packet = CreateWaterDrawCall<DrawWaterCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawWaterSurface: Failed to allocate water command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawWaterCommand>();
        cmd->header.type = CommandType::DrawWater;

        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = indexCount;
        cmd->shaderRendererID = s_Data.WaterShader->GetRendererID();
        cmd->modelTransform = modelTransform;
        cmd->normalMatrix = glm::transpose(glm::inverse(modelTransform));

        // Pack time into waveParams.x.
        glm::vec4 waveParams = params.waveParams;
        waveParams.x = time;
        cmd->waveParams = waveParams;
        cmd->waveDir0 = params.waveDir0;
        cmd->waveDir1 = params.waveDir1;
        cmd->waterColor = params.waterColor;
        cmd->waterDeepColor = params.waterDeepColor;
        cmd->visualParams = params.visualParams;
        cmd->normalMapScroll = params.normalMapScroll;
        cmd->normalMapSpeed = params.normalMapSpeed;
        // Pack previous-frame time into normalMapSpeed.z so the water shader can
        // re-evaluate the Gerstner sum at `t - dt` for per-fragment velocity
        // reprojection (closes the wave-animation gap in the RT3 motion vector).
        cmd->normalMapSpeed.z = prevTime;
        cmd->lightDirection = params.lightDirection;
        cmd->depthRefractionParams = params.depthRefractionParams;
        cmd->refractionColor = params.refractionColor;
        cmd->foamParams = params.foamParams;
        cmd->foamParams2 = params.foamParams2;
        cmd->sssColor = params.sssColor;
        cmd->ssrParams = params.ssrParams;
        cmd->tessParams = params.tessParams;
        cmd->fftParams = params.fftParams;
        cmd->normalMap0ID = params.normalMap0ID;
        cmd->normalMap1ID = params.normalMap1ID;
        cmd->noiseTextureID = params.noiseTextureID;
        cmd->foamTextureID = params.foamTextureID;
        cmd->fftDisplacementID = params.fftDisplacementID;
        cmd->fftDerivativesID = params.fftDerivativesID;
        cmd->refractionEnabled = params.refractionEnabled;
        cmd->ssrEnabled = params.ssrEnabled;
        cmd->entityID = entityID;

        // Water render state: translucent, depth test on, depth write off, alpha blend.
        // When `renderFromBelow` is set the water draws double-sided and the
        // fragment shader keeps the correct side per fragment (the waterline
        // discard, gated on u_NormalMapSpeed.w) — this is what lets the surface
        // be seen from below and straddle the waterline without holes or
        // interleaved-sheet artifacts. Otherwise it stays single-sided
        // back-culled (original top-down behaviour, invisible from below). §7.2.
        {
            PODRenderState waterState = CreateDefaultPODRenderState();
            waterState.depthTestEnabled = true;
            waterState.depthFunction = GL_LEQUAL;
            waterState.depthWriteMask = false;
            waterState.blendEnabled = true;
            waterState.blendSrcFactor = GL_SRC_ALPHA;
            waterState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            waterState.cullingEnabled = !params.renderFromBelow;
            waterState.cullFace = GL_BACK;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(waterState);
        }

        // Tell the shader whether to run the per-fragment waterline discard
        // (only meaningful when double-sided). Packed into the otherwise-unused
        // NormalMapSpeed.w channel of the water UBO.
        cmd->normalMapSpeed.w = params.renderFromBelow ? 1.0f : 0.0f;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: translucent, sorted back-to-front for correct blending.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = s_Data.WaterShader->GetRendererID() & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }
} // namespace OloEngine
