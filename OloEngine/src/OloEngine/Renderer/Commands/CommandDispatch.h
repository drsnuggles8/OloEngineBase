#pragma once

#include "RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <array>

namespace OloEngine
{
    class TiledForwardPlus;
    class UniformBuffer;
    class InstanceBuffer;
    class ShaderResourceRegistry;

    // The view one recording context draws from (ADR 0025): the camera, its
    // previous frame, and the render origin. A value, set and swapped whole, so
    // the dispatcher never holds half of one camera.
    struct DispatchView
    {
        // World space. The camera block's packing derives the render-relative
        // copies from these plus RenderOrigin.
        glm::mat4 ViewMatrix = glm::mat4(1.0f);
        glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
        glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
        // The same camera one frame ago, for the velocity writers. A view with
        // no history sets it to ViewProjectionMatrix.
        glm::mat4 PrevViewProjectionMatrix = glm::mat4(1.0f);
        // The TAA jitter the two view-projections carry (#1552): xy this
        // frame's, zw the previous frame's, in velocity units.
        glm::vec4 JitterUV = glm::vec4(0.0f);
        glm::vec3 ViewPosition = glm::vec3(0.0f);
        // Camera-relative render origin for this frame (issue #429).
        glm::vec3 RenderOrigin = glm::vec3(0.0f);
        // Whether this is the view the frame's screen-space buffers were made
        // for. The forward shaders' AO lane and the lighting tap (issues #1452,
        // #1526) are measured in the main view's screen space; another camera
        // uploads them as "not live" and binds the white placeholders.
        bool MainViewLanes = true;
    };

    class CommandDispatch
    {
      public:
        struct Statistics
        {
            u32 ShaderBinds = 0;
            u32 TextureBinds = 0;
            u32 DrawCalls = 0;
            u32 ConditionalDraws = 0; // Draws issued inside conditional render blocks

            void Reset()
            {
                ShaderBinds = 0;
                TextureBinds = 0;
                DrawCalls = 0;
                ConditionalDraws = 0;
            }
        };

        static void Initialize();
        static void Shutdown();

        static CommandDispatchFn GetDispatchFunction(CommandType type);

        // State tracking for current frame rendering
        static void ResetState();
        static void InvalidateRenderStateCache();
        // A pass-wide ceiling on the colour attachments a draw may write, one bit
        // per attachment, ANDed into every draw's colorAttachmentWriteMask until
        // it is set back to 0xFF (and at ResetState). For a pass that keeps every
        // attachment of its target in scope but must not write some of them:
        // ForwardOverlayPass (#1533).
        static void SetPassAttachmentWriteLimit(u8 attachmentMask);

        // Forget every "this is already bound" cache (shader, vertex array,
        // textures, UBOs, render state, material, GPU Scene table) without
        // touching the frame's counters or pass flags, so the next dispatch
        // re-binds everything it uses.
        //
        // For a command-bucket pass that runs after passes which bind GL state
        // directly. DeferredLightingPass unbinds the vertex array behind the
        // cache's back, so without this the first ForwardOverlayPass draw of a
        // mesh the G-Buffer pass drew last skips its VAO bind and draws from
        // VAO 0: GL_INVALID_OPERATION, or an access violation in the driver
        // (found during issue #1404).
        static void InvalidateBindingCaches();

        // What the command queue calls at the start of every replay
        // (CommandBucket::BeginReplay): the same reset, in the calling
        // recording context. The caches therefore describe one replay, and a
        // pass that replays twice with other work in between needs no call of
        // its own. InvalidateBindingCaches stays for the code that dispatches
        // without a replay (a pass executing packets itself, a direct draw).
        static void BeginReplay();

        // The same reset, unconditionally: it ignores the
        // FaultSkipDispatchBindingReset negative control. For the harness arm
        // that must really forget the caches while the fault is injected
        // (binding-cache-cold); engine code calls InvalidateBindingCaches.
        static void ForgetBindingCaches();

        // Apply a command's PODRenderState (skipped when the index matches the
        // last one applied -- InvalidateRenderStateCache() is how a pass that
        // touched state behind the queue's back forces a re-apply).
        //
        // Public because it is the half of every dispatch function that a
        // per-draw render-state contract has to be tested through: issue #853's
        // defect lives HERE, in the global SetColorMask this issues, and the
        // decal tenant that was supposed to catch it substituted its own
        // dispatch function that never called it. A per-draw contract verified
        // through a dispatch function the production path does not use is
        // verifying the setup, not the draw.
        static void ApplyPODRenderState(u16 renderStateIndex, RendererAPI& api);

        // Clear any cached texture-slot binding that points at this OpenGL texture ID.
        // Must be called when a Texture2D is destroyed so that a future call to
        // BindTrackedTexture with a recycled GL ID is not incorrectly skipped.
        static void InvalidateTextureBinding(RHI::ResourceHandle texture);

        // Tell the redundant-bind cache that `slot` was clobbered by a raw glBindTextureUnit
        // outside this dispatcher, so the next tracked bind for that slot actually happens.
        static void InvalidateTextureSlot(u32 slot);
        // `writeViewNormals` (issue #1452): the FORWARD prepass also writes the
        // view normal of scene attachment 2, so the screen-space AO passes can
        // run between it and forward colour. The forward PBR programs swap to
        // DepthNormalPrepass*.glsl, terrain and voxel keep their own program,
        // and every other attachment stays masked. Deferred never sets it: its
        // attachment 2 is the G-Buffer's emissive/flags RT.
        static void SetDepthPrepassActive(bool active, bool writeViewNormals = false);

        // The frame's shadow inputs — the CSM array, the local-light atlas,
        // their comparison-off raw views for the PCSS blocker search, and the
        // Virtual Shadow Map's sampling publish — for a pass that draws OUTSIDE
        // the command queue (issue #1323).
        //
        // EXPOSED RATHER THAN RE-IMPLEMENTED. The four units carry a specific
        // sampler state and a specific typed null kind, and every site that
        // stages a shadow-map offset has to agree about both or whichever pass
        // ran last silently wins (issue #691). GroomRenderPass is the first
        // consumer that is a render-graph node rather than a queued draw, and a
        // second copy of these binds is exactly the drift this comment is about.
        static void BindSceneShadowTextures();
        static void SetDepthPrepassColorPassActive(bool active);
        [[nodiscard]] static bool IsDepthPrepassActive();
        [[nodiscard]] static bool DoesDepthPrepassWriteNormals();
        // Overdraw debug view (#519): when active, ApplyPODRenderState forces
        // additive (GL_ONE, GL_ONE) blending with depth testing off and the
        // colour mask on, and batchable opaque draws are swapped for the
        // depth-only DepthPrepass* programs (fragment emits 1.0) so each covered
        // fragment adds 1 to the accumulation target. OverdrawRenderPass sets this
        // around a replay of the scene command bucket. Mutually exclusive with the
        // depth-prepass modes.
        static void SetOverdrawActive(bool active);
        // Water surface-depth capture: when active, ApplyPODRenderState forces
        // depth-only state (color writes off, depth writes on, GL_LESS, no blend)
        // even for the blended water draw, so the nearest wavy water surface is
        // written into a dedicated depth target for the underwater fog. The depth
        // prepass deliberately no-ops blended geometry, so this needs its own flag.
        static void SetWaterDepthCaptureActive(bool active);
        // The size of the target the water pass draws into, for the water UBO's
        // ScreenParams. Set by WaterRenderPass before its bucket runs (issue
        // #1486: the field used to come from a viewport nothing ever set).
        static void SetWaterTargetSize(u32 width, u32 height);
        // The recording view, whole (ADR 0025). The frame sets the primary
        // context's once; inside a recording item these address the item's own.
        static void SetView(const DispatchView& view);
        [[nodiscard]] static const DispatchView& GetView();
        // A view for a replay from another camera: the given camera, the
        // frame's render origin, no motion history of its own, and no claim on
        // the main view's screen-space lanes.
        [[nodiscard]] static DispatchView MakeAlternateView(const glm::mat4& view, const glm::mat4& projection,
                                                            const glm::mat4& viewProjection, const glm::vec3& position);
        // Draw from another view for the lifetime of this object. Installs the
        // whole view in the calling recording context, uploads its camera
        // block, and on every exit path puts the previous view back, re-uploads
        // its camera block and re-binds the main view's screen-space inputs.
        // The planar reflection and a bucket's view override both replay
        // through this; a fork inside it hands the alternate view to every item.
        class ScopedAlternateView
        {
          public:
            explicit ScopedAlternateView(const DispatchView& view);
            ~ScopedAlternateView();
            ScopedAlternateView(const ScopedAlternateView&) = delete;
            ScopedAlternateView& operator=(const ScopedAlternateView&) = delete;
            ScopedAlternateView(ScopedAlternateView&&) = delete;
            ScopedAlternateView& operator=(ScopedAlternateView&&) = delete;

          private:
            DispatchView m_Previous;
        };

        // For a tenant that draws through a camera buffer of its own on the
        // frame-published camera binding (a bake). The frame's camera block is
        // uploaded and bound again when the scope ends, on every exit: a bake
        // that fails half way must not leave its buffer, or nothing at all,
        // where the post passes read the camera without binding it (#1687).
        // Declare it before the tenant's own camera buffer is created, which
        // claims the binding point at construction.
        class ScopedFrameCameraRestore
        {
          public:
            ScopedFrameCameraRestore() = default;
            ~ScopedFrameCameraRestore();
            ScopedFrameCameraRestore(const ScopedFrameCameraRestore&) = delete;
            ScopedFrameCameraRestore& operator=(const ScopedFrameCameraRestore&) = delete;
            ScopedFrameCameraRestore(ScopedFrameCameraRestore&&) = delete;
            ScopedFrameCameraRestore& operator=(ScopedFrameCameraRestore&&) = delete;
        };

        // No field-wise setters: a view is replaced whole (SetView, or a
        // ScopedAlternateView), so the dispatcher never holds half of one
        // camera (ADR 0025 V2).

        // The GPU Scene material table shares slot 17 with the instance cull's
        // draw-indirect buffer (issue #994). Anything that binds its own buffer
        // there must call this, or the next draw that reads a canonical
        // material record reads that buffer instead.
        static void InvalidateGPUSceneMaterialBinding();
        // Draws this frame that rendered through a canonical GPU Scene record,
        // and draws that carried a link and fell back to their own copy. These
        // are the CONSUMPTION numbers — Renderer3D's link counters only say
        // whether extraction resolved, which stays true even if nothing reads
        // the result. Reset once per frame by ResetState().
        [[nodiscard]] static u32 GetGPUSceneConsumedDrawCount();
        [[nodiscard]] static u32 GetGPUSceneFallbackDrawCount();
        // @brief Camera-relative render origin for this frame (issue #429). The
        // stored view / view-projection / position remain *world*-space (sort
        // keys and the planar-reflection mirror need them); the camera-UBO
        // packing derives the relative matrices from those plus this origin so
        // the shared, terrain, voxel and mirror uploads all agree.
        static const glm::vec3& GetRenderOrigin();

        // Push the current CommandDispatch camera matrices (the Set* values above)
        // into the shared CameraUBO buffer and (re)bind it at UBO_CAMERA — the
        // same upload the terrain / voxel dispatch paths do inline. Exposed so a
        // pass that re-renders the opaque scene from an alternate camera (the
        // planar reflection) can swap the camera, replay the bucket, and restore.
        static void UploadCameraUBO();
        static void UploadCameraUBO(RendererAPI& api);

        // Put the frame's camera block back on the frame-published camera
        // binding if another buffer occupies it. The render graph runs this
        // before every pass (RenderGraph::SetPassEntryPublication): eleven
        // tenants create a buffer of the SAME layout on that binding (shadow
        // items, the particle batch, the bakes), so the block contract cannot
        // tell a predecessor's from the frame's, and several passes read the
        // camera without binding it (the groom and fluid passes among them).
        // The contents are not uploaded again: they are the frame's already.
        // A tenant that draws camera readers of its own afterwards, inside the
        // same pass, still restores (ScopedFrameCameraRestore).
        static void PublishFrameCamera();
        // How many pass entries this frame found another buffer there. Reset
        // once per frame by ResetState().
        [[nodiscard]] static u32 GetFrameCameraRepublishCount();

        // Shadow texture binding — set per-frame from Renderer3D/Scene.
        // The *Raw ids are the comparison-OFF views of the CSM array / shadow
        // atlas used by the PCSS blocker search (0 = none; bound only when
        // non-zero). The atlas replaced the old spot array + point cubemaps
        // (issue #435).
        static void SetShadowTextures(RHI::ResourceHandle csmTexture, RHI::ResourceHandle atlasTexture,
                                      RHI::ResourceHandle csmRawTexture = {},
                                      RHI::ResourceHandle atlasRawTexture = {});

        // The light grid of the view the dispatcher draws for (ADR 0026): each
        // scene view owns one, and Renderer3D re-points this when it activates
        // a view. SetUBOReferences publishes the first.
        static void SetForwardPlus(TiledForwardPlus* forwardPlus);

        // Snow accumulation depth texture — set per-frame
        static void SetSnowDepthTexture(RHI::ResourceHandle texture);

        // Cloud shadow transmittance map (R8, issue #633) — set per-frame
        // from CloudShadowMap by RenderPipeline::UploadExecutionState; bound
        // at TEX_CLOUD_SHADOW (62) during PBR mesh dispatch (0 = none; bound
        // only when non-zero, gated shader-side by the AtmosphereShadingUBO
        // enabled flag).
        static void SetCloudShadowTexture(RHI::ResourceHandle texture);

        // Screen-space AO for the FORWARD shaders' ambient term (issue #1452).
        // `params` is the CameraUBO::ScreenSpaceAOParams lane (x = live,
        // y = strength); `aoTexture` the AO buffer and `depthTexture` the
        // full-resolution scene depth COPY the prepass exported, published at
        // TEX_SSAO / TEX_POSTPROCESS_DEPTH by BindSceneResources for every
        // forward geometry pass. Set by ScenePass's colour half once the AO
        // passes have run; cleared by ResetState, so a pass that runs before
        // it (the prepass) reads the white placeholder with x = 0.
        static void SetForwardScreenSpaceAO(const glm::vec4& params, RHI::ResourceHandle aoTexture,
                                            RHI::ResourceHandle depthTexture);
        // The lighting tap the main view's colour passes read from the camera
        // lane (issue #1526, LightingTap). Set once per frame by the pipeline;
        // a suspended (mirrored) replay uploads 0 like the AO lane.
        static void SetLightingTap(u32 tap);
        // Publish just the two AO samplers (TEX_SSAO, TEX_POSTPROCESS_DEPTH).
        // BindSceneResources does this too; a forward geometry pass that does
        // not call that (foliage, groom, water) calls this before its draws.
        static void BindForwardScreenSpaceAO();

        // Getters for current frame state (used for sort key generation and per-bucket view state)
        static const glm::mat4& GetViewMatrix();
        static const glm::mat4& GetProjectionMatrix();
        static const glm::mat4& GetViewProjectionMatrix();
        static const glm::vec3& GetViewPosition();

        // Shared scene/runtime bindings supplied by the renderer frontend at init.
        /// Whether the shared scene buffers are currently published here.
        /// False after `Shutdown()`, and the signal `Renderer3D::BeginScene`
        /// uses to re-home its own buffers when something tore the dispatcher
        /// down underneath it (issue #1074).
        [[nodiscard]] static bool HasUBOReferences();

        static void SetUBOReferences(
            const Ref<UniformBuffer>& cameraUBO,
            const Ref<UniformBuffer>& materialUBO,
            const Ref<UniformBuffer>& boneMatricesUBO,
            const Ref<InstanceBuffer>& modelInstanceBuffer,
            const Ref<UniformBuffer>& prevBoneMatricesUBO = nullptr,
            TiledForwardPlus* forwardPlus = nullptr);

        // Rebind the shared scene camera UBO after earlier passes reused that
        // binding point, and ensure the Forward+ UBO baseline remains valid
        // even when tiled Forward+ lighting is inactive this frame.
        static void BindSceneResources();

        // Uploads the PBR material UBO + binds its textures for a non-bucket
        // draw path (virtualized geometry, issue #629). Uses the same
        // redundant-upload diffing state as the bucket dispatcher.
        static void UploadMaterialForDirectDraw(const PODMaterialData& mat, u16 materialDataIndex);

        // State management dispatch functions
        static void SetViewport(const void* data, RendererAPI& api);
        static void SetClearColor(const void* data, RendererAPI& api);
        static void Clear(const void* data, RendererAPI& api);
        static void ClearStencil(const void* data, RendererAPI& api);
        static void SetBlendState(const void* data, RendererAPI& api);
        static void SetBlendFunc(const void* data, RendererAPI& api);
        static void SetBlendEquation(const void* data, RendererAPI& api);
        static void SetDepthTest(const void* data, RendererAPI& api);
        static void SetDepthMask(const void* data, RendererAPI& api);
        static void SetDepthFunc(const void* data, RendererAPI& api);
        static void SetStencilTest(const void* data, RendererAPI& api);
        static void SetStencilFunc(const void* data, RendererAPI& api);
        static void SetStencilMask(const void* data, RendererAPI& api);
        static void SetStencilOp(const void* data, RendererAPI& api);
        static void SetCulling(const void* data, RendererAPI& api);
        static void SetCullFace(const void* data, RendererAPI& api);
        static void SetLineWidth(const void* data, RendererAPI& api);
        static void SetPolygonMode(const void* data, RendererAPI& api);
        static void SetPolygonOffset(const void* data, RendererAPI& api);
        static void SetScissorTest(const void* data, RendererAPI& api);
        static void SetScissorBox(const void* data, RendererAPI& api);
        static void SetColorMask(const void* data, RendererAPI& api);
        static void SetMultisampling(const void* data, RendererAPI& api);

        // Draw commands dispatch functions
        static void BindDefaultFramebuffer(const void* data, RendererAPI& api);
        static void BindTexture(const void* data, RendererAPI& api);
        static void DrawIndexed(const void* data, RendererAPI& api);
        static void DrawIndexedInstanced(const void* data, RendererAPI& api);
        static void DrawArrays(const void* data, RendererAPI& api);
        static void DrawLines(const void* data, RendererAPI& api);
        static void DrawMesh(const void* data, RendererAPI& api);
        static void DrawMeshInstanced(const void* data, RendererAPI& api);
        static void DrawSkybox(const void* data, RendererAPI& api);
        static void DrawInfiniteGrid(const void* data, RendererAPI& api);
        static void DrawQuad(const void* data, RendererAPI& api);

        // Terrain/Voxel dispatch functions
        static void DrawTerrainPatch(const void* data, RendererAPI& api);
        static void DrawVoxelMesh(const void* data, RendererAPI& api);

        // Decal dispatch function
        static void DrawDecal(const void* data, RendererAPI& api);

        // Foliage dispatch function
        static void DrawFoliageLayer(const void* data, RendererAPI& api);

        // Water dispatch function
        static void DrawWater(const void* data, RendererAPI& api);

        static Statistics& GetStatistics();

        // Renderer3D's direct draw helpers use the same item-owned uploads as
        // bucket replay. Outside an item these return the supplied main object.
        static Ref<UniformBuffer> ResolveRecordingUpload(u32 binding, const Ref<UniformBuffer>& mainBuffer);
        static Ref<InstanceBuffer> ResolveRecordingInstances(const Ref<InstanceBuffer>& mainBuffer);
    };
} // namespace OloEngine
