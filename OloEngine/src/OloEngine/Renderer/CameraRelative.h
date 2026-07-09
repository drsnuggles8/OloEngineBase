#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

// =============================================================================
// Camera-relative rendering (issue #429, first slice)
// =============================================================================
//
// Large world coordinates (tens of kilometres from the world origin) exceed the
// usable precision of f32: a mesh at world x = 40000 is stored with a ULP of
// ~0.004, and the GPU then computes gl_Position = ViewProjection * worldPos with
// operands of magnitude ~40000 whose true result is small (near the camera).
// That is catastrophic cancellation, and because the camera translation changes
// every frame the rounding changes too — the visible symptom is vertex jitter
// and shadow "swim".
//
// The fix is to render in a coordinate space *centred on the camera*: pick a
// per-frame "render origin" O near the camera, subtract it from every world
// position/matrix BEFORE it reaches the GPU, and the GPU then works entirely
// with small coordinates where f32 has abundant precision. The lighting math is
// unchanged because it is built from differences (V = cameraPos - worldPos,
// lightDir = lightPos - worldPos): subtracting a common O from both operands of
// every such difference leaves the result invariant.
//
// O is *snapped to a coarse grid* rather than tracking the exact camera
// position. This has two important consequences:
//   1. Within the first grid cell (camera within +/- gridSize/2 of the world
//      origin) O is exactly (0,0,0), so the whole transform is a byte-identical
//      no-op — every existing near-origin scene renders exactly as before.
//   2. O only changes when the camera crosses a cell boundary, so between
//      crossings the relative coordinates of static geometry are bit-stable
//      frame to frame (no per-frame micro-jitter, and CSM texel snapping stays
//      aligned).
//
// The residual error floor is the ULP of the *stored* world coordinate itself
// (the ECS still holds f32 world positions), which is static per object and
// therefore never manifests as motion. See docs/agent-rules/camera-relative-
// rendering.md for the full enumerated list of GPU upload sites this touches.

namespace OloEngine
{
    // Half the grid cell is the maximum distance of the camera from the render
    // origin, and therefore the magnitude of the largest relative coordinate the
    // GPU sees for on-screen geometry. 1024 keeps that under +/- 512 (ULP
    // ~6e-5 — far below a pixel) while being coarse enough that cell crossings
    // are rare during normal movement.
    inline constexpr f32 kRenderOriginGridSize = 1024.0f;

    // Snap a camera world position to the render-origin grid. Returns (0,0,0)
    // for any camera within the first cell so the whole feature is a no-op near
    // the world origin. gridSize <= 0 disables snapping (origin == exact camera
    // position); callers should pass the default.
    [[nodiscard]] inline glm::vec3 ComputeRenderOrigin(const glm::vec3& cameraWorldPos,
                                                       f32 gridSize = kRenderOriginGridSize)
    {
        if (gridSize <= 0.0f)
            return cameraWorldPos;

        return glm::vec3(
            std::round(cameraWorldPos.x / gridSize) * gridSize,
            std::round(cameraWorldPos.y / gridSize) * gridSize,
            std::round(cameraWorldPos.z / gridSize) * gridSize);
    }

    // Shift a world-space model/object matrix into render-relative space:
    //   modelRel = translate(-origin) * modelWorld
    // A translation only affects the 4th column, so this is exactly modelWorld
    // with its translation column reduced by origin — the rotation/scale 3x3 is
    // untouched. The subtraction (large world translation minus large origin)
    // is done once on the CPU; its result is small and, because origin is
    // grid-snapped, identical every frame while the camera stays in one cell.
    [[nodiscard]] inline glm::mat4 MakeModelRelative(const glm::mat4& modelWorld, const glm::vec3& origin)
    {
        glm::mat4 modelRel = modelWorld;
        modelRel[3][0] -= origin.x;
        modelRel[3][1] -= origin.y;
        modelRel[3][2] -= origin.z;
        return modelRel;
    }

    // Shift a world-space *point* into render-relative space (worldPos - origin).
    [[nodiscard]] inline glm::vec3 MakePositionRelative(const glm::vec3& worldPos, const glm::vec3& origin)
    {
        return worldPos - origin;
    }

    // Build a render-relative view matrix from a world view matrix:
    //   viewRel = viewWorld * translate(origin)
    // so that viewRel * (worldPos - origin) == viewWorld * worldPos. Because
    // translate(origin) only touches the 4th column, viewRel keeps viewWorld's
    // rotation 3x3 and recomputes only its translation column — which becomes
    // -R*(cameraPos - origin), a small value near the camera.
    [[nodiscard]] inline glm::mat4 MakeViewRelative(const glm::mat4& viewWorld, const glm::vec3& origin)
    {
        glm::mat4 viewRel = viewWorld;
        viewRel[3] = viewWorld * glm::vec4(origin, 1.0f);
        return viewRel;
    }

    // Build a render-relative view-projection (or any camera-space *) matrix
    // from its world counterpart:  mRel = mWorld * translate(origin), so that
    // mRel * (worldPos - origin) == mWorld * worldPos. Used for the main
    // view-projection, the previous-frame view-projection (motion vectors), and
    // every light-space / shadow matrix — all of which consume world positions
    // that are now supplied relative.
    [[nodiscard]] inline glm::mat4 MakeViewProjectionRelative(const glm::mat4& vpWorld, const glm::vec3& origin)
    {
        glm::mat4 vpRel = vpWorld;
        vpRel[3] = vpWorld * glm::vec4(origin, 1.0f);
        return vpRel;
    }
} // namespace OloEngine
