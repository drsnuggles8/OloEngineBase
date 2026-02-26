#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Task/ParallelFor.h"

namespace OloEngine
{
    void TerrainChunkManager::GenerateAllChunks(const TerrainData& terrainData,
                                                f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        u32 resolution = terrainData.GetResolution();
        if (resolution == 0)
        {
            OLO_CORE_WARN("TerrainChunkManager: Cannot generate chunks — heightmap resolution is 0");
            m_Chunks.clear();
            m_SelectedChunks.clear();
            m_NumChunksX = 0;
            m_NumChunksZ = 0;
            return;
        }

        // Determine chunk grid size based on heightmap resolution (ceil to ensure full coverage)
        m_NumChunksX = std::max(1u, (resolution + TerrainChunk::CHUNK_RESOLUTION - 1) / TerrainChunk::CHUNK_RESOLUTION);
        m_NumChunksZ = std::max(1u, (resolution + TerrainChunk::CHUNK_RESOLUTION - 1) / TerrainChunk::CHUNK_RESOLUTION);

        u32 totalChunks = m_NumChunksX * m_NumChunksZ;
        m_Chunks.resize(totalChunks);

        // Build chunk geometry in parallel (CPU only, no GL calls)
        ParallelFor("TerrainChunkBuild", static_cast<i32>(totalChunks),
                    [this, &terrainData, worldSizeX, worldSizeZ, heightScale](i32 index)
                    {
                        u32 cx = static_cast<u32>(index) % m_NumChunksX;
                        u32 cz = static_cast<u32>(index) / m_NumChunksX;
                        m_Chunks[static_cast<sizet>(index)].BuildGeometry(
                            terrainData, cx, cz, m_NumChunksX, m_NumChunksZ,
                            worldSizeX, worldSizeZ, heightScale);
                    });

        // Upload to GPU sequentially on the main/GL thread
        for (auto& chunk : m_Chunks)
        {
            chunk.UploadToGPU();
        }

        // Build quadtree for LOD selection
        // Max depth is ceil(log2) of number of chunks on one axis (so leaf = one chunk)
        u32 quadtreeDepth = 0;
        {
            u32 n = std::max(m_NumChunksX, m_NumChunksZ);
            while ((1u << quadtreeDepth) < n)
            {
                ++quadtreeDepth;
            }
        }
        quadtreeDepth = std::max(quadtreeDepth, 2u); // At least 2 levels
        m_Quadtree.Build(terrainData, worldSizeX, worldSizeZ, heightScale, quadtreeDepth);

        OLO_CORE_INFO("TerrainChunkManager: Built {}x{} chunks ({} total), quadtree depth {}",
                      m_NumChunksX, m_NumChunksZ, totalChunks, quadtreeDepth);
    }

    void TerrainChunkManager::RebuildChunk(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                                           f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (chunkX >= m_NumChunksX || chunkZ >= m_NumChunksZ)
        {
            return;
        }

        sizet index = static_cast<sizet>(chunkZ) * m_NumChunksX + chunkX;
        m_Chunks[index].Build(terrainData, chunkX, chunkZ, m_NumChunksX, m_NumChunksZ,
                              worldSizeX, worldSizeZ, heightScale);
    }

    void TerrainChunkManager::SelectVisibleChunks(const Frustum& frustum,
                                                  const glm::vec3& cameraPos,
                                                  const glm::mat4& viewProjection,
                                                  f32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_SelectedChunks.clear();

        // Run quadtree LOD selection
        m_Quadtree.SelectLOD(frustum, cameraPos, viewProjection, viewportHeight);

        const auto& selectedNodes = m_Quadtree.GetSelectedNodes();
        m_SelectedChunks.reserve(selectedNodes.size());

        for (const auto* node : selectedNodes)
        {
            const TerrainChunk* chunk = FindChunkForNode(*node);
            if (chunk && chunk->IsBuilt())
            {
                TerrainRenderChunk rc;
                rc.Chunk = chunk;
                rc.LODData = m_Quadtree.GetChunkLODData(*node);
                m_SelectedChunks.push_back(rc);
            }
        }
    }

    void TerrainChunkManager::GetVisibleChunks(const Frustum& frustum,
                                               std::vector<const TerrainChunk*>& outChunks) const
    {
        OLO_PROFILE_FUNCTION();

        outChunks.clear();
        outChunks.reserve(m_Chunks.size());

        for (const auto& chunk : m_Chunks)
        {
            if (!chunk.IsBuilt())
            {
                continue;
            }

            const auto& bounds = chunk.GetBounds();
            if (frustum.IsBoxVisible(bounds.Min, bounds.Max))
            {
                outChunks.push_back(&chunk);
            }
        }
    }

    void TerrainChunkManager::GetAllChunks(std::vector<const TerrainChunk*>& outChunks) const
    {
        OLO_PROFILE_FUNCTION();

        outChunks.clear();
        outChunks.reserve(m_Chunks.size());

        for (const auto& chunk : m_Chunks)
        {
            if (chunk.IsBuilt())
            {
                outChunks.push_back(&chunk);
            }
        }
    }

    const TerrainChunk* TerrainChunkManager::FindChunkForNode(const TerrainQuadNode& node) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_NumChunksX == 0 || m_NumChunksZ == 0)
        {
            return nullptr;
        }

        // Map the quadtree node center to a chunk grid coordinate
        f32 centerX = (node.MinX + node.MaxX) * 0.5f;
        f32 centerZ = (node.MinZ + node.MaxZ) * 0.5f;

        u32 cx = static_cast<u32>(centerX * static_cast<f32>(m_NumChunksX));
        u32 cz = static_cast<u32>(centerZ * static_cast<f32>(m_NumChunksZ));
        cx = std::min(cx, m_NumChunksX - 1);
        cz = std::min(cz, m_NumChunksZ - 1);

        sizet idx = static_cast<sizet>(cz) * m_NumChunksX + cx;
        return &m_Chunks[idx];
    }
} // namespace OloEngine
