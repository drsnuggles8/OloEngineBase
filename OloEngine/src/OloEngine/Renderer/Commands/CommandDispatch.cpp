// OLO_MATERIAL_RESOLVE_EXEMPT: this file EXECUTES already-built command packets. The material
// was resolved (through SubmeshMaterialResolve.h) by whoever built the packet; by the time a
// DrawMeshInstancedCommand reaches the dispatcher there is no submesh, no MaterialComponent and
// no imported-material table left to consult — only a PODMaterialData slot to bind. See
// RenderPathDrift.EveryMeshSubmissionPathUsesTheSharedMaterialResolver.
#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Wind/WindSystem.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandDispatchRecordingState.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/OITBlendState.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Renderer/SkinProfile.h"
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
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
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
    // RenderCommand.h spells PODMaterialData::skinProfileSlot's "none" code as a
    // literal so that hot header keeps its include set. This is where the two
    // are held together: a widened slot field that forgot the literal would
    // otherwise make every non-skin pixel name profile 7.
    static_assert(PODMaterialData{}.skinProfileSlot == kSkinProfileSlotNone,
                  "PODMaterialData::skinProfileSlot's default no longer matches kSkinProfileSlotNone "
                  "(Renderer/SkinProfile.h).");

    // Frozen for a recording region. Pass setup updates this on the primary;
    // bucket view overrides and every bind/cache/upload live in Data() below.
    struct CommandDispatchFrameData
    {
        TiledForwardPlus* ForwardPlus = nullptr;
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

        // Screen-space AO for the forward ambient term (issue #1452): the
        // camera-UBO lane and the two textures BindSceneResources publishes.
        glm::vec4 ForwardScreenSpaceAOParams{ 0.0f };
        RHI::ResourceHandle ForwardScreenSpaceAOTexture{};
        RHI::ResourceHandle ForwardScreenSpaceAODepth{};
        // The lighting tap (issue #1526), for the main view's camera lane. An
        // alternate view (DispatchView::MainViewLanes == false) uploads neither
        // this nor the AO lane: both are main-view data.
        f32 LightingTap = 0.0f;

        // Depth prepass override: when true, ApplyPODRenderState forces depth-only state
        bool DepthPrepassActive = false;
        // ...and the forward prepass also writes scene attachment 2's view
        // normal (issue #1452) — see SetDepthPrepassActive.
        bool DepthPrepassWritesNormals = false;
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
        // A pass-wide ceiling on which colour attachments a draw may write, ANDed
        // into every draw's colorAttachmentWriteMask (#1533). ForwardOverlayPass
        // keeps every scene attachment in its scope -- so the overlay shaders'
        // velocity and skin-diffuse outputs land on a real attachment rather than
        // on none, which the Vulkan validation layer reports as unconsumed -- and
        // stops them being written with this instead of narrowing the draw list.
        u8 PassAttachmentWriteLimit = 0xFF;
        // Water surface-depth capture: forces depth-only state even for the blended
        // water draw so the nearest water surface is written to its own depth target.
        bool WaterDepthCaptureActive = false;
        // Depth-only water program swapped in during the capture (snapshot in
        // SetWaterDepthCaptureActive so shader hot-reloads are picked up).
        RHI::ResourceHandle WaterDepthShaderID{};
        // WaterRenderPass's target size; see SetWaterTargetSize.
        u32 WaterTargetWidth = 0;
        u32 WaterTargetHeight = 0;
    };

    static CommandDispatchFrameData s_FrameData;

    // One recording context's state (ADR 0025): the primary's, or one worker
    // item's. Four groups with four lifetimes, each a struct so that a reset is
    // an assignment and cannot miss a field added later.
    struct CommandDispatchData
    {
        // The view this context draws from. The frame sets the primary's once
        // (CommandDispatch::SetView); ScopedAlternateView swaps it for a replay
        // from another camera; an item gets a copy at Prepare.
        DispatchView View;

        // The buffer objects this context uploads into. The primary's are the
        // renderer's shared ones (SetUBOReferences); an item's are its own,
        // created before the fork. Device objects, so not reset between
        // frames: Shutdown drops them with the rest of the context.
        struct UploadObjects
        {
            Ref<UniformBuffer> CameraUBO = nullptr;
            Ref<UniformBuffer> MaterialUBO = nullptr;
            Ref<UniformBuffer> BoneMatricesUBO = nullptr;
            Ref<UniformBuffer> PrevBoneMatricesUBO = nullptr;
            Ref<InstanceBuffer> ModelInstanceBuffer = nullptr;
            Ref<UniformBuffer> TerrainUBO;
            Ref<UniformBuffer> DecalUBO;
            Ref<UniformBuffer> FoliageUBO;
            Ref<UniformBuffer> WaterUBO;
            Ref<UniformBuffer> VirtualShadowUBO;
            // The backend SetUBOReferences published the set on (#1655); 0
            // (None) when nothing is published. A buffer belongs to the
            // backend that made it: see ResetState.
            u32 Backend = 0;
        };
        UploadObjects Uploads;

        // Every "this is already bound / already uploaded" answer. Emptied by
        // assignment at ResetState, InvalidateBindingCaches, Prepare and
        // Publish; the default value means "nothing is known to be bound".
        struct BindCache
        {
            // The redundant-bind cache keys on IDENTITIES, not driver names
            // (issue #691). That is a correctness change, not a
            // type change: GL reissues object names, so a deleted texture and a
            // newly created one could compare equal here and the cache would SKIP
            // a bind that genuinely had to happen. Two live handles cannot
            // collide, so the Invalidate* calls below stop being load-bearing
            // correctness and become the pure optimisation they read as.
            RHI::ResourceHandle CurrentBoundShader{};
            RHI::ResourceHandle CurrentBoundVAO{};
            // Whether the GPU Scene material table is the current occupant of the
            // aliased slot 17. See BindGPUSceneMaterialsIfNeeded().
            bool GPUSceneMaterialsBound = false;
            u16 LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
            // Whether the depth-normal prepass left scene attachment 2 writable
            // (ApplyPrepassViewNormalWrite). A cache hit in ApplyPODRenderState
            // closes it, so a draw that does not write the normal never inherits it.
            bool PrepassNormalWriteOpen = false;
            u16 LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
            // Heap re-initialisation epoch the cached material UBO was built under.
            // Its per-material heap offsets index THAT heap, so the cache must be
            // dropped when it bumps (issue #691).
            u64 HeapEpoch = 0u;
            // The Vulkan slot cache's generation the cached material UBO was built
            // under (ADR 0011 amendment (96)). Sibling of HeapEpoch above, one heap
            // over: that one tracks a re-initialised ENGINE heap, this one tracks the
            // backend's own slot cache reassigning a slot — which a texture reloaded
            // in place does while keeping its RHI handle. Always 0 on OpenGL.
            u64 ShaderHeapGeneration = 0u;
            glm::uvec3 MaterialTableAddressAndCount{ 0u };
            // Whether the CACHED material UBO was built with live heap offsets.
            //
            // The cache is keyed on the material index, but an offset's validity
            // depends on the PROGRAM in flight: ResolveTextureOffset returns nothing
            // for a slot-based program. So a material uploaded while a slot-based
            // shader was bound holds null offsets, and re-using that cached upload for
            // a BINDLESS shader would skip the binds (BindPBRTextures) AND supply no
            // offsets — the material renders with no textures at all. The program kind
            // is therefore part of the cache key (issue #691).
            bool LastMaterialOffsetsLive = false;
            std::array<RHI::ResourceHandle, ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS> BoundTextures{};
            // No uniform-buffer entry: which buffer occupies a binding point is
            // the backend's to answer (BindUBOIfNeeded, issue #1688).
        };
        BindCache Cache;

        // What this context drew. An item starts at zero and is summed into
        // the primary at the join.
        struct Counters
        {
            // Draws that actually rendered THROUGH a canonical record this frame,
            // and draws that carried a link and fell back. Counted here rather than
            // at extraction because this is the only place the record is read: a
            // path that stops passing its link, or a batcher that drops one, must
            // show up as a number that moved. A draw submitted to several passes
            // (depth prepass and colour) counts once per pass on purpose — the
            // question these answer is "did anything consume", not "how many
            // meshes".
            u32 GPUSceneConsumedDraws = 0;
            u32 GPUSceneFallbackDraws = 0;
            // Pass entries that found another buffer on the frame-published
            // camera binding and put the frame's back (PublishFrameCamera).
            u32 FrameCameraRepublished = 0;
            CommandDispatch::Statistics Stats;
        };
        Counters Tally;
    };

    // The primary recording context: the render thread's, and the only one on a
    // backend that never forks. Owned by the dispatcher from Initialize to
    // Shutdown, which assigns it back to its default.
    static CommandDispatchData s_MainData;
    static thread_local CommandDispatchData* s_RecordingData = nullptr;

    static CommandDispatchData& Data()
    {
        return s_RecordingData ? *s_RecordingData : s_MainData;
    }

    CommandDispatchRecordingState::CommandDispatchRecordingState()
        : m_Data(std::make_unique<CommandDispatchData>())
    {
    }

    CommandDispatchRecordingState::~CommandDispatchRecordingState() = default;

    void CommandDispatchRecordingState::Prepare(u32 instanceCapacity)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Prepare recording uploads before the fork");
        auto& item = *m_Data;
        // The item draws from the primary's view as it is at the fork: inside a
        // ScopedAlternateView that is the alternate view, whole.
        item.View = s_MainData.View;
        // Nothing is known to be bound in a secondary, and nothing drawn yet.
        item.Cache = {};
        item.Tally = {};

        // The item's upload objects are its own, retained between regions and
        // frames, seeded with the bytes the primary's hold at the fork.
        const CommandDispatchData::UploadObjects retainedUploads = std::move(item.Uploads);
        const auto clone = [](Ref<UniformBuffer> retained, const Ref<UniformBuffer>& source,
                              u32 size, u32 binding) -> Ref<UniformBuffer>
        {
            if (!source)
                return nullptr;
            // Size from the SOURCE, not only from the layout constant: the
            // shared object is what the copy has to hold, and a Debug-only
            // assert on the layout size would leave a Release build writing
            // past the item's buffer if the two ever diverged.
            const auto bytes = source->GetCachedData();
            const u32 required = std::max(size, static_cast<u32>(bytes.size()));
            if (retained && retained->GetSize() < required)
                retained.Reset();
            if (!retained)
                retained = UniformBuffer::Create(required, binding);
            if (retained)
                retained->DeclareBlockLike(*source);
            if (!bytes.empty())
                retained->SetData(bytes.data(), static_cast<u32>(bytes.size()));
            return retained;
        };
        const auto& main = s_MainData.Uploads;
        auto& uploads = item.Uploads;
        uploads = {};
        uploads.CameraUBO = clone(retainedUploads.CameraUBO, main.CameraUBO, ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        uploads.MaterialUBO = clone(retainedUploads.MaterialUBO, main.MaterialUBO,
                                    ShaderBindingLayout::PBRMaterialUBO::GetSize(), ShaderBindingLayout::UBO_MATERIAL);
        uploads.BoneMatricesUBO = clone(retainedUploads.BoneMatricesUBO, main.BoneMatricesUBO, ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
        uploads.PrevBoneMatricesUBO = clone(retainedUploads.PrevBoneMatricesUBO, main.PrevBoneMatricesUBO, ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION_PREV);
        uploads.TerrainUBO = clone(retainedUploads.TerrainUBO, Renderer3D::GetTerrainUBO(), ShaderBindingLayout::TerrainUBO::GetSize(), ShaderBindingLayout::UBO_TERRAIN);
        uploads.DecalUBO = clone(retainedUploads.DecalUBO, Renderer3D::GetDecalUBO(), ShaderBindingLayout::DecalUBO::GetSize(), ShaderBindingLayout::UBO_DECAL);
        uploads.FoliageUBO = clone(retainedUploads.FoliageUBO, Renderer3D::GetFoliageUBO(), ShaderBindingLayout::FoliageUBO::GetSize(), ShaderBindingLayout::UBO_FOLIAGE);
        uploads.WaterUBO = clone(retainedUploads.WaterUBO, Renderer3D::GetWaterUBO(), ShaderBindingLayout::WaterUBO::GetSize(), ShaderBindingLayout::UBO_WATER);
        // BindForSampling uploads the complete block, including the disabled
        // state, for every forward draw. Reserve its item-owned destination
        // even when no Renderer3D VSM exists (standalone pass consumers too).
        uploads.VirtualShadowUBO = retainedUploads.VirtualShadowUBO;
        if (RenderCommand::SupportsParallelRecording() && !uploads.VirtualShadowUBO)
            uploads.VirtualShadowUBO = UniformBuffer::Create(VSM::GlobalsUBO::GetSize(), ShaderBindingLayout::UBO_VIRTUAL_SHADOW);
        // The VSM's stand-ins are per backend (#1655) and an item may only bind
        // them, so the recording backend's set is made here, before the fork.
        Renderer3D::GetShadowMap().GetVirtualShadowMap().PrepareSamplingForCurrentBackend();
        uploads.ModelInstanceBuffer = retainedUploads.ModelInstanceBuffer;
        if (main.ModelInstanceBuffer)
        {
            if (!uploads.ModelInstanceBuffer)
                uploads.ModelInstanceBuffer = Ref<InstanceBuffer>::Create(std::max(1u, instanceCapacity));
            uploads.ModelInstanceBuffer->EnsureCapacity(std::max(1u, instanceCapacity));
        }
    }

    void CommandDispatchRecordingState::Publish()
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Publish recording statistics after the join");
        const auto& item = m_Data->Tally;
        auto& main = s_MainData.Tally;
        main.Stats.ShaderBinds += item.Stats.ShaderBinds;
        main.Stats.TextureBinds += item.Stats.TextureBinds;
        main.Stats.DrawCalls += item.Stats.DrawCalls;
        main.Stats.ConditionalDraws += item.Stats.ConditionalDraws;
        main.GPUSceneConsumedDraws += item.GPUSceneConsumedDraws;
        main.GPUSceneFallbackDraws += item.GPUSceneFallbackDraws;
        main.FrameCameraRepublished += item.FrameCameraRepublished;
        // The secondaries just executed leave the primary's command-buffer
        // state undefined, so nothing the primary believed bound still is.
        s_MainData.Cache = {};
    }

    ScopedCommandDispatchRecordingState::ScopedCommandDispatchRecordingState(CommandDispatchRecordingState& state)
        : m_Previous(s_RecordingData)
    {
        s_RecordingData = state.m_Data.get();
    }

    ScopedCommandDispatchRecordingState::~ScopedCommandDispatchRecordingState()
    {
        s_RecordingData = m_Previous;
    }

    Ref<UniformBuffer> CommandDispatch::ResolveRecordingUpload(u32 binding, const Ref<UniformBuffer>& mainBuffer)
    {
        if (!s_RecordingData)
            return mainBuffer;
        switch (binding)
        {
            case ShaderBindingLayout::UBO_TERRAIN:
                return Data().Uploads.TerrainUBO;
            case ShaderBindingLayout::UBO_DECAL:
                return Data().Uploads.DecalUBO;
            case ShaderBindingLayout::UBO_FOLIAGE:
                return Data().Uploads.FoliageUBO;
            case ShaderBindingLayout::UBO_WATER:
                return Data().Uploads.WaterUBO;
            case ShaderBindingLayout::UBO_VIRTUAL_SHADOW:
                return Data().Uploads.VirtualShadowUBO;
            default:
                OLO_CORE_ASSERT(false, "Unprepared recording upload binding {}", binding);
                return nullptr;
        }
    }

    Ref<InstanceBuffer> CommandDispatch::ResolveRecordingInstances(const Ref<InstanceBuffer>& mainBuffer)
    {
        return s_RecordingData ? Data().Uploads.ModelInstanceBuffer : mainBuffer;
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
        if (slot < Data().Cache.BoundTextures.size())
        {
            Data().Cache.BoundTextures[slot] = RHI::NullResource;
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
        for (auto& slot : Data().Cache.BoundTextures)
        {
            if (slot == texture)
                slot = RHI::NullResource;
        }
    }

    // Bind a UBO only when it is not already the binding point's occupant.
    //
    // The answer comes from the backend's occupancy record, not from a cache
    // here (issue #1688). The dispatcher used to remember what IT last bound at
    // bindings 0 to 7, which is not the same thing: a shadow item's camera
    // buffer, a bake or the particle pass binds directly, and the next draw
    // then skipped a bind it needed. Correctness rested on every such pass
    // remembering to invalidate. The record sees every bind the engine makes,
    // in the calling recording context.
    //
    // What it cannot see is a raw bind on OpenGL (a test, FSR2), so the skip
    // asks whether the backend vouches for the record, and the dispatcher
    // withdraws that at the points where it drops its own caches
    // (ResetBindCacheUnlessFaulted). Without that a bind made behind the
    // record was skipped for the rest of the process: the old table at least
    // forgot once a frame. With it, such a bind is trusted no longer than the
    // next frame start or explicit invalidation.
    static void BindUBOIfNeeded(RendererAPI& api, u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        if (api.IsUniformBufferBound(bindingPoint, buffer))
            return;
        api.BindUniformBuffer(bindingPoint, buffer);
    }

    // The GPU Scene material table at SSBO_INSTANCE_DRAW_INDIRECT (17). The
    // slot is shared with the GPU instance cull's indirect buffer, so the
    // "already bound" answer is only valid until that cull binds its own —
    // GPUFrustumCuller calls InvalidateGPUSceneMaterialBinding() when it does.
    // Without that hook a linked draw after a GPU-culled instanced draw would
    // read draw-indirect commands as material records.
    // Returns whether the table is bound NOW. Latching on a bind that did not
    // happen would leave slot 17 holding the instance cull's indirect buffer
    // while every later draw believed it held material records.
    static bool BindGPUSceneMaterialsIfNeeded()
    {
        if (Data().Cache.GPUSceneMaterialsBound)
            return true;
        Data().Cache.GPUSceneMaterialsBound = Renderer3D::BindGPUSceneMaterials();
        return Data().Cache.GPUSceneMaterialsBound;
    }

    // Conditionally bind a VAO only when it differs from the currently bound one.
    // This cache is why the draws below use the DrawBound* family rather than the
    // DrawIndexedRaw(vaoID, ...) one: the latter binds the VAO itself, which would
    // make the cache pointless.
    static void BindVAOIfNeeded(RendererAPI& api, RHI::ResourceHandle vertexArray)
    {
        if (Data().Cache.CurrentBoundVAO != vertexArray)
        {
            api.BindVertexArrayRaw(vertexArray);
            Data().Cache.CurrentBoundVAO = vertexArray;
        }
    }

    // Helper to apply POD render state to the renderer API (skips if same index as last).
    // Declared on CommandDispatch (not file-static) so a tenant can exercise the
    // real per-draw state application -- see the header for why (issue #853).
    void CommandDispatch::ApplyPODRenderState(u16 renderStateIndex, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        if (renderStateIndex == INVALID_RENDER_STATE_INDEX)
        {
            // Apply safe defaults so no stale GL state persists
            Data().Cache.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
            Data().Cache.PrepassNormalWriteOpen = false; // the global mask call below resets every attachment
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
            if (s_FrameData.DepthPrepassActive)
            {
                api.SetColorMask(false, false, false, false);
                api.SetDepthTest(true);
                api.SetDepthMask(true);
                api.SetDepthFunc(RHI::CompareOp::Less);
                api.SetBlendState(false);
            }
            // During color pass of depth prepass, override depth to GL_LEQUAL + no writes
            else if (s_FrameData.DepthPrepassColorPassActive)
            {
                api.SetDepthFunc(RHI::CompareOp::LessOrEqual);
                api.SetDepthMask(false);
            }
            // During the overdraw debug view, count every covered fragment:
            // additive blend, depth test off (so occluded fragments count too),
            // colour writes on, depth writes off.
            else if (s_FrameData.OverdrawActive)
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

        if (renderStateIndex == Data().Cache.LastRenderStateIndex)
        {
            // The cached state is still in force, except the view-normal write
            // a previous depth-normal prepass draw opened on top of it.
            // Outside the prepass the flag is merely stale (RunDepthPrepass
            // reopened every attachment when it ended) and is dropped silently.
            if (Data().Cache.PrepassNormalWriteOpen)
            {
                if (s_FrameData.DepthPrepassActive)
                    api.SetColorMaskForAttachment(2, false, false, false, false);
                Data().Cache.PrepassNormalWriteOpen = false;
            }
            return;
        }
        Data().Cache.LastRenderStateIndex = renderStateIndex;
        // The global colour-mask call below resets every attachment's mask.
        Data().Cache.PrepassNormalWriteOpen = false;

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

        // Per-attachment refinement of the global mask just applied.
        //
        // The SetColorMask above is the INDEXED call for every draw buffer
        // (glColorMask is defined as glColorMaski for all i, and both backends
        // model that -- see
        // docs/agent-rules/gl-global-setter-resets-indexed-state.md), so it has
        // just flattened whatever per-attachment masks anything else installed.
        // Anything narrower than "every channel of every attachment" therefore
        // has to be re-asserted here, per attachment, or it is gone before the
        // draw.
        //
        // Two inputs, ANDed, never widening past the global call:
        //   * colorAttachmentWriteMask -- one BIT per attachment, "may this
        //     attachment be written at all". This is all this loop used to
        //     honour, which is why DecalRenderPass's CHANNEL-level mode matrix
        //     was silently defeated in production on both backends (issue #853):
        //     a decal packet carries the default 0xFF here, so the loop
        //     re-narrowed nothing and the global call's all-channels-writable
        //     state stood.
        //   * colorAttachmentChannelMask -- one NIBBLE per attachment, which
        //     channels of it. Default 0xF per attachment, i.e. no refinement.
        // ...and the pass-wide ceiling (SetPassAttachmentWriteLimit), ANDed in.
        const u8 attachmentWriteMask = static_cast<u8>(state.colorAttachmentWriteMask & s_FrameData.PassAttachmentWriteLimit);
        if (attachmentWriteMask != 0xFF || state.colorAttachmentChannelMask != COLOR_CHANNEL_MASK_ALL)
        {
            const u8 globalChannels =
                MakeColorChannelMask(state.colorMaskR, state.colorMaskG, state.colorMaskB, state.colorMaskA);
            for (u32 i = 0; i < MAX_MASKED_COLOR_ATTACHMENTS; ++i)
            {
                const bool attachmentEnabled = (attachmentWriteMask & (1u << i)) != 0u;
                const u8 channels =
                    attachmentEnabled
                        ? static_cast<u8>(globalChannels & GetColorChannelMask(state.colorAttachmentChannelMask, i))
                        : u8{ 0 };
                // Skip the attachments the global call already left exactly
                // here. That keeps the old behaviour byte-identical for every
                // caller that only sets colorAttachmentWriteMask (the
                // Renderer3DUtilityDraws skeleton/joint/grid draws), and keeps
                // this loop from issuing indexed calls for draw buffers no
                // caller asked about.
                if (channels == globalChannels)
                    continue;
                api.SetColorMaskForAttachment(i, (channels & 0x1u) != 0u, (channels & 0x2u) != 0u,
                                              (channels & 0x4u) != 0u, (channels & 0x8u) != 0u);
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
        if (s_FrameData.DepthPrepassActive)
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
        else if (s_FrameData.DepthPrepassColorPassActive)
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
        else if (s_FrameData.OverdrawActive)
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
        if (s_FrameData.WaterDepthCaptureActive)
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
    // driver did not already have (issue #691).
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
    // exactly the kind of divergence a free function stops (issue #691).
    [[nodiscard]] static auto CacheEntryAfterSeam(const RHI::HeapOffset staged, const RHI::ResourceHandle texture)
        -> RHI::ResourceHandle
    {
        return staged.IsValid() ? RHI::NullResource : texture;
    }

    static void BindTrackedTexture(RendererAPI& api, RHI::ResourceHandle texture, u32 slot,
                                   RHI::NullSamplerKind kind = RHI::NullSamplerKind::Texture2D,
                                   const RHI::SamplerDesc& sampler = {})
    {
        if (!texture.IsValid())
            return;
        if (Data().Cache.BoundTextures[slot] == texture && !HeapBinding::WritesOffsetsForBoundProgram())
            return;

        // Persistent: material and IBL textures are asset-owned and outlive the
        // frame, so their descriptors are memoised rather than drawn from the ring.
        const RHI::HeapOffset staged =
            HeapBinding::BindTextureOrOffset(api, slot, texture, RHI::HeapSlotLifetime::Persistent, sampler, kind);

        // ONLY CLAIM A GL BINDING WHEN ONE ACTUALLY HAPPENED. The cache's
        // invariant, stated above, is "this slot's GL BINDING is already
        // correct" — sound only while every tracked call ends in a bind. Under
        // the heap it does not: a valid returned offset means the seam STAGED an
        // offset and issued no bind at all. Recording the texture anyway made the
        // array assert a binding that was never made, and the next SLOT-BASED
        // consumer of the same slot would hit the cache and skip its own bind —
        // sampling whatever that unit last held. Latent until a shader sharing a
        // slot with an unconverted one converts, which is precisely what the
        // shadow arrays and the IBL trio do (issue #691).
        Data().Cache.BoundTextures[slot] = CacheEntryAfterSeam(staged, texture);
        ++Data().Tally.Stats.TextureBinds;
    }

    // THE VULKAN ARM of the per-material offsets (ADR 0011 amendment (96)).
    //
    // A DIFFERENT MECHANISM FROM THE GL ARM BELOW, not a parameterisation of it,
    // and the amendment says why: GL_ARB_bindless_texture bakes sampler state into
    // its uvec2 handle, so a GL offset is a complete descriptor drawn from the
    // ENGINE heap; GL_EXT_descriptor_heap splits texture from sampler, and its
    // offsets are BYTE offsets into the BACKEND's own slot region — the very slots
    // the raster frame binds this texture through. The two cannot share a resolver
    // because they do not name the same thing.
    //
    // ONLY THE FIVE MATERIAL-LOCAL MAPS. The environment cubemap and the IBL trio
    // keep classic bindings on this arm (they are baked into render targets, and
    // ResolveShaderHeapTexture can only describe a texture AT REST), so their lanes
    // stay Invalid here and the converted shader never reads them.
    //
    // EVERY LANE THIS ARM WRITES NAMES A REAL DESCRIPTOR, and that is the whole
    // safety argument for letting a raster shader index the heap. An out-of-range
    // heap index is undefined behaviour, not a black texel, so "no map" and "could
    // not resolve" both resolve to the backend's 1x1 null rather than to
    // HeapOffset::Invalid.
    //
    // THE SHADER'S OWN `Use*Map` GATE IS NOT ENOUGH ON ITS OWN, which is why the
    // null matters rather than merely tidying up. The forward consumer reads that
    // flag from this UBO, so clearing it here would be sufficient there — but the
    // DEFERRED one (PBR_GBuffer) takes it from the GPU Scene material record
    // whenever the draw carries a live link, and this function cannot reach that
    // record. A material whose albedo failed to resolve would keep a set flag from
    // the record and index the heap anyway. The offset has to be safe by itself.
    //
    // THE FLAG IS STILL CLEARED ON FAILURE, for appearance rather than safety:
    // sampling the null multiplies the base colour by zero and the mesh goes BLACK,
    // where shading from the factor alone leaves it plainly untextured. The failure
    // is COUNTED and warned either way — never absorbed.
    static void WriteMaterialShaderHeapOffsets(const PODMaterialData& mat,
                                               ShaderBindingLayout::PBRMaterialUBO& ubo)
    {
        static std::atomic<u64> s_Unresolved{ 0 };

        // Resolved once per call rather than per lane: it is memoised in the
        // backend and every failing lane wants the same answer.
        //
        // AND IT IS ITSELF CHECKED, because HeapOffset::Invalid is 0xFFFFFFFF and
        // writing THAT into a lane is the exact out-of-bounds index this null
        // exists to prevent. Byte offset 0 is inside the heap buffer but it is NOT
        // a safe descriptor: it is the start of the implementation's reserved
        // range, whose entries name whatever image the driver's own copy or clear
        // last touched (#1511, where a shader reading lane 0 faulted on a freed
        // scene target). It is tolerable here only because the arm is stood down
        // as well (below), so nothing this function can reach samples it — and a
        // lane must never reach the shader as 0 by any other route. Unreachable in practice — a device
        // that cannot resolve a null cannot create the shader module either, and
        // fails loudly at vkCreateShaderModule — but the fallback must not be the
        // one value that turns a degraded frame into undefined behaviour.
        const RHI::HeapOffset resolvedNull = HeapBinding::ResolveShaderHeapNullTexture();
        const u32 nullTexture = resolvedNull.IsValid() ? resolvedNull.Value : 0u;

        // ONE SAMPLER FOR ALL FIVE, and it is frame-uniform rather than
        // per-material: every material 2D descriptor is minted with this one state
        // (HeapBinding::MaterialTexture2DSampler), which is what lets a single lane
        // serve the whole set. Resolved first because nothing else is usable
        // without it — a texture descriptor with no sampler cannot be sampled.
        const RHI::HeapOffset samplerOffset =
            HeapBinding::ResolveShaderHeapSampler(HeapBinding::MaterialTexture2DSampler());
        const bool armLive = samplerOffset.IsValid() && resolvedNull.IsValid();

        const auto resolve = [&](const RHI::ResourceHandle texture, i32& useFlag) -> u32
        {
            if (!texture.IsValid())
            {
                // No map, which is ordinary rather than a failure: `useFlag` is
                // already 0 (it was set from this same handle), so the shader takes
                // its factor branch. The null is written anyway so the lane is safe
                // to index for a consumer whose flag came from somewhere else.
                return nullTexture;
            }
            const RHI::HeapOffset offset =
                armLive ? HeapBinding::ResolveShaderHeapTexture(texture) : RHI::HeapOffset{};
            if (!offset.IsValid())
            {
                useFlag = 0;
                if (s_Unresolved.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    OLO_CORE_WARN("CommandDispatch: a material texture could not be resolved to a shader-heap "
                                  "descriptor; this material shades UNTEXTURED on the Vulkan heap arm "
                                  "(ADR 0011 amendment (96), issue #805).");
                }
                return nullTexture;
            }
            return offset.Value;
        };

        const u32 albedo = resolve(mat.albedoMapID, ubo.UseAlbedoMap);
        const u32 metallicRoughness = resolve(mat.metallicRoughnessMapID, ubo.UseMetallicRoughnessMap);
        const u32 normal = resolve(mat.normalMapID, ubo.UseNormalMap);
        const u32 ao = resolve(mat.aoMapID, ubo.UseAOMap);
        const u32 emissive = resolve(mat.emissiveMapID, ubo.UseEmissiveMap);
        // The thickness map (issue #1242) rides a BARE SCALAR lane rather than a
        // fourth HeapOffsets uvec4 — every lane of the three is taken ([2].w is
        // the Vulkan sampler offset), and the uvec4 shaping exists to dodge
        // `uint[9]`'s std140 stride, which a single scalar does not have. Same
        // `resolve` as every other map, so a map that cannot be published to the
        // heap stands its flag down here instead of sampling a null descriptor.
        ubo.ThicknessMapHeapOffset = resolve(mat.thicknessMapID, ubo.UseThicknessMap);

        ubo.HeapOffsets[0] = { albedo, metallicRoughness, normal, ao };
        // Vulkan-only reuse of the unused environment/IBL lanes: a deferred
        // instance can select all five maps by its GPU Scene material reference.
        // Forward/skinned/virtual shaders still use only the per-draw lanes.
        const glm::uvec3 table = armLive ? Renderer3D::GetMaterialShaderHeapTable().GetAddressAndCount() : glm::uvec3(0u);
        ubo.HeapOffsets[1] = { emissive, table.x, table.y, table.z };
        // [2].w is the sampler lane. It indexes the SAMPLER heap, not the resource
        // heap, so the null above is not a substitute for it — a failure there
        // stands the whole arm down (armLive), which every lane above has already
        // taken into account.
        ubo.HeapOffsets[2] = { nullTexture, nullTexture, nullTexture,
                               armLive ? samplerOffset.Value : 0u };
    }

    // Resolve a material's nine textures to heap offsets for the material UBO.
    //
    // Persistent lifetime throughout: these are asset-owned textures, never
    // graph-pooled, so the descriptors are memoised and the offsets are stable
    // across frames — which is what keeps the material UBO cacheable
    // (issue #691, ADR 0011 amendment (32)).
    //
    // Order MUST match OLO_MATERIAL_* in include/BindlessHeap.glsl. Getting it
    // wrong swaps two real textures rather than producing an obvious error.
    static void WriteMaterialHeapOffsets(const PODMaterialData& mat,
                                         ShaderBindingLayout::PBRMaterialUBO& ubo)
    {
        // TWO ARMS, forked on the BACKEND rather than on the heap lever (ADR 0011
        // amendment (96)). ShaderHeapIndexingSupported() is true only on Vulkan,
        // and ReadsMaterialHeapOffsets() only for a program that actually declares
        // the lanes — so this takes the shader-heap arm exactly when the shader in
        // flight was built to read byte offsets, and the engine-heap arm below in
        // every other case, including an unconverted Vulkan program (whose lanes
        // are then filled with the reserved nulls it never reads).
        if (HeapBinding::ShaderHeapIndexingSupported() && Shader::ReadsMaterialHeapOffsets())
        {
            WriteMaterialShaderHeapOffsets(mat, ubo);
            return;
        }

        // A BINDLESS DESCRIPTOR BAKES SAMPLER STATE; A SLOT BIND DOES NOT.
        // glBindTextureUnit samples with whatever the TEXTURE OBJECT carries,
        // while a heap handle carries what its descriptor was minted with — so
        // minting with SamplerDesc{} makes the converted shader sample
        // differently from the unconverted one, plausibly and silently. Measured
        // at RMSE 5.861 vs 1.413 on WorldOriginRebaseVisualEvidence before this
        // was carried through (issue #691).
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
        //
        // The value lives in HeapBinding::MaterialTexture2DSampler so the GPU-scene
        // material record (issue #992) mints its persistent descriptors with the
        // same state and the two cannot drift.
        const RHI::SamplerDesc& k2DSampler = HeapBinding::MaterialTexture2DSampler();
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
                                      "fall back to slot binds, issue #691).");
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
        // [2].y and [2].z are unused lanes, kept so the UBO layout (and the
        // Vulkan arm's [2].w sampler lane) does not move. They carried the Phong
        // diffuse / specular maps until that material type was removed (#1499).
        ubo.HeapOffsets[2] = { resolve(mat.brdfLutMapID, k2DSampler, k2D),
                               RHI::kNullHeapOffset, RHI::kNullHeapOffset, RHI::kNullHeapOffset };
        // The thickness map (issue #1242) on the ENGINE-heap arm. The second of
        // the two arms this function forks into — forgetting it here is how a
        // feature works on Vulkan and silently does not on a bindless GL build,
        // so both arms are filled or neither is.
        ubo.ThicknessMapHeapOffset = resolve(mat.thicknessMapID, k2DSampler, k2D);
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
    // (HeapBinding::WritesOffsetsForBoundProgram, issue #691).
    static void BindPBRTextures(RendererAPI& api, const PODMaterialData& mat)
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
        // exactly what BindTrackedTexture stages (issue #691).
        if (const bool materialLocalFromHeap = Shader::ReadsMaterialHeapOffsets(); !materialLocalFromHeap)
        {
            BindTrackedTexture(api, mat.albedoMapID, ShaderBindingLayout::TEX_DIFFUSE);
            BindTrackedTexture(api, mat.metallicRoughnessMapID, ShaderBindingLayout::TEX_SPECULAR);
            BindTrackedTexture(api, mat.normalMapID, ShaderBindingLayout::TEX_NORMAL);
            BindTrackedTexture(api, mat.aoMapID, ShaderBindingLayout::TEX_AMBIENT);
            BindTrackedTexture(api, mat.emissiveMapID, ShaderBindingLayout::TEX_EMISSIVE);
            // The thickness map (issue #1242). Inside the same slot-path guard
            // as the other five: on the bindless arm the offset travelled in the
            // material UBO and this bind would be pure waste.
            BindTrackedTexture(api, mat.thicknessMapID, ShaderBindingLayout::TEX_SKIN_THICKNESS);
        }
        // TEX_USER_0/1 are samplerCube here (irradiance, prefilter) and plain 2D in
        // other consumers — which is why the kind cannot be derived from the SLOT
        // and has to come from the call site that knows what it is binding.
        BindTrackedTexture(api, mat.environmentMapID, ShaderBindingLayout::TEX_ENVIRONMENT,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, mat.irradianceMapID, ShaderBindingLayout::TEX_USER_0, RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, mat.prefilterMapID, ShaderBindingLayout::TEX_USER_1, RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, mat.brdfLutMapID, ShaderBindingLayout::TEX_USER_2);
    }

    static void UploadMaterialState(RendererAPI& api, const PODMaterialData& mat, u16 materialDataIndex)
    {
        OLO_PROFILE_FUNCTION();

        // THE MATERIAL UBO IS CACHED ON THE MATERIAL INDEX, which is what makes
        // per-material heap offsets free — they ride in a buffer that is only
        // re-uploaded when the material actually changes. That cache has to be
        // dropped across a heap re-initialisation, though: the offsets in it index
        // the PREVIOUS heap, and unlike a stale texture bind that reads as a
        // plausible wrong image rather than an obvious one (issue #691).
        if (const u64 heapEpoch = RHI::DescriptorHeap::Get().GetInitEpoch(); heapEpoch != Data().Cache.HeapEpoch)
        {
            Data().Cache.HeapEpoch = heapEpoch;
            Data().Cache.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        }
        // THE SAME ARGUMENT ONE HEAP OVER (ADR 0011 amendment (96)). The epoch
        // above covers a re-INITIALISED engine heap; the Vulkan arm's offsets come
        // from the backend's own slot cache, which reassigns a slot whenever a
        // texture is released or reloaded IN PLACE. That keeps the RHI handle and
        // changes the VkImage, so this UBO's cached offsets would then name a
        // different descriptor — a plausible wrong texture, not black, and nothing
        // else in this function can notice. The generation moves only on those
        // events, so a steady frame still hits the cache.
        if (const u64 slotGeneration = HeapBinding::ShaderHeapGeneration();
            slotGeneration != Data().Cache.ShaderHeapGeneration)
        {
            Data().Cache.ShaderHeapGeneration = slotGeneration;
            Data().Cache.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        }

        // Part of the cache key, not an afterthought — see LastMaterialOffsetsLive.
        const glm::uvec3 table = Renderer3D::GetMaterialShaderHeapTable().GetAddressAndCount();
        if (table.x != Data().Cache.MaterialTableAddressAndCount.x || table.y != Data().Cache.MaterialTableAddressAndCount.y ||
            table.z != Data().Cache.MaterialTableAddressAndCount.z)
        {
            Data().Cache.MaterialTableAddressAndCount = table;
            Data().Cache.LastMaterialDataIndex = INVALID_MATERIAL_DATA_INDEX;
        }
        const bool offsetsLive = Shader::ReadsMaterialHeapOffsets();
        const bool sameIndex = (materialDataIndex == Data().Cache.LastMaterialDataIndex) &&
                               (offsetsLive == Data().Cache.LastMaterialOffsetsLive);
        Data().Cache.LastMaterialDataIndex = materialDataIndex;
        Data().Cache.LastMaterialOffsetsLive = offsetsLive;

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
            pbrMaterialData.PBRModel = mat.pbrModel;
            // Physical transmission / IOR / volume (issue #970). Copied
            // straight through — every value was sanitized by Material's
            // setters and the extinction was derived at submission, so
            // there is nothing left to validate at dispatch.
            pbrMaterialData.TransmissionFactor = mat.transmissionFactor;
            pbrMaterialData.IOR = mat.ior;
            pbrMaterialData.ThicknessFactor = mat.thicknessFactor;
            pbrMaterialData.AttenuationSigmaR = mat.attenuationSigma.r;
            pbrMaterialData.AttenuationSigmaG = mat.attenuationSigma.g;
            pbrMaterialData.AttenuationSigmaB = mat.attenuationSigma.b;
            // Material kind + skin profile (issue #1231). Copied straight
            // through for the same reason as the block above: the kind was
            // range-checked by Material's setter and the profile was
            // resolved (and its parameters sanitized) at submission, so
            // dispatch has nothing left to validate.
            pbrMaterialData.MaterialKind = mat.materialKind;
            pbrMaterialData.SkinProfileSlot = static_cast<i32>(mat.skinProfileSlot);
            pbrMaterialData.SkinSpecularTintR = mat.skinSpecularTint.r;
            pbrMaterialData.SkinSpecularTintG = mat.skinSpecularTint.g;
            pbrMaterialData.SkinSpecularTintB = mat.skinSpecularTint.b;
            pbrMaterialData.SkinEvaluationModel = mat.skinEvaluationModel;
            // Thin-region transmission (issue #1242). The two lanes were
            // packed at submission by SkinTransmissionScatterLane /
            // SkinTransmissionScalingLane, so — like every block above —
            // dispatch copies and does not decide. All-zero on a non-skin
            // material and below transport version 2, which shades as no
            // transmission.
            pbrMaterialData.SkinTransmitScatter = mat.skinTransmitScatter;
            pbrMaterialData.SkinTransmitScaling = mat.skinTransmitScaling;
            pbrMaterialData.UseThicknessMap = mat.thicknessMapID.IsValid() ? 1 : 0;
            pbrMaterialData.SkinThicknessBaseMM = mat.skinThicknessBaseMM;
            // The layered surface response (issue #1243). Packed at
            // submission by SkinSpecularLane / SkinDetailStrength, so —
            // like every block above — dispatch copies and does not decide.
            pbrMaterialData.SkinSpecularLane = mat.skinSpecularLane;
            pbrMaterialData.SkinOralLane = mat.skinOralLane;
            pbrMaterialData.SkinOcularCorneaLane = mat.skinOcularCorneaLane;
            pbrMaterialData.SkinOcularIrisLane = mat.skinOcularIrisLane;
            pbrMaterialData.SkinOcularResponseLane = mat.skinOcularResponseLane;
            pbrMaterialData.SkinOcularTintLane = mat.skinOcularTintLane;
            pbrMaterialData.SkinDetailStrength = mat.skinDetailStrength;
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

            if (Data().Uploads.MaterialUBO)
            {
                constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
                static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch");
                Data().Uploads.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
                BindUBOIfNeeded(api, ShaderBindingLayout::UBO_MATERIAL, Data().Uploads.MaterialUBO->GetRHIHandle());
            }
        }
        else if (Data().Uploads.MaterialUBO)
        {
            // Even when material data hasn't changed, re-establish the binding
            // point (other subsystems may have overwritten it).
            BindUBOIfNeeded(api, ShaderBindingLayout::UBO_MATERIAL, Data().Uploads.MaterialUBO->GetRHIHandle());
        }
        else
        {
            // No additional handling required.
        }

        // Always rebind textures — an intervening pass (e.g. DecalPass)
        // may have changed texture slots since the last material upload.
        BindPBRTextures(api, mat);
    }

    // Bind a texture to a unit only when it differs from the currently-tracked
    // binding, updating the redundant-bind tracker and the bind stat. A 0 id is a
    // no-op (no texture for that slot this frame). Shared by every tracked bind so
    // the check/update/increment logic lives in exactly one place.
    static void BindTrackedTextureUnit(RendererAPI& api, u32 slot, RHI::ResourceHandle texture,
                                       const RHI::SamplerDesc& sampler = {},
                                       RHI::NullSamplerKind kind = RHI::NullSamplerKind::Texture2D)
    {
        if (!texture.IsValid())
            return;
        // Same cache/offset split as BindTrackedTexture above — see the note there.
        if (Data().Cache.BoundTextures[slot] == texture && !HeapBinding::WritesOffsetsForBoundProgram())
            return;

        // Persistent: shadow maps, the snow clipmap and the cloud-shadow map are
        // system-owned and survive the frame, like the material textures above.
        const RHI::HeapOffset staged =
            HeapBinding::BindTextureOrOffset(api, slot, texture, RHI::HeapSlotLifetime::Persistent, sampler, kind);
        // Same correction as BindTrackedTexture — see the note there.
        Data().Cache.BoundTextures[slot] = CacheEntryAfterSeam(staged, texture);
        ++Data().Tally.Stats.TextureBinds;
    }

    static void BindShadowTextures(RendererAPI& api);

    void CommandDispatch::BindSceneShadowTextures()
    {
        BindShadowTextures(RenderCommand::GetRendererAPI());
    }

    // Helper: Bind per-frame shadow and snow depth textures (only relevant for PBR paths).
    // Relies on BoundTextureIDs tracking to avoid redundant binds.
    static void BindShadowTextures(RendererAPI& api)
    {
        // Shared with every other site that stages a shadow-map offset — the
        // state has to be identical or whichever pass ran last silently wins.
        static const RHI::SamplerDesc kShadowCompare = HeapBinding::ShadowDepthSampler(true);
        static const RHI::SamplerDesc kShadowRaw = HeapBinding::ShadowDepthSampler(false);

        // The KIND travels with the bind, not just the sampler: these four are the
        // array-typed inputs on the shared table, so a retired one must poison to
        // its own typed null rather than the 2D one (issue #691).
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SHADOW, s_FrameData.CSMShadowTexture, kShadowCompare,
                               RHI::NullSamplerKind::Texture2DArrayShadow);
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SHADOW_ATLAS, s_FrameData.AtlasShadowTexture, kShadowCompare,
                               RHI::NullSamplerKind::Texture2DArrayShadow);

        // Comparison-OFF raw-depth views for the PCSS blocker search (plain
        // sampler2DArray at TEX_SHADOW_CSM_RAW / TEX_SHADOW_ATLAS_RAW).
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SHADOW_CSM_RAW, s_FrameData.CSMRawShadowTexture, kShadowRaw,
                               RHI::NullSamplerKind::Texture2DArray);
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, s_FrameData.AtlasRawShadowTexture, kShadowRaw,
                               RHI::NullSamplerKind::Texture2DArray);

        // Virtual Shadow Maps (issue #702) — the forward path's half of the same
        // publish the deferred lighting pass does. Unconditional: an inactive VSM
        // publishes a disabled globals block, so the forward PBR shaders' runtime
        // branch resolves to CSM without a second shader variant.
        Renderer3D::GetShadowMap().GetVirtualShadowMap().BindForSampling();

        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SNOW_DEPTH, s_FrameData.SnowDepthTexture);

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
        if (s_FrameData.CloudShadowTexture.IsValid())
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_CLOUD_SHADOW,
                                                     s_FrameData.CloudShadowTexture,
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
        const auto& ids = s_FrameData.DepthPrepassShaders;
        const RHI::ResourceHandle program = mat.shaderRendererID;
        const bool isStatic = (program == ids.PBRStatic || program == ids.GBufferStatic);
        const bool isSkinned = !isStatic && (program == ids.PBRSkinned || program == ids.GBufferSkinned);
        if (!isStatic && !isSkinned)
            return program;

        const bool isMask = (mat.alphaMode == 1);
        // The forward prepass with screen-space AO live writes the view normal
        // too (issue #1452). Only the FORWARD programs swap to the normal
        // variants: a G-Buffer program's attachment 2 is not a view normal.
        const bool forwardProgram = (program == ids.PBRStatic || program == ids.PBRSkinned);
        if (s_FrameData.DepthPrepassActive && s_FrameData.DepthPrepassWritesNormals && forwardProgram)
        {
            const RHI::ResourceHandle normalShader =
                isStatic ? (isMask ? ids.DepthNormalMaskStatic : ids.DepthNormalStatic)
                         : (isMask ? ids.DepthNormalMaskSkinned : ids.DepthNormalSkinned);
            if (normalShader.IsValid())
                return normalShader;
        }
        const RHI::ResourceHandle depthShader = isStatic
                                                    ? (isMask ? ids.DepthMaskStatic : ids.DepthStatic)
                                                    : (isMask ? ids.DepthMaskSkinned : ids.DepthSkinned);
        return depthShader.IsValid() ? depthShader : program;
    }

    // THE FORWARD PREPASS'S ONE COLOUR WRITE (issue #1452): scene attachment
    // 2's view normal, for a draw whose program writes it — the
    // DepthNormalPrepass* swaps and the terrain / voxel programs, which run
    // whole in the prepass. Every other program stays fully masked (a
    // colour-masked custom program declares no normal the AO passes could
    // trust), and so does every blended draw.
    //
    // Set on every prepass draw that calls this. A draw that does not (the
    // skybox, a quad, a custom program) goes through ApplyPODRenderState alone:
    // its full apply resets every attachment, and its cache hit closes
    // attachment 2 when an earlier draw left it open (PrepassNormalWriteOpen),
    // so the render-state cache stays valid for the whole prepass.
    static void ApplyPrepassViewNormalWrite(RendererAPI& api, u16 renderStateIndex, bool programWritesViewNormal)
    {
        if (!s_FrameData.DepthPrepassActive || !s_FrameData.DepthPrepassWritesNormals)
            return;
        const bool blended = renderStateIndex != INVALID_RENDER_STATE_INDEX &&
                             FrameDataBufferManager::Get().GetRenderState(renderStateIndex).blendEnabled;
        const bool enable = programWritesViewNormal && !blended;
        if (enable != Data().Cache.PrepassNormalWriteOpen)
        {
            api.SetColorMaskForAttachment(2, enable, enable, false, false);
            Data().Cache.PrepassNormalWriteOpen = enable;
        }
    }

    [[nodiscard]] static bool IsDepthNormalPrepassProgram(RHI::ResourceHandle program)
    {
        const auto& ids = s_FrameData.DepthPrepassShaders;
        return program.IsValid() &&
               (program == ids.DepthNormalStatic || program == ids.DepthNormalSkinned ||
                program == ids.DepthNormalMaskStatic || program == ids.DepthNormalMaskSkinned ||
                program == ids.FoliageInstanceDepthNormal || program == ids.FoliageImpostorDepthNormal);
    }

    // The forward foliage programs' depth + view-normal twins (issue #1474),
    // for FoliagePrepassPass's replay of the foliage bucket ahead of the
    // screen-space AO passes. Only while the prepass writes normals: that is
    // the one prepass the foliage bucket is replayed in, and the deferred
    // Geometry-bucket prepass keeps the G-Buffer foliage program whole (see
    // DrawFoliageLayer). A MISSING twin returns the null handle and the caller
    // skips the draw: the colour program cannot write the view normal, and its
    // depth over whatever normal the scene prepass left in attachment 2 would
    // pair a leaf's depth with the ground's normal. Skipped, foliage is out of
    // the prepass exactly as before #1474, so its forward AO is that of the
    // surface behind it, which is said once rather than left to be found.
    [[nodiscard]] static RHI::ResourceHandle ResolveFoliageDepthNormalPrepassShader(RHI::ResourceHandle program)
    {
        if (!s_FrameData.DepthPrepassActive || !s_FrameData.DepthPrepassWritesNormals)
            return program;
        const auto& ids = s_FrameData.DepthPrepassShaders;
        RHI::ResourceHandle twin{};
        if (program == ids.FoliageInstance)
            twin = ids.FoliageInstanceDepthNormal;
        else if (program == ids.FoliageImpostor)
            twin = ids.FoliageImpostorDepthNormal;
        else
            return program;
        if (twin.IsValid())
            return twin;
        static std::atomic<bool> s_WarnedMissingTwin{ false };
        if (!s_WarnedMissingTwin.exchange(true, std::memory_order_relaxed))
        {
            OLO_CORE_WARN("CommandDispatch::DrawFoliageLayer: no Foliage_*_DepthNormal program is loaded, so foliage is left "
                          "out of the forward prepass and forward foliage takes the screen-space AO of the surface behind "
                          "it (issue #1474).");
        }
        return RHI::ResourceHandle{};
    }

    // Helper: Upload bone matrices from FrameDataBuffer.
    // Returns false ONLY when this draw needed a bone palette and did not get
    // one. A static draw, or an animated one with no bones, is nothing-to-do
    // rather than failure and returns true.
    //
    // The distinction matters because the caller must not draw on false: the
    // animation UBO then still holds the PREVIOUS draw's palette, so the mesh
    // renders in another entity's pose -- a plausible wrong image rather than a
    // visibly missing one, which is the harder kind to notice.
    [[nodiscard]] static bool UploadBoneMatrices(RendererAPI& api, bool isAnimated, u32 boneBufferOffset, u32 boneCount,
                                                 u32 prevBoneBufferOffset = UINT32_MAX)
    {
        if (!isAnimated || !Data().Uploads.BoneMatricesUBO || boneCount == 0)
            return true;

        using namespace UBOStructures;
        constexpr sizet MAX_BONES = AnimationConstants::MAX_BONES;
        sizet count = glm::min(static_cast<sizet>(boneCount), MAX_BONES);

        if (boneCount > MAX_BONES)
        {
            OLO_CORE_WARN("Animated mesh has {} bones, exceeding limit of {}. Bone matrices will be truncated.",
                          boneCount, MAX_BONES);
        }

        // Bounded accessor: this reads `count` matrices from the pointer, and
        // GetBoneMatrixPtr validates only the first one.
        const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixRange(boneBufferOffset, static_cast<u32>(count));
        if (!boneMatrices)
        {
            // GetBoneMatrixRange has already logged the offending range.
            return false;
        }
        Data().Uploads.BoneMatricesUBO->SetData(boneMatrices, static_cast<u32>(count * sizeof(glm::mat4)));
        BindUBOIfNeeded(api, ShaderBindingLayout::UBO_ANIMATION, Data().Uploads.BoneMatricesUBO->GetRHIHandle());

        // Previous-frame bone matrices for per-bone velocity. Both the forward
        // PBR_MultiLight_Skinned (scene FB RT3) and deferred PBR_GBuffer_Skinned
        // (G-Buffer RT3) variants bind this UBO at binding 31. Upload only when
        // the caller provided a distinct offset (UINT32_MAX sentinel means
        // "reuse current", which matches static / first-frame animated meshes).
        if (Data().Uploads.PrevBoneMatricesUBO)
        {
            const glm::mat4* prevBoneMatrices = nullptr;
            if (prevBoneBufferOffset != UINT32_MAX)
                prevBoneMatrices = FrameDataBufferManager::Get().GetBoneMatrixRange(prevBoneBufferOffset, static_cast<u32>(count));

            // Fall back to current bones whenever the prev stream is missing
            // (sentinel offset OR allocator pointer lookup returned null) so
            // the prev UBO never carries stale bytes from a previous draw —
            // skinned shaders then compute zero bone-motion instead of
            // garbage / leftover entity data.
            const glm::mat4* sourceData = prevBoneMatrices ? prevBoneMatrices : boneMatrices;
            if (sourceData)
            {
                Data().Uploads.PrevBoneMatricesUBO->SetData(sourceData, static_cast<u32>(count * sizeof(glm::mat4)));
                BindUBOIfNeeded(api, ShaderBindingLayout::UBO_ANIMATION_PREV, Data().Uploads.PrevBoneMatricesUBO->GetRHIHandle());
            }
        }

        return true;
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
        CommandBucket::SetParallelReplayClassifier([](const CommandPacket& packet) -> u32
                                                   {
            switch (packet.GetCommandType())
            {
                case CommandType::DrawMesh:
                    return packet.GetCommandData<DrawMeshCommand>()->occlusionQueryIndex == UINT32_MAX ? 1u : 0u;
                case CommandType::DrawMeshInstanced:
                    return std::clamp(packet.GetCommandData<DrawMeshInstancedCommand>()->transformCount,
                                      1u, CommandBucketConfig::kMaxDispatchableMeshInstances);
                case CommandType::DrawDecal:
                    return Renderer3D::IsDecalVisibilityObserved(packet.GetCommandData<DrawDecalCommand>()->entityID) ? 0u : 1u;
                case CommandType::DrawQuad:
                case CommandType::DrawSkybox:
                case CommandType::DrawInfiniteGrid:
                case CommandType::DrawTerrainPatch:
                case CommandType::DrawVoxelMesh:
                case CommandType::DrawFoliageLayer:
                case CommandType::DrawWater:
                    return 1u;
                default:
                    return 0u;
            } });

        // The redundant-bind caches are valid for one replay. Between two
        // replays a pass is free to bind programs, vertex arrays and textures
        // through the API (a compute dispatch, a fullscreen draw, a resolve),
        // and none of that reaches the caches. Every pass that replayed after
        // such work had to remember InvalidateBindingCaches, and the ones that
        // did each learned it from a draw out of vertex array 0 or a wrong
        // frame; the two GPU occlusion passes never did, and drew their
        // recovered instances with the cull's compute program bound whenever
        // the scene pass happened to end on the same G-Buffer program.
        CommandBucket::SetReplayEntryHook(&CommandDispatch::BeginReplay);

        // A bucket's view override replays inside the alternate-view scope.
        CommandBucket::SetViewScopeRunner([](const BucketViewState& view, const std::function<void()>& replay)
                                          {
            const ScopedAlternateView scope(
                MakeAlternateView(view.ViewMatrix, view.ProjectionMatrix, view.ViewProjectionMatrix, view.ViewPosition));
            replay(); });

        OLO_CORE_INFO("CommandDispatch: Initialized (UBOs managed by Renderer3D)");
    }

    void CommandDispatch::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(!s_RecordingData, "The dispatcher is shut down from the render thread, outside a recording item");
        RenderCommand::ReleaseParallelRecordingResources();
        // The whole primary context and the pass flags, by assignment (ADR
        // 0025): the shared buffer references (this is their co-owner), the
        // view, every bind cache and counter, and the shader and texture
        // identities the pass flags snapshot. The list this replaces dropped
        // five references and left a restarted or re-homed dispatcher with the
        // previous device's camera, heap epochs and prepass program identities.
        s_MainData = {};
        s_FrameData = {};
    }

    bool CommandDispatch::HasUBOReferences()
    {
        // The camera UBO stands for the set: SetUBOReferences publishes them
        // together and Shutdown resets them together. A set published on
        // another backend is no set (#1655): ResetState drops it, so the
        // renderer's BeginScene must re-publish rather than keep it.
        return Data().Uploads.CameraUBO != nullptr &&
               Data().Uploads.Backend == static_cast<u32>(Renderer::GetAPI());
    }

    void CommandDispatch::SetUBOReferences(
        const Ref<UniformBuffer>& cameraUBO,
        const Ref<UniformBuffer>& materialUBO,
        const Ref<UniformBuffer>& boneMatricesUBO,
        const Ref<InstanceBuffer>& modelInstanceBuffer,
        const Ref<UniformBuffer>& prevBoneMatricesUBO,
        TiledForwardPlus* forwardPlus)
    {
        Data().Uploads.CameraUBO = cameraUBO;
        Data().Uploads.MaterialUBO = materialUBO;
        Data().Uploads.BoneMatricesUBO = boneMatricesUBO;
        Data().Uploads.ModelInstanceBuffer = modelInstanceBuffer;
        Data().Uploads.PrevBoneMatricesUBO = prevBoneMatricesUBO;
        Data().Uploads.Backend = static_cast<u32>(Renderer::GetAPI());
        s_FrameData.ForwardPlus = forwardPlus;
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
                                 Ref<InstanceBuffer>& instanceBuffer,
                                 const glm::vec4& lightmapScaleOffset = glm::vec4(0.0f),
                                 const GPUSceneDrawLink* gpuSceneLink = nullptr,
                                 bool gpuSceneMaterialsBound = false,
                                 const glm::vec4& color = glm::vec4(1.0f),
                                 f32 custom = 0.0f)
        {
            if (!instanceBuffer)
                return;

            // This one choke point covers every single-instance mesh path —
            // main, depth-only, quad, terrain, voxel — and is therefore where
            // the GPU Scene migration lands: a draw that carries a resolved
            // canonical link takes its transforms from the record, everything
            // else keeps the camera-relative shift it always did. The normal
            // matrix is translation-invariant so it is uploaded unchanged.
            // Near origin the origin is (0,0,0) and the shift is a no-op. Read
            // the render origin from CommandDispatch's own state (the same
            // source UploadCameraUBO and DrawMeshInstanced use) so the relative
            // transform and the camera shift cannot diverge.
            const glm::vec3 origin = CommandDispatch::GetRenderOrigin();

            InstanceData inst;
            inst.Normal = modelData.Normal;
            inst.EntityID = modelData.EntityID;
            inst.LightmapScaleOffset = lightmapScaleOffset;
            // The command's own tint and free float (DrawMeshCommand::color /
            // custom; white and 0 for every caller that passes none). A batch
            // of these commands carries the same two values per source
            // (CommandBucket::BatchCommands), so a draw is tinted the same
            // whether or not it was batched.
            inst.Color = color;
            inst.Custom = custom;

            if (gpuSceneLink && gpuSceneLink->m_Resolved)
            {
                // The migrated path (issue #994). The record's transforms are
                // ALREADY render-relative — GPUScene encoded them with the same
                // origin this frame set — so they are used as they are; shifting
                // them again would double the offset and put the mesh at twice
                // the distance from the camera.
                //
                // The previous transform matters more than the current one. The
                // per-entity cache below is keyed on entity id and is written on
                // every DrawMesh call, so a multi-submesh mesh overwrote its own
                // history between submeshes and submeshes 1..N-1 reported zero
                // velocity. The instance record's history is per (entity,
                // geometry, submesh) — the same identity a future TLAS/ReSTIR
                // consumer resolves — so motion vectors and the RT contract now
                // read one truth.
                inst.Transform = gpuSceneLink->m_CurrentTransform;
                inst.PrevTransform = gpuSceneLink->m_PreviousTransform;
                inst.GPUSceneRef = gpuSceneLink->Ref();
                // The material half of the link is only honest while the
                // canonical material table is the current occupant of its
                // aliased slot. Without the bind the shader would resolve the
                // slot against whatever else is there, so the material lanes
                // are zeroed — generation zero is "no link" — and the draw
                // reads the per-draw material UBO exactly as before. The
                // transform half needs no binding and stays.
                if (!gpuSceneMaterialsBound)
                {
                    inst.GPUSceneRef.z = 0u;
                    inst.GPUSceneRef.w = GPUSceneDrawRefUnlinked;
                }
                ++Data().Tally.GPUSceneConsumedDraws;
            }
            else
            {
                if (gpuSceneLink)
                {
                    // A link that was carried but did not resolve: the record
                    // was rejected at extraction or retired before the commit.
                    ++Data().Tally.GPUSceneFallbackDraws;
                }
                // Camera-relative (issue #429): shift the world transform (and
                // its previous-frame counterpart, for motion vectors) into
                // render-relative space before upload. This is the legacy
                // adapter half of the choke point — every path that has no
                // canonical link yet (GPUSceneLegacyAdapters.h) lands here.
                inst.Transform = MakeModelRelative(modelData.Model, origin);
                inst.PrevTransform = MakeModelRelative(modelData.PrevModel, origin);
            }

            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();
        }
    } // namespace

    void CommandDispatch::PublishFrameCamera()
    {
        const Ref<UniformBuffer>& camera = Data().Uploads.CameraUBO;
        if (!camera)
            return;
        auto& api = RenderCommand::GetRendererAPI();
        const RHI::ResourceHandle frameCamera = camera->GetRHIHandle();
        if (api.IsUniformBufferBound(ShaderBindingLayout::UBO_CAMERA, frameCamera))
            return;
        api.BindUniformBuffer(ShaderBindingLayout::UBO_CAMERA, frameCamera);
        ++Data().Tally.FrameCameraRepublished;
    }

    u32 CommandDispatch::GetFrameCameraRepublishCount()
    {
        return Data().Tally.FrameCameraRepublished;
    }

    void CommandDispatch::BindSceneResources()
    {
        auto& api = RenderCommand::GetRendererAPI();
        if (Data().Uploads.CameraUBO)
        {
            BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
        }

        if (s_FrameData.ForwardPlus)
        {
            s_FrameData.ForwardPlus->UploadDisabledUBO();
            // The disabled UBO keeps a replayed ForwardPlusCommon.glsl shader
            // from READING bindings 9-12 and 18, but it still DECLARES them,
            // and Vulkan logs an [error] per shader for a declared storage
            // binding with no occupant. The planar-reflection replay runs after
            // ScenePass's UnbindAfterShading emptied them (issue #1487). Their
            // other tenant, GPUScene (9/10), binds its own tables per pass.
            s_FrameData.ForwardPlus->PublishBuffers();
        }

        // The motion-blur matrices the G-Buffer programs, the deferred
        // lighting pass and the virtual-geometry G-Buffer stage read at
        // UBO_USER_1. That binding is pass-scoped: it has other tenants (the
        // Gaussian-splat sort block) and no frame owner, so the reader binds.
        // Here, before any fork, so every recording item inherits it (#1687).
        Renderer3D::BindMotionMatrices();

        // Publish all shared PBR sampled inputs before the fork, including
        // freshly compute-written cloud shadows and the VSM physical pool.
        // Their first sampled transitions must belong to the caller.
        BindShadowTextures(api);

        BindForwardScreenSpaceAO();
    }

    void CommandDispatch::BindForwardScreenSpaceAO()
    {
        // The forward shaders' screen-space AO inputs (issue #1452) — see
        // include/ForwardScreenSpaceAO.glsl. White when AO is not live for
        // this pass, so the declared samplers never dangle; the camera UBO's
        // ScreenSpaceAOParams.x is what says whether the value is meaningful.
        const Ref<Texture2D>& white = Renderer3D::GetWhiteTexture();
        const RHI::ResourceHandle whiteHandle = white ? white->GetRHIHandle() : RHI::ResourceHandle{};
        const bool aoSuspended = !Data().View.MainViewLanes;
        const RHI::ResourceHandle aoTexture = !aoSuspended && s_FrameData.ForwardScreenSpaceAOTexture.IsValid()
                                                  ? s_FrameData.ForwardScreenSpaceAOTexture
                                                  : whiteHandle;
        const RHI::ResourceHandle aoDepth = !aoSuspended && s_FrameData.ForwardScreenSpaceAODepth.IsValid()
                                                ? s_FrameData.ForwardScreenSpaceAODepth
                                                : whiteHandle;
        if (aoTexture.IsValid())
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_SSAO, aoTexture,
                                                     RHI::HeapSlotLifetime::FrameTransient);
            InvalidateTextureSlot(ShaderBindingLayout::TEX_SSAO);
        }
        if (aoDepth.IsValid())
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, aoDepth,
                                                     RHI::HeapSlotLifetime::FrameTransient);
            InvalidateTextureSlot(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);
        }
    }

    void CommandDispatch::SetForwardScreenSpaceAO(const glm::vec4& params, RHI::ResourceHandle aoTexture,
                                                  RHI::ResourceHandle depthTexture)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        const bool live = params.x > 0.5f && aoTexture.IsValid() && depthTexture.IsValid();
        s_FrameData.ForwardScreenSpaceAOParams = live ? params : glm::vec4(0.0f);
        s_FrameData.ForwardScreenSpaceAOTexture = live ? aoTexture : RHI::ResourceHandle{};
        s_FrameData.ForwardScreenSpaceAODepth = live ? depthTexture : RHI::ResourceHandle{};
    }

    void CommandDispatch::SetLightingTap(u32 tap)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.LightingTap = static_cast<f32>(tap);
    }

    void CommandDispatch::UploadMaterialForDirectDraw(const PODMaterialData& mat, u16 materialDataIndex)
    {
        auto& api = RenderCommand::GetRendererAPI();
        UploadMaterialState(api, mat, materialDataIndex);
    }

    void CommandDispatch::InvalidateGPUSceneMaterialBinding()
    {
        Data().Cache.GPUSceneMaterialsBound = false;
    }

    u32 CommandDispatch::GetGPUSceneConsumedDrawCount()
    {
        return Data().Tally.GPUSceneConsumedDraws;
    }

    u32 CommandDispatch::GetGPUSceneFallbackDrawCount()
    {
        return Data().Tally.GPUSceneFallbackDraws;
    }

    // The reset both ResetState (frame start) and InvalidateBindingCaches (pass
    // entry) perform, and the one place the #1349 negative control cuts.
    //
    // With the fault on, the texture, material and render-state caches outlive
    // every reset the engine asks for, so state something else changed is
    // skipped as already in place. It is a control, never a behaviour. Until
    // #1688 the fault only cut the frame-start reset, and what it broke was the
    // uniform-buffer cache. The dispatcher keeps no such record any more, and
    // measured on the control's trace the frame-start and pass-entry resets
    // alone are no longer missed by anything: the caches that remain are also
    // dropped by targeted invalidations. So the fault cuts those two resets and
    // InvalidateRenderStateCache, and it is the stale render state (a pass that
    // set depth or blend state directly, then a draw whose state index matches
    // the last one applied) that binding-cache-cold catches. The shader and
    // vertex-array caches still reset: a stale VAO draws from VAO 0, which some
    // drivers answer with an access violation rather than a wrong frame, and a
    // control must fail a check, not take the process down.
    //
    // `distrustUniformBuffers` is the uniform-buffer half of the same reset,
    // which lives in the backend. A replay entry leaves it alone: the backend
    // record sees every bind the engine makes between two replays, which is
    // exactly what this cache cannot, so only the places that follow work done
    // behind the engine (frame start, an explicit invalidation) withdraw it.
    static void ResetBindCacheUnlessFaulted(bool distrustUniformBuffers)
    {
        auto& cache = Data().Cache;
        if (Levers::FaultSkipDispatchBindingReset())
        {
            cache.CurrentBoundShader = {};
            cache.CurrentBoundVAO = {};
        }
        else
        {
            cache = {};
            if (distrustUniformBuffers)
                RenderCommand::DistrustUniformBufferOccupancy();
        }
    }

    void CommandDispatch::ResetState()
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        // Shared buffer references published on another backend are not this
        // frame's to bind (#1655). One process runs both only in the test
        // binary: the OpenGL renderer publishes its camera and material buffers
        // once, and a Vulkan suite that dispatches next bound those GL handles,
        // which cannot resolve there. Dropped, not kept: the renderer that
        // published them re-publishes at its next BeginScene
        // (ReclaimSharedRenderState, on HasUBOReferences), on its own backend.
        if (s_MainData.Uploads.Backend != 0u && s_MainData.Uploads.Backend != static_cast<u32>(Renderer::GetAPI()))
            s_MainData.Uploads = {};
        ResetBindCacheUnlessFaulted(true);
        s_FrameData.CSMShadowTexture = {};
        s_FrameData.AtlasShadowTexture = {};
        s_FrameData.CSMRawShadowTexture = {};
        s_FrameData.AtlasRawShadowTexture = {};
        s_FrameData.SnowDepthTexture = {};
        s_FrameData.CloudShadowTexture = {};
        s_FrameData.ForwardScreenSpaceAOParams = glm::vec4(0.0f);
        s_FrameData.ForwardScreenSpaceAOTexture = {};
        s_FrameData.ForwardScreenSpaceAODepth = {};
        s_FrameData.DepthPrepassActive = false;
        s_FrameData.DepthPrepassWritesNormals = false;
        s_FrameData.DepthPrepassColorPassActive = false;
        s_FrameData.OverdrawActive = false;
        s_FrameData.PassAttachmentWriteLimit = 0xFF;
        Data().Tally = {};
    }

    void CommandDispatch::InvalidateRenderStateCache()
    {
        // The #1349 negative control cuts this too: ResetBindCacheUnlessFaulted.
        if (Levers::FaultSkipDispatchBindingReset())
            return;
        Data().Cache.LastRenderStateIndex = INVALID_RENDER_STATE_INDEX;
    }

    void CommandDispatch::SetPassAttachmentWriteLimit(u8 attachmentMask)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.PassAttachmentWriteLimit = attachmentMask;
        // The ceiling is part of every applied state, so a cached state
        // index from before it changed no longer describes the context.
        InvalidateRenderStateCache();
    }

    void CommandDispatch::BeginReplay()
    {
        // Legal inside a recording item (a serial bucket replayed by a worker):
        // Data() is then that item's cache.
        ResetBindCacheUnlessFaulted(false);
    }

    void CommandDispatch::InvalidateBindingCaches()
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Binding caches are reset between passes, not during recording");
        // The binding half of ResetState(): the counters, viewport and pass
        // flags describe the frame so far and stay.
        ResetBindCacheUnlessFaulted(true);
    }

    void CommandDispatch::ForgetBindingCaches()
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Binding caches are reset between passes, not during recording");
        Data().Cache = {};
        RenderCommand::DistrustUniformBufferOccupancy();
    }

    void CommandDispatch::SetDepthPrepassActive(bool active, bool writeViewNormals)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.DepthPrepassActive = active;
        s_FrameData.DepthPrepassWritesNormals = active && writeViewNormals;
        if (active)
        {
            s_FrameData.DepthPrepassColorPassActive = false;
            // Snapshot the depth-only swap programs once per prepass — cheap,
            // and keeps the swap correct across shader hot-reloads (renderer
            // IDs can change when a program is recompiled).
            s_FrameData.DepthPrepassShaders = Renderer3D::GetDepthPrepassShaderIDs();
        }
        // Invalidate cache so the next command re-applies state
        InvalidateRenderStateCache();
    }

    bool CommandDispatch::IsDepthPrepassActive()
    {
        return s_FrameData.DepthPrepassActive;
    }

    bool CommandDispatch::DoesDepthPrepassWriteNormals()
    {
        return s_FrameData.DepthPrepassActive && s_FrameData.DepthPrepassWritesNormals;
    }

    void CommandDispatch::SetDepthPrepassColorPassActive(bool active)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.DepthPrepassColorPassActive = active;
        if (active)
        {
            s_FrameData.DepthPrepassActive = false;
        }
        // Invalidate cache so the next command re-applies state
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetOverdrawActive(bool active)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.OverdrawActive = active;
        if (active)
        {
            // Overdraw is mutually exclusive with the depth-prepass modes.
            s_FrameData.DepthPrepassActive = false;
            s_FrameData.DepthPrepassColorPassActive = false;
            // Snapshot the depth-only swap programs once (same rationale as
            // SetDepthPrepassActive) — overdraw reuses them so every batchable
            // opaque geometry type keeps its correct vertex transform while its
            // fragment stage emits the constant overdraw count.
            s_FrameData.DepthPrepassShaders = Renderer3D::GetDepthPrepassShaderIDs();
        }
        // Invalidate cache so the next command re-applies state.
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetWaterTargetSize(u32 width, u32 height)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.WaterTargetWidth = width;
        s_FrameData.WaterTargetHeight = height;
    }

    void CommandDispatch::SetWaterDepthCaptureActive(bool active)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.WaterDepthCaptureActive = active;
        if (active)
        {
            // Snapshot the depth-only water program once per capture — same
            // hot-reload rationale as SetDepthPrepassActive.
            s_FrameData.WaterDepthShaderID = Renderer3D::GetWaterDepthShaderID();
        }
        // Invalidate so the next command — and the post-capture color command —
        // re-applies state; otherwise a same-render-state water command would
        // early-out and leak the depth-only override into the color pass.
        InvalidateRenderStateCache();
    }

    void CommandDispatch::SetView(const DispatchView& view)
    {
        Data().View = view;
    }

    const DispatchView& CommandDispatch::GetView()
    {
        return Data().View;
    }

    DispatchView CommandDispatch::MakeAlternateView(const glm::mat4& view, const glm::mat4& projection,
                                                    const glm::mat4& viewProjection, const glm::vec3& position)
    {
        const DispatchView& current = Data().View;
        DispatchView alternate;
        alternate.ViewMatrix = view;
        alternate.ProjectionMatrix = projection;
        alternate.ViewProjectionMatrix = viewProjection;
        alternate.ViewPosition = position;
        // One render origin per frame: every upload in the frame is relative to
        // it, whichever camera draws.
        alternate.RenderOrigin = current.RenderOrigin;
        // No history of its own, so the alternate camera is its own previous
        // frame: zero camera velocity, the convention a fresh view uses. The
        // jitter is the frame's (an alternate projection is derived from the
        // jittered one) and is likewise its own previous.
        alternate.PrevViewProjectionMatrix = viewProjection;
        alternate.JitterUV = glm::vec4(current.JitterUV.x, current.JitterUV.y, current.JitterUV.x, current.JitterUV.y);
        alternate.MainViewLanes = false;
        return alternate;
    }

    CommandDispatch::ScopedAlternateView::ScopedAlternateView(const DispatchView& view)
        : m_Previous(Data().View)
    {
        Data().View = view;
        UploadCameraUBO();
        // The applied render state was chosen under the other view's pass.
        InvalidateRenderStateCache();
    }

    CommandDispatch::ScopedAlternateView::~ScopedAlternateView()
    {
        Data().View = m_Previous;
        // Restoring the view restores what it uploads, not what it binds: the
        // alternate view published the white placeholders at the two AO units.
        if (m_Previous.MainViewLanes && !s_RecordingData)
            BindForwardScreenSpaceAO();
        UploadCameraUBO();
        InvalidateRenderStateCache();
    }

    CommandDispatch::ScopedFrameCameraRestore::~ScopedFrameCameraRestore()
    {
        UploadCameraUBO();
    }

    const glm::mat4& CommandDispatch::GetViewMatrix()
    {
        return Data().View.ViewMatrix;
    }

    const glm::mat4& CommandDispatch::GetProjectionMatrix()
    {
        return Data().View.ProjectionMatrix;
    }

    const glm::mat4& CommandDispatch::GetViewProjectionMatrix()
    {
        return Data().View.ViewProjectionMatrix;
    }

    const glm::vec3& CommandDispatch::GetViewPosition()
    {
        return Data().View.ViewPosition;
    }

    const glm::vec3& CommandDispatch::GetRenderOrigin()
    {
        return Data().View.RenderOrigin;
    }

    void CommandDispatch::UploadCameraUBO()
    {
        UploadCameraUBO(RenderCommand::GetRendererAPI());
    }

    void CommandDispatch::UploadCameraUBO(RendererAPI& api)
    {
        if (!Data().Uploads.CameraUBO)
            return;

        // Camera-relative (issue #429): the stored matrices are world-space, so
        // rebuild the view / view-projection about the render origin and supply
        // the camera position relative to it before upload. This one path covers
        // the shared re-upload *and* the planar-reflection mirror camera (which
        // sets world mirror matrices via Set* then calls this).
        const glm::vec3 origin = Data().View.RenderOrigin;
        const glm::mat4 relView = MakeViewRelative(Data().View.ViewMatrix, origin);
        // Use the stored projection directly. Reconstructing it as VP * inverse(V)
        // is algebraically valid but not bit-stable in f32; terrain and voxel
        // draws re-upload this shared UBO between the depth and colour replays,
        // so even a small reconstruction error can make GL_LEQUAL reject pixels.
        const glm::mat4& projection = Data().View.ProjectionMatrix;

        // A8 projection seam: the stored matrices stay GL-convention; the
        // GPU-visible copies flip at upload (identity on GL). This one seam
        // covers the shared re-upload AND the planar-reflection mirror camera.
        // PrevViewProjection rides the same rasterizer-flavour flip: its
        // consumers difference clip .xy against the current (flipped)
        // rasterized position, and the two flip flavours agree on .xy.
        ShaderBindingLayout::CameraUBO cameraData{};
        cameraData.ViewProjection = RHI::AdjustProjectionForBackend(
            MakeViewProjectionRelative(projection, Data().View.ViewMatrix, origin));
        cameraData.View = relView;
        cameraData.Projection = RHI::AdjustProjectionForBackend(projection);
        cameraData.Position = MakePositionRelative(Data().View.ViewPosition, origin);
        cameraData.Pad0 = 0.0f;
        cameraData.PrevViewProjection = RHI::AdjustProjectionForBackend(
            MakeViewProjectionRelative(Data().View.PrevViewProjectionMatrix, origin));
        // The jitter those two carry, which every velocity writer subtracts (#1552).
        cameraData.JitterUV = Data().View.JitterUV;
        cameraData.RenderOrigin = origin; // for pattern shaders (triplanar/noise/etc.)
        // Reconstruction flavour (#691): terrain tessellation scale,
        // water depth math and the culling compute read this member.
        cameraData.ProjectionForReconstruction = RHI::AdjustProjectionForShaderReconstruction(projection);
        // The forward shaders' screen-space AO lane (issue #1452). A mirrored
        // replay clears it first (PlanarReflectionRenderPass): the AO buffer
        // belongs to the main view.
        const bool mainViewLanes = Data().View.MainViewLanes;
        cameraData.ScreenSpaceAOParams = mainViewLanes ? s_FrameData.ForwardScreenSpaceAOParams : glm::vec4(0.0f);
        cameraData.LightingTap = mainViewLanes ? s_FrameData.LightingTap : 0.0f;
        Data().Uploads.CameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
        BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
    }

    void CommandDispatch::SetShadowTextures(RHI::ResourceHandle csmTexture, RHI::ResourceHandle atlasTexture,
                                            RHI::ResourceHandle csmRawTexture, RHI::ResourceHandle atlasRawTexture)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.CSMShadowTexture = csmTexture;
        s_FrameData.AtlasShadowTexture = atlasTexture;
        s_FrameData.CSMRawShadowTexture = csmRawTexture;
        s_FrameData.AtlasRawShadowTexture = atlasRawTexture;
    }

    void CommandDispatch::SetForwardPlus(TiledForwardPlus* forwardPlus)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.ForwardPlus = forwardPlus;
    }

    void CommandDispatch::SetSnowDepthTexture(RHI::ResourceHandle texture)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.SnowDepthTexture = texture;
    }

    void CommandDispatch::SetCloudShadowTexture(RHI::ResourceHandle texture)
    {
        OLO_CORE_ASSERT(!s_RecordingData, "Frame state is frozen during recording");
        s_FrameData.CloudShadowTexture = texture;
    }

    CommandDispatch::Statistics& CommandDispatch::GetStatistics()
    {
        return Data().Tally.Stats;
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
        // is the point (issue #691). Whether a handler owes a flush is a
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
        BindVAOIfNeeded(api, cmd->vertexArrayID);
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
        BindVAOIfNeeded(api, cmd->vertexArrayID);
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
        BindVAOIfNeeded(api, cmd->vertexArrayID);
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
        BindVAOIfNeeded(api, cmd->vertexArrayID);
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
        if (s_FrameData.DepthPrepassActive)
        {
            shaderToBind = ResolveDepthPrepassShader(mat);
            prepassDepthOnly = (shaderToBind != mat.shaderRendererID);
            ApplyPrepassViewNormalWrite(api, cmd->renderStateIndex, IsDepthNormalPrepassProgram(shaderToBind));
        }
        else if (s_FrameData.OverdrawActive)
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
        if (Data().Cache.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            Data().Cache.CurrentBoundShader = shaderToBind;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // During the depth prepass OR the overdraw view, only the model matrix and
        // bones are needed (vertex transform). Skip material, textures, normal
        // matrix, and light UBOs — the overdraw fragment stage emits a constant,
        // and MASK materials still get their UBO + albedo below for the alpha test.
        if (s_FrameData.DepthPrepassActive || s_FrameData.OverdrawActive)
        {
            // Camera UBO is still needed for vertex transform (u_ViewProjection)
            if (Data().Uploads.CameraUBO)
            {
                BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
            }

            if (Data().Uploads.ModelInstanceBuffer)
            {
                ShaderBindingLayout::ModelUBO modelData;
                modelData.Model = cmd->transform;
                // Read only by a prepass that writes view normals (#1452).
                modelData.Normal = s_FrameData.DepthPrepassWritesNormals ? glm::transpose(glm::inverse(cmd->transform))
                                                                         : glm::mat4(1.0f);
                modelData.EntityID = cmd->entityID;
                modelData.PadEntity[0] = 0;
                modelData.PadEntity[1] = 0;
                modelData.PadEntity[2] = 0;
                modelData.PrevModel = cmd->prevTransform;

                // The prepass must transform the vertex with EXACTLY the matrix
                // the colour pass will use — that is what the `invariant
                // gl_Position` contract and the GL_LEQUAL re-test depend on —
                // so it consumes the same link rather than the command's copy.
                // The prepass reads no material, so it takes the link for its
                // transforms only and never binds the material table.
                UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer, glm::vec4(0.0f),
                                    Renderer3D::GetGPUSceneDrawLink(cmd->gpuSceneDrawLink), false);
            }

            // MASK materials still need the material UBO (alpha cutoff) and the
            // albedo texture so the depth-only alpha test carves the same
            // coverage as the color pass. (Also fixes the pre-swap behavior,
            // where the prepass ran the full shader against whatever material
            // state the previous draw left bound.)
            //
            // A prepass that writes view normals (issue #1452) reads the whole
            // material: the normal map, its scale, the skin detail band and the
            // ocular lanes all shape the normal it writes.
            if (prepassDepthOnly && (mat.alphaMode == 1 || s_FrameData.DepthPrepassWritesNormals))
            {
                UploadMaterialState(api, mat, cmd->materialDataIndex);
            }

            // Bone matrices are still needed for skinned mesh vertex positions.
            // prevBoneBufferOffset uses UINT32_MAX as a sentinel meaning "alias current"
            // (static / first-frame / non-Deferred path) — the helper then skips the second
            // upload and the skinned shader reads the same data for both current and prev.
            if (!UploadBoneMatrices(api, cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCount, cmd->prevBoneBufferOffset))
            {
                // No palette reached the GPU: drawing now would skin this mesh
                // with the previous draw's bones.
                return;
            }
        }
        else
        {
            // Camera and Light UBO data is uploaded once per frame during
            // `RenderPipeline::PrepareFrame(...)`, but their
            // binding points may be overwritten by other subsystem UBOs (e.g.
            // ShadowMap creates its own Camera UBO at the same binding point).
            // Re-establish the binding so shaders read the correct scene-camera buffer.
            if (Data().Uploads.CameraUBO)
            {
                BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
            }

            // Update model matrix UBO
            if (Data().Uploads.ModelInstanceBuffer)
            {
                ShaderBindingLayout::ModelUBO modelData;
                modelData.Model = cmd->transform;
                modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
                modelData.EntityID = cmd->entityID;
                modelData.PadEntity[0] = 0;
                modelData.PadEntity[1] = 0;
                modelData.PadEntity[2] = 0;
                modelData.PrevModel = cmd->prevTransform;

                constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
                static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");

                // The canonical material table is a pass-local alias of slot 17
                // (GPUScene.h), so it is taken immediately before the draw that
                // reads it and never left bound as global state. Only the
                // material table: the per-draw InstanceData stream still owns
                // slot 15 and binding the canonical instances there would
                // replace it mid-pass. Bind BEFORE the upload, because the
                // upload writes the material half of the link only when the
                // bind succeeded.
                const GPUSceneDrawLink* gpuSceneLink = Renderer3D::GetGPUSceneDrawLink(cmd->gpuSceneDrawLink);
                const bool gpuSceneMaterialsBound =
                    gpuSceneLink && gpuSceneLink->m_Resolved && BindGPUSceneMaterialsIfNeeded();
                UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer, cmd->lightmapScaleOffset, gpuSceneLink,
                                    gpuSceneMaterialsBound, cmd->color, cmd->custom);
            }

            // Material UBO + texture bindings (skipped when material unchanged)
            UploadMaterialState(api, mat, cmd->materialDataIndex);

            // Shadow/snow textures (per-frame, outside material diffing)
            BindShadowTextures(api);

            // Bone matrices
            if (!UploadBoneMatrices(api, cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCount, cmd->prevBoneBufferOffset))
            {
                // No palette reached the GPU: drawing now would skin this mesh
                // with the previous draw's bones.
                return;
            }
        }

        if (cmd->indexCount == 0)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
            return;
        }

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(api, cmd->vertexArrayID);

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
        // is the hardest kind to notice (issue #691).
        //
        // Cheap when nothing changed: FlushOffsets early-outs on a clean table
        // and DescriptorHeap::Flush uploads only the dirty span.
        HeapBinding::FlushOffsets();
        // baseIndex offsets into a single IBO shared by a multi-submesh
        // MeshSource. The index-count-to-byte-offset conversion now lives in
        // the backend, which is the only layer that knows the index stride.
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                             RHI::IndexType::UInt32, cmd->baseIndex);
        ++Data().Tally.Stats.DrawCalls;

        if (startedConditionalRender)
        {
            ++Data().Tally.Stats.ConditionalDraws;
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
        if (s_FrameData.DepthPrepassActive)
        {
            shaderToBind = ResolveDepthPrepassShader(mat);
            prepassDepthOnly = (shaderToBind != mat.shaderRendererID);
            ApplyPrepassViewNormalWrite(api, cmd->renderStateIndex, IsDepthNormalPrepassProgram(shaderToBind));
        }
        else if (s_FrameData.OverdrawActive)
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
        if (Data().Cache.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            Data().Cache.CurrentBoundShader = shaderToBind;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Camera UBO: re-bind in case a prior pass (e.g. ShadowMap)
        // overwrote the binding point.  Mirrors the logic in DrawMesh's color path.
        if (Data().Uploads.CameraUBO)
        {
            BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
        }

        // Material UBO + texture bindings (skipped when material unchanged).
        // A depth-only prepass draw needs material state only for the MASK
        // alpha test (cutoff + albedo); opaque depth-only draws skip it.
        if (!prepassDepthOnly || mat.alphaMode == 1 || s_FrameData.DepthPrepassWritesNormals)
        {
            UploadMaterialState(api, mat, cmd->materialDataIndex);
        }

        // GPU-frustum-cull fast path: the cull compute already wrote
        // compacted survivors to `cullOutputInstanceBufferID` and the
        // surviving count into `cullIndirectBufferID`. Skip the FrameDataBuffer
        // -> InstanceData scratch loop and the upload; bind the pre-populated
        // output buffer at SSBO_INSTANCE_DATA and draw indirect.
        if (const bool useGPUCull = cmd->cullIndirectBufferID.IsValid() && cmd->cullOutputInstanceBufferID.IsValid(); useGPUCull)
        {
            // Rebind slot 15 to the per-submission output buffer. The engine-
            // wide `Data().Uploads.ModelInstanceBuffer` is unchanged so it can be
            // reused by subsequent CPU-path draws in the same frame.
            api.BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_DATA, cmd->cullOutputInstanceBufferID);

            // Shadow/snow textures (per-frame, outside material diffing).
            // Depth-only prepass draws never sample shadows.
            if (!prepassDepthOnly)
                BindShadowTextures(api);

            // Bone matrices (no-op for non-animated GPU-cull submissions)
            if (!UploadBoneMatrices(api, cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCountPerInstance,
                                    cmd->prevBoneBufferOffset))
            {
                return;
            }

            if (cmd->indexCount == 0)
            {
                OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced (GPU cull): No indices to draw");
                return;
            }

            BindVAOIfNeeded(api, cmd->vertexArrayID);
            ++Data().Tally.Stats.DrawCalls;
            // Publish before the indirect draw, same as the two direct paths.
            // UploadMaterialState and BindShadowTextures have staged offsets by
            // here; an indirect draw reads them exactly as a direct one does.
            HeapBinding::FlushOffsets();
            if (cmd->cullRootDataBufferID.IsValid())
            {
                // The cull compute owns SSBO_INSTANCE_DATA's address. Complete
                // the remaining reflected fields from this draw's bindings and
                // select the GPU buffer for exactly the next attempted draw.
                // A layout mismatch (for example a depth-prepass replacement
                // shader) returns false and PrepareDrawCommon conservatively
                // assembles ordinary CPU-written root data instead.
                [[maybe_unused]] const bool selectedGpuRoot = api.SetNextDrawRootData(
                    cmd->cullRootDataBufferID,
                    ShaderBindingLayout::SSBO_INSTANCE_DATA,
                    cmd->cullRootDataAddressOffsetBytes);
            }
            // The VAO is already bound by BindVAOIfNeeded above — draw from it
            // rather than re-binding behind the redundant-bind cache's back.
            api.DrawBoundElementsIndirect(cmd->cullIndirectBufferID, RHI::PrimitiveTopology::TriangleList, 0);

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
        constexpr sizet maxInstances = CommandBucketConfig::kMaxDispatchableMeshInstances;
        sizet instanceCount = static_cast<sizet>(cmd->transformCount);
        if (instanceCount > maxInstances)
        {
            OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Too many instances ({}). Only first {} will be rendered.",
                          instanceCount, maxInstances);
            instanceCount = maxInstances;
        }

        // Replay reads the frame's payloads; it never writes them (issue #1335).
        const FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
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
        const glm::vec4* lightmapRegions = nullptr;
        if (cmd->lightmapRegionBufferOffset != UINT32_MAX)
            lightmapRegions = frameBuffer.GetColorPtr(cmd->lightmapRegionBufferOffset);
        const glm::uvec4* gpuSceneRefs = nullptr;
        if (cmd->gpuSceneRefBufferOffset != UINT32_MAX)
            gpuSceneRefs = frameBuffer.GetGPUSceneRefPtr(cmd->gpuSceneRefBufferOffset);
        const bool gpuSceneMaterialsBound = gpuSceneRefs && BindGPUSceneMaterialsIfNeeded();

        if (transforms && Data().Uploads.ModelInstanceBuffer)
        {
            // Thread-local scratch — heap-backed so MaxMeshInstances can scale
            // to thousands without blowing the stack (16384 * 240 B = 3.75 MB
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
            const glm::vec3 origin = Data().View.RenderOrigin;
            for (sizet i = 0; i < instanceCount; ++i)
            {
                // Start every instance from the defaults. `scratch` outlives this
                // draw, so a field written only under a condition would otherwise
                // keep the previous batch's value: an unlinked batch (a batched
                // Model) kept the previous linked batch's GPU-scene references and
                // the G-Buffer shader shaded it with that entity's material, on
                // Deferred only -- found by the renderer state-machine harness's
                // batch-vs-nobatch pair (#1349).
                InstanceData& inst = scratch[i];
                inst = InstanceData{};
                // CommandBucket replaces a resolved link's transform entries
                // with its GPU Scene record values. Those values are already
                // render-origin-relative, unlike the legacy command values.
                const bool resolvedGPUSceneTransform = gpuSceneRefs && gpuSceneRefs[i].w != GPUSceneDrawRefUnlinked;
                inst.Transform = resolvedGPUSceneTransform ? transforms[i] : MakeModelRelative(transforms[i], origin);
                inst.Normal = glm::transpose(glm::inverse(transforms[i]));
                inst.PrevTransform = resolvedGPUSceneTransform ? prevTransforms[i] : MakeModelRelative(prevTransforms[i], origin);
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
                inst.LightmapScaleOffset = lightmapRegions ? lightmapRegions[i] : glm::vec4(0.0f);
                if (gpuSceneRefs)
                {
                    inst.GPUSceneRef = gpuSceneRefs[i];
                    if (!gpuSceneMaterialsBound)
                    {
                        inst.GPUSceneRef.z = 0u;
                        inst.GPUSceneRef.w = GPUSceneDrawRefUnlinked;
                    }
                }
            }
            const std::span<const InstanceData> instances(scratch.data(), instanceCount);
            Data().Uploads.ModelInstanceBuffer->Upload(instances);
            Data().Uploads.ModelInstanceBuffer->Bind();
        }

        // Shadow/snow textures (per-frame, outside material diffing).
        // Depth-only prepass draws never sample shadows.
        if (!prepassDepthOnly)
            BindShadowTextures(api);

        // Bone matrices
        if (!UploadBoneMatrices(api, cmd->isAnimatedMesh, cmd->boneBufferOffset, cmd->boneCountPerInstance,
                                cmd->prevBoneBufferOffset))
        {
            return;
        }

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
        // is the hardest kind to notice (issue #691).
        //
        // Cheap when nothing changed: FlushOffsets early-outs on a clean table
        // and DescriptorHeap::Flush uploads only the dirty span.
        HeapBinding::FlushOffsets();
        // Bind VAO (cached) and draw instanced
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        ++Data().Tally.Stats.DrawCalls;
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
        if (Data().Cache.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            Data().Cache.CurrentBoundShader = cmd->shaderRendererID;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Re-establish camera UBO binding (may be overwritten by shadow pass)
        if (Data().Uploads.CameraUBO)
        {
            BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
        }

        // Bind skybox cubemap texture using renderer ID directly
        // The offset write must survive a cache hit — see BindTrackedTexture's note.
        if (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] != cmd->skyboxTextureID ||
            HeapBinding::WritesOffsetsForBoundProgram())
        {
            const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_ENVIRONMENT, cmd->skyboxTextureID, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Cube);
            Data().Cache.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] = CacheEntryAfterSeam(staged, cmd->skyboxTextureID);
            ++Data().Tally.Stats.TextureBinds;
        }

        // See the note above the flush in DrawIndexed: every draw in this file
        // publishes, because which handler owes one is a property of its SHADER.
        HeapBinding::FlushOffsets();

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);

        // Update statistics
        ++Data().Tally.Stats.DrawCalls;
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
        if (Data().Cache.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            Data().Cache.CurrentBoundShader = cmd->shaderRendererID;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Update model matrix UBO
        if (Data().Uploads.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = -1;
            modelData.PadEntity[0] = 0;
            modelData.PadEntity[1] = 0;
            modelData.PadEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // static quad: no motion contribution

            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        // Bind texture using renderer ID directly
        // The offset write must survive a cache hit — see BindTrackedTexture's note.
        if (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] != cmd->textureID ||
            HeapBinding::WritesOffsetsForBoundProgram())
        {
            const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_DIFFUSE, cmd->textureID, RHI::HeapSlotLifetime::Persistent);
            Data().Cache.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] = CacheEntryAfterSeam(staged, cmd->textureID);
            ++Data().Tally.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw quad
        BindVAOIfNeeded(api, cmd->quadVAID);
        ++Data().Tally.Stats.DrawCalls;
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
        if (Data().Cache.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            Data().Cache.CurrentBoundShader = cmd->shaderRendererID;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Note: Grid shader reads view/projection from Camera UBO (binding 0)
        // Re-establish the binding (may be overwritten by shadow pass)
        if (Data().Uploads.CameraUBO)
        {
            BindUBOIfNeeded(api, ShaderBindingLayout::UBO_CAMERA, Data().Uploads.CameraUBO->GetRHIHandle());
        }

        // No grid-scale upload: InfiniteGrid.glsl declares no u_GridScale -- its
        // spacing is the c_GridScale constant -- so the old SetProgramUniformFloat
        // here set nothing on GL (glGetUniformLocation answered -1) and hit an
        // unimplemented stub on Vulkan every frame the grid drew (#1556, found
        // by the windowless Vulkan scene fixture's per-tick stub check).
        // cmd->gridScale stays recorded for the day the shader reads one, which
        // will be a UBO member (ADR 0011 amendment (9)), never a named uniform.

        // Bind fullscreen quad VAO (cached) and draw
        BindVAOIfNeeded(api, cmd->quadVAOID);
        HeapBinding::FlushOffsets();
        api.DrawBoundArrays(RHI::PrimitiveTopology::TriangleList, 0, 6);

        ++Data().Tally.Stats.DrawCalls;
    }

    // The ambient ladder's sources for a forward TERRAIN draw (issue #1336).
    // Mesh draws bind the IBL trio per material (BindPBRTextures); terrain and
    // voxel draws bound none, so their forward shaders could only use the flat
    // fill while the deferred pass lit the same terrain with the full ladder.
    // The global maps are what DeferredLightingPass shades with, so both paths
    // now take the same rung. A missing map is simply not bound; the
    // AmbientLadder lane's EnableIBL — from the same availability test —
    // keeps the shader from reading the slot.
    static void BindTerrainAmbientLadder(RendererAPI& api, ShaderBindingLayout::TerrainUBO& ubo)
    {
        ubo.AmbientLadder = DeferredLightingPass::AmbientLadderControls();
        BindTrackedTexture(api, Renderer3D::GetGlobalIrradianceMapHandle(), ShaderBindingLayout::TEX_USER_0,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, Renderer3D::GetGlobalPrefilterMapHandle(), ShaderBindingLayout::TEX_USER_1,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, Renderer3D::GetGlobalBRDFLutMapHandle(), ShaderBindingLayout::TEX_USER_2);
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
        // Terrain runs its whole program in the prepass, which writes the
        // snow-filled view normal of attachment 2 (issue #1452).
        ApplyPrepassViewNormalWrite(api, cmd->renderStateIndex, true);

        // Bind shader
        if (Data().Cache.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            Data().Cache.CurrentBoundShader = cmd->shaderRendererID;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Upload camera UBO (camera-relative packing, issue #429 — origin
        // subtraction happens inside UploadCameraUBO).
        UploadCameraUBO(api);

        // Upload model matrix UBO
        if (Data().Uploads.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData.PadEntity[0] = 0;
            modelData.PadEntity[1] = 0;
            modelData.PadEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // terrain: no motion tracking (Deferred draws Terrain_GBuffer through ScenePass)
            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        // Upload terrain UBO (per-chunk data with tess factors)
        if (auto terrainUBO = Renderer3D::GetTerrainUBO(); terrainUBO)
        {
            ShaderBindingLayout::TerrainUBO uboData = cmd->terrainUBOData;
            BindTerrainAmbientLadder(api, uboData);
            terrainUBO->SetData(&uboData, ShaderBindingLayout::TerrainUBO::GetSize());
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
        // The macro map (issue #1721) rides TEX_DIFFUSE, which the terrain
        // programs declare as u_TerrainMacroMap (ShaderBindingLayout's
        // IsKnownTextureBinding says why that slot). TRACKED, unlike the binds
        // above: the mesh path binds its diffuse through the same per-slot
        // record, and an untracked bind here would leave that record naming a
        // texture the unit no longer holds, so the next mesh draw with it would
        // skip its bind. A terrain with no macro map binds nothing; its UBO
        // flag keeps the shader off the slot.
        BindTrackedTexture(api, cmd->macroMapTextureID, ShaderBindingLayout::TEX_DIFFUSE);

        // Terrain virtual texturing (issue #715). Persistent lifetime, like the
        // splat/layer textures above: both resources are owned by the terrain's
        // TerrainVirtualTexture across frames, not minted per pass.
        //
        // These go through the heap seam for the same reason the layer arrays
        // do — Terrain_PBR/Terrain_GBuffer are on the bindless route, so a plain
        // BindTexture here would be recorded as an offset by nobody and read as
        // black (docs/agent-rules/glsl-shaders.md §5c). The feedback SSBO is a
        // buffer, not a descriptor, so it binds directly.
        if (cmd->virtualTexture.IsComplete())
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_VT_INDIRECTION,
                                             cmd->virtualTexture.indirectionTextureID,
                                             RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_TERRAIN_VT_CACHE,
                                             cmd->virtualTexture.cacheTextureID, RHI::HeapSlotLifetime::Persistent,
                                             {}, RHI::NullSamplerKind::Texture2DArray);
            api.BindStorageBuffer(ShaderBindingLayout::SSBO_TERRAIN_VT,
                                  cmd->virtualTexture.feedbackBufferID);
        }

        // Bind the full shadow contract the terrain shaders sample — CSM, spot,
        // the PCSS comparison-OFF raw-depth views, the point-light cubemaps, and
        // the snow depth map. Sharing BindShadowTextures keeps terrain in lockstep
        // with the mesh path; the point cubemaps were previously omitted here, so
        // terrain point shadows relied on an earlier mesh draw having bound units
        // 14-17 in the same frame.
        BindShadowTextures(api);

        // Bind VAO (cached) and draw with GL_PATCHES
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        api.SetPatchVertexCount(cmd->patchVertexCount);

        // GPU-driven LOD (issue #714): one instanced indirect draw over the
        // shared unit patch grid, its instance count written by
        // TerrainCullArgs.comp. The visible-node list has to be bound BEFORE the
        // draw because the vertex stage indexes it by gl_InstanceIndex.
        if (cmd->terrainIndirectArgsID.IsValid() && cmd->terrainVisibleNodesID.IsValid())
        {
            api.BindStorageBuffer(ShaderBindingLayout::SSBO_TERRAIN_VISIBLE_NODES, cmd->terrainVisibleNodesID);
            HeapBinding::FlushOffsets();
            api.DrawBoundElementsIndirect(cmd->terrainIndirectArgsID, RHI::PrimitiveTopology::PatchList, 0);
            ++Data().Tally.Stats.DrawCalls;
            return;
        }

        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::PatchList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++Data().Tally.Stats.DrawCalls;
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
        // Voxel terrain runs its whole program in the prepass, which writes the
        // view normal of attachment 2 (issue #1452).
        ApplyPrepassViewNormalWrite(api, cmd->renderStateIndex, true);

        // Bind shader
        if (Data().Cache.CurrentBoundShader != cmd->shaderRendererID)
        {
            api.BindShaderProgram(cmd->shaderRendererID);
            Data().Cache.CurrentBoundShader = cmd->shaderRendererID;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Upload camera UBO (camera-relative packing, issue #429 — origin
        // subtraction happens inside UploadCameraUBO).
        UploadCameraUBO(api);

        // Upload model matrix UBO
        if (Data().Uploads.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData;
            modelData.Model = cmd->transform;
            modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
            modelData.EntityID = cmd->entityID;
            modelData.PadEntity[0] = 0;
            modelData.PadEntity[1] = 0;
            modelData.PadEntity[2] = 0;
            modelData.PrevModel = cmd->transform; // voxel: no motion tracking (Deferred draws the voxel G-Buffer variant through ScenePass)
            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        // The terrain UBO the voxel shaders read (issue #1336). This draw used
        // to upload none, so u_TerrainParams.w and u_LayerTilingScales0/1 were
        // whatever the last heightmap chunk left in the buffer — zeros on a
        // voxel-only terrain — and the ambient ladder lane would have been too.
        if (auto terrainUBO = Renderer3D::GetTerrainUBO(); terrainUBO)
        {
            ShaderBindingLayout::TerrainUBO uboData{};
            uboData.TerrainParams.w = cmd->surface.TriplanarSharpness;
            uboData.LayerTilingScales0 = cmd->surface.LayerTilingScales0;
            uboData.LayerTilingScales1 = cmd->surface.LayerTilingScales1;
            // The tile size the voxel shaders turn repeats-across-the-tile into
            // repeats per metre with, per axis. No height scale: a voxel
            // surface has none.
            uboData.WorldSizeAndHeightScale = glm::vec4(cmd->surface.TileWorldSize, 0.0f, 0.0f);
            BindTerrainAmbientLadder(api, uboData);
            terrainUBO->SetData(&uboData, ShaderBindingLayout::TerrainUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_TERRAIN, terrainUBO->GetRHIHandle());
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
        BindShadowTextures(api);

        // Bind VAO (cached) and draw
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        if (cmd->instanceCount > 0)
        {
            // Packed-quad path (issue #727): indexCount is the shared unit
            // quad's 6, and each instance is one merged greedy quad.
            api.DrawBoundIndexedInstanced(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                                          RHI::IndexType::UInt32, 0, cmd->instanceCount);

            // Same shape as DrawMeshInstanced / DrawFoliageLayer: one draw call
            // standing for many instances. Without these the profiler's
            // "Instanced Draws" tab and its triangle/vertex totals cannot show
            // the geometry reduction that is this path's whole point.
            auto& profiler = RendererProfiler::GetInstance();
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancedDrawCalls, 1);
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancesRendered, cmd->instanceCount);
            if (cmd->instanceCount > 1)
                profiler.IncrementCounter(RendererProfiler::MetricType::InstancesBatched, cmd->instanceCount - 1);
            profiler.IncrementCounter(RendererProfiler::MetricType::TrianglesRendered,
                                      (cmd->indexCount / 3u) * cmd->instanceCount);
            profiler.IncrementCounter(RendererProfiler::MetricType::VerticesRendered,
                                      cmd->indexCount * cmd->instanceCount);
        }
        else
        {
            api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        }
        ++Data().Tally.Stats.DrawCalls;
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

        // Resolve and apply render state from table.
        //
        // The packet's state is the FORWARD decal's blend, and applying it sets
        // one global blend function over every attachment. On the OIT variant
        // that erased the per-attachment accumulate / multiply pair the pass had
        // stated: the accumulation overflowed, the revealage stayed at 1, and
        // the decal never reached the composite (#1417). So the OIT pair is
        // re-stated EVERY time the packet's state is applied, here and after
        // the receiver diagnostic below.
        const auto applyPacketState = [&]()
        {
            ApplyPODRenderState(cmd->renderStateIndex, api);
            if (cmd->oitProgramOverride.IsValid())
                ApplyWeightedBlendedOITBlend(api);
        };
        applyPacketState();

        // Bind shader (cached). DecalRenderPass may have installed an OIT
        // override on the packet itself (oitProgramOverride) -- substitute
        // the Decal_OIT program when set so forward decal commands target the
        // graph-owned OIT MRT layout without requiring resubmission of the
        // bucket. Reading the override from the command keeps the queue
        // stateless and replay-safe.
        const RHI::ResourceHandle decalProgram = cmd->oitProgramOverride.IsValid()
                                                     ? cmd->oitProgramOverride
                                                     : cmd->shaderRendererID;
        if (Data().Cache.CurrentBoundShader != decalProgram)
        {
            api.BindShaderProgram(decalProgram);
            Data().Cache.CurrentBoundShader = decalProgram;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Upload model UBO
        if (Data().Uploads.ModelInstanceBuffer)
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
            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        const auto uploadDecalData = [&](const glm::vec4& params)
        {
            auto decalUBO = Renderer3D::GetDecalUBO();
            if (!decalUBO)
                return;
            ShaderBindingLayout::DecalUBO decalData{};
            decalData.InverseDecalTransform = cmd->inverseDecalTransform;
            decalData.InverseViewProjection = cmd->inverseViewProjection;
            decalData.DecalColor = cmd->decalColor;
            decalData.DecalParams = params;
            decalUBO->SetData(&decalData, ShaderBindingLayout::DecalUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_DECAL, decalUBO->GetRHIHandle());
        };

        // Bind albedo texture (with redundancy check)
        if (cmd->albedoTextureID.IsValid())
        {
            // The offset write must survive a cache hit — see BindTrackedTexture's
            // note. The three decal slots were the last redundancy checks still
            // missing this: a second decal with the same texture skipped the call
            // entirely, leaving whatever another pass had staged in TEX_USER_0..2.
            if (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_0] != cmd->albedoTextureID ||
                HeapBinding::WritesOffsetsForBoundProgram())
            {
                const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_0, cmd->albedoTextureID, RHI::HeapSlotLifetime::Persistent);
                Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_0] = CacheEntryAfterSeam(staged, cmd->albedoTextureID);
                ++Data().Tally.Stats.TextureBinds;
            }
        }

        // Bind optional decal normal + RMA textures (used by Decal_GBuffer_Normal
        // and Decal_GBuffer_RMA variants). Unused modes pass 0 and the slot is
        // left alone — the variant shader only samples the slot it needs.
        if (cmd->normalTextureID.IsValid() &&
            (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_1] != cmd->normalTextureID ||
             HeapBinding::WritesOffsetsForBoundProgram()))
        {
            const RHI::HeapOffset stagedNormal = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_1, cmd->normalTextureID, RHI::HeapSlotLifetime::Persistent);
            Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_1] = CacheEntryAfterSeam(stagedNormal, cmd->normalTextureID);
            ++Data().Tally.Stats.TextureBinds;
        }
        if (cmd->rmaTextureID.IsValid() &&
            (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_2] != cmd->rmaTextureID ||
             HeapBinding::WritesOffsetsForBoundProgram()))
        {
            const RHI::HeapOffset stagedRma = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_USER_2, cmd->rmaTextureID, RHI::HeapSlotLifetime::Persistent);
            Data().Cache.BoundTextures[ShaderBindingLayout::TEX_USER_2] = CacheEntryAfterSeam(stagedRma, cmd->rmaTextureID);
            ++Data().Tally.Stats.TextureBinds;
        }

        // Bind VAO (cached) and draw decal cube
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        HeapBinding::FlushOffsets();

        // When olo_render_why_not_visible has armed this entity, issue one
        // colour-write-free diagnostic draw. DecalParams.z makes every decal
        // shader return immediately after the projection-volume test, before
        // texture sampling or alpha/fade discards. Its query therefore answers
        // the separate question "does scene depth reconstruct inside the box?"
        // without changing the rendered frame.
        const bool receiverQuery = Renderer3D::BeginDecalReceiverIntersectionQuery(cmd->entityID);
        if (receiverQuery)
        {
            glm::vec4 diagnosticParams = cmd->decalParams;
            diagnosticParams.z = 1.0f;
            uploadDecalData(diagnosticParams);
            api.SetColorMask(false, false, false, false);
            api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);
            Renderer3D::EndDecalReceiverIntersectionQuery();
            InvalidateRenderStateCache();
            applyPacketState();
        }

        uploadDecalData(cmd->decalParams);
        const bool visibilityQuery = Renderer3D::BeginDecalVisibilityQuery(cmd->entityID);
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        if (visibilityQuery)
            Renderer3D::EndDecalVisibilityQuery();
        ++Data().Tally.Stats.DrawCalls;
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

        // Bind shader (cached). TWO prepasses replay foliage, and they differ.
        //
        // DEFERRED: no swap. The Geometry bucket is replayed with
        // DepthPrepassActive, so a G-Buffer foliage program runs its FULL
        // fragment stage in the prepass (colour writes masked) and again in the
        // colour pass. Measured on courtyard.diagnostic when the impostor card
        // joined that bucket (#1225): ScenePass/DepthPrepass +0.085 ms median,
        // N=5, Debug, RTX 4090, ~720 cards.
        //
        // FORWARD with screen-space AO live (issue #1474): FoliagePrepassPass
        // replays the foliage bucket ahead of the AO passes, and the forward
        // program swaps for its Foliage_*_DepthNormal twin, which writes the
        // view normal of scene attachment 2 and nothing else. The twins
        // reproduce the colour program's coverage exactly (the same vertex
        // stage with `invariant gl_Position`, the same discards), or the
        // LEqual colour pass would lose edge fragments.
        const RHI::ResourceHandle shaderToBind = ResolveFoliageDepthNormalPrepassShader(cmd->shaderRendererID);
        if (!shaderToBind.IsValid())
            return; // no twin: this layer stays out of the forward prepass (see the resolver)
        ApplyPrepassViewNormalWrite(api, cmd->renderStateIndex, IsDepthNormalPrepassProgram(shaderToBind));
        if (Data().Cache.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            Data().Cache.CurrentBoundShader = shaderToBind;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Upload model UBO (parent terrain transform)
        if (Data().Uploads.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->modelTransform;
            modelData.Normal = cmd->normalMatrix;
            modelData.EntityID = cmd->entityID;
            modelData.PrevModel = cmd->modelTransform; // foliage: no per-instance prev history — alias current for zero motion
            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        // Upload foliage UBO (per-layer parameters)
        if (auto foliageUBO = Renderer3D::GetFoliageUBO(); foliageUBO)
        {
            ShaderBindingLayout::FoliageUBO foliageData{};
            foliageData.Time = cmd->time;
            foliageData.WindStrength = cmd->windStrength;
            foliageData.WindSpeed = cmd->windSpeed;
            foliageData.WindWeights = cmd->windWeights;
            const auto wind = WindSystem::GetGPUData();
            foliageData.WindDirection = wind.DirectionAndSpeed;
            foliageData.WindGust = wind.GustAndTurbulence;
            foliageData.PrevMeshViewPos = glm::vec4(MakePositionRelative(Renderer3D::GetPreviousViewPosition(), Renderer3D::GetRenderOrigin()), 0.0f);
            foliageData.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), wind.TimeAndFlags.y);
            foliageData.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
            foliageData.ViewDistance = cmd->viewDistance;
            foliageData.FadeStart = cmd->fadeStart;
            foliageData.AlphaCutoff = cmd->alphaCutoff;
            foliageData.PrevTime = cmd->prevTime;
            // The dither's frame rides BaseColor.w (#1533): it advances only while
            // a temporal resolve accumulates the foliage dither, and the main-view
            // programs fold it into their keep/discard pattern, so a half-faded
            // plant averages to its fade instead of standing as a screen door.
            // See OLO_FOLIAGE_DITHER_FRAME in include/FoliageInstanceGeometry.glsl.
            const bool temporalResolve = Renderer3D::IsEngineTAAWanted() || Renderer3D::IsTemporalUpscaleActive();
            const f32 ditherFrame =
                temporalResolve ? static_cast<f32>(Renderer3D::GetStochasticFrameIndex() & 63u) : 0.0f;
            foliageData.BaseColor = glm::vec4(glm::vec3(cmd->baseColor), ditherFrame);
            // Octahedral impostor params (issue #433) — zero on the billboard path.
            foliageData.ImpostorParams0 = glm::vec4(cmd->impostorFramesPerAxis, cmd->impostorHemi, cmd->impostorStartDistance, cmd->impostorBand);
            foliageData.ImpostorParams1 = glm::vec4(cmd->impostorEnabled, cmd->impostorRadius, cmd->impostorParallaxScale, 0.0f);
            // Authored plant mesh near field (issue #1233). The view position
            // rides the foliage UBO rather than being read from the camera one,
            // because the shadow pass's camera is the LIGHT — see
            // ShaderBindingLayout::FoliageUBO::MeshViewPos. Same expression the
            // camera UBO uses for its own Position, so the two agree exactly.
            foliageData.MeshParams =
                glm::vec4(cmd->isAuthoredMesh, cmd->meshHandoverStart, cmd->meshHandoverEnd, cmd->cardNormalLane);
            foliageData.MeshViewPos =
                glm::vec4(MakePositionRelative(Data().View.ViewPosition, Data().View.RenderOrigin), 0.0f);
            // LOD transition + coverage-preserving density (issue #1237).
            // Carried on the command rather than read here, unlike the wind and
            // interaction snapshots above: these are per-LAYER authored numbers,
            // not a global per-frame set, so the layer the command names is the
            // only thing that can supply them.
            foliageData.LodTransition0 = cmd->lodTransition0;
            foliageData.LodTransition1 = cmd->lodTransition1;

            // The interaction field (issue #1238), read HERE rather than
            // carried on the command, exactly as the wind snapshot above is.
            // The set is global per frame, so a command recorded before the
            // field ticked would otherwise bend this layer against last frame's
            // influences while the shadow pass used this frame's.
            ApplyFoliageInteraction(foliageData, cmd->interactionResponse);

            // ── Leaf material (issue #1234) ─────────────────────────────────
            // The map bitfield is derived from the HANDLES, not from the
            // authored paths: a layer that names a normal map the loader could
            // not open arrives here with a null handle, and the bit stays
            // clear, so the shader samples the authored constant instead of the
            // engine's typed-null black. A path that says "normal map" and a
            // sampler that reads black is how a leaf ends up shaded by a normal
            // of (-1,-1,-1) with nothing in the log.
            f32 leafMapFlags = 0.0f;
            if (cmd->leafNormalTextureID.IsValid())
                leafMapFlags += 1.0f; // OLO_LEAF_MAP_NORMAL
            if (cmd->leafRoughnessTextureID.IsValid())
                leafMapFlags += 2.0f; // OLO_LEAF_MAP_ROUGHNESS
            if (cmd->leafThicknessTextureID.IsValid())
                leafMapFlags += 4.0f; // OLO_LEAF_MAP_THICKNESS
            foliageData.LeafSurface = glm::vec4(cmd->leafRoughness, cmd->leafNormalStrength,
                                                cmd->leafThickness, leafMapFlags);
            // Tint PRE-MULTIPLIED by strength, exactly as
            // FoliageLeafProfileTintLane packs it for the deferred table — so
            // the forward path and the deferred path multiply in the same order
            // and cannot differ by a rounding of the product.
            foliageData.LeafTransmit = glm::vec4(cmd->leafTransmissionColor * cmd->leafTransmissionStrength,
                                                 cmd->leafTransmissionStrength);
            foliageData.LeafLobe = glm::vec4(cmd->leafTransmissionDistortion, cmd->leafTransmissionPower,
                                             cmd->leafTransmissionWrap, cmd->leafTransmissionAmbient);
            // .y says whether the global IBL trio is actually bound, .z is its
            // intensity, .w the probe-volume switch. Foliage carries no
            // Material, so these cannot ride a material UBO the way every mesh's
            // do — and a scene with no EnvironmentMap binds nothing, which the
            // shader must be able to tell from "bound and black". They are the
            // deferred pass's own ladder controls (issue #1336), so forward
            // foliage takes the same rung.
            const glm::vec4 ladderControls = DeferredLightingPass::AmbientLadderControls();
            foliageData.LeafIds = glm::vec4(static_cast<f32>(cmd->leafProfileSlot), ladderControls.x, ladderControls.z,
                                            ladderControls.y);

            foliageUBO->SetData(&foliageData, ShaderBindingLayout::FoliageUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_FOLIAGE, foliageUBO->GetRHIHandle());
        }

        // Bind albedo texture (with redundancy check). On the impostor path this
        // is the octahedral albedo atlas.
        if (cmd->albedoTextureID.IsValid())
        {
            // The offset write must survive a cache hit — see BindTrackedTexture's note.
            if (Data().Cache.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] != cmd->albedoTextureID ||
                HeapBinding::WritesOffsetsForBoundProgram())
            {
                const RHI::HeapOffset staged = HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_DIFFUSE, cmd->albedoTextureID, RHI::HeapSlotLifetime::Persistent);
                Data().Cache.BoundTextures[ShaderBindingLayout::TEX_DIFFUSE] = CacheEntryAfterSeam(staged, cmd->albedoTextureID);
                ++Data().Tally.Stats.TextureBinds;
            }
        }

        // Bind the octahedral normal+depth atlas (impostor path only).
        // BindTrackedTextureUnit skips a 0 id and does the redundancy check.
        //
        // MOVED OFF TEX_USER_0 by issue #1234. TEX_USER_0/1/2 are the engine's
        // IBL trio (irradiance / prefilter / BRDF LUT) everywhere else, and the
        // foliage shaders now need them for environment lighting — so the
        // impostor atlas on slot 10 would have been a WITHIN-SHADER collision in
        // Foliage_Impostor.glsl, which is the one rule that actually has to
        // hold (ShaderBindingLayout, namespace note A2). TEX_SPECULAR is unused
        // by every foliage program.
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_SPECULAR, cmd->impostorNormalDepthTextureID);

        // The leaf maps (issue #1234) on their semantic slots, plus TEX_METALLIC
        // repurposed for thickness — foliage is never metallic, and the engine
        // already repurposes a semantic slot per shader this way (see
        // PBR_MultiLight's u_MetallicRoughnessMap on TEX_SPECULAR). A null
        // handle is skipped by BindTrackedTextureUnit, and the map bitfield
        // uploaded above is what stops the shader sampling the slot anyway.
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_NORMAL, cmd->leafNormalTextureID);
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_ROUGHNESS, cmd->leafRoughnessTextureID);
        BindTrackedTextureUnit(api, ShaderBindingLayout::TEX_METALLIC, cmd->leafThicknessTextureID);

        // The full shadow contract, for the same reason the terrain draw binds
        // it (issue #1234): the forward foliage program samples the CSM and the
        // local-light atlas now, and a pass that relied on an earlier mesh draw
        // having bound units 8/13/33/34 in the same frame would light a canopy
        // correctly only when something else drew first. On the DEFERRED route
        // the foliage program writes the G-Buffer and samples none of these —
        // the binds are redundant there and BindTrackedTextureUnit's cache
        // makes them free after the first draw.
        BindShadowTextures(api);

        // The scene's global IBL, bound EXPLICITLY rather than inherited.
        // BindPBRTextures publishes the same trio on the same slots from a
        // material, so in a frame with lit meshes these would usually already be
        // right — "usually" being the problem. Reading them from
        // Renderer3D's global IBL state makes a canopy's ambient and its
        // transmission's environment half independent of draw order.
        // TEX_USER_0/1 are samplerCube here, so the typed null kind matters.
        BindTrackedTexture(api, Renderer3D::GetGlobalIrradianceMapHandle(), ShaderBindingLayout::TEX_USER_0,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, Renderer3D::GetGlobalPrefilterMapHandle(), ShaderBindingLayout::TEX_USER_1,
                           RHI::NullSamplerKind::Cube);
        BindTrackedTexture(api, Renderer3D::GetGlobalBRDFLutMapHandle(), ShaderBindingLayout::TEX_USER_2);

        // Bind VAO (cached) and draw instanced foliage
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        HeapBinding::FlushOffsets();
        if (cmd->indirectBufferID.IsValid())
        {
            // The cull compute wrote count / firstIndex / instanceCount; nothing
            // here re-derives them. `baseIndex` and `instanceCount` on the
            // command are the CPU's copy of the first two and the GENERATED
            // count, kept for the profiler and the fallback below — reading them
            // here instead would draw the wrong number of the right plants.
            api.DrawBoundElementsIndirect(cmd->indirectBufferID, RHI::PrimitiveTopology::TriangleList,
                                          cmd->indirectOffsetBytes);
        }
        else
        {
            api.DrawBoundIndexedInstanced(RHI::PrimitiveTopology::TriangleList, cmd->indexCount,
                                          RHI::IndexType::UInt32, cmd->baseIndex, cmd->instanceCount);
        }
        ++Data().Tally.Stats.DrawCalls;
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
        // (the depth-prepass shader-swap shape, #691).
        RHI::ResourceHandle shaderToBind = cmd->shaderRendererID;
        if (s_FrameData.WaterDepthCaptureActive && s_FrameData.WaterDepthShaderID.IsValid())
        {
            shaderToBind = s_FrameData.WaterDepthShaderID;
        }
        if (Data().Cache.CurrentBoundShader != shaderToBind)
        {
            api.BindShaderProgram(shaderToBind);
            Data().Cache.CurrentBoundShader = shaderToBind;
            ++Data().Tally.Stats.ShaderBinds;
        }

        // Upload model UBO
        if (Data().Uploads.ModelInstanceBuffer)
        {
            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = cmd->modelTransform;
            modelData.Normal = cmd->normalMatrix;
            modelData.EntityID = cmd->entityID;
            modelData.PrevModel = cmd->modelTransform; // water: surface is animated in-shader; mesh transform stable — alias for zero rigid motion
            UploadModelInstance(modelData, Data().Uploads.ModelInstanceBuffer);
        }

        // Upload water UBO
        if (auto waterUBO = Renderer3D::GetWaterUBO(); waterUBO)
        {
            // The water pass's own target (SetWaterTargetSize). It used to be a
            // viewport tracked from SetViewport packets, which nothing submits,
            // so ScreenParams was zero on every backend (issue #1486).
            const f32 viewportWidth = static_cast<f32>(s_FrameData.WaterTargetWidth);
            const f32 viewportHeight = static_cast<f32>(s_FrameData.WaterTargetHeight);

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
            waterData.FFTCascadeParams = cmd->fftCascadeParams;
            // Projected grid (issue #1035). Carried per-surface, unlike the
            // wake / shore / rain fields below: it is a property of THIS
            // surface's own plane and rect, and two water tiles at different
            // heights legitimately get different bands.
            waterData.ProjectedGridParams = cmd->projectedGridParams;
            waterData.ProjectedGridParams2 = cmd->projectedGridParams2;
            // Boat / actor wake foam field (issue #967). Read from the service
            // here rather than carried on the draw command, because the field is
            // ONE global resource and not a per-surface property: routing it
            // through DrawWaterCommand would let two water tiles in the same
            // frame disagree about where the field is, which would show as the
            // wake jumping between them.
            //
            // Reports the disabled state (w == 0) whenever the field is
            // unusable — not initialised, disabled, or never yet written — so
            // filling this unconditionally cannot put a stale field on screen.
            waterData.WakeFieldParams = WaterDisturbanceSystem::GetShaderParams();
            waterData.WakeFieldParams2 = WaterDisturbanceSystem::GetShaderParams2();
            // Boat / actor wake SHAPE (issue #968). Same reasoning as the foam
            // field above — one global record read from the service here rather
            // than carried per-surface, so two water tiles cannot disagree about
            // where the boat is. GetRenderHeightScale() reports 0 for every
            // reason the wake could be unusable (feature off, no hulls this
            // frame), which is why this is packed unconditionally: there is no
            // path that leaves last frame's hull pressed into the sea.
            waterData.WakeShapeParams =
                glm::vec4(static_cast<f32>(WaterWakeSystem::GetHullCount()),
                          WaterWakeSystem::GetRenderHeightScale(),
                          WaterWakeSystem::GetSettings().m_FlattenStrength, 0.0f);
            // Shore wave deformation (issue #1033). Same "one global service,
            // read here rather than carried per-surface" rule as the two wake
            // blocks above, and for the same reason: the sea floor is a
            // property of the SCENE, not of a water tile, and two tiles that
            // disagreed about it would shoal against different coastlines.
            //
            // GetShaderParams() reports the disabled state (w == 0) for every
            // reason the field could be unusable, so packing it unconditionally
            // cannot put a stale or half-baked seabed on screen.
            waterData.ShoreParams = WaterShoreDepthSystem::GetShaderParams();
            waterData.ShoreParams2 = WaterShoreDepthSystem::GetShaderParams2();
            // Rain-impact ripples (issue #1034). Same rule again, and here it
            // is the least arguable of the four: whether it is RAINING is a
            // property of the sky, not of a water tile, and two tiles that
            // disagreed about it would put a downpour on one and a dry calm on
            // the other in the same frame.
            //
            // GetShaderParams() reports x == 0 for every reason the ripples
            // could be unusable — the tile has them off, it is not raining, or
            // what is falling is snow — so packing it unconditionally cannot
            // leave a stale stipple on a sea the weather has since dried.
            waterData.RainRippleParams = WaterRainRippleSystem::GetShaderParams();
            waterData.RainRippleParams2 = WaterRainRippleSystem::GetShaderParams2();
            // Advected open-ocean foam (issue #1034). The .g channel of the SAME
            // texture WakeFieldParams above describes, so it needs no binding of
            // its own — but it does need its own window params, because the two
            // features gate independently and WakeFieldParams reports all-zero
            // on a sea with no boat in it.
            waterData.FoamFieldParams = WaterDisturbanceSystem::GetFoamShaderParams();
            std::memcpy(waterData.WakeHulls, WaterWakeSystem::GetHullData(),
                        sizeof(waterData.WakeHulls));
            waterUBO->SetData(&waterData, ShaderBindingLayout::WaterUBO::GetSize());
            api.BindUniformBuffer(ShaderBindingLayout::UBO_WATER, waterUBO->GetRHIHandle());
        }

        // Bind normal map and noise textures (tracked for redundancy elimination and stats)
        BindTrackedTexture(api, cmd->normalMap0ID, ShaderBindingLayout::TEX_WATER_NORMAL_0);
        BindTrackedTexture(api, cmd->normalMap1ID, ShaderBindingLayout::TEX_WATER_NORMAL_1);
        BindTrackedTexture(api, cmd->noiseTextureID, ShaderBindingLayout::TEX_WATER_NOISE);
        BindTrackedTexture(api, cmd->foamTextureID, ShaderBindingLayout::TEX_WATER_FOAM);
        // FFT ocean cascade textures (sampled when u_FFTParams.x > 0.5). 2D
        // ARRAYS since issue #969 — one layer per band — so the null-sampler
        // kind must be the array one: the fallback descriptor a heap miss lands
        // on has to have the type the shader declares, or the sampler reads a
        // sampler2D through a sampler2DArray handle.
        BindTrackedTexture(api, cmd->fftDisplacementID, ShaderBindingLayout::TEX_WATER_FFT_DISPLACEMENT,
                           RHI::NullSamplerKind::Texture2DArray);
        BindTrackedTexture(api, cmd->fftDerivativesID, ShaderBindingLayout::TEX_WATER_FFT_DERIVATIVES,
                           RHI::NullSamplerKind::Texture2DArray);
        // Seabed depth field (issue #1033). Published rather than plain-bound
        // for the reason WaterDisturbanceSystem::BindFieldTexture records: the
        // water shader exists in a slot-based and a bindless variant and which
        // one is in flight is not knowable here, so the seam does both. A frame
        // with no baked field publishes nothing and the shader's own
        // `u_ShoreParams.w <= 0` test keeps it from sampling.
        WaterShoreDepthSystem::PublishFieldTexture();

        // Bind the global environment cubemap for water reflections (binding 9).
        // The water pass doesn't otherwise touch this slot, so set it explicitly
        // instead of relying on a prior pass having left the skybox bound there —
        // without this, grazing-angle water reflects an unbound (black/grey)
        // cubemap and looks see-through rather than reflective. When there's no
        // environment map, deterministically clear the slot rather than leaving a
        // stale cubemap from a previous frame/scene (BindTrackedTexture skips id 0,
        // so clear it directly and update the tracking).
        //
        // Unconditionally: this used to clear only when the cache said the slot
        // held something, which made the cache decide an action and not a skip.
        // The cache is empty at every replay entry and cannot say what an
        // earlier replay left there (a skybox drawn with image lighting off
        // leaves its cubemap on this unit).
        if (const auto envMap = Renderer3D::GetGlobalEnvironmentMapHandle(); envMap.IsValid())
        {
            BindTrackedTexture(api, envMap, ShaderBindingLayout::TEX_ENVIRONMENT);
        }
        else
        {
            HeapBinding::BindTextureOrOffset(api, ShaderBindingLayout::TEX_ENVIRONMENT, RHI::NullResource, RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Cube);
            Data().Cache.BoundTextures[ShaderBindingLayout::TEX_ENVIRONMENT] = {};
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
        BindVAOIfNeeded(api, cmd->vertexArrayID);
        api.SetPatchVertexCount(3);
        HeapBinding::FlushOffsets();
        api.DrawBoundIndexed(RHI::PrimitiveTopology::PatchList, cmd->indexCount, RHI::IndexType::UInt32, 0);
        ++Data().Tally.Stats.DrawCalls;
    }
} // namespace OloEngine
