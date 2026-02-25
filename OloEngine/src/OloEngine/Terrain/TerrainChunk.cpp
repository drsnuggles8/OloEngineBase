#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainVertex.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"

#include <vector>

namespace OloEngine
{
    void TerrainChunk::Build(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                             u32 numChunksX, u32 numChunksZ,
                             f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        BuildGeometry(terrainData, chunkX, chunkZ, numChunksX, numChunksZ,
                      worldSizeX, worldSizeZ, heightScale);
        UploadToGPU();
    }

    void TerrainChunk::BuildGeometry(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                                     u32 numChunksX, u32 numChunksZ,
                                     f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        u32 vertsPerSide = CHUNK_RESOLUTION + 1;
        u32 vertCount = vertsPerSide * vertsPerSide;
        u32 quadCount = CHUNK_RESOLUTION * CHUNK_RESOLUTION;
        m_IndexCount = quadCount * 6;

        m_StagedVertices.clear();
        m_StagedVertices.reserve(vertCount);

        m_StagedIndices.clear();
        m_StagedIndices.reserve(m_IndexCount);

        // Chunk world-space extents
        f32 chunkWorldW = worldSizeX / static_cast<f32>(numChunksX);
        f32 chunkWorldD = worldSizeZ / static_cast<f32>(numChunksZ);
        f32 chunkOriginX = static_cast<f32>(chunkX) * chunkWorldW;
        f32 chunkOriginZ = static_cast<f32>(chunkZ) * chunkWorldD;

        glm::vec3 boundsMin(chunkOriginX, std::numeric_limits<f32>::max(), chunkOriginZ);
        glm::vec3 boundsMax(chunkOriginX + chunkWorldW, std::numeric_limits<f32>::lowest(), chunkOriginZ + chunkWorldD);

        for (u32 z = 0; z < vertsPerSide; ++z)
        {
            for (u32 x = 0; x < vertsPerSide; ++x)
            {
                // Normalized UV across entire terrain [0, 1]
                f32 normX = (static_cast<f32>(chunkX) + static_cast<f32>(x) / static_cast<f32>(CHUNK_RESOLUTION)) / static_cast<f32>(numChunksX);
                f32 normZ = (static_cast<f32>(chunkZ) + static_cast<f32>(z) / static_cast<f32>(CHUNK_RESOLUTION)) / static_cast<f32>(numChunksZ);

                f32 height = terrainData.GetHeightAt(normX, normZ) * heightScale;
                glm::vec3 normal = terrainData.GetNormalAt(normX, normZ, worldSizeX, worldSizeZ, heightScale);

                f32 worldX = chunkOriginX + static_cast<f32>(x) / static_cast<f32>(CHUNK_RESOLUTION) * chunkWorldW;
                f32 worldZ = chunkOriginZ + static_cast<f32>(z) / static_cast<f32>(CHUNK_RESOLUTION) * chunkWorldD;

                m_StagedVertices.emplace_back(
                    glm::vec3(worldX, height, worldZ),
                    glm::vec2(normX, normZ),
                    normal);

                boundsMin.y = std::min(boundsMin.y, height);
                boundsMax.y = std::max(boundsMax.y, height);
            }
        }

        // Ensure non-degenerate bounds if terrain is completely flat
        if (boundsMin.y >= boundsMax.y)
        {
            boundsMin.y -= 0.01f;
            boundsMax.y += 0.01f;
        }

        m_Bounds = BoundingBox(boundsMin, boundsMax);

        // Generate triangle indices
        for (u32 z = 0; z < CHUNK_RESOLUTION; ++z)
        {
            for (u32 x = 0; x < CHUNK_RESOLUTION; ++x)
            {
                u32 topLeft = z * vertsPerSide + x;
                u32 topRight = topLeft + 1;
                u32 bottomLeft = (z + 1) * vertsPerSide + x;
                u32 bottomRight = bottomLeft + 1;

                // First triangle
                m_StagedIndices.push_back(topLeft);
                m_StagedIndices.push_back(bottomLeft);
                m_StagedIndices.push_back(topRight);

                // Second triangle
                m_StagedIndices.push_back(topRight);
                m_StagedIndices.push_back(bottomLeft);
                m_StagedIndices.push_back(bottomRight);
            }
        }
    }

    void TerrainChunk::UploadToGPU()
    {
        OLO_PROFILE_FUNCTION();

        if (m_StagedVertices.empty() || m_StagedIndices.empty())
        {
            return;
        }

        m_VAO = VertexArray::Create();

        auto vbo = VertexBuffer::Create(m_StagedVertices.data(), static_cast<u32>(m_StagedVertices.size() * sizeof(TerrainVertex)));
        vbo->SetLayout(TerrainVertex::GetLayout());
        m_VAO->AddVertexBuffer(vbo);

        auto ibo = IndexBuffer::Create(m_StagedIndices.data(), static_cast<u32>(m_StagedIndices.size()));
        m_VAO->SetIndexBuffer(ibo);

        // Free staging memory
        m_StagedVertices.clear();
        m_StagedVertices.shrink_to_fit();
        m_StagedIndices.clear();
        m_StagedIndices.shrink_to_fit();
    }
} // namespace OloEngine
