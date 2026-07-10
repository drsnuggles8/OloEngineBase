#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/CameraRelative.h" // kRenderOriginGridSize

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // =========================================================================
    // Floating-origin / origin-rebasing (issue #429, rebase slice)
    // =========================================================================
    //
    // Camera-relative rendering (the other two-thirds of #429, already shipped —
    // see Renderer/CameraRelative.h) fixes GPU-side vertex jitter far from the
    // world origin, but the ECS still stores absolute f32 world positions, and
    // the physics simulation still integrates them. Far from origin the *stored*
    // coordinate's ULP grows (~0.004 at 40 km), degrading physics precision and
    // every CPU-side gameplay/spatial query — a floor camera-relative rendering
    // cannot remove because it never touches the stored state.
    //
    // Floating-origin closes that gap: when the reference point (the primary
    // camera / player) drifts more than RebaseThreshold from the current rebased
    // origin, Scene shifts EVERY stored world position — root entity transforms,
    // 3D/2D physics bodies, character controllers — back toward the origin by a
    // grid-snapped delta, atomically on the game thread. Velocities, rotations,
    // and joint constraints are preserved because the whole world translates by
    // the same delta at once, so nothing moves *relative* to anything else.
    //
    // The two mechanisms compose cleanly and are orthogonal: rebasing changes
    // what "world position" means (the stored value), camera-relative rendering
    // is a fixed per-frame downstream transform that re-zeroes around whatever
    // the current camera position is. After a rebase the camera sits near origin
    // again, so ComputeRenderOrigin snaps back to (0,0,0) and the render path is
    // a no-op — exactly as for any near-origin scene.
    struct WorldOriginSettings
    {
        // Opt-in per scene. Default off keeps every existing scene byte-identical
        // (no rebase ever fires, m_WorldOrigin stays (0,0,0)).
        bool Enabled = false;

        // Trigger distance (world units) from the rebased origin to the
        // reference point. Must sit comfortably above SnapGridSize·√3/2 (the
        // worst-case post-rebase distance) so a rebase lands the reference well
        // inside the threshold — that gap is the hysteresis that prevents a
        // rebase from re-firing every frame at the boundary. SanitizeWorldOrigin-
        // Settings enforces the minimum.
        f32 RebaseThreshold = 2048.0f;

        // The shift is snapped to this grid, so repeated rebases land on exact
        // multiples (grid-snapped f32 shifts are represented exactly, so the
        // accumulated origin offset never drifts) and static geometry keeps tidy
        // coordinates. Matches the renderer's render-origin grid by default.
        f32 SnapGridSize = kRenderOriginGridSize; // 1024
    };

    inline void SanitizeWorldOriginSettings(WorldOriginSettings& s)
    {
        if (!std::isfinite(s.RebaseThreshold))
            s.RebaseThreshold = 2048.0f;
        if (!std::isfinite(s.SnapGridSize))
            s.SnapGridSize = kRenderOriginGridSize;

        s.SnapGridSize = std::max(s.SnapGridSize, 1.0f);

        // Guarantee the trigger sits above the worst-case post-rebase distance
        // (SnapGridSize·√3/2, when the reference lands at a cell corner) so the
        // rebase cannot immediately re-fire. √3/2 ≈ 0.8660254.
        const f32 minThreshold = s.SnapGridSize * 0.8660254f + 1.0f;
        s.RebaseThreshold = std::max(s.RebaseThreshold, minThreshold);
    }
} // namespace OloEngine
