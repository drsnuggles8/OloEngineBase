#pragma once

#include "OloEngine/Terrain/Voxel/VoxelRaycast.h"

#include <optional>
#include <unordered_map>

namespace OloEngine
{
    enum class VoxelBrushOperation : u8
    {
        Place,
        Carve,
        Fill,
        Paint
    };

    struct VoxelBrushSettings
    {
        VoxelBrushOperation Operation = VoxelBrushOperation::Carve;
        // World-space radius; a zero radius still edits precisely one cell.
        f32 Radius = 0.0f;
        u8 Material = 0;
    };

    // Whole-chunk snapshots make a sparse-volume stroke lossless: undoing an
    // edit that created a chunk removes it again, rather than leaving an empty
    // allocation behind. Only chunks whose cells actually changed are kept.
    struct VoxelEditStroke
    {
        std::unordered_map<VoxelCoord, std::optional<VoxelChunk>, VoxelCoordHash> Before;
        std::unordered_map<VoxelCoord, std::optional<VoxelChunk>, VoxelCoordHash> After;

        [[nodiscard]] bool Empty() const
        {
            return Before.empty();
        }
        void ApplyBefore(VoxelOverride& voxels) const;
        void ApplyAfter(VoxelOverride& voxels) const;
    };

    [[nodiscard]] VoxelEditStroke ApplyVoxelBrush(VoxelOverride& voxels, const VoxelRayHit& hit,
                                                  const VoxelBrushSettings& settings);
} // namespace OloEngine
