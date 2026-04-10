#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <cstring>
#include <vector>

namespace OloEngine
{
    // Single LOD level with a mesh and a distance threshold
    struct LODLevel
    {
        AssetHandle MeshHandle = 0; // Mesh asset handle for this LOD
        f32 MaxDistance = 0.0f;     // Maximum display distance for this level
        u32 TriangleCount = 0;      // Optional triangle count for debugging

        LODLevel() = default;
        LODLevel(AssetHandle meshHandle, f32 maxDistance, u32 triangleCount = 0)
            : MeshHandle(meshHandle), MaxDistance(maxDistance), TriangleCount(triangleCount) {}

        // Bitwise-exact comparison for undo/redo change detection (memcmp for float per rule 2b).
        auto operator==(const LODLevel& other) const -> bool
        {
            return MeshHandle == other.MeshHandle && std::memcmp(&MaxDistance, &other.MaxDistance, sizeof(f32)) == 0 && TriangleCount == other.TriangleCount;
        }
    };

    // Group of LOD levels, sorted by ascending distance
    struct LODGroup
    {
        std::vector<LODLevel> Levels; // Sorted by ascending MaxDistance
        f32 Bias = 1.0f;              // Multiplier for tuning LOD selection distances

        LODGroup() = default;

        auto operator==(const LODGroup& other) const -> bool
        {
            return Levels == other.Levels && std::memcmp(&Bias, &other.Bias, sizeof(f32)) == 0;
        }

        // Returns the index of the appropriate LODLevel for the given distance.
        // Returns -1 if the group has no levels.
        [[nodiscard]] i32 SelectLOD(f32 distance) const;
    };

    // Result of LOD mesh selection
    struct LODSelectionResult
    {
        i32 SelectedLODIndex = -1;
        bool Switched = false;
    };

    // Resolves the appropriate mesh for a given LOD group and viewing distance.
    // Returns the LOD mesh if a valid one exists, otherwise the original mesh.
    [[nodiscard]] LODSelectionResult SelectLODMesh(
        const Ref<class Mesh>& mesh,
        const glm::mat4& modelMatrix,
        const glm::vec3& viewPosition,
        const LODGroup* lodGroup,
        Ref<class Mesh>& outMesh);
} // namespace OloEngine
