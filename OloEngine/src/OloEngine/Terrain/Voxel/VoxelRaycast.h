#pragma once

#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

namespace OloEngine
{
    struct VoxelRayHit
    {
        bool Hit = false;
        VoxelGridCoord Voxel;
        // Outward normal of the face entered by the ray. It is zero only when
        // the ray starts inside a solid cell, where there is no entered face.
        glm::ivec3 FaceNormal{ 0 };
        f32 Distance = 0.0f;
        glm::vec3 Point{ 0.0f };
    };

    // Amanatides-Woo traversal over VoxelOverride's sparse grid. Missing
    // chunks are simply empty cells, so traversal naturally crosses chunk
    // boundaries without a second-level chunk marcher.
    [[nodiscard]] VoxelRayHit RaycastVoxels(const VoxelOverride& voxels, const Ray& ray);
} // namespace OloEngine
