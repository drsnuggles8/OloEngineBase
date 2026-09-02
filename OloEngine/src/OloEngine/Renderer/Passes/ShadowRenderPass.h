#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <functional>
#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class FoliageRenderer;
    class Shader;

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

    struct ShadowFoliageCaster
    {
        FoliageRenderer* renderer = nullptr;
        Ref<Shader> depthShader;
        f32 time = 0.0f;
    };

    // @brief Render pass for shadow map generation.
    //
    // Executes before SceneRenderPass. For each shadow-casting light,
    // renders scene geometry from the light's perspective into the
    // appropriate shadow map texture layer.
    //
    // Data-driven design: Scene.cpp adds shadow casters during its entity
    // traversal loop. Execute() iterates the caster lists per cascade/face,
    // binding the appropriate depth shader for each geometry type.
    // No callbacks, no duplicate entity traversal.
    //
    // Parallel recording (issue #806, ADR 0011 amendment (91)). The CSM
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
        // Which half of a cascade / entry RenderCascadeOrFace records (issue #806).
        enum class ShadowCasterHalf : u8
        {
            ParallelSafe,  // static mesh batches, skinned casters, voxel casters — runs as a RecordParallel item
            SequentialTail // terrain, foliage, virtual geometry — runs on the render thread after the join
        };

        // The GPU objects one item writes (amendment (91) rule 6): created by
        // EnsureItemResources on the render thread, indexed by item, shared by
        // the CSM region and the atlas region of one Execute (the two regions
        // never overlap, and a write after the join is just the next version).
        struct ItemResources
        {
            Ref<UniformBuffer> Camera;     // ShaderBindingLayout::UBO_CAMERA — this item's light VP
            Ref<UniformBuffer> Animation;  // ShaderBindingLayout::UBO_ANIMATION — bones of the skinned caster in flight
            Ref<InstanceBuffer> Instances; // SSBO_INSTANCE_DATA — the transforms of the batch / caster in flight
        };

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
            std::vector<ShadowInstancedDrawRecord> InstancedDraws;
        };

        // The shaders the parallel-safe half binds, resolved on the render
        // thread before the fork. ShaderLibrary::Get is a non-const map
        // operator[] (it inserts on a miss), so an item must not call it.
        struct ShadowCasterShaders
        {
            Ref<Shader> Mesh;      // "ShadowDepth"
            Ref<Shader> Skinned;   // "ShadowDepthSkinned" — null when there are no skinned casters
            Ref<Shader> Voxel;     // Renderer3D::GetVoxelDepthShader()
            Ref<Shader> VoxelQuad; // Renderer3D::GetVoxelGreedyDepthShader()
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

        // Records one half (see ShadowCasterHalf) of one cascade / atlas entry
        // into the currently selected target + viewport. `resources` are this
        // item's objects; `tally` is where the parallel-safe half puts its
        // profiler records (null = the profiler is not recording). The
        // sequential tail only re-binds the camera UBO the parallel-safe half
        // of the SAME item uploaded, so the two halves must run for the same
        // (lightVP, resources) pair, parallel-safe half first.
        void RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight,
                                 const Frustum* cullFrustum, ShadowCasterHalf half,
                                 const ShadowCasterShaders& shaders, ItemResources& resources,
                                 ItemProfilerTally* tally) const;

        // Grow the per-item pool to `count` entries. Render thread, before the
        // fork: rule 7 refuses resource creation on an item context.
        void EnsureItemResources(u32 count, u32 instanceCapacity);
        // The fork / replay / tail protocol shared by the CSM and atlas
        // regions: record the parallel-safe half of every view in
        // m_ActiveViews as a RecordParallel item (after `selectTarget`), hand
        // the profiler tallies over in item order, then — when any sequential
        // caster exists — walk the same views on the render thread for the
        // tail, re-selecting the target each time. `selectTarget` is the only
        // thing the two regions do differently (a layer + clear per cascade,
        // a viewport per atlas entry).
        void RecordShadowRegion(ShadowPassType type, const ShadowCasterShaders& shaders, bool recordingInstancedDraws,
                                bool hasSequentialCasters, u32 instanceCapacity,
                                const std::function<void(const ActiveShadowView&)>& selectTarget,
                                bool clearPerItem);

        // Hand the items' tallies to RendererProfiler in item order, then clear
        // them. Render thread, after the join.
        void ReplayProfilerTallies(ShadowPassType type, bool recording);

        ShadowMap* m_ShadowMap = nullptr;
        Ref<Framebuffer> m_ShadowFramebuffer; // Depth-only FBO for shadow rendering

        // Shadow caster lists — cleared after each Execute()
        std::vector<ShadowMeshCaster> m_MeshCasters;
        std::vector<ShadowSkinnedCaster> m_SkinnedCasters;
        std::vector<ShadowTerrainCaster> m_TerrainCasters;
        std::vector<ShadowVoxelCaster> m_VoxelCasters;
        std::vector<ShadowFoliageCaster> m_FoliageCasters;

        // Per-item state of the parallel regions (issue #806). Grown lazily to
        // the largest region seen; never resized while a region is open, so
        // item i touches element i and nothing else.
        std::vector<ItemResources> m_ItemResources;
        std::vector<ItemProfilerTally> m_ItemTallies;
        std::vector<ActiveShadowView> m_ActiveViews; // the current region's items, in item order

        bool m_WarnedOnce = false;
        bool m_LoggedOnce = false;
    };
} // namespace OloEngine
