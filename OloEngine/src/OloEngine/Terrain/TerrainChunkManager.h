#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainChunk.h"
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

        // Select visible chunks via quadtree LOD and frustum culling
        void SelectVisibleChunks(const Frustum& frustum,
                                 const glm::vec3& cameraPos,
                                 const glm::mat4& viewProjection,
                                 f32 viewportHeight);

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
    };
} // namespace OloEngine
