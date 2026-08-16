// OLO_MATERIAL_RESOLVE_EXEMPT: this file EXECUTES already-built command packets. The material
// was resolved (through SubmeshMaterialResolve.h) by whoever built the packet; by the time a
// DrawMeshInstancedCommand reaches the dispatcher there is no submesh, no MaterialComponent and
// no imported-material table left to consult — only a PODMaterialData slot to bind. See
// RenderPathDrift.EveryMeshSubmissionPathUsesTheSharedMaterialResolver.
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/CameraRelative.h"
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
        // Camera-relative render origin for this frame (issue #429). The view /
        // view-projection / position above stay world-space; camera-UBO packing
        // subtracts this origin so the GPU renders near 0. (0,0,0) near origin.
        glm::vec3 RenderOrigin = glm::vec3(0.0f);

        // The redundant-bind cache keys on IDENTITIES, not driver names
        // (issue #691 step 3, slice 6). That is a correctness change, not a
        // type change: GL reissues object names, so a deleted texture and a
        // newly created one could compare equal here and the cache would SKIP
        // a bind that genuinely had to happen. Two live handles cannot
        // collide, so the Invalidate* calls below stop being load-bearing
        // correctness and become the pure optimisation they read as.
        RHI::ResourceHandle CurrentBoundShader{};
        RHI::ResourceHandle CurrentBoundVAO{};
        u16 LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        u16 LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        // Heap re-initialisation epoch the cached material UBO was built under.
        // Its per-material heap offsets index THAT heap, so the cache must be
        // dropped when it bumps (issue #691 Phase 3).
        u64 HeapEpoch = 0u;
        // Whether the CACHED material UBO was built with live heap offsets.
        //
        // The cache is keyed on the material index, but an offset's validity
        // depends on the PROGRAM in flight: ResolveTextureOffset returns nothing
        // for a slot-based program. So a material uploaded while a slot-based
        // shader was bound holds null offsets, and re-using that cached upload for
        // a BINDLESS shader would skip the binds (BindPBRTextures) AND supply no
        // offsets — the material renders with no textures at all. The program kind
        // is therefore part of the cache key (issue #691 Phase 3).
        bool LastMaterialOffsetsLive = false;
        std::array<RHI::ResourceHandle, ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS> BoundTextures{};
        u32 CurrentViewportWidth = 0;
        u32 CurrentViewportHeight = 0;

        // Track currently bound UBO renderer IDs per binding point to avoid
        // redundant glBindBufferBase calls. Indexed by ShaderBindingLayout::UBO_*.
        static constexpr u32 MAX_TRACKED_UBO_BINDINGS = 8;
        std::array<RHI::ResourceHandle, MAX_TRACKED_UBO_BINDINGS> BoundUBOs{};

        // Shadow texture identities (set per-frame)
        RHI::ResourceHandle CSMShadowTexture{};
        RHI::ResourceHandle AtlasShadowTexture{};
        // Comparison-OFF raw-depth views of the CSM array / shadow atlas (PCSS blocker search)
        RHI::ResourceHandle CSMRawShadowTexture{};
        RHI::ResourceHandle AtlasRawShadowTexture{};

        // Snow accumulation depth texture (set per-frame)
        RHI::ResourceHandle SnowDepthTexture{};

        // Cloud shadow transmittance map (set per-frame, issue #633)
        RHI::ResourceHandle CloudShadowTexture{};

        // Depth prepass override: when true, ApplyPODRenderState forces depth-only state
        bool DepthPrepassActive = false;
        // Renderer-ID snapshot for the prepass depth-only shader swap, refreshed
        // in SetDepthPrepassActive(true) so shader hot-reloads are picked up.
        Renderer3D::DepthPrepassShaderIDs DepthPrepassShaders;
        // Color pass of depth prepass: override depth func to GL_LEQUAL + depth mask false
        bool DepthPrepassColorPassActive = false;
        // Overdraw debug view (#519): when true, ApplyPODRenderState forces
        // additive (GL_ONE, GL_ONE) blending with depth testing DISABLED and the
        // colour mask ON, and the batchable opaque draws are swapped for the
        // depth-only DepthPrepass* programs (reusing DepthPrepassShaders) whose
        // fragment stage emits 1.0 — so every covered fragment adds 1 to the
        // accumulation target's red channel. Geometry with no depth-only variant
        // (skybox / terrain / voxel / custom shaders) is skipped so its full
        // material shader can't pollute the counter.
        bool OverdrawActive = false;
        // Water surface-depth capture: forces depth-only state even for the blended
        // water draw so the nearest water surface is written to its own depth target.
        bool WaterDepthCaptureActive = false;
        // Depth-only water program swapped in during the capture (snapshot in
        // SetWaterDepthCaptureActive so shader hot-reloads are picked up).
        RHI::ResourceHandle WaterDepthShaderID{};

        CommandDispatch::Statistics Stats;
    };

    static CommandDispatchData s_Data;

    void CommandDispatch::InvalidateUBOCache(u32 bindingPoint)
    {
        if (bindingPoint < CommandDispatchData::MAX_TRACKED_UBO_BINDINGS)
        {
            s_Data.BoundUBOs[bindingPoint] = RHI::NullResource;
        }
    }

    void CommandDispatch::InvalidateTextureSlot(u32 slot)
    {
        // For a pass that binds a texture unit with RAW GL (glBindTextureUnit) behind this
        // cache's back. The cache would otherwise still claim the slot holds whatever it last
        // put there, and BindTrackedTextureUnit would SKIP the real bind — leaving the
        // raw-bound texture live in the slot.
        //
        // That is exactly what VirtualGeometryPass hit: it binds the Hi-Z pyramid to unit 0
        // for the cull compute, and unit 0 is also u_AlbedoMap. Any material whose albedo ID
        // matched the stale cache entry silently sampled the HZB depth texture as its albedo.
        //
        // Still required after the identity migration: the raw binder bypasses this cache
        // entirely, so the cache's claim about the slot is simply untrue and no keying
        // scheme can detect that from the inside.
        if (slot < s_Data.BoundTextures.size())
        {
            s_Data.BoundTextures[slot] = RHI::NullResource;
        }
    }

    void CommandDispatch::InvalidateTextureBinding(RHI::ResourceHandle texture)
    {
        if (!texture.IsValid())
            return;

        // MORE load-bearing after the identity migration, not less — the one
        // place where keying on handles is WEAKER than keying on driver names,
        // so it is worth being explicit about.
        //
        // The old hazard is gone: a deleted texture's handle is retired, so it
        // can never compare equal to a live one, and the recycled-GL-name skip
        // this used to guard against is now unrepresentable.
        //
        // But an IN-PLACE RELOAD deliberately PRESERVES the identity while
        // replacing the storage behind it (ScopedResourceHandle::Sync never
        // retires — see OpenGLTexture::InvalidateImpl, which is exactly this
        // path). The cache would then still hold this very handle, conclude
        // "already bound", and skip a bind that genuinely must happen, leaving
        // the unit pointing at the deleted GL name. Under the old native-id
        // keying that self-corrected, because the name changed.
        //
        // So: every site that recreates a texture's storage MUST call this.
        for (auto& slot : s_Data.BoundTextures)
        {
            if (slot == texture)
                slot = RHI::NullResource;
        }
    }

    // Conditionally bind a UBO only when the binding point has changed,
    // avoiding a redundant binding-point update each draw.
    static void BindUBOIfNeeded(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        if (bindingPoint < CommandDispatchData::MAX_TRACKED_UBO_BINDINGS)
        {
            if (s_Data.BoundUBOs[bindingPoint] == buffer)
                return;
            s_Data.BoundUBOs[bindingPoint] = buffer;
        }
        RenderCommand::BindUniformBuffer(bindingPoint, buffer);
    }

    // Conditionally bind a VAO only when it differs from the currently bound one.
    // This cache is why the draws below use the DrawBound* family rather than the
    // DrawIndexedRaw(vaoID, ...) one: the latter binds the VAO itself, which would
    // make the cache pointless.
    static void BindVAOIfNeeded(RHI::ResourceHandle vertexArray)
    {
        if (s_Data.CurrentBoundVAO != vertexArray)
        {
            RenderCommand::BindVertexArrayRaw(vertexArray);
            s_Data.CurrentBoundVAO = vertexArray;
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
            api.SetPolygonMode(s_Default.polygonMode);
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
                api.SetDepthFunc(RHI::CompareOp::Less);
                api.SetBlendState(false);
            }
            // During color pass of depth prepass, override depth to GL_LEQUAL + no writes
            else if (s_Data.DepthPrepassColorPassActive)
            {
                api.SetDepthFunc(RHI::CompareOp::LessOrEqual);
                api.SetDepthMask(false);
            }
            // During the overdraw debug view, count every covered fragment:
            // additive blend, depth test off (so occluded fragments count too),
            // colour writes on, depth writes off.
            else if (s_Data.OverdrawActive)
            {
                api.SetColorMask(true, true, true, true);
                api.SetDepthTest(false);
                api.SetDepthMask(false);
                api.SetBlendState(true);
                api.SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::One);
                api.SetBlendEquation(RHI::BlendOp::Add);
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
        api.SetPolygonMode(state.polygonMode);

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
                api.SetDepthFunc(RHI::CompareOp::Less);
                api.SetBlendState(false);
                api.SetStencilMask(0); // Depth-prepass opaques emit depth only — leave stencil alone.
            }
        }
        // During color pass of depth prepass, override depth to GL_LEQUAL + no writes
        else if (s_Data.DepthPrepassColorPassActive)
        {
            api.SetDepthFunc(RHI::CompareOp::LessOrEqual);
            api.SetDepthMask(false);
        }
        // During the overdraw debug view, count every covered fragment regardless
        // of the command's own blend/depth state: additive accumulation, depth
        // test off (occluded fragments count), colour writes on, depth writes off.
        // The per-command cull face is kept (from the state applied above) so a
        // back-face-culled opaque mesh still counts one layer per pixel, matching
        // what the real forward/G-Buffer pass would shade.
        else if (s_Data.OverdrawActive)
        {
            api.SetColorMask(true, true, true, true);
            api.SetDepthTest(false);
            api.SetDepthMask(false);
            api.SetBlendState(true);
            api.SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::One);
            api.SetBlendEquation(RHI::BlendOp::Add);
            api.SetStencilMask(0);
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
            api.SetDepthFunc(RHI::CompareOp::Less);
            api.SetBlendState(false);
        }
    }

    // Helper: Upload material UBO and bind material textures.
    // Skips entirely when materialDataIndex matches the last-used index.
    // Helper: Conditionally bind a texture only when the slot isn't already
    // bound to the same ID, updating tracking and stats.
    //
    // The texture TARGET parameter is gone. It existed because this used the
    // legacy glActiveTexture + glBindTexture(target, id) pair, which needs to be
    // told which target of the unit to touch; the facade's BindTexture is the
    // DSA form, where the target is a property of the texture object itself. The
    // 2D-vs-cubemap distinction was therefore never carrying information the
    // driver did not already have (issue #691 Phase 2 step 2).
    // THE REDUNDANT-BIND CACHE MUST NOT SHORT-CIRCUIT THE OFFSET WRITE, and the
    // reason is that the two caches guard different claims. `BoundTextures[slot]`
    // means "this slot's GL BINDING is already correct", which is sound because a
    // GL binding persists until something rebinds it and every tracked rebind goes
    // through here. It does NOT mean "this slot's OFFSET is already correct": the
    // offset table is shared with every pass that binds through the seam, and those
    // passes do not update this array. Skipping the write would leave another
    // pass's offset in the slot and sample a different real texture.
    //
    // Skipping also saves nothing under the heap — the write is a store into a CPU
    // scratch array, not a driver call. The cache still earns its keep on the
    // slot-based path, which is what the predicate selects between.
    // What the cache is allowed to claim after a seam call, in one place.
    //
    // A VALID RETURNED OFFSET MEANS NO GL BIND HAPPENED — the seam staged an
    // offset instead. Recording the texture anyway makes BoundTextures assert a
    // binding that was never made, and the next SLOT-BASED consumer of the same
    // slot hits the cache and skips its own bind, sampling whatever that unit
    // last held. The two helpers below got this right while every direct
    // dispatch-handler call site (skybox, quad, decal, foliage) did not, which is
    // exactly the kind of divergence a free function stops (issue #691 Phase 3).
    [[nodiscard]] static auto CacheEntryAfterSeam(const RHI::HeapOffset staged, const RHI::ResourceHandle texture)
        -> RHI::ResourceHandle
    {
        return staged.IsValid() ? RHI::NullResource : texture;
    }

    static void BindTrackedTexture(RHI::ResourceHandle texture, u32 slot,
                                   RHI::NullSamplerKind kind = RHI::NullSamplerKind::Texture2D,
                                   const RHI::SamplerDesc& sampler = {})
    {
        if (!texture.IsValid())
            return;
        if (s_Data.BoundTextures[slot] == texture && !HeapBinding::WritesOffsetsForBoundProgram())
            return;

        // Persistent: material and IBL textures are asset-owned and outlive the
        // frame, so their descriptors are memoised rather than drawn from the ring.
        const RHI::HeapOffset staged =
            HeapBinding::BindTextureOrOffset(slot, texture, RHI::HeapSlotLifetime::Persistent, sampler, kind);

        // ONLY CLAIM A GL BINDING WHEN ONE ACTUALLY HAPPENED. The cache's
        // invariant, stated above, is "this slot's GL BINDING is already
        // correct" — sound only while every tracked call ends in a bind. Under
        // the heap it does not: a valid returned offset means the seam STAGED an
        // offset and issued no bind at all. Recording the texture anyway made the
        // array assert a binding that was never made, and the next SLOT-BASED
        // consumer of the same slot would hit the cache and skip its own bind —
        // sampling whatever that unit last held. Latent until a shader sharing a
        // slot with an unconverted one converts, which is precisely what the
        // shadow arrays and the IBL trio do (issue #691 Phase 3).
        s_Data.BoundTextures[slot] = CacheEntryAfterSeam(staged, texture);
        ++s_Data.Stats.TextureBinds;
    }

    // Resolve a material's nine textures to heap offsets for the material UBO.
    //
    // Persistent lifetime throughout: these are asset-owned textures, never
    // graph-pooled, so the descriptors are memoised and the offsets are stable
    // across frames — which is what keeps the material UBO cacheable
    // (issue #691 Phase 3, ADR 0011 amendment (32)).
    //
    // Order MUST match OLO_MATERIAL_* in include/BindlessHeap.glsl. Getting it
    // wrong swaps two real textures rather than producing an obvious error.
    static void WriteMaterialHeapOffsets(const PODMaterialData& mat,
                                         ShaderBindingLayout::PBRMaterialUBO& ubo)
    {
        // A BINDLESS DESCRIPTOR BAKES SAMPLER STATE; A SLOT BIND DOES NOT.
        // glBindTextureUnit samples with whatever the TEXTURE OBJECT carries,
        // while a heap handle carries what its descriptor was minted with — so
        // minting with SamplerDesc{} makes the converted shader sample
        // differently from the unconverted one, plausibly and silently. Measured
        // at RMSE 5.861 vs 1.413 on WorldOriginRebaseVisualEvidence before this
        // was carried through (issue #691 Phase 3).
        //
        // The right state is READ OFF THE BACKEND, not assumed: every
        // OpenGLTexture2D path sets LINEAR / LINEAR_MIPMAP_LINEAR + GL_REPEAT and
        // no anisotropy, while OpenGLTextureCubemap sets the same filters with
        // GL_CLAMP_TO_EDGE. So the only field that differs from the default is the
        // address mode, and it differs by texture TYPE — which this call site
        // knows statically. That is why the sampler does not need to travel in
        // PODMaterialData: it is a property of the backend's uniform policy, not
        // of the individual material.
        //
        // IF THAT POLICY EVER BECOMES PER-TEXTURE (a user-configurable wrap mode,
        // a clamped LUT), this stops being derivable here and the state must
        // travel with the handle. The symptom would be a subtly wrong image, so
        // change these together with OpenGLTexture2D/Cubemap, never separately.
        static const RHI::SamplerDesc k2DSampler = []
        {
            RHI::SamplerDesc desc;
            desc.Source = RHI::SamplerSource::Explicit;
            desc.AddressU = RHI::AddressMode::Repeat;
            desc.AddressV = RHI::AddressMode::Repeat;
            desc.AddressW = RHI::AddressMode::Repeat;
            return desc;
        }();
        // Default: the descriptor inherits the cubemap object's own state
        // (OpenGLTextureCubemap is CLAMP_TO_EDGE) — see AcquireSampledDescriptor.
        static const RHI::SamplerDesc kCubeSampler{};

        // TWO DIFFERENT NULLS, and only one of them is correct. A material with no
        // albedo map SHOULD resolve to the reserved null — the shader gates on
        // u_UseAlbedoMap and never samples it. A material that HAS one and fails to
        // get a descriptor (heap exhausted, dead resource, a view this backend
        // cannot express) resolves to the same null and renders black, silently.
        //
        // There is no per-draw fallback to reach for: a shader builds EITHER the
        // bindless program or the slot-based one at compile time, so by the time
        // this runs the program in flight already reads material offsets and its
        // slot-based binds have been withdrawn (§5c). Binding the five slots anyway
        // would change nothing the shader reads. So the honest handling is to make
        // the failure OBSERVABLE rather than to pretend it degraded gracefully —
        // silently-black is precisely the failure mode this phase keeps finding.
        // THE NULL IT FALLS BACK TO IS TYPED. A shader builds `samplerCube` from
        // the environment / irradiance / prefilter lanes, and a descriptor whose
        // target does not match the constructor is undefined to sample. The GLSL
        // accessor already remaps offset 0 to the cube null (OLO_HEAP_TYPED_NULL in
        // include/BindlessHeap.glsl), so this is not a live defect — it makes the
        // UBO correct ON ITS OWN rather than only in company with that macro.
        const auto resolve = [](const RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler,
                                const RHI::NullSamplerKind kind)
        {
            if (!texture.IsValid())
            {
                return RHI::NullOffsetForSamplerKind(kind);
            }
            const RHI::HeapOffset offset =
                HeapBinding::ResolveTextureOffset(texture, RHI::HeapSlotLifetime::Persistent, sampler, kind);
            if (!offset.IsValid())
            {
                // ADR 0011 (68): a diagnostic naming a CAUSE must exclude the
                // other causes of the same symptom. An invalid offset here has
                // two structurally different sources, and only one is a
                // failure: when the heap path simply is not live (Vulkan's
                // deliberate slot-path design, or OLO_RHI_BINDLESS off on GL)
                // the slot binds carry the texture and nothing renders black —
                // warning "render BLACK" there sent live bring-up to the wrong
                // layer. Only a program that actually READS material offsets
                // can be hurt by a null offset.
                if (HeapBinding::WritesOffsetsForBoundProgram())
                {
                    // Warned a bounded number of times: a heap that has run
                    // out stays out, so an unbounded log would bury every
                    // other message.
                    if (static std::atomic<u64> s_ResolveFailures{ 0 };
                        s_ResolveFailures.fetch_add(1, std::memory_order_relaxed) < 8)
                    {
                        OLO_CORE_WARN("CommandDispatch: material texture heap-descriptor ACQUISITION failed — the "
                                      "bindless program will sample the reserved null and render BLACK (it cannot "
                                      "fall back to slot binds, issue #691 Phase 3).");
                    }
                }
                return RHI::NullOffsetForSamplerKind(kind);
            }
            return offset.Value;
        };

        constexpr auto k2D = RHI::NullSamplerKind::Texture2D;
        constexpr auto kCube = RHI::NullSamplerKind::Cube;
        ubo.HeapOffsets[0] = { resolve(mat.albedoMapID, k2DSampler, k2D),
                               resolve(mat.metallicRoughnessMapID, k2DSampler, k2D),
                               resolve(mat.normalMapID, k2DSampler, k2D),
                               resolve(mat.aoMapID, k2DSampler, k2D) };
        ubo.HeapOffsets[1] = { resolve(mat.emissiveMapID, k2DSampler, k2D),
                               resolve(mat.environmentMapID, kCubeSampler, kCube),
                               resolve(mat.irradianceMapID, kCubeSampler, kCube),
                               resolve(mat.prefilterMapID, kCubeSampler, kCube) };
        // The BRDF LUT is a Texture2D (EnvironmentMap.cpp creates it through
        // Texture2D::Create), so it takes GL_REPEAT like every other 2D map —
        // matching the slot path, which is the property that matters here even
        // though a lookup table would ideally clamp.
        ubo.HeapOffsets[2] = { resolve(mat.brdfLutMapID, k2DSampler, k2D),
                               resolve(mat.diffuseMapID, k2DSampler, k2D),
                               resolve(mat.specularMapID, k2DSampler, k2D), RHI::kNullHeapOffset };
    }

    // Helper: Bind all PBR material textures (albedo, metallic-roughness, normal,
    // AO, emissive, environment cubemap, irradiance, prefilter, BRDF LUT).
    //
    // THE NINE BINDS ARE THE POINT OF THE WHOLE PHASE. When the program in flight
    // reads its material textures out of the heap, they are pure waste — the
    // offsets already travelled in the material UBO. Skipping them here is what
    // turns bindless from a lateral move into a win on the hot path.
    //
    // Gated on the PROGRAM, not on the heap toggle: a slot-based shader in a
    // bindless-enabled build still needs every one of these
    // (HeapBinding::WritesOffsetsForBoundProgram, issue #691 Phase 3).
    static void BindPBRTextures(const PODMaterialData& mat)
    {
        // THE NARROW QUESTION, not the broad one. WritesOffsetsForBoundProgram()
        // answers "is this a bindless variant", which is true for a program that
        // converted any unrelated input while still declaring slot-based material
        // samplers — skipping the binds for one of those renders it unlit.
        //
        // AND ONLY THE FIVE MATERIAL-LOCAL MAPS ARE SKIPPED. The environment map
        // and the IBL trio are PUBLISHED state, not material state: most materials
        // carry no handles for them, and what the shader must sample is whatever
        // DeferredLightingPass published to TEX_ENVIRONMENT / TEX_USER_0..2 for the
        // frame. Routing those through per-material offsets resolves an invalid
        // handle to the reserved null and the mesh loses all ambient light — a
        // dark scene with no error. They stay on the shared offset table, which is
        // exactly what BindTrackedTexture stages (issue #691 Phase 3).
        if (const bool materialLocalFromHeap = Shader::ReadsMaterialHeapOffsets(); !materialLocalFromHeap)
        {
            BindTrackedTexture(mat.albedoMapID, ShaderBindingLayout::TEX_DIFFUSE);
            BindTrackedTexture(mat.metallicRoughnessMapID, ShaderBindingLayout::TEX_SPECULAR);
            BindTrackedTexture(mat.normalMapID, ShaderBindingLayout::TEX_NORMAL);
            BindTrackedTexture(mat.aoMapID, ShaderBindingLayout::TEX_AMBIENT);
            BindTrackedTexture(mat.emissiveMapID, ShaderBindingLayout::TEX_EMISSIVE);
        }
        // TEX_USER_0/1 are samplerCube here (irradiance, prefilter) and plain 2D in
        // other consumers — which is why the kind cannot be derived from the SLOT
        // and has to come from the call site that knows what it is binding.
        BindTrackedTexture(mat.environmentMapID, ShaderBindingLayout::TEX_ENVIRONMENT,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(mat.irradianceMapID, ShaderBindingLayout::TEX_USER_0, RHI::NullSamplerKind::Cube);
        BindTrackedTexture(mat.prefilterMapID, ShaderBindingLayout::TEX_USER_1, RHI::NullSamplerKind::Cube);
        BindTrackedTexture(mat.brdfLutMapID, ShaderBindingLayout::TEX_USER_2);
    }

    // Helper: Bind legacy material textures (diffuse, specular).
    static void BindLegacyTextures(const PODMaterialData& mat)
    {
        BindTrackedTexture(mat.diffuseMapID, ShaderBindingLayout::TEX_DIFFUSE);
        BindTrackedTexture(mat.specularMapID, ShaderBindingLayout::TEX_SPECULAR);
    }

    static void UploadMaterialState(const PODMaterialData& mat, u16 materialDataIndex)
    {
        OLO_PROFILE_FUNCTION();

        // THE MATERIAL UBO IS CACHED ON THE MATERIAL INDEX, which is what makes
        // per-material heap offsets free — they ride in a buffer that is only
        // re-uploaded when the material actually changes. That cache has to be
        // dropped across a heap re-initialisation, though: the offsets in it index
        // the PREVIOUS heap, and unlike a stale texture bind that reads as a
        // plausible wrong image rather than an obvious one (issue #691 Phase 3).
        if (const u64 heapEpoch = RHI::DescriptorHeap::Get().GetInitEpoch(); heapEpoch != s_Data.HeapEpoch)
        {
            s_Data.HeapEpoch = heapEpoch;
            s_Data.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        }

        // Part of the cache key, not an afterthought — see LastMaterialOffsetsLive.
        const bool offsetsLive = Shader::ReadsMaterialHeapOffsets();
        const bool sameIndex = (materialDataIndex == s_Data.LastMaterialDataIndex) &&
                               (offsetsLive == s_Data.LastMaterialOffsetsLive);
        s_Data.LastMaterialDataIndex = materialDataIndex;
        s_Data.LastMaterialOffsetsLive = offsetsLive;

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
                pbrMaterialData.UseAlbedoMap = mat.albedoMapID.IsValid() ? 1 : 0;
                pbrMaterialData.UseNormalMap = mat.normalMapID.IsValid() ? 1 : 0;
                pbrMaterialData.UseMetallicRoughnessMap = mat.metallicRoughnessMapID.IsValid() ? 1 : 0;
                pbrMaterialData.UseAOMap = mat.aoMapID.IsValid() ? 1 : 0;
                pbrMaterialData.UseEmissiveMap = mat.emissiveMapID.IsValid() ? 1 : 0;
                pbrMaterialData.EnableIBL = mat.enableIBL ? 1 : 0;
                pbrMaterialData.ApplyGammaCorrection = 1;
                pbrMaterialData.AlphaCutoff = mat.alphaCutoff;
                pbrMaterialData.AlphaMode = mat.alphaMode;
                // Issue #632: this was a hard-coded 0, which made the forward
                // path's probe-ambient shader code dead. Wire it to the same
                // master toggle the deferred path uses so Forward+ scenes get
                // probe GI (baked SH or realtime DDGI) too.
                pbrMaterialData.EnableLightProbes = Renderer3D::GetRendererSettings().Deferred.EnableLightProbes ? 1 : 0;
                pbrMaterialData.IBLIntensity = mat.iblIntensity;

                // Per-material heap offsets. Persistent, not FrameTransient: these
                // are ASSET-owned textures, so their descriptors are memoised and
                // stable — which is precisely what lets the UBO stay cached on the
                // material index instead of being re-uploaded every frame. A
                // FrameTransient offset would go stale at the frame boundary while
                // the cache happily served last frame's value.
                //
                // Every one resolves to an invalid offset when the heap path is not
                // live for the program in flight, so the slot-path binds below still
                // happen and nothing changes on the default path.
                WriteMaterialHeapOffsets(mat, pbrMaterialData);

                if (s_Data.MaterialUBO)
                {
                    constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
                    static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch");
                    s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
                    BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRHIHandle());
                }
            }
            else if (s_Data.MaterialUBO)
            {
                // Even when material data hasn't changed, re-establish the binding
                // point (other subsystems may have overwritten it).
                BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRHIHandle());
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
                    BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRHIHandle());
                }
            }
            else if (s_Data.MaterialUBO)
            {
                // Even when material data hasn't changed, re-establish the
                // binding point — other subsystems (e.g. ParticleBatchRenderer)
                // may have overwritten UBO_MATERIAL.
                BindUBOIfNeeded(ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRHIHandle());
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

    // Bind a texture to a unit only when it differs from the currently-tracked
    // binding, updating the redundant-bind tracker and the bind stat. A 0 id is a
    // no-op (no texture for that slot this frame). Shared by every tracked bind so
    // the check/update/increment logic lives in exactly one place.
    static void BindTrackedTextureUnit(u32 slot, RHI::ResourceHandle texture,
                                       const RHI::SamplerDesc& sampler = {},
                                       RHI::NullSamplerKind kind = RHI::NullSamplerKind::Texture2D)
    {
        if (!texture.IsValid())
            return;
        // Same cache/offset split as BindTrackedTexture above — see the note there.
        if (s_Data.BoundTextures[slot] == texture && !HeapBinding::WritesOffsetsForBoundProgram())
            return;

        // Persistent: shadow maps, the snow clipmap and the cloud-shadow map are
        // system-owned and survive the frame, like the material textures above.
        const RHI::HeapOffset staged =
            HeapBinding::BindTextureOrOffset(slot, texture, RHI::HeapSlotLifetime::Persistent, sampler, kind);
        // Same correction as BindTrackedTexture — see the note there.
        s_Data.BoundTextures[slot] = CacheEntryAfterSeam(staged, texture);
        ++s_Data.Stats.TextureBinds;
    }

    // Helper: Bind per-frame shadow and snow depth textures (only relevant for PBR paths).
    // Relies on BoundTextureIDs tracking to avoid redundant binds.
    static void BindShadowTextures()
    {
        // Shared with every other site that stages a shadow-map offset — the
        // state has to be identical or whichever pass ran last silently wins.
        static const RHI::SamplerDesc kShadowCompare = HeapBinding::ShadowDepthSampler(true);
        static const RHI::SamplerDesc kShadowRaw = HeapBinding::ShadowDepthSampler(false);

        // The KIND travels with the bind, not just the sampler: these four are the
        // array-typed inputs on the shared table, so a retired one must poison to
        // its own typed null rather than the 2D one (issue #691 Phase 3).
        BindTrackedTextureUnit(ShaderBindingLayout::TEX_SHADOW, s_Data.CSMShadowTexture, kShadowCompare,
                               RHI::NullSamplerKind::Texture2DArrayShadow);
        BindTrackedTextureUnit(ShaderBindingLayout::TEX_SHADOW_ATLAS, s_Data.AtlasShadowTexture, kShadowCompare,
                               RHI::NullSamplerKind::Texture2DArrayShadow);

        // Comparison-OFF raw-depth views for the PCSS blocker search (plain
        // sampler2DArray at TEX_SHADOW_CSM_RAW / TEX_SHADOW_ATLAS_RAW).
        BindTrackedTextureUnit(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, s_Data.CSMRawShadowTexture, kShadowRaw,
                               RHI::NullSamplerKind::Texture2DArray);
        BindTrackedTextureUnit(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, s_Data.AtlasRawShadowTexture, kShadowRaw,
                               RHI::NullSamplerKind::Texture2DArray);

        // Virtual Shadow Maps (issue #702) — the forward path's half of the same
        // publish the deferred lighting pass does. Unconditional: an inactive VSM
        // publishes a disabled globals block, so the forward PBR shaders' runtime
        // branch resolves to CSM without a second shader variant.
        Renderer3D::GetShadowMap().GetVirtualShadowMap().BindForSampling();

        BindTrackedTextureUnit(ShaderBindingLayout::TEX_SNOW_DEPTH, s_Data.SnowDepthTexture);

        // Cloud shadow transmittance map (issue #633). A 0 id binds nothing —
        // the AtmosphereShadingUBO enabled flag gates the shader-side sample,
        // so an unbound-but-declared sampler is never actually read.
        //
        // PUBLISHED AND BOUND, the DDGI-atlas case: its declaration lives in the
        // SHARED include/AtmosphereShading.glsl, so one includer being on the
        // bindless route (PBR_MultiLight) does not convert the declaration for
        // the slot-based ones (Terrain_PBR, DeferredLightingShared) — and it
        // cannot be converted there without dragging every includer onto the
        // route, since the header's own `#ifdef OLO_BINDLESS` IS the opt-in
        // token. Staging the offset AND binding serves both readers, which is
        // exactly what this seam entry point exists for.
        if (s_Data.CloudShadowTexture.IsValid())
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_CLOUD_SHADOW,
                                                     s_Data.CloudShadowTexture,
                                                     RHI::HeapSlotLifetime::Persistent);
        }
    }

    // Helper: resolve the program to bind for a mesh draw during the depth
    // prepass. The four standard mesh programs (PBR forward / G-Buffer, static
    // / skinned) are swapped for the minimal DepthPrepass*.glsl depth-only
    // programs — running the full lighting FS in the prepass multiplies the
    // per-pixel cost by (overdraw + 1) instead of eliminating overdraw, which
    // measured as ~90% of the whole scene pass on Sponza (PCSS per covered
    // fragment, twice). MASK materials get the *_Mask variants so the glTF
    // alpha test keeps carving the same depth coverage as the color pass.
    // Anything else (custom shaders) keeps its own program: its vertex path is
    // unknown, so only it is guaranteed to reproduce its color-pass depth.
    static RHI::ResourceHandle ResolveDepthPrepassShader(const PODMaterialData& mat)
    {
        const auto& ids = s_Data.DepthPrepassShaders;
        const bool isStatic = (mat.shaderRendererID == ids.PBRStatic ||
                               mat.shaderRendererID == ids.GBufferStatic);
        const bool isSkinned = !isStatic &&
                               (mat.shaderRendererID == ids.PBRSkinned ||
                                mat.shaderRendererID == ids.GBufferSkinned);
        if (!isStatic && !isSkinned)
            return mat.shaderRendererID;

        const bool isMask = (mat.alphaMode == 1);
        const RHI::ResourceHandle depthShader = isStatic
                                                    ? (isMask ? ids.DepthMaskStatic : ids.DepthStatic)
                                                    : (isMask ? ids.DepthMaskSkinned : ids.DepthSkinned);
        return depthShader.IsValid() ? depthShader : mat.shaderRendererID;
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
            BindUBOIfNeeded(ShaderBindingLayout::UBO_ANIMATION, s_Data.BoneMatricesUBO->GetRHIHandle());
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
                BindUBOIfNeeded(ShaderBindingLayout::UBO_ANIMATION_PREV, s_Data.PrevBoneMatricesUBO->GetRHIHandle());
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

            // Camera-relative (issue #429): shift the world transform (and its
            // previous-frame counterpart, for motion vectors) into render-
            // relative space before upload. This one choke point covers every
            // single-instance mesh path — main, depth-only, quad, terrain,
            // voxel. The normal matrix is translation-invariant so it is
            // uploaded unchanged. Near origin the origin is (0,0,0) and this is
            // a no-op. Read the render origin from CommandDispatch's own state
            // (the same source UploadCameraUBO and DrawMeshInstanced use) so the
            // relative transform and the camera shift cannot diverge.
            const glm::vec3 origin = CommandDispatch::GetRenderOrigin();

            InstanceData inst;
            inst.Transform = MakeModelRelative(modelData.Model, origin);
            inst.Normal = modelData.Normal;
            inst.PrevTransform = MakeModelRelative(modelData.PrevModel, origin);
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
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
        }

        if (s_Data.ForwardPlus)
        {
            s_Data.ForwardPlus->UploadDisabledUBO();
        }
    }

    void CommandDispatch::UploadMaterialForDirectDraw(const PODMaterialData& mat, u16 materialDataIndex)
    {
        UploadMaterialState(mat, materialDataIndex);
    }

    void CommandDispatch::ResetState()
    {
        s_Data.CurrentBoundShader = {};
        s_Data.CurrentBoundVAO = {};
        s_Data.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
        s_Data.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        s_Data.BoundTextures.fill(RHI::NullResource);
        s_Data.CurrentViewportWidth = 0;
        s_Data.CurrentViewportHeight = 0;
        s_Data.BoundUBOs.fill(RHI::NullResource);
        s_Data.CSMShadowTexture = {};
        s_Data.AtlasShadowTexture = {};
        s_Data.CSMRawShadowTexture = {};
        s_Data.AtlasRawShadowTexture = {};
        s_Data.SnowDepthTexture = {};
        s_Data.CloudShadowTexture = {};
        s_Data.DepthPrepassActive = false;
        s_Data.DepthPrepassColorPassActive = false;
        s_Data.OverdrawActive = false;
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
            // Snapshot the depth-only swap programs once per prepass — cheap,
            // and keeps the swap correct across shader hot-reloads (renderer
            // IDs can change when a program is recompiled).
            s_Data.DepthPrepassShaders = Renderer3D::GetDepthPrepassShaderIDs();
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

    void CommandDispatch::SetOverdrawActive(bool active)
    {
        s_Data.OverdrawActive = active;
        if (active)
        {
            // Overdraw is mutually exclusive with the depth-prepass modes.
            s_Data.DepthPrepassActive = false;
            s_Data.DepthPrepassColorPassActive = false;
            // Snapshot the depth-only swap programs once (same rationale as
            // SetDepthPrepassActive) — overdraw reuses them so every batchable
            // opaque geometry type keeps its correct vertex transform while its
            // fragment stage emits the constant overdraw count.
            s_Data.DepthPrepassShaders = Renderer3D::GetDepthPrepassShaderIDs();
        }
        // Invalidate cache so the next command re-applies state.
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetWaterDepthCaptureActive(bool active)
    {
        s_Data.WaterDepthCaptureActive = active;
        if (active)
        {
            // Snapshot the depth-only water program once per capture — same
            // hot-reload rationale as SetDepthPrepassActive.
            s_Data.WaterDepthShaderID = Renderer3D::GetWaterDepthShaderID();
        }
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

    void CommandDispatch::SetRenderOrigin(const glm::vec3& origin)
    {
        s_Data.RenderOrigin = origin;
    }

    const glm::vec3& CommandDispatch::GetRenderOrigin()
    {
        return s_Data.RenderOrigin;
    }

    void CommandDispatch::UploadCameraUBO()
    {
        if (!s_Data.CameraUBO)
            return;

        // Same packing as the terrain/voxel inline uploads — derive Projection
        // from VP * inverse(View) so callers only have to set the three matrices
        // + position via Set*. PrevViewProjection comes from the true previous
        // frame propagated by Renderer3D (no aliasing of the current VP).
        //
        // Camera-relative (issue #429): the stored matrices are world-space, so
        // rebuild the view / view-projection about the render origin and supply
        // the camera position relative to it before upload. This one path covers
        // the shared re-upload *and* the planar-reflection mirror camera (which
        // sets world mirror matrices via Set* then calls this).
        const glm::vec3 origin = s_Data.RenderOrigin;
        const glm::mat4 relView = MakeViewRelative(s_Data.ViewMatrix, origin);
        // Projection is translation-free, so it is identical whether taken from
        // the world or relative VP — derive it once for the UBO's Projection.
        const glm::mat4 projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);

        // A8 projection seam: the stored matrices stay GL-convention; the
        // GPU-visible copies flip at upload (identity on GL). This one seam
        // covers the shared re-upload AND the planar-reflection mirror camera.
        // PrevViewProjection rides the same rasterizer-flavour flip: its
        // consumers difference clip .xy against the current (flipped)
        // rasterized position, and the two flip flavours agree on .xy.
        ShaderBindingLayout::CameraUBO cameraData{};
        cameraData.ViewProjection = RHI::AdjustProjectionForBackend(projection * relView);
        cameraData.View = relView;
        cameraData.Projection = RHI::AdjustProjectionForBackend(projection);
        cameraData.Position = MakePositionRelative(s_Data.ViewPos, origin);
        cameraData._padding0 = 0.0f;
        cameraData.PrevViewProjection = RHI::AdjustProjectionForBackend(
            MakeViewProjectionRelative(s_Data.PrevViewProjectionMatrix, origin));
        cameraData.RenderOrigin = origin; // for pattern shaders (triplanar/noise/etc.)
        // Reconstruction flavour (#691 Phase 8): terrain tessellation scale,
        // water depth math and the culling compute read this member.
        cameraData.ProjectionForReconstruction = RHI::AdjustProjectionForShaderReconstruction(projection);
        s_Data.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
        BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
    }

    void CommandDispatch::SetShadowTextures(RHI::ResourceHandle csmTexture, RHI::ResourceHandle atlasTexture,
                                            RHI::ResourceHandle csmRawTexture, RHI::ResourceHandle atlasRawTexture)
    {
        s_Data.CSMShadowTexture = csmTexture;
        s_Data.AtlasShadowTexture = atlasTexture;
        s_Data.CSMRawShadowTexture = csmRawTexture;
        s_Data.AtlasRawShadowTexture = atlasRawTexture;
    }

    void CommandDispatch::SetSnowDepthTexture(RHI::ResourceHandle texture)
    {
        s_Data.SnowDepthTexture = texture;
    }

    void CommandDispatch::SetCloudShadowTexture(RHI::ResourceHandle texture)
    {
        s_Data.CloudShadowTexture = texture;
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
        api.SetPolygonMode(cmd->mode);
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
        HeapBinding::BindTextureOrOffset(api, cmd->slot, cmd->textureID, RHI::HeapSlotLifetime::Persistent);
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

    void CommandDispatch::DrawIndexed(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedCommand*>(data);

        if (!cmd->vertexArrayID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexed: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw.
        //
        // EVERY DRAW IN THIS FILE PUBLISHES THE STAGED OFFSETS, and the uniformity
        // is the point (issue #691 Phase 3). Whether a handler owes a flush is a
        // property of the SHADER it happens to be dispatching — which changes as
        // shaders convert — not of the handler, so pairing them per site is a rule
        // that has to be re-derived every time a `.glsl` gains an `#ifdef
        // OLO_BINDLESS`. That is the §5c failure mode ("the unit of conversion is a
        // C++ bind AND its declaration") one level up, and it fails the quiet way:
        // the pass keeps rendering and reads last flush's offsets.
        //
        // ADR 0011 amendment (32) argued the opposite — that a per-draw flush gives
        // back the win, since it re-uploads the table and re-binds the heap. Half of
        // that is now false: `HeapBinding::StageOffset` ignores an identical write,
        // so a bucket of draws sharing textures dirties nothing and the upload does
        // not happen. What remains is `DescriptorHeap::Flush`'s heap rebind, which
        // is deliberately unconditional for a reason of its own, and one
        // glBindBufferBase per draw against the up-to-nine texture binds it removes.
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, cmd->indexType, 0);
    }

    void CommandDispatch::DrawIndexedInstanced(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedInstancedCommand*>(data);

        if (!cmd->vertexArrayID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexedInstanced: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw instanced
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexedInstanced(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, cmd->indexType,
                                      0, cmd->instanceCount);
    }

    void CommandDispatch::DrawArrays(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawArraysCommand*>(data);

        if (!cmd->vertexArrayID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawArrays: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw arrays
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundArrays(cmd->primitiveType, 0, cmd->vertexCount);
    }

    void CommandDispatch::DrawLines(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawLinesCommand*>(data);

        if (!cmd->vertexArrayID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawLines: Invalid vertex array ID");
            return;
        }

        // Bind VAO (cached) and draw lines
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundArrays(RHI::PrimitiveTopology::LineList, 0, cmd->vertexCount);
    }

    void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        auto const* cmd = static_cast<const DrawMeshCommand*>(data);

        // Resolve material data from table
        const auto& mat = FrameDataBufferManager::Get().GetMaterialData(cmd->materialDataIndex);

        // Validate POD renderer IDs
        if (!cmd->vertexArrayID.IsValid() || !mat.shaderRendererID.IsValid())
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

        // Bind shader using renderer ID directly. During the depth prepass the
        // standard PBR programs are swapped for minimal depth-only ones — the
        // prepass exists to eliminate overdraw, not to run the lighting FS
        // once more per covered fragment.
        RHI::ResourceHandle shaderToBind = mat.shaderRendererID;
        bool prepassDepthOnly = false;
        if (s_Data.DepthPrepassActive)
        {
            shaderToBind = ResolveDepthPrepassShader(mat);
            prepassDepthOnly = (shaderToBind != mat.shaderRendererID);
        }
        else if (s_Data.OverdrawActive)
        {
            // Overdraw counts coverage via the depth-only programs (their fragment
            // stage emits 1.0). Reuse the prepass shader resolve so every batchable
            // opaque geometry type keeps its correct vertex transform. Geometry
            // with no depth-only variant (skybox / terrain / voxel / custom
            // shaders) would otherwise run its full material shader additively and
            // pollute the counter, so skip it (an honest under-count of exotic
            // geometry rather than garbage).
            shaderToBind = ResolveDepthPrepassShader(mat);
            if (shaderToBind == mat.shaderRendererID)
                return;
            prepassDepthOnly = true;
        }
        else
        {
            /* Neither the depth prepass nor the overdraw view is active — the
               material's own shaderToBind and prepassDepthOnly=false set above
               already describe the normal colour-pass draw. */
        }
        if (s_Data.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            s_Data.CurrentBoundShader = shaderToBind;
            ++s_Data.Stats.ShaderBinds;
        }

        // During the depth prepass OR the overdraw view, only the model matrix and
        // bones are needed (vertex transform). Skip material, textures, normal
        // matrix, and light UBOs — the overdraw fragment stage emits a constant,
        // and MASK materials still get their UBO + albedo below for the alpha test.
        if (s_Data.DepthPrepassActive || s_Data.OverdrawActive)
        {
            // Camera UBO is still needed for vertex transform (u_ViewProjection)
            if (s_Data.CameraUBO)
            {
                BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
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

            // MASK materials still need the material UBO (alpha cutoff) and the
            // albedo texture so the depth-only alpha test carves the same
            // coverage as the color pass. (Also fixes the pre-swap behavior,
            // where the prepass ran the full shader against whatever material
            // state the previous draw left bound.)
            if (prepassDepthOnly && mat.alphaMode == 1)
            {
                UploadMaterialState(mat, cmd->materialDataIndex);
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
                BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
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
            const RHI::ResourceHandle query =
                OcclusionQueryPool::GetInstance().GetQueryHandle(cmd->occlusionQueryIndex);
            if (query.IsValid())
            {
                api.BeginConditionalRender(query);
                startedConditionalRender = true;
            }
        }

        // PUBLISH BEFORE THE DRAW THAT READS IT. The mesh paths stage two kinds
        // of offset — the material five into PBRMaterialUBO via
        // WriteMaterialHeapOffsets, and the published env/IBL slots into the
        // shared table via BindPBRTextures — and neither is visible to the GPU
        // until this call, which uploads the dirty DESCRIPTORS and then the
        // table. Without it the draw indexes slots whose descriptors were only
        // ever written to the CPU mirror. It survived because descriptors are
        // persistent and some later pass flushes them, so the damage is confined
        // to the first frame a material is seen — a one-frame wrong image, which
        // is the hardest kind to notice (issue #691 Phase 3).
        //
        // Cheap when nothing changed: FlushOffsets early-outs on a clean table
        // and DescriptorHeap::Flush uploads only the dirty span.
        HeapBinding::FlushOffsets();
        // baseIndex offsets into a single IBO shared by a multi-submesh
        // MeshSource. The index-count-to-byte-offset conversion now lives in
        // the backend, which is the only layer that knows the index stride.
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                             RHI::IndexType::UInt32, cmd->baseIndex);
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
        if (!cmd->vertexArrayID.IsValid() || !mat.shaderRendererID.IsValid())
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

        // Bind shader using renderer ID directly. During the depth prepass the
        // standard PBR programs are swapped for minimal depth-only ones — see
        // ResolveDepthPrepassShader (mirrors DrawMesh).
        RHI::ResourceHandle shaderToBind = mat.shaderRendererID;
        bool prepassDepthOnly = false;
        if (s_Data.DepthPrepassActive)
        {
            shaderToBind = ResolveDepthPrepassShader(mat);
            prepassDepthOnly = (shaderToBind != mat.shaderRendererID);
        }
        else if (s_Data.OverdrawActive)
        {
            // Overdraw counts coverage via the depth-only programs (their fragment
            // stage emits 1.0). Reuse the prepass shader resolve so every batchable
            // opaque geometry type keeps its correct vertex transform. Geometry
            // with no depth-only variant (skybox / terrain / voxel / custom
            // shaders) would otherwise run its full material shader additively and
            // pollute the counter, so skip it (an honest under-count of exotic
            // geometry rather than garbage).
            shaderToBind = ResolveDepthPrepassShader(mat);
            if (shaderToBind == mat.shaderRendererID)
                return;
            prepassDepthOnly = true;
        }
        else
        {
            /* Neither the depth prepass nor the overdraw view is active — the
               material's own shaderToBind and prepassDepthOnly=false set above
               already describe the normal colour-pass draw. */
        }
        if (s_Data.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            s_Data.CurrentBoundShader = shaderToBind;
            ++s_Data.Stats.ShaderBinds;
        }

        // Camera UBO: re-bind in case a prior pass (e.g. ShadowMap)
        // overwrote the binding point.  Mirrors the logic in DrawMesh's color path.
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
        }

        // Material UBO + texture bindings (skipped when material unchanged).
        // A depth-only prepass draw needs material state only for the MASK
        // alpha test (cutoff + albedo); opaque depth-only draws skip it.
        if (!prepassDepthOnly || mat.alphaMode == 1)
        {
            UploadMaterialState(mat, cmd->materialDataIndex);
        }

        // GPU-frustum-cull fast path: the cull compute already wrote
        // compacted survivors to `cullOutputInstanceBufferID` and the
        // surviving count into `cullIndirectBufferID`. Skip the FrameDataBuffer
        // -> InstanceData scratch loop and the upload; bind the pre-populated
        // output buffer at SSBO_INSTANCE_DATA and draw indirect.
        if (const bool useGPUCull = cmd->cullIndirectBufferID.IsValid() && cmd->cullOutputInstanceBufferID.IsValid(); useGPUCull)
        {
            // Rebind slot 15 to the per-submission output buffer. The engine-
            // wide `s_Data.ModelInstanceBuffer` is unchanged so it can be
            // reused by subsequent CPU-path draws in the same frame.
            api.BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_DATA, cmd->cullOutputInstanceBufferID);

            // Shadow/snow textures (per-frame, outside material diffing).
            // Depth-only prepass draws never sample shadows.
            if (mat.enablePBR && !prepassDepthOnly)
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
            // Publish before the indirect draw, same as the two direct paths.
            // UploadMaterialState and BindShadowTextures have staged offsets by
            // here; an indirect draw reads them exactly as a direct one does.
            HeapBinding::FlushOffsets();
            // The VAO is already bound by BindVAOIfNeeded above — draw from it
            // rather than re-binding behind the redundant-bind cache's back.
            api.DrawBoundElementsIndirect(cmd->cullIndirectBufferID);

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
                    cmd->vertexArrayID.Index,
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

            // Camera-relative (issue #429): shift every instance's world
            // transform (and its prev-frame transform) into render-relative
            // space. The normal matrix is translation-invariant, so it is still
            // derived from the world transform. No-op when origin is (0,0,0).
            const glm::vec3 origin = s_Data.RenderOrigin;
            for (sizet i = 0; i < instanceCount; ++i)
            {
                InstanceData& inst = scratch[i];
                inst.Transform = MakeModelRelative(transforms[i], origin);
                inst.Normal = glm::transpose(glm::inverse(transforms[i]));
                inst.PrevTransform = MakeModelRelative(prevTransforms[i], origin);
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

        // Shadow/snow textures (per-frame, outside material diffing).
        // Depth-only prepass draws never sample shadows.
        if (mat.enablePBR && !prepassDepthOnly)
            BindShadowTextures();

        // Bone matrices
        UploadBoneMatrices(cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCountPerInstance);

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: No indices to draw");
            return;
        }

        // PUBLISH BEFORE THE DRAW THAT READS IT. The mesh paths stage two kinds
        // of offset — the material five into PBRMaterialUBO via
        // WriteMaterialHeapOffsets, and the published env/IBL slots into the
        // shared table via BindPBRTextures — and neither is visible to the GPU
        // until this call, which uploads the dirty DESCRIPTORS and then the
        // table. Without it the draw indexes slots whose descriptors were only
        // ever written to the CPU mirror. It survived because descriptors are
        // persistent and some later pass flushes them, so the damage is confined
        // to the first frame a material is seen — a one-frame wrong image, which
        // is the hardest kind to notice (issue #691 Phase 3).
        //
        // Cheap when nothing changed: FlushOffsets early-outs on a clean table
        // and DescriptorHeap::Flush uploads only the dirty span.
        HeapBinding::FlushOffsets();
        // Bind VAO (cached) and draw instanced
        BindVAOIfNeeded(cmd->vertexArrayID);
        ++s_Data.Stats.DrawCalls;
        api.DrawBoundIndexedInstanced(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                                      RHI::IndexType::UInt32, cmd->baseIndex, instanceCount);

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
                cmd->vertexArrayID.Index,
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
        if (!cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid() || !cmd->skyboxTextureID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawSkybox: Invalid vertex array ID, shader ID, or skybox texture ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind skybox shader using renderer ID directly
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Re-establish camera UBO binding (may be overwritten by shadow pass)
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
        }

        // Bind skybox cubemap texture using renderer ID directly
        // The offset write must survive a cache hit — see BindTrackedTexture's note.
        if (s_Data.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->skyboxTextureID ||
            HeapBinding::WritesOffsetsForBoundProgram())
        {
            const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_ENVIRONMENT, cmd->skyboxTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Cube);
            s_Data.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] = CacheEntryAfterSeam(staged, cmd->skyboxTextureID);
            ++s_Data.Stats.TextureBinds;
        }

        // See the note above the flush in DrawIndexed: every draw in this file
        // publishes, because which handler owes one is a property of its SHADER.
        HeapBinding::FlushOffsets();

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);

        // Update statistics
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawQuad(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawQuadCommand*>(data);

        // Validate POD renderer IDs
        if (!cmd->quadVAID.IsValid() || !cmd->shaderRendererID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawQuad: Invalid vertex array ID or shader ID");
            return;
        }

        if (!cmd->textureID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawQuad: Missing texture for quad");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader using renderer ID directly
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
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
        // The offset write must survive a cache hit — see BindTrackedTexture's note.
        if (s_Data.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] != cmd->textureID ||
            HeapBinding::WritesOffsetsForBoundProgram())
        {
            const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_DIFFUSE, cmd->textureID, RHI::HeapSlotLifetime::Persistent);
            s_Data.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] = CacheEntryAfterSeam(staged, cmd->textureID);
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw quad
        BindVAOIfNeeded(cmd->quadVAID);
        ++s_Data.Stats.DrawCalls;
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, 6, RHI::IndexType::UInt32, 0);
    }

    void CommandDispatch::DrawInfiniteGrid(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawInfiniteGridCommand*>(data);

        // Validate POD renderer IDs
        if (!cmd->quadVAOID.IsValid() || !cmd->shaderRendererID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawInfiniteGrid: Invalid VAO ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind grid shader using renderer ID directly
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Note: Grid shader reads view/projection from Camera UBO (binding 0)
        // Re-establish the binding (may be overwritten by shadow pass)
        if (s_Data.CameraUBO)
        {
            BindUBOIfNeeded(ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRHIHandle());
        }

        // Set grid scale uniform if the shader supports it.
        //
        // This is the one facade call with no faithful Vulkan lowering — SPIR-V
        // has push constants and UBO members, not a name-queryable default
        // uniform block. Phase 6 folds u_GridScale into a UBO and deletes
        // SetProgramUniformFloat (ADR 0011 amendment (9)); the debt is recorded
        // rather than hidden so Phase 7 bring-up is not surprised by it.
        api.SetProgramUniformFloat(cmd->shaderRendererID, "u_GridScale", cmd->gridScale);

        // Bind fullscreen quad VAO (cached) and draw
        BindVAOIfNeeded(cmd->quadVAOID);
        HeapBinding::FlushOffsets();
        api.DrawBoundArrays(RHI::PrimitiveTopology::TriangleList, 0, 6);

        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawTerrainPatch(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawTerrainPatchCommand*>(data);

        if (!cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawTerrainPatch: Invalid vertex array ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload camera UBO (camera-relative packing, issue #429 — origin
        // subtraction happens inside UploadCameraUBO).
        UploadCameraUBO();

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
            api.BindUniformBuffer(ShaderBindingLayout::UBO_TERRAIN, terrainUBO->GetRHIHandle());
        }

        // Bind terrain textures
        if (cmd->heightmapTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_HEIGHTMAP, cmd->heightmapTextureID, RHI::HeapSlotLifetime::Persistent);
        }
        if (cmd->splatmapTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_SPLATMAP, cmd->splatmapTextureID, RHI::HeapSlotLifetime::Persistent);
        }
        if (cmd->splatmap1TextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_SPLATMAP_1, cmd->splatmap1TextureID, RHI::HeapSlotLifetime::Persistent);
        }
        if (cmd->albedoArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_ALBEDO_ARRAY, cmd->albedoArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }
        if (cmd->normalArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_NORMAL_ARRAY, cmd->normalArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }
        if (cmd->armArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_ARM_ARRAY, cmd->armArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }

        // Bind the full shadow contract the terrain shaders sample — CSM, spot,
        // the PCSS comparison-OFF raw-depth views, the point-light cubemaps, and
        // the snow depth map. Sharing BindShadowTextures keeps terrain in lockstep
        // with the mesh path; the point cubemaps were previously omitted here, so
        // terrain point shadows relied on an earlier mesh draw having bound units
        // 14-17 in the same frame.
        BindShadowTextures();

        // Bind VAO (cached) and draw with GL_PATCHES
        BindVAOIfNeeded(cmd->vertexArrayID);
        api.SetPatchVertexCount(cmd->patchVertexCount);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::PatchList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawVoxelMesh(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawVoxelMeshCommand*>(data);

        if (!cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid())
        {
            OLO_CORE_ERROR("CommandDispatch::DrawVoxelMesh: Invalid vertex array ID or shader ID");
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
            ++s_Data.Stats.ShaderBinds;
        }

        // Upload camera UBO (camera-relative packing, issue #429 — origin
        // subtraction happens inside UploadCameraUBO).
        UploadCameraUBO();

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
        if (cmd->albedoArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_ALBEDO_ARRAY, cmd->albedoArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }
        if (cmd->normalArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_NORMAL_ARRAY, cmd->normalArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }
        if (cmd->armArrayTextureID.IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_ARM_ARRAY, cmd->armArrayTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        }

        // Bind the shadow contract Terrain_Voxel.glsl samples (CSM + spot + PCSS
        // raw-depth views + point cubemaps). Previously absent here, so voxel
        // shadows depended entirely on a prior mesh draw having bound them.
        BindShadowTextures();

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawDecal(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        auto const* cmd = static_cast<const DrawDecalCommand*>(data);

        if (!cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid())
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
        const RHI::ResourceHandle decalProgram = cmd->oitProgramOverride.IsValid()
                                                     ? cmd->oitProgramOverride
                                                     : cmd->shaderRendererID;
        if (s_Data.CurrentBoundShader != decalProgram)
        {
            api.BindShaderProgram(decalProgram);
            s_Data.CurrentBoundShader = decalProgram;
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
            api.BindUniformBuffer(ShaderBindingLayout::UBO_DECAL, decalUBO->GetRHIHandle());
        }

        // Bind albedo texture (with redundancy check)
        if (cmd->albedoTextureID.IsValid())
        {
            // The offset write must survive a cache hit — see BindTrackedTexture's
            // note. The three decal slots were the last redundancy checks still
            // missing this: a second decal with the same texture skipped the call
            // entirely, leaving whatever another pass had staged in TEX_USER_0..2.
            if (s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_0] != cmd->albedoTextureID ||
                HeapBinding::WritesOffsetsForBoundProgram())
            {
                const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_0, cmd->albedoTextureID, RHI::HeapSlotLifetime::Persistent);
                s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_0] = CacheEntryAfterSeam(staged, cmd->albedoTextureID);
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind optional decal normal + RMA textures (used by Decal_GBuffer_Normal
        // and Decal_GBuffer_RMA variants). Unused modes pass 0 and the slot is
        // left alone — the variant shader only samples the slot it needs.
        if (cmd->normalTextureID.IsValid() &&
            (s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_1] != cmd->normalTextureID ||
             HeapBinding::WritesOffsetsForBoundProgram()))
        {
            const RHI::HeapOffset stagedNormal = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_1, cmd->normalTextureID, RHI::HeapSlotLifetime::Persistent);
            s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_1] = CacheEntryAfterSeam(stagedNormal, cmd->normalTextureID);
            ++s_Data.Stats.TextureBinds;
        }
        if (cmd->rmaTextureID.IsValid() &&
            (s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_2] != cmd->rmaTextureID ||
             HeapBinding::WritesOffsetsForBoundProgram()))
        {
            const RHI::HeapOffset stagedRma = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_2, cmd->rmaTextureID, RHI::HeapSlotLifetime::Persistent);
            s_Data.BoundTextures[ShaderBindingLayout::TEX_USER_2] = CacheEntryAfterSeam(stagedRma, cmd->rmaTextureID);
            ++s_Data.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw decal cube
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++s_Data.Stats.DrawCalls;
    }

    void CommandDispatch::DrawFoliageLayer(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        const auto* cmd = static_cast<const DrawFoliageLayerCommand*>(data);

        if (!cmd || !cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid() || cmd->instanceCount == 0 || cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawFoliageLayer: Invalid foliage command (VAO={}, shader={}, instances={}, indices={})",
                           cmd ? cmd->vertexArrayID : RHI::NullResource,
                           cmd ? cmd->shaderRendererID : RHI::NullResource,
                           cmd ? cmd->instanceCount : 0, cmd ? cmd->indexCount : 0);
            return;
        }

        // Resolve and apply render state from table
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader (cached)
        if (s_Data.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            s_Data.CurrentBoundShader = cmd->shaderRendererID;
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
            // Octahedral impostor params (issue #433) — zero on the billboard path.
            foliageData.ImpostorParams0 = glm::vec4(cmd->impostorFramesPerAxis, cmd->impostorHemi, cmd->impostorStartDistance, cmd->impostorBand);
            foliageData.ImpostorParams1 = glm::vec4(cmd->impostorEnabled, cmd->impostorRadius, cmd->impostorParallaxScale, 0.0f);
            foliageUBO->SetData(&foliageData, ShaderBindingLayout::FoliageUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_FOLIAGE, foliageUBO->GetRHIHandle());
        }

        // Bind albedo texture (with redundancy check). On the impostor path this
        // is the octahedral albedo atlas.
        if (cmd->albedoTextureID.IsValid())
        {
            // The offset write must survive a cache hit — see BindTrackedTexture's note.
            if (s_Data.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] != cmd->albedoTextureID ||
                HeapBinding::WritesOffsetsForBoundProgram())
            {
                const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_DIFFUSE, cmd->albedoTextureID, RHI::HeapSlotLifetime::Persistent);
                s_Data.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] = CacheEntryAfterSeam(staged, cmd->albedoTextureID);
                ++s_Data.Stats.TextureBinds;
            }
        }

        // Bind the octahedral normal+depth atlas (impostor path only).
        // BindTrackedTextureUnit skips a 0 id and does the redundancy check.
        BindTrackedTextureUnit(ShaderBindingLayout::TEX_USER_0, cmd->impostorNormalDepthTextureID);

        // Bind VAO (cached) and draw instanced foliage
        BindVAOIfNeeded(cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexedInstanced(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                                      RHI::IndexType::UInt32, 0, cmd->instanceCount);
        ++s_Data.Stats.DrawCalls;
    }
    void CommandDispatch::DrawWater(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        const auto* cmd = static_cast<const DrawWaterCommand*>(data);

        if (!cmd || !cmd->vertexArrayID.IsValid() || !cmd->shaderRendererID.IsValid() || cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawWater: Invalid water command (VAO={}, shader={}, indices={})",
                           cmd ? cmd->vertexArrayID : RHI::NullResource,
                           cmd ? cmd->shaderRendererID : RHI::NullResource,
                           cmd ? cmd->indexCount : 0);
            return;
        }
        // Resolve and apply render state from table (cull, depth, blend enable).
        ApplyPODRenderState(cmd->renderStateIndex, api);

        // Bind shader (cached). During the surface-depth capture the water
        // color program is swapped for Water_Depth — the same VS/TCS/TES
        // displacement chain with a no-color-output fragment stage, so the
        // depth-only capture target needs no scene-MRT attachment mirroring
        // (the depth-prepass shader-swap shape, #691 Phase 8).
        RHI::ResourceHandle shaderToBind = cmd->shaderRendererID;
        if (s_Data.WaterDepthCaptureActive && s_Data.WaterDepthShaderID.IsValid())
        {
            shaderToBind = s_Data.WaterDepthShaderID;
        }
        if (s_Data.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            s_Data.CurrentBoundShader = shaderToBind;
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
            waterData.FFTParams = cmd->fftParams;
            waterUBO->SetData(&waterData, ShaderBindingLayout::WaterUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_WATER, waterUBO->GetRHIHandle());
        }

        // Bind normal map and noise textures (tracked for redundancy elimination and stats)
        BindTrackedTexture(cmd->normalMap0ID, ShaderBindingLayout::TEX_WATER_NORMAL_0);
        BindTrackedTexture(cmd->normalMap1ID, ShaderBindingLayout::TEX_WATER_NORMAL_1);
        BindTrackedTexture(cmd->noiseTextureID, ShaderBindingLayout::TEX_WATER_NOISE);
        BindTrackedTexture(cmd->foamTextureID, ShaderBindingLayout::TEX_WATER_FOAM);
        // FFT ocean cascade textures (sampled when u_FFTParams.x > 0.5)
        BindTrackedTexture(cmd->fftDisplacementID, ShaderBindingLayout::TEX_WATER_FFT_DISPLACEMENT);
        BindTrackedTexture(cmd->fftDerivativesID, ShaderBindingLayout::TEX_WATER_FFT_DERIVATIVES);

        // Bind the global environment cubemap for water reflections (binding 9).
        // The water pass doesn't otherwise touch this slot, so set it explicitly
        // instead of relying on a prior pass having left the skybox bound there —
        // without this, grazing-angle water reflects an unbound (black/grey)
        // cubemap and looks see-through rather than reflective. When there's no
        // environment map, deterministically clear the slot rather than leaving a
        // stale cubemap from a previous frame/scene (BindTrackedTexture skips id 0,
        // so clear it directly and update the tracking).
        if (const auto envMap = Renderer3D::GetGlobalEnvironmentMapHandle(); envMap.IsValid())
        {
            BindTrackedTexture(envMap, ShaderBindingLayout::TEX_ENVIRONMENT);
        }
        else if (s_Data.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT].IsValid())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_ENVIRONMENT, RHI::NullResource, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Cube);
            s_Data.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] = {};
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
        api.SetPatchVertexCount(3);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::PatchList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++s_Data.Stats.DrawCalls;
    }
} // namespace OloEngine
