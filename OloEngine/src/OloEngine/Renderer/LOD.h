#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vector>

namespace OloEngine
{
    // Single LOD level: a mesh plus the two independent selection metrics.
    //
    // `MaxDistance` is the legacy hand-authored threshold. `Error` is the
    // generated, view-independent one (issue #711): the accumulated
    // simplification error of this level relative to LOD 0, expressed as a
    // fraction of the model's largest bounding-box extent. Multiplying it by
    // the level's projected on-screen size yields an estimated pixel error,
    // which is what `LODGroup::SelectLODByPixelError` thresholds. LOD 0 is
    // exact, so its Error is 0; a hand-authored level that nothing measured
    // also carries 0, which is how `HasErrorData()` tells the two apart.
    struct LODLevel
    {
        AssetHandle MeshHandle = 0; // Mesh asset handle for this LOD
        f32 MaxDistance = 0.0f;     // Maximum display distance for this level
        u32 TriangleCount = 0;      // Optional triangle count for debugging
        f32 Error = 0.0f;           // Accumulated relative error vs LOD 0, in model-extent units

        LODLevel() = default;
        LODLevel(AssetHandle meshHandle, f32 maxDistance, u32 triangleCount = 0, f32 error = 0.0f)
            : MeshHandle(meshHandle), MaxDistance(maxDistance), TriangleCount(triangleCount), Error(error) {}

        // Bitwise-exact comparison for undo/redo change detection — float fields
        // use Math::BitwiseEqual per cpp-coding-quality §2a.
        auto operator==(const LODLevel& other) const -> bool
        {
            return MeshHandle == other.MeshHandle && Math::BitwiseEqual(MaxDistance, other.MaxDistance) &&
                   TriangleCount == other.TriangleCount && Math::BitwiseEqual(Error, other.Error);
        }
    };

    // Everything LOD selection needs about the viewer, gathered once per frame.
    //
    // Deliberately NOT the camera's view or projection MATRIX: pixel-error
    // selection must be independent of where the camera is *looking*, or the
    // selected level changes when the camera merely rotates in place and the
    // mesh pops. Only the camera POSITION, the field of view and the render
    // resolution are inputs — see EstimateProjectedPixelSize.
    struct LODViewParams
    {
        glm::vec3 ViewPosition{ 0.0f };
        // Render-target height in pixels. Pixel error scales with this, which is
        // what makes one threshold correct at both 1080p and 4K.
        u32 ScreenHeight = 1080;
        // tan(fovY / 2) of the render camera. 1.0 == a 90° vertical FOV.
        f32 TanHalfFovY = 1.0f;
        // Estimated pixel error a level may introduce before it is rejected.
        // 1 px is imperceptible; larger values trade fidelity for triangles.
        f32 PixelErrorThreshold = 1.0f;
    };

    // Group of LOD levels, ordered coarsest-last.
    struct LODGroup
    {
        std::vector<LODLevel> Levels; // Sorted by ascending MaxDistance / Error
        f32 Bias = 1.0f;              // Multiplier for tuning LOD selection distances

        LODGroup() = default;

        auto operator==(const LODGroup& other) const -> bool
        {
            return Levels == other.Levels && Math::BitwiseEqual(Bias, other.Bias);
        }

        // Returns the index of the appropriate LODLevel for the given distance.
        // Returns -1 if the group has no levels.
        [[nodiscard]] i32 SelectLOD(f32 distance) const;

        // True when at least one level carries generated error data, i.e. the
        // group can be selected by pixel error instead of by authored distance.
        [[nodiscard]] bool HasErrorData() const;

        // Selects the coarsest level whose estimated pixel error
        // (`projectedPixelSize * Level.Error`) stays under the threshold,
        // scanning from fine to coarse and stopping at the first level that
        // exceeds it. Returns -1 for an empty group.
        //
        // `projectedPixelSize` comes from EstimateProjectedPixelSize.
        [[nodiscard]] i32 SelectLODByPixelError(f32 projectedPixelSize, f32 pixelErrorThreshold) const;
    };

    // Estimated on-screen size, in pixels, of a mesh with `localBounds` placed by
    // `modelMatrix` and viewed from `view.ViewPosition`.
    //
    // This is the orientation-independent projection the whole scheme rests on
    // (issue #711). The AABB is projected onto an image plane *rotated to face
    // the mesh from the camera position* rather than onto the camera's own image
    // plane, which has two consequences the camera-plane version does not have:
    //
    //   - the result depends on camera POSITION only, so a camera that rotates
    //     in place cannot change the selected level and meshes do not pop; and
    //   - it stays meaningful for geometry off-screen or behind the camera,
    //     which is what shadow casters (and, later, ray tracing) need.
    //
    // A camera-plane projection would pass every value-based test while
    // reintroducing exactly the orientation-dependent popping this replaces.
    [[nodiscard]] f32 EstimateProjectedPixelSize(const BoundingBox& localBounds,
                                                 const glm::mat4& modelMatrix,
                                                 const LODViewParams& view);

    // Result of LOD mesh selection
    struct LODSelectionResult
    {
        i32 SelectedLODIndex = -1;
        bool Switched = false;
    };

    // Resolves the appropriate mesh for a given LOD group and viewer.
    // Uses pixel-error selection when the group carries generated error data and
    // falls back to the authored distance thresholds when it does not.
    // Returns the LOD mesh if a valid one exists, otherwise the original mesh.
    [[nodiscard]] LODSelectionResult SelectLODMesh(
        const Ref<class Mesh>& mesh,
        const glm::mat4& modelMatrix,
        const LODViewParams& view,
        const LODGroup* lodGroup,
        Ref<class Mesh>& outMesh);
} // namespace OloEngine
