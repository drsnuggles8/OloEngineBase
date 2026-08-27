#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelRaycast.h"

#include <cmath>
#include <limits>

namespace OloEngine
{
    VoxelRayHit RaycastVoxels(const VoxelOverride& voxels, const Ray& ray)
    {
        constexpr f32 epsilon = 1.0e-8f;
        if (ray.TMax < ray.TMin || glm::dot(ray.Direction, ray.Direction) <= epsilon * epsilon)
            return {};

        if (voxels.GetChunks().empty())
            return {};

        glm::vec3 boundsMin(std::numeric_limits<f32>::infinity());
        glm::vec3 boundsMax(-std::numeric_limits<f32>::infinity());
        for (const auto& entry : voxels.GetChunks())
        {
            const BoundingBox bounds = voxels.GetChunkBounds(entry.first);
            boundsMin = glm::min(boundsMin, bounds.Min);
            boundsMax = glm::max(boundsMax, bounds.Max);
        }

        f32 tEnter = ray.TMin;
        f32 tExit = ray.TMax;
        glm::ivec3 enteredFace{ 0 };
        for (glm::length_t axis = 0; axis < 3; ++axis)
        {
            const f32 direction = ray.Direction[axis];
            if (std::abs(direction) <= epsilon)
            {
                if (ray.Origin[axis] < boundsMin[axis] || ray.Origin[axis] > boundsMax[axis])
                    return {};
                continue;
            }
            const f32 first = (boundsMin[axis] - ray.Origin[axis]) / direction;
            const f32 second = (boundsMax[axis] - ray.Origin[axis]) / direction;
            // Not `near`/`far`: those are object-like macros in the Windows SDK
            // headers, so a local of either name is a compile error the moment
            // any translation unit in the include graph pulls windows.h in.
            const f32 slabEnter = glm::min(first, second);
            const f32 slabExit = glm::max(first, second);
            if (slabEnter > tEnter)
            {
                tEnter = slabEnter;
                enteredFace = glm::ivec3(0);
                enteredFace[axis] = direction > 0.0f ? -1 : 1;
            }
            tExit = glm::min(tExit, slabExit);
            if (tExit < tEnter)
                return {};
        }

        const f32 size = voxels.GetVoxelSize();
        // Nudge into the box so a negative-direction ray entering exactly on
        // its maximum plane floors to the owned cell, not the cell beyond it.
        const f32 traversalStart = glm::min(tExit, tEnter + epsilon * 4.0f);
        const glm::vec3 start = ray.At(traversalStart);
        VoxelGridCoord cell{
            static_cast<i32>(std::floor(start.x / size)),
            static_cast<i32>(std::floor(start.y / size)),
            static_cast<i32>(std::floor(start.z / size))
        };

        glm::ivec3 step{ 0 };
        glm::vec3 tMax{ std::numeric_limits<f32>::infinity() };
        glm::vec3 tDelta{ std::numeric_limits<f32>::infinity() };
        for (glm::length_t axis = 0; axis < 3; ++axis)
        {
            const f32 direction = ray.Direction[axis];
            if (std::abs(direction) <= epsilon)
                continue;
            step[axis] = direction > 0.0f ? 1 : -1;
            const i32 coordinate = axis == 0 ? cell.X : (axis == 1 ? cell.Y : cell.Z);
            const f32 axisBoundary = static_cast<f32>(coordinate + (step[axis] > 0 ? 1 : 0)) * size;
            tMax[axis] = traversalStart + (axisBoundary - start[axis]) / direction;
            tDelta[axis] = size / std::abs(direction);
        }

        f32 t = traversalStart;
        while (t <= tExit)
        {
            if (voxels.GetVoxelSDF(cell) < 0.0f)
                return { true, cell, enteredFace, t, ray.At(t) };

            glm::length_t axis = 0;
            if (tMax.y < tMax[axis])
                axis = 1;
            if (tMax.z < tMax[axis])
                axis = 2;
            t = tMax[axis];
            if (t > tExit)
                break;

            if (axis == 0)
                cell.X += step.x;
            else if (axis == 1)
                cell.Y += step.y;
            else
                cell.Z += step.z;
            enteredFace = glm::ivec3(0);
            enteredFace[axis] = -step[axis];
            tMax[axis] += tDelta[axis];
        }
        return {};
    }
} // namespace OloEngine
