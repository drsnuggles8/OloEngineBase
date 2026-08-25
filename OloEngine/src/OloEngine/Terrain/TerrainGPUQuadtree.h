#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // @brief GPU-resident terrain LOD quadtree — descent, selection and seam
    // resolution, all on the GPU (issue #714).
    //
    // Replaces the per-frame CPU work in `TerrainQuadtree::SelectLOD()`: a
    // recursive `SelectNode()` descent, an `unordered_set`-backed
    // `ResolveNeighborLODs()` pass doing four `FindLeafAt()` point queries per
    // selected node, and a per-chunk `TerrainChunkLODData` upload. All of that
    // was single-threaded pointer chasing proportional to the visible node
    // count, and it is what capped how fine the quadtree could practically get.
    //
    // The GPU replacement is a persistent worklist:
    //
    //   1. `TerrainNodeSelect.comp` runs once per tree level. Each thread pops
    //      one pending node, frustum-culls it, compares its screen-space error
    //      against the split threshold, and `atomicAdd`-appends either its four
    //      children to the next-pass buffer or itself to the visible list.
    //   2. `TerrainCullArgs.comp` (1 thread) swaps the counters between levels
    //      and writes the next `DispatchComputeIndirect` arguments plus the
    //      terrain draw's `DrawElementsIndirectCommand.instanceCount`.
    //   3. `TerrainLODMap.comp` turns the split map into a per-texel selected
    //      level, and `TerrainSeamMap.comp` samples it around each visible node
    //      to pack four `max(0, myLevel - neighbourLevel)` deltas into one uint.
    //   4. The terrain vertex stage unpacks those deltas and snaps the matching
    //      edge's vertices onto the coarser neighbour's spacing.
    //
    // Nothing reads back. The only CPU-side per-frame cost is uploading the six
    // frustum planes plus a handful of scalars, and issuing the dispatches.
    //
    // **The selection math is a transcription, not a reinterpretation.** The
    // frustum planes come from `Frustum::Update()` and `projScale` from
    // `TerrainQuadtree::CalculateScreenSpaceError()`, both precomputed on the
    // CPU and uploaded, so the GPU descent selects the same node set the CPU
    // descent would. `TerrainGPUQuadtreeParityTest` asserts exactly that; keep
    // any future change to one side mirrored in the other or the test fails
    // loudly rather than the screen changing quietly.
    class TerrainGPUQuadtree : public RefCounted
    {
      public:
        // Vertices per patch edge. A visible node is drawn as one instance of a
        // shared (K+1)^2 unit grid, so this is the terrain's geometric
        // resolution per node — the same density `TerrainChunk` bakes today.
        // Must be a power of two: the seam snapping drops low bits of a grid
        // index, which only lands on the coarse neighbour's vertices when the
        // edge length is a power of two.
        static constexpr u32 kPatchGridResolution = 64;
        static_assert((kPatchGridResolution & (kPatchGridResolution - 1)) == 0,
                      "kPatchGridResolution must be a power of two for edge snapping to align");

        // log2(kPatchGridResolution) — the largest neighbour-level jump a patch
        // edge can express. A deeper jump clamps rather than collapsing the
        // edge to a single vertex.
        static constexpr u32 kMaxSeamDelta = 6;
        static_assert((1u << kMaxSeamDelta) == kPatchGridResolution,
                      "kMaxSeamDelta must be log2(kPatchGridResolution)");

        // Hard ceiling on the tree depth. 12 is where the packed node coord's
        // 14-bit x/y fields stop being the binding constraint and the node
        // pyramid (4^13/3 entries) starts costing real memory; the practical
        // limit is the buffer caps below, not this.
        static constexpr u32 kMaxDepth = 12;
        // The packed coord (oloTerrainPackNode) gives the level 4 bits and each
        // of x/y 14 bits, and a node at level L has coords up to 2^L - 1.
        // Violating either is silent truncation, not a compile error, so assert
        // it where the constant lives.
        static_assert(kMaxDepth <= 15, "packed node coord stores the level in 4 bits");
        static_assert(kMaxDepth <= 14, "packed node coord stores x/y in 14 bits each");

        // Twin of OLO_TERRAIN_NODE_SKIP in TerrainQuadtreeCommon.glsl: a
        // worklist slot the select kernel reserved but could not use, because
        // its four-child reservation straddled the capacity. The kernel writes
        // this rather than leaving the slot untouched, since PendingCount is
        // clamped to capacity and an unwritten slot would be read as whatever
        // the previous level left there.
        //
        // Safe as a sentinel only because its level field (15) is above
        // kMaxDepth, so PackNode can never produce it — asserted below rather
        // than trusted, since a future depth bump is exactly the change that
        // would silently turn a real node into "skip me".
        static constexpr u32 kNodeSkipSentinel = 0xFFFFFFFFu;
        static_assert((kNodeSkipSentinel >> 28u) > kMaxDepth,
                      "the skip sentinel's level must be unreachable for a real node");

        // Per-pass worklist capacity and visible-node capacity. Both are
        // clamped rather than grown per frame — an overflow sets a flag the CPU
        // reads occasionally and warns about, which is far cheaper than sizing
        // for a worst case no camera ever produces.
        static constexpr u32 kMaxNodeListEntries = 1u << 18; // 262144 nodes/level
        static constexpr u32 kMaxVisibleNodes = 1u << 16;    // 65536 drawn patches

        struct CullInputs
        {
            // All terrain-LOCAL (see MakeTerrainLocalCullInputs in Scene.cpp) —
            // node bounds are terrain-local, and evaluating there keeps the
            // math precise far from the world origin (#429).
            Frustum ViewFrustum;
            glm::vec3 CameraPos{ 0.0f };
            glm::mat4 ViewProjection{ 1.0f };
            f32 ViewportHeight = 1080.0f;
            f32 TargetTriangleSize = 8.0f;
        };

        TerrainGPUQuadtree();
        ~TerrainGPUQuadtree();

        // Upload the node pyramid and (re)size every GPU buffer. `nodeMinMaxY`
        // is level-major — level L occupies 4^L entries at (4^L - 1) / 3, row
        // major — and holds each node's world-space height extremes, exactly
        // what `TerrainQuadtree` computed for its own `Bounds`. Safe to call
        // again after a sculpt; buffers are reused when the shape is unchanged.
        void Build(const std::vector<glm::vec2>& nodeMinMaxY, u32 maxDepth,
                   f32 worldSizeX, f32 worldSizeZ);

        // Run the whole descent for one frame. Must be called on the render
        // thread with a live GL/Vulkan context. No-op (and returns false) if the
        // tree was never built or a compute shader failed to load, so a caller
        // can fall back to the CPU path.
        bool Dispatch(const CullInputs& inputs);

        [[nodiscard]] bool IsBuilt() const
        {
            return m_MaxDepth > 0 && m_NodeBoundsBuffer != nullptr;
        }

        // True when the GPU descent produced THIS frame's selection, so the
        // indirect buffers hold a real draw. Deliberately not a latch: the CPU
        // fallback clears it (TerrainChunkManager::SelectVisibleChunks), because
        // a latched flag would keep the submission path drawing the last GPU
        // frame's node list forever after a single successful dispatch — which
        // is exactly what the A/B lever would hit first.
        [[nodiscard]] bool HasDispatched() const
        {
            return m_HasDispatched;
        }
        void ClearDispatched()
        {
            m_HasDispatched = false;
        }

        // Release the process-wide patch mesh. Must be called while the GL /
        // Vulkan context is still alive — a static Ref<VertexArray> destroyed at
        // process exit would delete a VAO against a dead context. Called from
        // Renderer3D::Shutdown alongside the other GPU pools.
        static void ReleaseSharedPatchMesh();

        // The DrawElementsIndirectCommand the terrain draw sources its
        // instance count from, and the visible-node list the vertex stage
        // indexes by gl_InstanceIndex.
        [[nodiscard]] RHI::ResourceHandle GetDrawArgsHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetVisibleNodesHandle() const;

        // The min/max height pyramid, shared with the ray-guided pick descent
        // (TerrainGPUPicker, issue #717). Exposed rather than duplicated: the
        // pyramid is what makes picking "reuse the culling machinery" literally
        // true, and a second copy would be a second thing to keep in step with
        // a sculpt.
        [[nodiscard]] RHI::ResourceHandle GetNodeBoundsHandle() const;

        [[nodiscard]] u32 GetMaxDepth() const
        {
            return m_MaxDepth;
        }

        // The shared unit-space patch grid every visible node instances. Lazily
        // created on first use (needs a live context) and shared process-wide —
        // it is pure [0,1]^2 topology with no terrain-specific data in it.
        static const Ref<VertexArray>& GetSharedPatchMesh();
        static u32 GetSharedPatchIndexCount();

        // Number of nodes in a full pyramid of the given depth: (4^(D+1) - 1) / 3.
        [[nodiscard]] static u32 TotalNodeCount(u32 maxDepth);
        // First index of level L in that pyramid: (4^L - 1) / 3.
        [[nodiscard]] static u32 LevelOffset(u32 level);

        // Packing twin of oloTerrainPackNode() in TerrainQuadtreeCommon.glsl.
        [[nodiscard]] static u32 PackNode(u32 level, u32 nx, u32 ny);

      private:
        void EnsureShaders();
        // Allocate/resize the GPU buffers for `maxDepth`. Returns false when a
        // buffer could not be created.
        bool EnsureBuffers(u32 maxDepth);
        void UploadCullParams(const CullInputs& inputs);
        // Read the overflow flags occasionally and warn. Two-phase, and both
        // halves matter: one call issues a GPU-side copy of the 4 flag bytes
        // into m_OverflowStaging, a LATER call reads that staging buffer. The
        // cull-state SSBO itself is never read by the CPU — see PollOverflow's
        // implementation comment for why that is not just a stall.
        void PollOverflow();

        Ref<ComputeShader> m_SelectShader;
        Ref<ComputeShader> m_ArgsShader;
        Ref<ComputeShader> m_LODMapShader;
        Ref<ComputeShader> m_SeamShader;
        bool m_ShadersLoaded = false;
        bool m_ShaderLoadFailed = false;

        Ref<UniformBuffer> m_CullParamsUBO;
        Ref<StorageBuffer> m_NodeBoundsBuffer; // vec2[totalNodes] world-space min/max Y
        Ref<StorageBuffer> m_NodeListA;        // uint[] packed coords — ping
        Ref<StorageBuffer> m_NodeListB;        // uint[] packed coords — pong
        Ref<StorageBuffer> m_CullStateBuffer;  // TerrainGpuCullState + both indirect arg blocks
        Ref<StorageBuffer> m_VisibleNodes;     // uvec2[] (packed coord, packed seams)
        Ref<StorageBuffer> m_SplitMap;         // uint[totalNodes]
        Ref<StorageBuffer> m_LODMap;           // uint[(1 << maxDepth)^2]
        Ref<StorageBuffer> m_DrawArgsBuffer;   // DrawElementsIndirectCommand

        u32 m_MaxDepth = 0;
        u32 m_TotalNodes = 0;
        u32 m_NodeListCapacity = 0;
        u32 m_LODMapResolution = 0;
        f32 m_WorldSizeX = 0.0f;
        f32 m_WorldSizeZ = 0.0f;
        bool m_HasDispatched = false;
        u32 m_FramesSinceOverflowPoll = 0;
        bool m_OverflowWarned = false;
        // DeviceToHost staging for the overflow flags, plus whether a copy into
        // it is outstanding. A raw handle, so — like TerrainGPUPicker's ring —
        // nothing frees it on its own and the destructor has to.
        RHI::ResourceHandle m_OverflowStaging = RHI::NullResource;
        bool m_OverflowCopyPending = false;
    };
} // namespace OloEngine
