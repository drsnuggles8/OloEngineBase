#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Terrain/TerrainVertex.h"

#include <glm/glm.hpp>
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    class TerrainData;

    // A 64×64 quad chunk of terrain mesh, built from a heightmap region
    class TerrainChunk
    {
      public:
        static constexpr u32 CHUNK_RESOLUTION = 64;

        TerrainChunk() = default;

        // Build mesh from heightmap data for a given grid position
        // chunkX/chunkZ: chunk grid coordinates (0..numChunks-1)
        // worldSizeX/Z: total terrain world size
        // heightScale: vertical scale multiplier
        void Build(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                   u32 numChunksX, u32 numChunksZ,
                   f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // CPU-only geometry generation (thread-safe, no GL calls)
        void BuildGeometry(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                           u32 numChunksX, u32 numChunksZ,
                           f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // Upload staged geometry to GPU (must be called on main/GL thread)
        void UploadToGPU();

        [[nodiscard]] const Ref<VertexArray>& GetVertexArray() const
        {
            return m_VAO;
        }
        [[nodiscard]] u32 GetIndexCount() const
        {
            return m_IndexCount;
        }
        [[nodiscard]] const BoundingBox& GetBounds() const
        {
            return m_Bounds;
        }
        [[nodiscard]] bool IsBuilt() const
        {
            return m_VAO != nullptr;
        }

        friend struct TIsTriviallyRelocatable<TerrainChunk>;

      private:
        Ref<VertexArray> m_VAO;
        u32 m_IndexCount = 0;
        BoundingBox m_Bounds;

        // Staging buffers for CPU→GPU split
        TArray<TerrainVertex> m_StagedVertices;
        TArray<u32> m_StagedIndices;
    };

    // External intrusive VAO Ref, owned TArray staging buffers and scalar/bounds values.
    template<>
    struct TIsTriviallyRelocatable<TerrainChunk>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(TerrainChunk::m_VAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainChunk::m_IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainChunk::m_Bounds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainChunk::m_StagedVertices)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainChunk::m_StagedIndices)>::Value;
    };
} // namespace OloEngine
