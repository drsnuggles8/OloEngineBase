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
        OLO_PROFILE_FUNCTION();

        BuildGeometry(terrainData, chunkX, chunkZ, numChunksX, numChunksZ,
                      worldSizeX, worldSizeZ, heightScale);
        UploadToGPU();
    }

    void TerrainChunk::BuildGeometry(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                                     u32 numChunksX, u32 numChunksZ,
                                     f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (numChunksX == 0 || numChunksZ == 0)
        {
            OLO_CORE_WARN("TerrainChunk::BuildGeometry - Zero chunk count");
            return;
        }

        u32 vertsPerSide = CHUNK_RESOLUTION + 1;
        u32 vertCount = vertsPerSide * vertsPerSide;
        u32 quadCount = CHUNK_RESOLUTION * CHUNK_RESOLUTION;
        m_IndexCount = quadCount * 6;

        m_StagedVertices.Reset();
        m_StagedVertices.Reserve(vertCount);

        m_StagedIndices.Reset();
        m_StagedIndices.Reserve(m_IndexCount);

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

                m_StagedVertices.Emplace(
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
                m_StagedIndices.Add(topLeft);
                m_StagedIndices.Add(bottomLeft);
                m_StagedIndices.Add(topRight);

                // Second triangle
                m_StagedIndices.Add(topRight);
                m_StagedIndices.Add(bottomLeft);
                m_StagedIndices.Add(bottomRight);
            }
        }

        // The shadow levels (TerrainShadowLod): the same grid at a stride of 2,
        // 4 and 8 quads, indexing the vertices above. Same winding.
        m_StagedShadowIndices.Reset();
        m_ShadowLodIndexCounts.Reset();
        for (u32 level = 1; level < TerrainShadowLod::kLevelCount; ++level)
        {
            const u32 stride = 1u << level;
            const u32 quads = CHUNK_RESOLUTION / stride;
            for (u32 z = 0; z < quads; ++z)
            {
                for (u32 x = 0; x < quads; ++x)
                {
                    const u32 topLeft = (z * stride) * vertsPerSide + x * stride;
                    const u32 topRight = topLeft + stride;
                    const u32 bottomLeft = ((z + 1) * stride) * vertsPerSide + x * stride;
                    const u32 bottomRight = bottomLeft + stride;
                    m_StagedShadowIndices.Add(topLeft);
                    m_StagedShadowIndices.Add(bottomLeft);
                    m_StagedShadowIndices.Add(topRight);
                    m_StagedShadowIndices.Add(topRight);
                    m_StagedShadowIndices.Add(bottomLeft);
                    m_StagedShadowIndices.Add(bottomRight);
                }
            }
            m_ShadowLodIndexCounts.Add(quads * quads * 6u);
        }
    }

    void TerrainChunk::UploadToGPU()
    {
        OLO_PROFILE_FUNCTION();

        if (m_StagedVertices.IsEmpty() || m_StagedIndices.IsEmpty())
        {
            return;
        }

        m_VAO = VertexArray::Create();

        auto vbo = VertexBuffer::Create(m_StagedVertices.GetData(), static_cast<u32>(m_StagedVertices.Num() * sizeof(TerrainVertex)));
        vbo->SetLayout(TerrainVertex::GetLayout());
        m_VAO->AddVertexBuffer(vbo);

        auto ibo = IndexBuffer::Create(m_StagedIndices.GetData(), static_cast<u32>(m_StagedIndices.Num()));
        m_VAO->SetIndexBuffer(ibo);

        // One vertex array per shadow level, over the vertex buffer above.
        m_ShadowLodVAOs.Reset();
        u32 first = 0;
        for (const u32 count : m_ShadowLodIndexCounts)
        {
            auto levelVAO = VertexArray::Create();
            levelVAO->AddVertexBuffer(vbo);
            levelVAO->SetIndexBuffer(IndexBuffer::Create(m_StagedShadowIndices.GetData() + first, count));
            m_ShadowLodVAOs.Add(levelVAO);
            first += count;
        }

        // Free staging memory
        m_StagedVertices.Reset();
        m_StagedVertices.Shrink();
        m_StagedIndices.Reset();
        m_StagedIndices.Shrink();
        m_StagedShadowIndices.Reset();
        m_StagedShadowIndices.Shrink();
    }
} // namespace OloEngine
