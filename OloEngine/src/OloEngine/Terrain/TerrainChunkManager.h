#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainGPUPicker.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"

#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace OloEngine
{
    class TerrainData;
    class Frustum;

    // Result of LOD selection — a chunk to render + its tessellation data
    struct TerrainRenderChunk
    {
        const TerrainChunk* Chunk = nullptr;
        TerrainChunkLODData LODData;
    };

    // Manages terrain chunks with quadtree-based LOD.
    // Owns a grid of base chunks and a quadtree for adaptive selection.
    class TerrainChunkManager : public RefCounted
    {
      public:
        TerrainChunkManager() = default;

        // Build all chunks and quadtree from terrain data using parallel for
        void GenerateAllChunks(const TerrainData& terrainData,
                               f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // Rebuild a single chunk (for brush editing)
        void RebuildChunk(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                          f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // Select visible chunks via quadtree LOD and frustum culling.
        // The pre-#714 CPU path: a recursive descent plus a neighbour-LOD
        // resolution pass, both per frame. Still the fallback when the GPU
        // descent is unavailable (no compute shaders) or switched off.
        void SelectVisibleChunks(const Frustum& frustum,
                                 const glm::vec3& cameraPos,
                                 const glm::mat4& viewProjection,
                                 f32 viewportHeight);

        // Run the GPU LOD descent for this frame (issue #714). Returns false if
        // the GPU tree is not usable, in which case the caller should fall back
        // to SelectVisibleChunks. Nothing is read back and no per-chunk LOD data
        // is produced — the terrain draws from GetGPUQuadtree()'s indirect list.
        bool DispatchGPULOD(const Frustum& frustum,
                            const glm::vec3& cameraPos,
                            const glm::mat4& viewProjection,
                            f32 viewportHeight,
                            f32 targetTriangleSize);

        [[nodiscard]] const Ref<TerrainGPUQuadtree>& GetGPUQuadtree() const
        {
            return m_GPUQuadtree;
        }

        // The GPU ray picker (issue #717). Created on first use, which is the
        // first frame something actually submits a ray — an editor tool, not the
        // renderer. Null until then, so a game that never picks pays nothing.
        //
        // NOT gated on TerrainComponent::m_TessellationEnabled, unlike the LOD
        // descent above. Picking is an editor interaction and has to work in
        // every terrain scene; tying it to a flag that (before #714's test scene)
        // no scene set is exactly how the quadtree ended up with zero runtime
        // coverage — see docs/agent-rules/terrain-gpu-lod-quadtree.md §1.
        // Non-const on purpose: Ref<T> propagates constness to the pointee, so a
        // `const Ref&` here would make every SubmitRay() call at the call site a
        // compile error. This accessor mutates anyway — it creates the picker.
        [[nodiscard]] Ref<TerrainGPUPicker>& GetOrCreateGPUPicker();
        [[nodiscard]] const Ref<TerrainGPUPicker>& GetGPUPicker() const
        {
            return m_GPUPicker;
        }

        // Poll the pick ring and, if a ray is queued, run the pick pass. No-op
        // when nothing has ever asked for a pick. Render thread, live context.
        void UpdateGPUPicking(const TerrainGPUPicker::TerrainInputs& terrain);

        // Process-wide A/B lever for the GPU LOD descent. Defaults to enabled
        // unless OLO_TERRAIN_CPU_LOD=1 is set — same shape as the gameplay
        // scheduler's OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL, and for the same
        // reason: when terrain looks wrong, the first question is which of the
        // two selection paths produced it, and that has to be answerable
        // without a rebuild.
        static void SetGpuDrivenLODEnabled(bool enabled);
        [[nodiscard]] static bool IsGpuDrivenLODEnabled();

        // Get chunks visible to the given frustum (Phase 1 compat — flat culling)
        void GetVisibleChunks(const Frustum& frustum,
                              std::vector<const TerrainChunk*>& outChunks) const;

        // Get all chunks (for shadow rendering which uses its own frustum)
        void GetAllChunks(std::vector<const TerrainChunk*>& outChunks) const;

        // Get selected chunks from last SelectVisibleChunks call (with LOD data)
        [[nodiscard]] const std::vector<TerrainRenderChunk>& GetSelectedChunks() const
        {
            return m_SelectedChunks;
        }

        [[nodiscard]] u32 GetNumChunksX() const
        {
            return m_NumChunksX;
        }
        [[nodiscard]] u32 GetNumChunksZ() const
        {
            return m_NumChunksZ;
        }
        [[nodiscard]] u32 GetTotalChunks() const
        {
            return m_NumChunksX * m_NumChunksZ;
        }
        [[nodiscard]] bool IsBuilt() const
        {
            return !m_Chunks.empty();
        }

        [[nodiscard]] TerrainQuadtree& GetQuadtree()
        {
            return m_Quadtree;
        }
        [[nodiscard]] const TerrainQuadtree& GetQuadtree() const
        {
            return m_Quadtree;
        }

        // Enable/disable tessellation (fallback to plain triangle rendering)
        bool TessellationEnabled = true;

        // The INCLUSIVE chunk-grid rectangle a quadtree node covers, in chunk
        // indices along one axis. Returned by value rather than as four
        // same-typed out-parameters — X0/X1/Z0/Z1 were adjacent u32& and
        // trivially swappable by a caller mistake (cpp:S5419); nothing here
        // has more than one caller, so there is no call-site cost to bundling.
        struct ChunkRange
        {
            u32 X0 = 0;
            u32 X1 = 0;
            u32 Z0 = 0;
            u32 Z1 = 0;
        };

      private:
        // A leaf node yields a single chunk; a node selected above the leaf
        // level yields the whole 2^k x 2^k block beneath it. nullopt only when
        // there is no chunk grid yet.
        //
        // Replaced a FindChunkForNode() that mapped the node's CENTRE to one
        // chunk — correct for leaves, and a hole for everything coarser.
        [[nodiscard("the chunk range must be used to enumerate the covered chunks")]] std::optional<ChunkRange>
        RangeForNode(const TerrainQuadNode& node) const;

        std::vector<TerrainChunk> m_Chunks;
        u32 m_NumChunksX = 0;
        u32 m_NumChunksZ = 0;

        TerrainQuadtree m_Quadtree;
        std::vector<TerrainRenderChunk> m_SelectedChunks;
        // One byte per chunk, reset at the start of each CPU selection: which
        // chunks this frame's selection has already claimed. Needed because the
        // chunk grid is not necessarily a power of two, so a quadtree boundary
        // can fall inside a chunk and two nodes can both cover it.
        std::vector<u8> m_ChunkClaimed;

        // Created lazily by GenerateAllChunks; null when the terrain never
        // built. Held by Ref because Scene.cpp hands its buffers to a render
        // command that outlives the submission call.
        Ref<TerrainGPUQuadtree> m_GPUQuadtree;
        Ref<TerrainGPUPicker> m_GPUPicker;
    };
} // namespace OloEngine
