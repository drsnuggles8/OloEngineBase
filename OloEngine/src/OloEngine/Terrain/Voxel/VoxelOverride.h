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

    // Which mesher a voxel volume uses (issue #727). These are different tools,
    // not quality levels: marching cubes reads the SDF as a smooth isosurface
    // (caves, overhangs), greedy cubic reads it as solid/empty and merges
    // co-planar faces into maximal axis-aligned quads (blocky worlds).
    //
    // The numbering is serialized into scenes — append, never reorder.
    enum class VoxelMesherKind : u8
    {
        MarchingCubes = 0,
        GreedyCubic = 1
    };

    // A single voxel chunk: a dense 3D grid of SDF values.
    // Negative = solid (inside), positive = empty (outside).
    // Only allocated when modifications exist in this region.
    struct VoxelChunk
    {
        static constexpr u32 CHUNK_SIZE = 32; // 32³ voxels per chunk
        static constexpr u32 TOTAL_VOXELS = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

        std::vector<f32> SDFData; // Row-major [x + y*SIZE + z*SIZE*SIZE]

        // Optional per-voxel material index, same row-major indexing as SDFData.
        // Left EMPTY for chunks that only ever use material 0 (the marching-cubes
        // path never writes it), so the 32 KiB is paid only where a cubic chunk
        // actually paints strata. The greedy mesher takes a fully bitwise fast
        // path when this is empty — see VoxelGreedyMesher.
        std::vector<u8> MaterialData;

        bool Dirty = true; // Needs mesh rebuild

        VoxelChunk()
        {
            SDFData.resize(TOTAL_VOXELS, 1.0f); // Default: all empty (positive)
        }

        [[nodiscard]] f32& At(u32 x, u32 y, u32 z)
        {
            OLO_CORE_ASSERT(x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE, "VoxelChunk::At out of bounds");
            return SDFData[static_cast<sizet>(x) + static_cast<sizet>(y) * CHUNK_SIZE + static_cast<sizet>(z) * CHUNK_SIZE * CHUNK_SIZE];
        }

        [[nodiscard]] f32 At(u32 x, u32 y, u32 z) const
        {
            OLO_CORE_ASSERT(x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE, "VoxelChunk::At out of bounds");
            return SDFData[static_cast<sizet>(x) + static_cast<sizet>(y) * CHUNK_SIZE + static_cast<sizet>(z) * CHUNK_SIZE * CHUNK_SIZE];
        }

        [[nodiscard]] static sizet Index(u32 x, u32 y, u32 z)
        {
            return static_cast<sizet>(x) + static_cast<sizet>(y) * CHUNK_SIZE + static_cast<sizet>(z) * CHUNK_SIZE * CHUNK_SIZE;
        }

        // Material index of a voxel. Returns 0 for a chunk that never had one
        // written, which is exactly the "single material" case the mesher fast
        // path assumes.
        [[nodiscard]] u8 MaterialAt(u32 x, u32 y, u32 z) const
        {
            OLO_CORE_ASSERT(x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE, "VoxelChunk::MaterialAt out of bounds");
            return MaterialData.empty() ? u8{ 0 } : MaterialData[Index(x, y, z)];
        }

        // Writing any non-zero material allocates the array on first use.
        void SetMaterialAt(u32 x, u32 y, u32 z, u8 material)
        {
            OLO_CORE_ASSERT(x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE, "VoxelChunk::SetMaterialAt out of bounds");
            if (MaterialData.empty())
            {
                if (material == 0)
                {
                    return; // Still uniformly material 0 — stay unallocated.
                }
                MaterialData.assign(TOTAL_VOXELS, u8{ 0 });
            }
            MaterialData[Index(x, y, z)] = material;
        }
    };

    // Integer 3D coordinate for chunk addressing
    struct VoxelCoord
    {
        i32 X = 0;
        i32 Y = 0;
        i32 Z = 0;

        bool operator==(const VoxelCoord& other) const
        {
            return X == other.X && Y == other.Y && Z == other.Z;
        }
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

    // Integer coordinate of one cell in the unbounded voxel grid.  Chunks are
    // an implementation detail here: picking and brushes must not change their
    // behaviour at a 32-cell chunk edge.
    struct VoxelGridCoord
    {
        i32 X = 0;
        i32 Y = 0;
        i32 Z = 0;

        bool operator==(const VoxelGridCoord& other) const = default;
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

        // Fill the whole terrain extent with heightmap-derived voxels and paint
        // depth-based material strata (issue #727).
        //
        // The marching-cubes path deliberately leaves this volume SPARSE — it is
        // an override for carved caves and overhangs on top of a heightmap
        // terrain, so an empty map is the correct starting state. A cubic voxel
        // world is the opposite: the voxels ARE the terrain, so there has to be
        // a way to say "fill this extent from the height field". Callers gate
        // this on the greedy mesher being selected.
        //
        // Chunk count is capped; a request past the cap fills what it can and
        // warns rather than allocating unbounded memory from a scene file.
        //
        // Budget the default deliberately: a chunk is 32^3 voxels = 128 KiB of
        // SDFData, plus 32 KiB of MaterialData once PaintDepthStrata runs. At
        // the 1024 cap that is ~160 MiB resident. Typical scenes are nowhere
        // near it (a 256-unit world at VoxelSize 2 seeds 16 chunks = ~2.5 MiB);
        // the cap exists to stop a hand-edited scene file from asking for
        // gigabytes, not as a target.
        void SeedFromHeightmap(const TerrainData& terrainData, f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                               u32 maxChunks = 1024);

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

        [[nodiscard]] f32 GetVoxelSize() const
        {
            return m_VoxelSize;
        }

        // Sparse-grid cell access. A cell in a missing chunk is empty (SDF +1)
        // with material zero. SetVoxel creates the owning chunk only when a
        // caller actually writes a value.
        [[nodiscard]] f32 GetVoxelSDF(const VoxelGridCoord& voxel) const;
        [[nodiscard]] u8 GetVoxelMaterial(const VoxelGridCoord& voxel) const;
        void SetVoxel(const VoxelGridCoord& voxel, f32 sdf, u8 material = 0);

        [[nodiscard]] VoxelCoord GridToChunkCoord(const VoxelGridCoord& voxel) const;
        [[nodiscard]] VoxelGridCoord ChunkToGridCoord(const VoxelCoord& chunk, u32 lx, u32 ly, u32 lz) const;

        // A face on a chunk border changes its neighbour's mesh too. This is
        // deliberately public so a bulk editor can coalesce writes and still
        // preserve the same incremental-remesh contract as SetVoxel.
        void MarkVoxelAndNeighboursDirty(const VoxelGridCoord& voxel);
        [[nodiscard]] u32 GetChunkCount() const
        {
            return static_cast<u32>(m_Chunks.size());
        }

        // Access chunk map for serialization / iteration
        [[nodiscard]] const std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash>& GetChunks() const
        {
            return m_Chunks;
        }
        [[nodiscard]] std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash>& GetChunks()
        {
            return m_Chunks;
        }

        // RLE serialization
        [[nodiscard]] std::vector<u8> SerializeRLE() const;
        bool DeserializeRLE(const std::vector<u8>& data);

      private:
        // Get all chunks overlapping a sphere region
        void GetChunksInSphere(const glm::vec3& center, f32 radius, std::vector<VoxelCoord>& outCoords) const;

        // Paint surface/subsoil/bedrock material bands into one seeded chunk.
        void PaintDepthStrata(const VoxelCoord& coord);

        // Dirty the existing chunks sharing a face with the cell at local
        // (lx, ly, lz) of `chunkCoord`. Split out so a caller that already
        // holds the owning chunk does not pay to resolve it a second time.
        void MarkNeighbourChunksDirty(const VoxelCoord& chunkCoord, u32 lx, u32 ly, u32 lz);

        std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash> m_Chunks;
        f32 m_VoxelSize = 1.0f;
        f32 m_WorldSizeX = 256.0f;
        f32 m_WorldSizeZ = 256.0f;
        f32 m_HeightScale = 64.0f;
    };
} // namespace OloEngine
