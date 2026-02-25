#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class TerrainData;

    // A single voxel chunk: a dense 3D grid of SDF values.
    // Negative = solid (inside), positive = empty (outside).
    // Only allocated when modifications exist in this region.
    struct VoxelChunk
    {
        static constexpr u32 CHUNK_SIZE = 32; // 32³ voxels per chunk
        static constexpr u32 TOTAL_VOXELS = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

        std::vector<f32> SDFData; // Row-major [x + y*SIZE + z*SIZE*SIZE]
        bool Dirty = true;        // Needs mesh rebuild

        VoxelChunk()
        {
            SDFData.resize(TOTAL_VOXELS, 1.0f); // Default: all empty (positive)
        }

        [[nodiscard]] f32& At(u32 x, u32 y, u32 z)
        {
            return SDFData[static_cast<sizet>(x) + static_cast<sizet>(y) * CHUNK_SIZE + static_cast<sizet>(z) * CHUNK_SIZE * CHUNK_SIZE];
        }

        [[nodiscard]] f32 At(u32 x, u32 y, u32 z) const
        {
            return SDFData[static_cast<sizet>(x) + static_cast<sizet>(y) * CHUNK_SIZE + static_cast<sizet>(z) * CHUNK_SIZE * CHUNK_SIZE];
        }
    };

    // Integer 3D coordinate for chunk addressing
    struct VoxelCoord
    {
        i32 X = 0;
        i32 Y = 0;
        i32 Z = 0;

        bool operator==(const VoxelCoord& other) const { return X == other.X && Y == other.Y && Z == other.Z; }
    };

    struct VoxelCoordHash
    {
        sizet operator()(const VoxelCoord& c) const
        {
            sizet h = std::hash<i32>{}(c.X);
            h ^= std::hash<i32>{}(c.Y) * 0x9E3779B97F4A7C15ULL + 0x9E3779B9ULL + (h << 6) + (h >> 2);
            h ^= std::hash<i32>{}(c.Z) * 0x517CC1B727220A95ULL + 0x9E3779B9ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Sparse 3D SDF grid overlaid on the heightmap terrain.
    // Only regions with explicit modifications store voxel data.
    // Used for caves, overhangs, and other non-heightmap geometry.
    class VoxelOverride : public RefCounted
    {
      public:
        VoxelOverride() = default;
        ~VoxelOverride() override = default;

        // Initialize with terrain world-space dimensions
        void Initialize(f32 worldSizeX, f32 worldSizeZ, f32 heightScale, f32 voxelSize = 1.0f);

        // Sphere carve: set SDF to empty (positive) in a sphere region
        void CarveSphere(const glm::vec3& center, f32 radius);

        // Sphere add: set SDF to solid (negative) in a sphere region
        void AddSphere(const glm::vec3& center, f32 radius);

        // Initialize SDF values for a chunk from the heightmap surface
        void InitializeChunkFromHeightmap(const VoxelCoord& coord, const TerrainData& terrainData,
                                          f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // Get or create the chunk at the given coordinate
        VoxelChunk& GetOrCreateChunk(const VoxelCoord& coord);

        // Check if a chunk exists at the given coordinate
        [[nodiscard]] bool HasChunk(const VoxelCoord& coord) const;

        // Get all dirty chunks (those needing mesh rebuild)
        void GetDirtyChunks(std::vector<VoxelCoord>& outCoords) const;

        // Mark a chunk as clean (after mesh has been rebuilt)
        void MarkChunkClean(const VoxelCoord& coord);

        // Convert world position to chunk coordinate
        [[nodiscard]] VoxelCoord WorldToChunkCoord(const glm::vec3& worldPos) const;

        // Convert chunk coordinate + local voxel index to world position
        [[nodiscard]] glm::vec3 VoxelToWorld(const VoxelCoord& chunkCoord, u32 lx, u32 ly, u32 lz) const;

        // Get world-space bounding box for a chunk
        [[nodiscard]] BoundingBox GetChunkBounds(const VoxelCoord& coord) const;

        [[nodiscard]] f32 GetVoxelSize() const { return m_VoxelSize; }
        [[nodiscard]] u32 GetChunkCount() const { return static_cast<u32>(m_Chunks.size()); }

        // Access chunk map for serialization / iteration
        [[nodiscard]] const std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash>& GetChunks() const { return m_Chunks; }
        [[nodiscard]] std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash>& GetChunks() { return m_Chunks; }

        // RLE serialization
        [[nodiscard]] std::vector<u8> SerializeRLE() const;
        bool DeserializeRLE(const std::vector<u8>& data);

      private:
        // Get all chunks overlapping a sphere region
        void GetChunksInSphere(const glm::vec3& center, f32 radius, std::vector<VoxelCoord>& outCoords) const;

        std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash> m_Chunks;
        f32 m_VoxelSize = 1.0f;
        f32 m_WorldSizeX = 256.0f;
        f32 m_WorldSizeZ = 256.0f;
        f32 m_HeightScale = 64.0f;
    };
} // namespace OloEngine
