#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

namespace OloEngine
{
    // Vertex output from Marching Cubes
    struct VoxelVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    // A renderable mesh generated from one voxel chunk via Marching Cubes
    struct VoxelMesh
    {
        VoxelCoord ChunkCoord;
        BoundingBox Bounds;
        Ref<VertexArray> VAO;
        u32 IndexCount = 0;
    };

    // Generates meshes from voxel SDF data using the Marching Cubes algorithm.
    // Normals are computed from the SDF gradient (central differences).
    class MarchingCubes
    {
      public:
        // Generate a mesh from a single voxel chunk.
        // Returns true if the chunk has a non-empty isosurface.
        static bool GenerateMesh(const VoxelChunk& chunk, const VoxelCoord& coord,
                                 f32 voxelSize, VoxelMesh& outMesh);

        // Generate meshes for all dirty chunks in a VoxelOverride.
        // Meshes are stored in the provided map, keyed by chunk coordinate.
        static void RebuildDirtyMeshes(VoxelOverride& voxels,
                                       std::unordered_map<VoxelCoord, VoxelMesh, VoxelCoordHash>& meshes);

      private:
        // Edge table and triangle table for MC lookup
        static const i32 s_EdgeTable[256];
        static const i32 s_TriTable[256][16];

        // Interpolate vertex position along an edge based on SDF values
        static glm::vec3 InterpolateEdge(const glm::vec3& p1, const glm::vec3& p2, f32 v1, f32 v2);

        // Compute SDF gradient (normal direction) at a given voxel position
        static glm::vec3 ComputeGradient(const VoxelChunk& chunk, u32 x, u32 y, u32 z);
    };
} // namespace OloEngine
