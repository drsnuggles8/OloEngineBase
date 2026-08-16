#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"

#include <glm/glm.hpp>
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

        // Rebuild a single chunk (for brush editing in Phase 4)
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

        // Enable/disable tessellation (fallback to Phase 1 triangle rendering)
        bool TessellationEnabled = true;

      private:
        // Find the chunk that covers a given terrain-space point
        const TerrainChunk* FindChunkForNode(const TerrainQuadNode& node) const;

        std::vector<TerrainChunk> m_Chunks;
        u32 m_NumChunksX = 0;
        u32 m_NumChunksZ = 0;

        TerrainQuadtree m_Quadtree;
        std::vector<TerrainRenderChunk> m_SelectedChunks;

        // Created lazily by GenerateAllChunks; null when the terrain never
        // built. Held by Ref because Scene.cpp hands its buffers to a render
        // command that outlives the submission call.
        Ref<TerrainGPUQuadtree> m_GPUQuadtree;
    };
} // namespace OloEngine
