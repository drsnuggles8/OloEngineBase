#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"

#include <atomic>
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

namespace OloEngine
{
    namespace
    {
        // Select the stream whose framebuffer matches the active foliage
        // shader's fragment interface: a G-Buffer variant executes in
        // Geometry/SceneRenderPass, a forward variant in FoliageRenderPass.
        // Both the billboard and the impostor card have a G-Buffer sibling
        // (#1225), so in Deferred the only question is whether it loaded.
        [[nodiscard]] constexpr Renderer3D::RenderStreamType SelectFoliageRenderStream(
            RenderingPath path, bool gBufferRouteReady) noexcept
        {
            return path == RenderingPath::Deferred && gBufferRouteReady
                       ? Renderer3D::RenderStreamType::Geometry
                       : Renderer3D::RenderStreamType::Foliage;
        }

        using FoliageStream = Renderer3D::RenderStreamType;
        static_assert(SelectFoliageRenderStream(RenderingPath::Deferred, true) == FoliageStream::Geometry);
        // Either G-Buffer sibling missing falls back to the forward pass, which
        // renders correctly — just without the G-Buffer contribution.
        static_assert(SelectFoliageRenderStream(RenderingPath::Deferred, false) == FoliageStream::Foliage);
        static_assert(SelectFoliageRenderStream(RenderingPath::Forward, true) == FoliageStream::Foliage);
        static_assert(SelectFoliageRenderStream(RenderingPath::ForwardPlus, true) == FoliageStream::Foliage);
    } // namespace

    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RHI::ResourceHandle albedoTextureID,
        i32 entityID)
    {
        // Delegate to the extended variant with Albedo mode + zero extra textures.
        return DrawDecal(decalTransform, inverseDecalTransform, decalColor, decalParams,
                         albedoTextureID, /*normal*/ RHI::NullResource, /*rma*/ RHI::NullResource,
                         DrawDecalCommand::DecalMode::Albedo,
                         /*transparent*/ false, entityID);
    }

    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RHI::ResourceHandle albedoTextureID,
        RHI::ResourceHandle normalTextureID,
        RHI::ResourceHandle rmaTextureID,
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

        cmd->vertexArrayID = va->GetRHIHandle();
        cmd->indexCount = s_Data.DecalCubeMesh->GetIndexCount();
        cmd->shaderRendererID = decalShader->GetRHIHandle();
        // Camera-relative (issue #429): the decal cube's model matrix goes up
        // through UploadModelInstance, which shifts it by the render origin, so
        // the rendered box is in render-relative space (correct screen position
        // under the relative view-projection). The shader reconstructs the
        // fragment's world position from depth via inverseViewProjection — which
        // is the *world* inverse-VP and so yields an *absolute* world position —
        // then maps it into the decal box with the world-space inverseDecalTransform.
        cmd->decalTransform = decalTransform;
        cmd->inverseDecalTransform = inverseDecalTransform;
        // A8 seam, shader-reconstruction flavour (#691): the shader
        // builds `ndc = vec3(screenUV*2-1, depth*2-1)` and multiplies by this,
        // so it needs inverse(Y * VP), not inverse(VP). Recomputed from the
        // flipped forward rather than flipping the stored inverse — inverting
        // first is the drift this helper exists to prevent. Identity on GL, so
        // it stays bit-identical to s_Data.InverseViewProjectionMatrix there.
        cmd->inverseViewProjection = RHI::AdjustedInverseForShaderReconstruction(s_Data.ViewProjectionMatrix);
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

        // Decal render state, including the deferred mode matrix's per-attachment
        // channel mask. Shared with the pass tenant on purpose — see
        // CreateDecalPODRenderState (issue #853).
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(
            CreateDecalPODRenderState(cmd->mode, deferredPath));

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: in Forward/Forward+ decals are transparent overlays
        // rendered after opaque geometry; in Deferred they write into the
        // G-Buffer pre-lighting so they are opaque from the sorter's POV.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = cmd->shaderRendererID.Index & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(decalTransform);
        metadata.m_SortKey = deferredPath
                                 ? DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth)
                                 : DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    void Renderer3D::DrawFoliageLayer(
        RHI::ResourceHandle vertexArrayID, u32 baseIndex, u32 indexCount, u32 instanceCount,
        RHI::ResourceHandle albedoTextureID,
        const glm::mat4& modelTransform,
        f32 time,
        f32 prevTime,
        f32 windStrength, f32 windSpeed,
        f32 viewDistance, f32 fadeStart, f32 alphaCutoff,
        const glm::vec4& baseColor,
        const BoundingBox& layerBounds,
        i32 entityID,
        const FoliageImpostorParams& impostor,
        const FoliageLeafMaterial& leaf,
        bool isAuthoredMesh,
        f32 meshHandoverStart,
        f32 meshHandoverEnd)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->RenderStreamPasses.Foliage)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: FoliagePass is null!");
            return;
        }

        if (!s_Data.FoliageShader)
        {
            return;
        }

        // Octahedral impostor card (issue #433) vs the flat billboard. A DATA
        // decision — the layer asked for an impostor and the atlas baked — not
        // a shader-availability one; a missing program is handled below, loudly.
        // The authored-mesh near draw is never an impostor card: the impostor
        // IS the far-field representation, and routing the mesh through it
        // would replace exactly the geometry this draw exists to render
        // (issue #1233). FoliageRenderer already only tags the card draw, so
        // this is a second lock rather than the decision.
        bool useImpostor = !isAuthoredMesh && impostor.Enabled && impostor.AlbedoAtlasID.IsValid();

        // Each card has a forward and a deferred program; pick the pair once.
        const Ref<Shader>& forwardShader = useImpostor ? s_Data.FoliageImpostorShader : s_Data.FoliageShader;
        const Ref<Shader>& deferredShader = useImpostor ? s_Data.FoliageImpostorGBufferShader : s_Data.FoliageGBufferShader;

        // Deferred routes through ScenePass (the G-Buffer FB) so foliage takes
        // part in the deferred lighting composite. The forward FoliagePass is
        // the fallback when the deferred program is missing — and that fallback
        // IS the #1225 symptom (a canopy absent from the G-Buffer, so SSAO /
        // SSGI / SSR see sky), so it is never silent.
        const bool gBufferRouteReady = deferredShader && s_Data.Pipeline->FrameCorePasses.Scene;
        const RenderStreamType targetStream = SelectFoliageRenderStream(s_Data.Settings.Path, gBufferRouteReady);
        const bool useGBufferVariant = targetStream == RenderStreamType::Geometry;
        if (s_Data.Settings.Path == RenderingPath::Deferred && !gBufferRouteReady)
        {
            static std::atomic<bool> s_WarnedNoDeferredFoliage{ false };
            if (!s_WarnedNoDeferredFoliage.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("Renderer3D::DrawFoliageLayer: no deferred program for the {} ({}) — drawing it in the "
                              "forward FoliagePass, where it writes NO G-Buffer and SSAO/SSGI/SSR treat it as sky. "
                              "Further layers not logged.",
                              useImpostor ? "impostor card" : "billboard",
                              useImpostor ? "Foliage_Impostor_GBuffer" : "Foliage_Instance_GBuffer");
            }
        }

        Ref<Shader> activeShader = useGBufferVariant ? deferredShader : forwardShader;
        if (!activeShader)
        {
            // Only reachable for the impostor (the billboard's forward program
            // was checked at the top). Draw the flat card rather than drop the
            // layer, and say so once.
            static std::atomic<bool> s_WarnedNoImpostorProgram{ false };
            if (!s_WarnedNoImpostorProgram.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("Renderer3D::DrawFoliageLayer: impostor program missing — drawing the flat billboard "
                              "instead. Further layers not logged.");
            }
            useImpostor = false;
            activeShader = useGBufferVariant ? s_Data.FoliageGBufferShader : s_Data.FoliageShader;
        }

        // Frustum cull the entire layer using the precomputed bounding box.
        if (s_Data.FrustumCullingEnabled)
        {
            const BoundingBox worldBounds = layerBounds.Transform(modelTransform);
            if (!s_Data.ViewFrustum.IsBoundingBoxVisible(worldBounds))
            {
                return;
            }
        }

        CommandPacket* packet = CreateRenderStreamDrawCall<DrawFoliageLayerCommand>(targetStream);
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: Failed to allocate foliage command packet!");
            return;
        }
        auto* cmd = packet->GetCommandData<DrawFoliageLayerCommand>();
        cmd->header.type = CommandType::DrawFoliageLayer;

        cmd->vertexArrayID = vertexArrayID;
        cmd->baseIndex = baseIndex;
        cmd->indexCount = indexCount;
        cmd->instanceCount = instanceCount;
        cmd->shaderRendererID = activeShader->GetRHIHandle();
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
        cmd->albedoTextureID = useImpostor ? impostor.AlbedoAtlasID : albedoTextureID;
        cmd->entityID = entityID;
        cmd->isAuthoredMesh = isAuthoredMesh ? 1.0f : 0.0f;
        cmd->meshHandoverStart = meshHandoverStart;
        cmd->meshHandoverEnd = meshHandoverEnd;

        // Leaf material (issue #1234). Copied for EVERY draw of the layer —
        // the authored mesh, the flat card and the impostor card alike — so a
        // plant cannot change what it is made of as it crosses a hand-over
        // band. That is the distance half of the fourth acceptance criterion,
        // and it is structural here rather than a thing three call sites have
        // to remember.
        cmd->leafNormalTextureID = leaf.NormalTextureID;
        cmd->leafRoughnessTextureID = leaf.RoughnessTextureID;
        cmd->leafThicknessTextureID = leaf.ThicknessTextureID;
        cmd->leafRoughness = leaf.Roughness;
        cmd->leafNormalStrength = leaf.NormalStrength;
        cmd->leafThickness = leaf.Thickness;
        cmd->leafTransmissionStrength = leaf.TransmissionStrength;
        cmd->leafTransmissionColor = leaf.TransmissionColor;
        cmd->leafTransmissionDistortion = leaf.TransmissionDistortion;
        cmd->leafTransmissionPower = leaf.TransmissionPower;
        cmd->leafTransmissionWrap = leaf.TransmissionWrap;
        cmd->leafTransmissionAmbient = leaf.TransmissionAmbient;
        // Intern the layer-constant half of the lobe so the DEFERRED path can
        // name it per pixel (issue #1234). A non-leaf layer resolves to
        // kFoliageLeafSlotNone for free — Resolve() short-circuits on strength
        // 0 — so the overwhelmingly common case costs nothing and spends no
        // slot. See FoliageLeafProfile.h for why this mirrors skin's plumbing.
        cmd->leafProfileSlot = GetFoliageLeafProfileTable().Resolve(FoliageLeafProfile{
            leaf.TransmissionColor, leaf.TransmissionStrength, leaf.TransmissionDistortion,
            leaf.TransmissionPower, leaf.TransmissionWrap, leaf.TransmissionAmbient });

        // Octahedral impostor payload (issue #433).
        if (useImpostor)
        {
            cmd->impostorNormalDepthTextureID = impostor.NormalDepthAtlasID;
            cmd->impostorEnabled = 1.0f;
            cmd->impostorFramesPerAxis = static_cast<f32>(impostor.FramesPerAxis);
            cmd->impostorHemi = impostor.Hemi ? 1.0f : 0.0f;
            cmd->impostorStartDistance = impostor.StartDistance;
            cmd->impostorBand = impostor.TransitionBand;
            cmd->impostorRadius = impostor.Radius;
            cmd->impostorParallaxScale = impostor.ParallaxScale;
        }

        // Foliage render state: opaque alpha-tested, depth test + write, no blend.
        {
            PODRenderState foliageState = CreateDefaultPODRenderState();
            foliageState.depthTestEnabled = true;
            foliageState.depthFunction = RHI::CompareOp::LessOrEqual;
            foliageState.depthWriteMask = true;
            foliageState.blendEnabled = false;
            // The impostor card is a camera-facing quad — draw it double-sided so
            // it never culls to nothing on either winding (matches the two-sided
            // foliage lighting); the flat billboard keeps back-face culling.
            //
            // The authored plant mesh is two-sided too: foliage meshes are
            // built from single-sided leaf cards, and back-face culling shows
            // half of every leaf as a hole from the wrong side.
            foliageState.cullingEnabled = !useImpostor && !isAuthoredMesh;
            foliageState.cullFace = RHI::CullMode::Back;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(foliageState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: opaque, sorted by shader then depth (front-to-back).
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = cmd->shaderRendererID.Index & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        // Allocation and submission use the SAME selected stream. Previously
        // the G-Buffer packet was allocated from Geometry but returned to a
        // caller that unconditionally submitted it to FoliageRenderPass. That
        // executed Foliage_Instance_GBuffer against the forward Scene MRT,
        // mapping its float location 1 onto Scene's R32_SINT entity-ID target.
        SubmitRenderStreamPacket(targetStream, packet);
    }

    CommandPacket* Renderer3D::DrawWaterSurface(
        RHI::ResourceHandle vertexArrayID, u32 indexCount,
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
        cmd->shaderRendererID = s_Data.WaterShader->GetRHIHandle();
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
        cmd->fftCascadeParams = params.fftCascadeParams;
        cmd->projectedGridParams = params.projectedGridParams;
        cmd->projectedGridParams2 = params.projectedGridParams2;
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
            waterState.depthFunction = RHI::CompareOp::LessOrEqual;
            waterState.depthWriteMask = false;
            waterState.blendEnabled = true;
            waterState.blendSrcFactor = RHI::BlendFactor::SrcAlpha;
            waterState.blendDstFactor = RHI::BlendFactor::OneMinusSrcAlpha;
            waterState.cullingEnabled = !params.renderFromBelow;
            waterState.cullFace = RHI::CullMode::Back;
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
        const u32 shaderID = cmd->shaderRendererID.Index & 0xFFFF;
        const u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }
} // namespace OloEngine
