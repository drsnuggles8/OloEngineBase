#pragma once

#include "OloEngine/Containers/Array.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.h"

#include <functional>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class FoliageRenderer;
    class GroomRenderPass; // #1323, held by pointer only -- see SetGroomPass
    class StorageBuffer;
    class Shader;

    // Which caster families a shadow region draws (#1533). A region with a
    // groom caster renders in two halves -- its opaque casters, then its grooms
    // -- with the map copied between them into ShadowMap's opaque copy, so a
    // groom can be shadowed by the body it grows on without its own strands
    // counting twice. Everywhere else a region draws everything.
    enum class ShadowCasterFilter : u8
    {
        All,
        NoGrooms,
        GroomsOnly,
    };

    // Indicates which shadow target is being rendered in the current invocation
    enum class ShadowPassType : u8
    {
        CSM,  // Directional light cascaded shadow map
        Atlas // Local-light shadow atlas entry (spot tile or point cube-face tile, issue #435)
    };

    // POD shadow caster descriptors — collected during entity traversal, replayed per cascade/face.
    // This replaces the callback pattern: Scene.cpp adds casters during its entity loop,
    // and ShadowRenderPass::Execute() iterates them per light cascade/face with the
    // appropriate depth shader. No duplicate entity traversal, no per-frame lambda allocation.

    struct ShadowMeshCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        u32 baseIndex = 0; // Offset (in u32 entries) into the IBO — non-zero for submeshes sharing a combined IBO
        glm::mat4 transform = glm::mat4(1.0f);
        RHI::ResourceHandle shadowVaoID{};  // Position-merged shadow IB; invalid = use vaoID
        BoundingBox WorldBounds = NoBounds; // World-space AABB; NoBounds = always include
        // Material is MaterialFlag::TwoSided — rendered into the shadow map with culling DISABLED
        // instead of the default front-face cull, so single-sided planar geometry (a quad, a
        // banner, a foliage sheet) still casts a shadow when lit from the front (issue #650).
        bool twoSided = false;
    };

    struct ShadowSkinnedCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        u32 baseIndex = 0; // Same role as in ShadowMeshCaster
        glm::mat4 transform = glm::mat4(1.0f);
        u32 boneBufferOffset = 0;
        u32 boneCount = 0;
        BoundingBox WorldBounds = NoBounds; // World-space AABB; NoBounds = always include
    };

    struct ShadowTerrainCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        u32 patchVertexCount = 3;
        glm::mat4 transform = glm::mat4(1.0f);
        RHI::ResourceHandle heightmapTextureID{};
        ShaderBindingLayout::TerrainUBO terrainUBO{};
    };

    struct ShadowVoxelCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        // Non-zero selects the packed-quad depth shader and an instanced draw
        // (issue #727). Zero is the marching-cubes triangle soup. The shadow
        // silhouette MUST be rebuilt by the same code as the lit one, which is
        // why the two paths carry different depth shaders rather than sharing.
        u32 instanceCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
    };

    // A groom, rasterised from the light as widened ribbons (issue #1323).
    //
    // THE SIXTH CASTER FAMILY, and the one that needed a different shape from
    // the other five. They submit during Scene's entity traversal because their
    // geometry already exists by then; a groom's does not — it is built from
    // cooked curves into a vertex buffer by GroomRenderPass's own cache, and
    // that pass runs AFTER this one. So these are gathered by ShadowRenderPass
    // itself, at the top of Execute, through GroomRenderPass::AcquireShadowCaster:
    // the geometry stays owned by the groom pass and is built at most once per
    // frame, whichever consumer reaches it first.
    //
    // GATHERED ON THE RENDER THREAD, BEFORE ANY FORK, and that is a hard
    // requirement rather than a convenience: acquiring CREATES vertex and index
    // buffers on a miss and uploads the frame's deformation, and amendment (92)
    // rule 7 refuses resource creation inside a parallel-recording item. The
    // cascade items then only read this list. The issue calls this trap out by
    // name — it is silent on OpenGL and a fault on Vulkan.
    //
    // A BOUND COAT IS DEFORMED IN THE VERTEX STAGE (#1427), so its stream is a
    // rest stream in each root's bind frame and means nothing drawn as final
    // geometry. The deformation lanes below are the ones the strand pass draws
    // with, and the depth shaders include the same GroomStrandDeform.glsl, so
    // the shadow is cast by the coat the camera sees rather than by its bind
    // pose. Mode 0 is a final stream (an unbound groom) and reads neither.
    //
    // TRIVIALLY COPYABLE ON PURPOSE (a raw buffer pointer, not a Ref): the list
    // is a TArray64 that relocates by memcpy. The buffer outlives the frame's
    // list because the groom pass owns it and evicts only at the top of a frame.
    struct ShadowGroomCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
        BoundingBox WorldBounds = NoBounds; // World-space AABB of the POSED coat; NoBounds = always include
        // The width the coat is DRAWN at — the per-groom authoring scale (the
        // per-role coverage compensation is baked into the stream's radii since
        // #1428) — and the transform's mean axis length, so the caster is
        // exactly as thick as the ribbons the strand pass draws.
        f32 widthScale = 1.0f;
        f32 objectScale = 1.0f;
        // The width floor in TEXELS of whatever target this is rasterised into.
        f32 minWidthTexels = 1.0f;
        // GPU strand deformation (#1427): the per-frame root/guide buffer and
        // the two lane vectors GroomStrandParams carries. Null / mode 0 = a
        // final stream.
        StorageBuffer* deformBuffer = nullptr;
        glm::ivec4 deformModes{ 0 };
        glm::ivec4 deformBases{ 0 };
        // The caster's identity across frames (entity and groom), and whether its
        // silhouette changes every frame without its transform moving — a bound
        // coat follows its body's pose. Both feed the Virtual Shadow Map's page
        // invalidation (SubmitGroomDynamicInvalidations).
        u64 key = 0;
        bool deforming = false;
    };

    struct ShadowFoliageCaster
    {
        FoliageRenderer* renderer = nullptr;
        Ref<Shader> depthShader;
        f32 time = 0.0f;
    };

    // @brief Render pass for shadow map generation.
    //
    // Executes before SceneRenderPass — which is also why a groom caster's
    // geometry is acquired from GroomRenderPass ahead of that pass's own
    // Execute (issue #1323; see GroomRenderPass::AcquireShadowCaster). For each
    // shadow-casting light,
    // renders scene geometry from the light's perspective into the
    // appropriate shadow map texture layer.
    //
    // Data-driven design: Scene.cpp adds shadow casters during its entity
    // traversal loop. Execute() iterates the caster lists per cascade/face,
    // binding the appropriate depth shader for each geometry type.
    // No callbacks, no duplicate entity traversal.
    //
    // Parallel recording (issue #806, ADR 0011 amendment (92)). The CSM
    // cascades and the atlas entries are independent depth targets, so each
    // is one item of a RenderCommand::RecordParallel region: on a backend that
    // forks, item i records on a worker into its own command buffer and the
    // items execute in ascending order at the fork point; everywhere else the
    // items run inline, in order, on the calling thread — the command stream
    // is the same either way. Two consequences shape this class:
    //
    //   * ONE WRITER PER RESOURCE OBJECT PER REGION (rule 6). A UBO or an
    //     instance buffer versions its bytes per object, so two items writing
    //     one object would interleave. The pass therefore owns a camera UBO, an
    //     animation UBO and an instance buffer PER ITEM (ItemResources), created
    //     on the render thread before the fork — never inside an item (rule 7).
    //   * GROOMS (issue #1323) ARE ITEM-SAFE, and they are the sixth family.
    //     The only object one writes is ItemResources::Groom, so they record in
    //     the parallel half. Their GEOMETRY, however, is built by
    //     CollectGroomCasters on the render thread before any region opens —
    //     acquiring from the groom pass can CREATE buffers, and rule 7
    //     refuses that on an item context.
    //   * NOT EVERY CASTER IS ITEM-SAFE. Terrain (HeapBinding's one process-wide
    //     offset table, the shared terrain UBO), foliage (the FoliageRenderer's
    //     own shared buffers) and virtual geometry (compute dispatches, file
    //     statics) write objects the pass does not own per item. They record
    //     AFTER the join, sequentially, per cascade / entry — the
    //     ShadowCasterHalf::SequentialTail of RenderCascadeOrFace. Depth-only
    //     rendering is order-independent, so drawing them after the item-safe
    //     casters paints the same map.
    //
    // RendererProfiler is a plain singleton, so an item never touches it: each
    // item tallies its auto-batched draws (ItemProfilerTally) and the pass
    // replays them into the profiler after the join, in item order.
    class ShadowRenderPass : public RenderGraphNode
    {
      public:
        ShadowRenderPass();
        ~ShadowRenderPass() override;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;

        // Setup() declares nothing while shadows are off.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetShadowMap(ShadowMap* shadowMap)
        {
            m_ShadowMap = shadowMap;
        }

        /// The groom pass whose strand geometry the groom caster family draws
        /// (#1323). NOT owned: the render pipeline owns both passes. Null
        /// means no groom casts, which is exactly the pre-#1323 behaviour.
        void SetGroomPass(GroomRenderPass* pass) noexcept
        {
            m_GroomPass = pass;
        }

        // Shadow caster submission — called during Scene entity traversal.
        // Pass worldBounds (world-space AABB) when available; it enables per-cascade
        // frustum culling in Execute() so empty cascades skip all GPU work.
        // Leave as NoBounds when no tight bounds are available (foliage, terrain, etc.).
        void AddMeshCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                           RHI::ResourceHandle shadowVaoID = {}, const BoundingBox& worldBounds = NoBounds,
                           bool twoSided = false);
        void AddSkinnedCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                              u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds = NoBounds);
        void AddTerrainCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 patchVertexCount,
                              const glm::mat4& transform, RHI::ResourceHandle heightmapTextureID,
                              const ShaderBindingLayout::TerrainUBO& terrainUBO);
        void AddVoxelCaster(RHI::ResourceHandle vaoID, u32 indexCount, const glm::mat4& transform,
                            u32 instanceCount = 0);
        void AddFoliageCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time);

      private:
        // Fills m_GroomCasters from this frame's published groom requests.
        //
        // NOT a Scene-side submit like the other five families: the VAO a groom
        // caster draws does not exist until the strand geometry is built, and
        // GroomRenderPass owns that build. Runs on the render thread at the top
        // of Execute, before any parallel region opens, because acquiring can
        // CREATE buffers.
        void CollectGroomCasters();

        // One texel of the COARSEST CSM cascade, in world metres: the largest
        // the light-space width floor can be for any view a groom caster is
        // tested against, and therefore the right pad for its cull bounds.
        // Zero when no cascade matrix is usable yet.
        [[nodiscard]] f32 WidestShadowTexelMetres() const;

        // Draws every groom caster of one view. Shared by the cascade region,
        // the atlas region and (through a different shader and projection
        // source) the Virtual Shadow Map route, so the three techniques cannot
        // drift apart in how a strand is widened.
        // THE CALLER BINDS THE PROGRAM, not this function, and the split is
        // load-bearing for the VSM route: VirtualShadowMap::BindPhysicalPoolImage
        // forks on whether the program CURRENTLY IN FLIGHT is bindless, so it has
        // to run between the program bind and the draws. A version of this that
        // bound the shader itself would leave the pool bind either before the
        // program (wrong fork) or nowhere it could be expressed.
        void RenderGroomCasters(const Frustum* cullFrustum, const glm::vec3& renderOrigin,
                                f32 resolutionTexels, i32 clipLevel, UniformBuffer& paramsUBO) const;

        // The groom half of the Virtual Shadow Map route (#1323), invoked
        // INSIDE the VSM raster scope through the ExternalCasterRenderer seam
        // #1149 opened. Returns the number of level draws it issued.
        //
        // It re-binds the physical pool image after binding its own program,
        // which is not optional: BindPhysicalPoolImage forks on whether the
        // program currently in flight is bindless, so it cannot be hoisted out
        // of a shader switch — the failure is a silently unshadowed frame with
        // no error anywhere (virtual-geometry-into-a-second-shadow-technique.md
        // §2).
        u32 RenderGroomVirtualShadowLevels(VirtualShadowMap& vsm);
        // The GPU objects one item writes (amendment (92) rule 6): created by
        // EnsureItemResources on the render thread, indexed by item, shared by
        // the CSM region and the atlas region of one Execute (the two regions
        // never overlap, and a write after the join is just the next version).
        struct ItemResources
        {
            Ref<UniformBuffer> Camera;     // ShaderBindingLayout::UBO_CAMERA — this item's light VP
            Ref<UniformBuffer> Animation;  // ShaderBindingLayout::UBO_ANIMATION — bones of the skinned caster in flight
            Ref<InstanceBuffer> Instances; // SSBO_INSTANCE_DATA — the transforms of the batch / caster in flight
            Ref<UniformBuffer> Groom;      // UBO_USER_0 — the groom caster in flight (#1323)
        };
        friend struct TIsTriviallyRelocatable<ItemResources>;

        // One auto-batched shadow draw, as RendererProfiler::RecordInstancedDraw
        // wants it. Not the profiler's own record type: that one carries a
        // std::string label and an entity-id vector the shadow path never fills.
        struct ShadowInstancedDrawRecord
        {
            u32 VertexArrayIndex = 0;
            u32 IndexCount = 0;
            u32 InstanceCount = 0;
        };

        // What one item would have told the profiler. A vector, not a fixed
        // array, because there is one record per distinct submesh in the item.
        struct ItemProfilerTally
        {
            TArray64<ShadowInstancedDrawRecord> InstancedDraws;
        };
        friend struct TIsTriviallyRelocatable<ItemProfilerTally>;

        // The shaders the parallel-safe half binds, resolved on the render
        // thread before the fork. ShaderLibrary::Get is a non-const map
        // operator[] (it inserts on a miss), so an item must not call it.
        struct ShadowCasterShaders
        {
            Ref<Shader> Mesh;      // "ShadowDepth"
            Ref<Shader> Skinned;   // "ShadowDepthSkinned" — null when there are no skinned casters
            Ref<Shader> Voxel;     // Renderer3D::GetVoxelDepthShader()
            Ref<Shader> VoxelQuad; // Renderer3D::GetVoxelGreedyDepthShader()
            Ref<Shader> Terrain;
            Ref<Shader> Groom; // "GroomStrandDepth" — null when no groom casts
        };

        // One cascade or atlas entry that will actually render this frame —
        // the survivors of the skip logic in Execute — with what its item body
        // needs, so the body reads and never recomputes.
        struct ActiveShadowView
        {
            u32 Index = 0; // cascade index (CSM) or atlas entry index (Atlas)
            glm::mat4 LightVP = glm::mat4(1.0f);
            Frustum CullFrustum;
        };

        // Returns true if caster has valid bounds AND those bounds fail the frustum test.
        // Casters with NoBounds always pass (are included).
        [[nodiscard]] static bool ShouldCull(const BoundingBox& worldBounds, const Frustum& frustum);

        // Does any virtualized-geometry instance submitted this frame cast a shadow?
        //
        // The cascade-skip check treats virtual geometry as an UNBOUNDED caster (like terrain /
        // foliage / voxels): its per-instance bounds never enter the CPU caster lists, because
        // the cluster cull culls on the GPU, per cluster. Without this, a cascade whose only
        // casters were virtual meshes was skipped outright and Nanite geometry cast no shadow.
        [[nodiscard]] static bool AnyVirtualShadowCaster();

        // ---- Virtual geometry into the Virtual Shadow Map (issue #1149) ------
        //
        // Both read this frame's prepared virtual-mesh instances, so both require
        // VirtualGeometryShadow::PrepareViews to have run first.

        // Both read m_VsmVirtualBounds, which CollectVirtualCasterBounds fills
        // once per frame — the list is immutable for the frame and walking the
        // registry twice for it bought nothing.
        [[nodiscard]] bool CollectVirtualCasterBounds();

        // Fills m_VsmClipViews with the clip levels at least one shadow-casting
        // virtual instance reaches. The GPU would reject the rest anyway; doing
        // it here is what stops an untouched level from costing a dispatch per
        // instance.
        void BuildVirtualClipViews(const VirtualShadowMap& vsm);

        // Re-dirties the pages a virtual caster covers when what it draws there
        // changed: it MOVED, it APPEARED, or it DISAPPEARED. Without this a
        // cached page keeps the old silhouette — the page cache's characteristic
        // artefact, and one that looks like a lighting bug rather than a caching
        // one.
        //
        // All three matter and only the first is a transform question. A caster
        // that is deleted, or has CastShadows unticked, simply stops appearing in
        // the frame list; nothing compares against it, so without the departure
        // half its shadow stays on screen forever. An arrival is the mirror case:
        // a freshly spawned instance has Transform == PrevTransform, reads as
        // "did not move", and lands on pages that are already clean.
        void SubmitVirtualDynamicInvalidations(VirtualShadowMap& vsm);

        // The same three cases for the groom caster family (#1523; #1380 never
        // invalidated at all, so with the Virtual Shadow Map on a walking coat
        // left its old silhouette in every cached page it had crossed). A bound
        // coat is a mover every frame — its body's pose moves it without its
        // transform changing — and an unbound one moves when its transform does.
        // BEFORE UpdatePages, for the reason SubmitVirtualDynamicInvalidations
        // states.
        void SubmitGroomDynamicInvalidations(VirtualShadowMap& vsm);

        // Records every caster category of one view using item-owned uploads
        // and cull outputs. All shader lookups and resource growth precede it.
        void RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight,
                                 const Frustum* cullFrustum,
                                 const ShadowCasterShaders& shaders, ItemResources& resources,
                                 ItemProfilerTally* tally, VirtualGeometryShadow::ViewResources* virtualResources,
                                 // This view's item index in the region, so the foliage draw can pick the
                                 // cull slot RecordShadowRegion filled for it (issue #1235).
                                 u32 shadowViewIndex) const;

        // Grow the per-item pool to `count` entries. Render thread, before the
        // fork: rule 7 refuses resource creation on an item context.
        void EnsureItemResources(u32 count, u32 instanceCapacity);
        // One item per active CSM layer or atlas viewport, joined in view order.
        // The texels of one light view the receiving grooms can sample, as a
        // rect inside its tile (tileX/tileY/tileSize: the whole layer for a
        // cascade, the entry's tile for the atlas): their box projected
        // through lightVP, padded for the filter kernels, clamped to the tile.
        // False when the box misses the view.
        [[nodiscard]] bool GroomReceiverTexelRect(const glm::mat4& lightVP, u32 tileX, u32 tileY, u32 tileSize, u32& x,
                                                  u32& y, u32& width, u32& height) const;

        void RecordShadowRegion(ShadowPassType type, const ShadowCasterShaders& shaders, bool recordingInstancedDraws,
                                u32 instanceCapacity,
                                const std::function<void(const ActiveShadowView&)>& selectTarget,
                                bool clearPerItem, ShadowCasterFilter filter = ShadowCasterFilter::All);

        // Hand the items' tallies to RendererProfiler in item order, then clear
        // them. Render thread, after the join.
        void ReplayProfilerTallies(ShadowPassType type, bool recording);

        ShadowMap* m_ShadowMap = nullptr;
        Ref<Framebuffer> m_ShadowFramebuffer; // Depth-only FBO for shadow rendering

        // Shadow caster lists — cleared after each Execute()
        TArray64<ShadowMeshCaster> m_MeshCasters;
        TArray64<ShadowSkinnedCaster> m_SkinnedCasters;
        TArray64<ShadowTerrainCaster> m_TerrainCasters;
        TArray64<ShadowVoxelCaster> m_VoxelCasters;
        TArray64<ShadowFoliageCaster> m_FoliageCasters;
        // Rebuilt every frame from the published groom requests (#1323), not
        // submitted into. Reset with the other five at the end of Execute.
        TArray64<ShadowGroomCaster> m_GroomCasters;
        // The families the region being recorded draws (#1533). Set on the
        // render thread before a region forks and read-only while its items
        // record; All outside RecordShadowRegion.
        ShadowCasterFilter m_CasterFilter = ShadowCasterFilter::All;
        // Where a strand can sample an opaque copy this frame (#1533): the
        // union of the posed boxes of the grooms that RECEIVE the scene's
        // shadow, so a copy need cover no more of a map than that. Unknown
        // (a receiver without bounds) copies whole maps; no receiver at all
        // makes no copy.
        BoundingBox m_GroomReceiverBounds = NoBounds;
        bool m_HasGroomReceivers = false;
        bool m_GroomReceiverBoundsUnknown = false;
        // The groom pass owns the strand geometry (its cache, its GPU
        // deformation buffer); the caster family borrows it through
        // GroomRenderPass::AcquireShadowCaster. Wired by CreateFramePasses.
        GroomRenderPass* m_GroomPass = nullptr;
        // One params UBO for the VSM route, which is SEQUENTIAL and therefore
        // needs only one — the same reason m_VsmVirtualResources is a single
        // object while m_VirtualItemResources is a vector.
        Ref<UniformBuffer> m_GroomVsmParamsUBO;
        Ref<Shader> m_GroomVsmDepthShader;

        // Per-item state of the parallel regions (issue #806). Grown lazily to
        // the largest region seen; never resized while a region is open, so
        // item i touches element i and nothing else.
        TArray64<ItemResources> m_ItemResources;
        TArray64<VirtualGeometryShadow::ViewResources> m_VirtualItemResources;
        TArray64<ItemProfilerTally> m_ItemTallies;
        TArray64<ActiveShadowView> m_ActiveViews; // the current region's items, in item order

        // ---- Virtual geometry into the Virtual Shadow Map (issue #1149) ------
        //
        // ONE set of cull outputs, not one per view like m_VirtualItemResources
        // above: the VSM raster is a single sequential region, so its clip levels
        // are drawn one after another and can reuse the same command / args /
        // visible buffers. The cascade region forks, which is the only reason
        // that one is a vector.
        VirtualGeometryShadow::ViewResources m_VsmVirtualResources;
        TArray64<VirtualGeometryShadow::VsmClipView> m_VsmClipViews;
        // Scratch for this frame's virtual shadow casters, kept as a member so
        // the allocation survives across frames (cleared, never read across a
        // frame boundary, render thread only).
        TArray64<VirtualGeometryShadow::ShadowCasterBounds> m_VsmVirtualBounds;
        // Last frame's virtual casters, by stable key, so a caster that vanished
        // can still have its pages invalidated — it is not in this frame's list
        // to be compared against. Keyed rather than indexed because the frame
        // list is rebuilt each frame and its order is not stable.
        struct VirtualCasterFootprint
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
        };
        std::unordered_map<u64, VirtualCasterFootprint> m_PrevVirtualCasters;
        // Last frame's groom casters, by ShadowGroomCaster::key, for the same
        // departure and sweep reasons (#1523). The transform is kept so an
        // unbound coat that did not move costs no invalidation.
        struct GroomCasterFootprint
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
            glm::mat4 Transform{ 1.0f };
        };
        std::unordered_map<u64, GroomCasterFootprint> m_PrevGroomCasters;
        // Latched once per session: groom casters and the VSM local-light layers.
        bool m_WarnedGroomVsmLocalLights = false;

        bool m_WarnedOnce = false;
        bool m_LoggedOnce = false;
    };
    // Owns only audited pointer-based containers/Refs; no address-relative members.
    template<>
    struct TIsTriviallyRelocatable<ShadowRenderPass::ItemResources>
    {
        using Record = ShadowRenderPass::ItemResources;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Camera)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Animation)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Instances)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Groom)>;
    };
    // Owns only audited pointer-based containers/Refs; no address-relative members.
    template<>
    struct TIsTriviallyRelocatable<ShadowRenderPass::ItemProfilerTally>
    {
        using Record = ShadowRenderPass::ItemProfilerTally;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::InstancedDraws)>;
    };
    // External object identities and Ref ownership survive byte relocation; no self pointers.
    template<>
    struct TIsTriviallyRelocatable<ShadowFoliageCaster>
    {
        using Record = ShadowFoliageCaster;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::renderer)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::depthShader)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::time)>;
    };
} // namespace OloEngine
