#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::PlanarReflection
{
    // =========================================================================
    // Pure planar-reflection math (no GL state, no renderer deps).
    //
    // A planar reflection renders the scene a second time from a camera mirrored
    // across a reflection plane (the water / mirror surface), with an oblique
    // near-clip plane coincident with that surface so geometry on the far side
    // of the plane is culled and cannot leak into the reflection. The water /
    // mirror material then samples the resulting texture projectively.
    //
    // Conventions (verified against the engine's GLM build — no
    // GLM_FORCE_DEPTH_ZERO_TO_ONE / GLM_FORCE_LEFT_HANDED):
    //   * Right-handed view space, OpenGL clip volume (NDC z in [-1, 1], near
    //     plane at clip z = -w). `glm::perspective` == `perspectiveRH_NO`.
    //   * Matrices are column-major (`m[col][row]`); points are column vectors
    //     transformed as `m * p`.
    //   * A plane is `vec4(n.xyz, d)` with the half-space test `dot(n, p) + d`;
    //     a point is on the plane when that is 0, on the "kept" side when > 0.
    //
    // The math is pinned by PlanarReflectionMathTest.cpp.
    // =========================================================================

    /// Normalize a plane so its normal (xyz) is unit length, scaling d to match.
    /// Returns the input unchanged if the normal is degenerate (near-zero
    /// length) so callers never divide by zero.
    [[nodiscard]] glm::vec4 NormalizePlane(const glm::vec4& plane);

    /// World-space reflection (Householder) matrix that mirrors points across
    /// `plane` (expected normalized — pass through NormalizePlane first). For a
    /// horizontal water plane `vec4(0,1,0,-h)` this maps (x,y,z) → (x, 2h-y, z).
    /// The matrix has determinant -1, so it flips winding order — the reflection
    /// pass must invert front-face culling when rendering with a mirrored view.
    [[nodiscard]] glm::mat4 MakeReflectionMatrix(const glm::vec4& plane);

    /// Reflect a single world-space point across `plane` (normalized).
    [[nodiscard]] glm::vec3 ReflectPoint(const glm::vec4& plane, const glm::vec3& point);

    /// View matrix for the mirrored camera: `view * reflection(plane)`. Rendering
    /// the *unmodified* world geometry with this view produces the reflected
    /// image (a world point p is reflected to R*p, then viewed: view*(R*p)).
    /// `worldPlane` is normalized internally.
    [[nodiscard]] glm::mat4 MakeMirrorView(const glm::mat4& view, const glm::vec4& worldPlane);

    /// Transform a world-space plane into the space of `transform` (the matrix
    /// that maps points world → that space). Planes are covectors, so this is
    /// `transpose(inverse(transform)) * worldPlane`. Used to express the
    /// reflection plane in mirror-view space for the oblique clip.
    [[nodiscard]] glm::vec4 TransformPlane(const glm::mat4& transform, const glm::vec4& worldPlane);

    /// Lengyel's oblique near-plane clip: rewrite the clip-z row of `projection`
    /// so the conventional near plane is replaced by `viewSpacePlane` (the
    /// reflection plane expressed in view space, normal pointing toward the kept
    /// half-space). After this, a vertex exactly on the plane projects to clip
    /// z = -w (the near plane) and anything on the far side is clipped. See
    /// Eric Lengyel, "Oblique View Frustum Depth Projection and Clipping" (2005),
    /// adapted to GLM's column-major OpenGL projection.
    [[nodiscard]] glm::mat4 MakeObliqueProjection(const glm::mat4& projection, const glm::vec4& viewSpacePlane);

    /// Everything the reflection pass needs for one plane, computed together so
    /// the view/clip-plane/oblique-projection stay consistent.
    struct ReflectionMatrices
    {
        glm::mat4 MirrorView{ 1.0f };           ///< view * reflection(plane)
        glm::mat4 ObliqueProjection{ 1.0f };    ///< projection with near plane == reflection plane
        glm::mat4 ViewProjection{ 1.0f };       ///< ObliqueProjection * MirrorView
        glm::vec3 MirrorCameraPosition{ 0.0f }; ///< camera position reflected across the plane
    };

    /// Build the full set of mirrored-camera matrices for rendering a planar
    /// reflection of `worldPlane` (normal pointing toward the half-space that
    /// stays visible — e.g. `vec4(0,1,0,-h)` to reflect everything above a water
    /// surface at height h). `view`/`projection` are the real camera's matrices;
    /// `cameraPosition` is the real camera world position.
    [[nodiscard]] ReflectionMatrices BuildReflectionMatrices(const glm::mat4& view,
                                                             const glm::mat4& projection,
                                                             const glm::vec3& cameraPosition,
                                                             const glm::vec4& worldPlane);
} // namespace OloEngine::PlanarReflection
