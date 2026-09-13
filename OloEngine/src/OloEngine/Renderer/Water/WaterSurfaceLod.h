#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::WaterSurfaceLod
{
    // =========================================================================
    // HOW THE WATER SURFACE DECIDES ITS GEOMETRY (issue #1035)
    //
    // Two mechanisms pick which vertices the ocean spends, and this header is
    // the CPU side of both:
    //
    //   * the WORLD-SPACE GRID — a fixed lattice of `GridResolutionX/Z` quads
    //     over `WorldSizeX/Z`, each triangle a tessellation patch whose
    //     subdivision comes from `calcTessLevel` (distance-based) after a
    //     displacement-inflated frustum reject. This is what shipped before
    //     #1035 and is still the default;
    //   * the PROJECTED GRID (water-ocean.md §4.1, Johanson 2004) — the SAME
    //     lattice, but its (u, v) is read as a screen-space coordinate and
    //     ray-cast onto the water plane, so vertex density is set by screen
    //     resolution instead of world size. IN PROGRESS: everything in THIS
    //     file is complete and pinned, but the shader that consumes it still
    //     renders the surface incorrectly, and no scene enables it. §4.1 of
    //     water-ocean.md records what is left.
    //
    // Mirrored by:
    //   * include/WaterTessControlStage.glsl  — the tess level and the cull;
    //   * include/WaterVertexStage.glsl       — the projected-grid ray cast;
    //   * Scene.cpp                           — calls ComputeNdcBounds per frame
    //                                           and packs the UBO;
    //   * WaterGeometryLodProfileTest.cpp     — the census that chose between
    //                                           the two, and the contract tests.
    //
    // WaterRenderingTest.cpp carries its own independent copies of the two
    // tess-control mirrors below. That is deliberate: an independent
    // re-derivation is what makes a mirror test worth running, so do not
    // collapse them into calls to this header.
    //
    // ---- WHY THE PROJECTED GRID, AND NOT GRADIENT-ADAPTIVE TESSELLATION -----
    //
    // #1035 posed the two as competitors and made the profile the gate. The
    // census (WaterGeometryLodProfileTest) measured, on the two shipped water
    // scenes at the two camera poses the issue names:
    //
    //   * 80-94% of the triangles that survive the frustum reject at a low
    //     grazing angle are SUB-PIXEL — the surface is paying for geometry
    //     finer than the raster can show;
    //   * every base patch costs its vertex and tess-control invocations
    //     BEFORE the reject can discard it: 1.57 M and 2.46 M vertex-shader
    //     invocations per frame for the two scenes, at every camera pose,
    //     including the overhead one where 86-95% of them are then thrown away;
    //   * the roughness signal that gradient-adaptive tessellation would
    //     redistribute by is nearly flat. `|grad h|` spans p5..p95 of
    //     0.027..0.125 (WaterShowcase) and 0.004..0.024 (Drift), and the
    //     existing distance-based rule already lands 9.7-10.0% of its geometry
    //     on the roughest decile of patches — which is the share a uniform
    //     rule would land there. There is no concentration of cost on calm
    //     water to move onto crests, because #943's mesh band-limit already
    //     removes every octave the base grid cannot sample, so the rendered
    //     surface cannot be rougher than the grid it is drawn on.
    //
    // Both of the first two are "vertices spent where the screen cannot use
    // them", which is the projected grid's problem, not the adaptive
    // tessellator's.
    // =========================================================================

    // ---- Tess-control mirrors ----------------------------------------------

    /// Conservative per-axis bound on Gerstner displacement, the culling margin
    /// `computeMaxWaveDisplacement()` derives in the tess-control stage.
    /// `waveParams` is the UBO's (time, speed, amplitude, frequency);
    /// `waveDir0/1` are `WaterComponent::PackWaveDir0/1()`.
    [[nodiscard]] f32 MaxWaveDisplacement(const glm::vec4& waveParams,
                                          const glm::vec4& waveDir0,
                                          const glm::vec4& waveDir1);

    /// The TIGHT bound on how far the Gerstner ladder actually moves a vertex,
    /// in any one axis. This is what the projected grid is laid out against, and
    /// it is deliberately NOT `MaxWaveDisplacement`.
    ///
    /// The two differ by 2.8x on WaterShowcase's waves (1.28 m against 3.60 m),
    /// and that gap is not slack to be tidied away — it is the right answer to
    /// two different questions. For the tess-control cull, over-estimating is
    /// free: a patch wrongly kept costs one patch. For the grid it is not, and
    /// the failure is sharp rather than gradual: the layout rectangle has to
    /// reach below the screen by the displacement, and once the margin
    /// approaches the CAMERA HEIGHT the displaceable volume swallows the
    /// viewpoint and the rectangle runs away. Measured on the grazing pose at a
    /// 3 m eye: 11% of rows outside the screen at a 0 m margin, 61% at the tight
    /// 1.28 m, and at 2 m it hits `kNdcBoundsCap` with 92% of the grid off
    /// screen.
    ///
    /// So this sums the per-octave amplitudes the ladder is actually built from
    /// (`steepness / k`, scaled by the global amplitude and the octave's weight)
    /// with the real per-octave wavelengths, where `MaxWaveDisplacement` bounds
    /// every detail octave by the largest one and multiplies by a 1.5x safety
    /// factor.
    ///
    /// The invariant that matters is `MaxWaveDisplacement >= MaxSurfaceDisplacement`
    /// — the cull must never reject a patch the grid still places — and it is
    /// pinned by WaterGeometryLodProfileTest.
    ///
    /// No mesh band-limit is applied: under a projected grid the spacing varies
    /// across the frame, and near the camera every octave is live.
    [[nodiscard]] f32 MaxSurfaceDisplacement(const glm::vec4& waveParams,
                                             const glm::vec4& waveDir0,
                                             const glm::vec4& waveDir1);

    /// True when the patch, each corner grown by `margin` on every axis, lies
    /// wholly outside the frustum of `viewProj`. Gribb-Hartmann planes kept
    /// un-normalised, exactly as `isPatchOutsideFrustum()` does.
    [[nodiscard]] bool IsPatchOutsideFrustum(const glm::vec3& p0, const glm::vec3& p1,
                                             const glm::vec3& p2, f32 margin,
                                             const glm::mat4& viewProj);

    /// Subdivision level for one patch edge. `tessParams` is the UBO's
    /// (factor, minDist, maxDist, cullEnable).
    ///
    /// The `max(..., 1.0)` floor is load-bearing and not a tidy-up: a level
    /// below 1 DISCARDS the patch (GL 4.6 §11.2.2), which once culled all the
    /// near water and left a see-through hole to the seafloor. Anything that
    /// multiplies into the factor keeps the floor.
    [[nodiscard]] f32 CalcTessLevel(const glm::vec3& p0, const glm::vec3& p1,
                                    const glm::vec3& cameraPos, const glm::vec4& tessParams);

    // ---- Projected grid ----------------------------------------------------

    /// The NDC rectangle a projected grid is laid out over.
    ///
    /// Not simply [-1, 1]^2, for two reasons that pull in opposite directions:
    ///
    ///   * rows above the horizon reach no plane at all, so the band has to
    ///     stop short of the sky — without that, roughly half the rows collapse
    ///     onto the horizon line at a grazing angle;
    ///   * the surface is DISPLACED after the grid is placed, and a vertex at
    ///     the screen edge lifted by a wave crest moves off screen, leaving a
    ///     band of missing water behind it. From a 3 m eye a 1.5 m crest moves
    ///     the bottom row up by 24% of the vertical frame, which is a hole
    ///     across the nearest quarter of the picture rather than a subtlety.
    ///     So the rectangle EXTENDS past the screen edges by however much the
    ///     displacement can move a vertex there.
    ///
    /// Both fall out of one construction (Johanson 2004): intersect the frustum
    /// with the VOLUME the surface can occupy — the slab between
    /// `planePoint +/- displacementMargin` — flatten what is visible of it onto
    /// the base plane, grow it by the same margin in-plane for the horizontal
    /// half of the displacement, and take the NDC bounds of the result.
    struct NdcBounds
    {
        glm::vec2 m_Min{ -1.0f, -1.0f };
        glm::vec2 m_Max{ 1.0f, 1.0f };
        /// False when no sampled ray reaches the slab at all (the camera is
        /// looking away from the water). Callers keep the full [-1, 1]^2 in that
        /// case — every vertex then clamps onto the surface rim and the patches
        /// are rejected by the frustum test, which is the same picture an empty
        /// screen would give, with no special case in the shader.
        bool m_Visible = false;
    };

    /// Lattice resolution ComputeNdcBounds probes, per axis. 17 puts the pitch
    /// at 0.125 NDC; the returned rectangle is widened by one pitch on every
    /// side so a partially sampled edge can never be cropped OUT of the grid.
    /// Over-covering costs a few collapsed rows; under-covering would clip the
    /// water short of where it is drawn.
    inline constexpr i32 kNdcProbeSteps = 17;

    /// How far outside the screen the rectangle may grow, per side. The
    /// excursion is unbounded as the camera approaches the water plane (the
    /// bottom row's intersection runs off to the horizon), and no finite grid
    /// covers that case, so it is capped rather than allowed to make the grid
    /// useless everywhere else.
    inline constexpr f32 kNdcBoundsCap = 4.0f;

    /// The NDC rectangle for a projected grid on the plane through
    /// (`planePoint`, `planeNormal`), allowing for `displacementMargin` metres
    /// of surface displacement in any direction.
    ///
    /// Pass a margin that dominates the SAME bound the tess-control cull uses
    /// (`MaxWaveDisplacement`), widened to the draw's own vertical extent so it
    /// also covers the FFT ocean — whose crests the Gerstner-derived bound does
    /// not describe, which is why the FFT path turns that cull off. The bound
    /// over-estimates, and the cost of that here is real (rows spent outside the
    /// screen) where for the cull it was free. Sharing it anyway is the
    /// deliberate trade: two different answers to "how far can this surface
    /// move" is the kind of drift that shows up later as a thin band of missing
    /// water at the bottom of the frame.
    ///
    /// `viewProj` must be the matrix the VERTEX STAGE uses — i.e. after
    /// `RHI::AdjustProjectionForBackend`, because Vulkan's row flip mirrors NDC
    /// y and the rectangle would otherwise describe the wrong half of the
    /// screen. Which SPACE it is built in (absolute or render-relative) does not
    /// matter: both produce identical NDC.
    /// `cameraPos` must be in the SAME space `viewProj` maps from — the ray is
    /// cast from it, which is what keeps this independent of the depth
    /// convention. See RayPlaneHit in the .cpp for why that matters.
    [[nodiscard]] NdcBounds ComputeNdcBounds(const glm::mat4& viewProj,
                                             const glm::vec3& cameraPos,
                                             const glm::vec3& planePoint,
                                             const glm::vec3& planeNormal,
                                             f32 displacementMargin);

    /// The world position one projected-grid vertex lands on: unproject `ndc`
    /// through `invViewProj` into a segment spanning the depth range, intersect
    /// it with the plane, and fall back to the rim when the ray misses (above
    /// the horizon, or parallel to the surface).
    ///
    /// `rimRadius` is how far out a missing ray is pushed; pass the surface's
    /// half-diagonal so a missed row lands outside the rect and is then clamped
    /// onto its edge by the caller. Mirrors `waterProjectGridVertex()` in
    /// include/WaterVertexStage.glsl.
    [[nodiscard]] glm::vec3 ProjectGridVertex(const glm::mat4& invViewProj,
                                              const glm::vec3& cameraPos, const glm::vec2& ndc,
                                              const glm::vec3& planePoint,
                                              const glm::vec3& planeNormal,
                                              f32 rimRadius);

    /// Clamp a surface-local XZ into the authored rect. The projected grid is a
    /// screen-space lattice and has no idea where the water ends, so this is
    /// what keeps a finite water tile finite: rows that would land beyond the
    /// rect pile onto its edge as zero-area triangles instead of extending the
    /// ocean to the horizon. Mirrors the clamp in the vertex stage.
    [[nodiscard]] inline glm::vec2 ClampToRect(const glm::vec2& localXZ, f32 halfX, f32 halfZ)
    {
        return { std::clamp(localXZ.x, -halfX, halfX), std::clamp(localXZ.y, -halfZ, halfZ) };
    }
} // namespace OloEngine::WaterSurfaceLod
