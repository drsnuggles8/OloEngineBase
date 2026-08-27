#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"

#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        void ApplySnapshot(VoxelOverride& voxels,
                           const std::unordered_map<VoxelCoord, std::optional<VoxelChunk>, VoxelCoordHash>& snapshots)
        {
            auto& chunks = voxels.GetChunks();
            for (const auto& [coord, snapshot] : snapshots)
            {
                if (snapshot)
                    chunks.insert_or_assign(coord, *snapshot);
                else
                    chunks.erase(coord);
            }

            // Boundary faces are shared by two chunk meshes. Every restored
            // chunk and its existing six neighbours need a fresh neighbourhood.
            for (const auto& entry : snapshots)
            {
                const VoxelCoord& coord = entry.first;
                static constexpr glm::ivec3 offsets[] = {
                    { 0, 0, 0 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
                };
                for (const glm::ivec3 offset : offsets)
                {
                    const VoxelCoord candidate{ coord.X + offset.x, coord.Y + offset.y, coord.Z + offset.z };
                    if (auto it = chunks.find(candidate); it != chunks.end())
                        it->second.Dirty = true;
                }
            }
        }
    } // namespace

    void VoxelEditStroke::ApplyBefore(VoxelOverride& voxels) const
    {
        ApplySnapshot(voxels, Before);
    }
    void VoxelEditStroke::ApplyAfter(VoxelOverride& voxels) const
    {
        ApplySnapshot(voxels, After);
    }

    VoxelEditStroke ApplyVoxelBrush(VoxelOverride& voxels, const VoxelRayHit& hit, const VoxelBrushSettings& settings)
    {
        VoxelEditStroke stroke;
        if (!hit.Hit || settings.Radius < 0.0f)
            return stroke;

        VoxelGridCoord center = hit.Voxel;
        if (settings.Operation == VoxelBrushOperation::Place)
        {
            center.X += hit.FaceNormal.x;
            center.Y += hit.FaceNormal.y;
            center.Z += hit.FaceNormal.z;
        }

        const f32 voxelSize = voxels.GetVoxelSize();
        const i32 extent = static_cast<i32>(std::ceil(settings.Radius / voxelSize));
        const glm::vec3 centerWorld = (glm::vec3(center.X, center.Y, center.Z) + glm::vec3(0.5f)) * voxelSize;
        for (i32 z = center.Z - extent; z <= center.Z + extent; ++z)
        {
            for (i32 y = center.Y - extent; y <= center.Y + extent; ++y)
            {
                for (i32 x = center.X - extent; x <= center.X + extent; ++x)
                {
                    const VoxelGridCoord cell{ x, y, z };
                    const glm::vec3 cellWorld = (glm::vec3(x, y, z) + glm::vec3(0.5f)) * voxelSize;
                    if (settings.Radius > 0.0f && glm::length(cellWorld - centerWorld) > settings.Radius)
                        continue;

                    const f32 oldSdf = voxels.GetVoxelSDF(cell);
                    const u8 oldMaterial = voxels.GetVoxelMaterial(cell);
                    f32 newSdf = oldSdf;
                    u8 newMaterial = oldMaterial;
                    switch (settings.Operation)
                    {
                        case VoxelBrushOperation::Place:
                            newSdf = -1.0f;
                            newMaterial = settings.Material;
                            break;
                        case VoxelBrushOperation::Carve:
                            newSdf = 1.0f;
                            break;
                        case VoxelBrushOperation::Fill:
                            if (oldSdf >= 0.0f)
                            {
                                newSdf = -1.0f;
                                newMaterial = settings.Material;
                            }
                            break;
                        case VoxelBrushOperation::Paint:
                            if (oldSdf < 0.0f)
                                newMaterial = settings.Material;
                            break;
                    }
                    // Bitwise, not tolerance: this decides whether the cell
                    // belongs in the undo snapshot, so "changed by less than an
                    // epsilon" still has to count as changed.
                    if (Math::BitwiseEqual(newSdf, oldSdf) && newMaterial == oldMaterial)
                        continue;

                    const VoxelCoord chunk = voxels.GridToChunkCoord(cell);
                    if (!stroke.Before.contains(chunk))
                    {
                        const auto found = voxels.GetChunks().find(chunk);
                        stroke.Before.emplace(chunk, found == voxels.GetChunks().end() ? std::nullopt : std::optional<VoxelChunk>(found->second));
                    }
                    voxels.SetVoxel(cell, newSdf, newMaterial);
                }
            }
        }
        for (const auto& [coord, unused] : stroke.Before)
        {
            const auto found = voxels.GetChunks().find(coord);
            stroke.After.emplace(coord, found == voxels.GetChunks().end() ? std::nullopt : std::optional<VoxelChunk>(found->second));
        }
        return stroke;
    }
} // namespace OloEngine
