#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// WaterGeometryLodProfileTest — the profile that chose the water surface's LOD
// scheme (issue #1035), and the contract tests for the one it chose.
//
// #1035's FIRST acceptance criterion is a measurement, not a feature: "a profile
// identifying whether water vertex cost is dominated by wasted off-screen /
// horizon vertices (-> projected grid) or by under-tessellated crests
// (-> gradient-adaptive tessellation), captured before either is built."
//
// So the census lives here rather than in a scratch script. It is cheap, it is
// deterministic, and keeping it in the suite means the decision can be
// RE-CHECKED when the scenes or the wave model change, instead of being a
// paragraph in a merged PR body that nobody can re-run. `ProfileCensus` below
// replays the real tess-control stage over the real shipped grids at the two
// camera poses the issue names, and the `Census*` tests assert the SHAPE of the
// answer — not the exact counts, which are allowed to move with the scenes.
//
// What it measured (printed by the tests; numbers from the first run):
//
//   scene / camera                  culled   sub-pixel   VS invocations
//   WaterShowcase / low grazing      53.2%      79.9%         1,572,864
//   WaterShowcase / high overhead    85.6%       0.0%         1,572,864
//   Drift / low grazing              70.9%      93.6%         2,457,600
//   Drift / high overhead            94.7%       0.0%         2,457,600
//
// Two things dominate and neither is crest detail:
//
//   * at a grazing angle most of the drawn triangles are finer than a pixel;
//   * the vertex and tess-control invocations are paid for EVERY base patch at
//     EVERY pose, before the frustum reject can discard 53-95% of them. That
//     cost is a function of world size and grid resolution and of nothing else,
//     so no tessellation rule can reach it.
//
// And the crest signal is not there to redistribute: the roughness spread is
// narrow (CensusRoughnessSpreadIsNarrow), and the existing distance rule already
// spends a uniform share of its geometry on the roughest patches
// (CensusDistanceTessIsUncorrelatedWithRoughness). #943's mesh band-limit is
// why — it removes every octave the base grid cannot sample, so the surface
// cannot be rougher than the grid drawn under it.
//
// The projected grid (water-ocean.md §4.1) is the scheme that addresses both
// dominant terms, and the ProjectedGrid* tests pin its contract:
// WaterSurfaceLod.h is the CPU side, include/WaterVertexStage.glsl the GPU one.
// =============================================================================

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Water/WaterSurfaceLod.h"
#include "OloEngine/Scene/Components.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // ---- the two camera poses #1035's acceptance criteria name --------------
    //
    // EditorCamera's own defaults (45 deg vertical FOV, 16:9, 0.1..1000), so the
    // census describes the frustum the editor actually shows, and a 1080p
    // viewport so "sub-pixel" means sub-pixel on a real screen.
    constexpr f32 kFovYDegrees = 45.0f;
    constexpr f32 kAspect = 16.0f / 9.0f;
    constexpr f32 kNearClip = 0.1f;
    constexpr f32 kFarClip = 1000.0f;
    constexpr f32 kViewportWidth = 1920.0f;
    constexpr f32 kViewportHeight = 1080.0f;

    struct CameraPose
    {
        const char* m_Label;
        glm::vec3 m_Eye;
        glm::vec3 m_Target;
    };

    // Low grazing: a boat-deck eye height looking nearly along the surface —
    // the pose that puts the horizon on screen and foreshortens every far quad
    // to nothing. High overhead: a steep look-down, where nothing is
    // foreshortened and the frustum contains a small patch of a large sea.
    constexpr CameraPose kLowGrazing{ "low grazing (eye 3 m)", { 0.0f, 3.0f, 300.0f }, { 0.0f, 2.0f, -300.0f } };
    constexpr CameraPose kHighOverhead{ "high overhead (eye 150 m)", { 0.0f, 150.0f, 120.0f }, { 0.0f, 0.0f, -40.0f } };

    /// The REAL EditorCamera's view-projection, not a hand-rolled
    /// glm::perspective * glm::lookAt.
    ///
    /// This matters more than it looks. The projected grid unprojects NDC
    /// through the inverse of this matrix, and the first implementation assumed
    /// which NDC depth lands in front of the camera. Against a hand-built
    /// perspective that assumption held and every test here passed; against the
    /// camera the engine actually renders with it did not, and the whole water
    /// surface collapsed onto the horizon line on screen. A mirror test that
    /// builds its own matrix is testing the mirror against itself.
    [[nodiscard]] EditorCamera MakeCamera(const CameraPose& pose)
    {
        EditorCamera camera(kFovYDegrees, kAspect, kNearClip, kFarClip);
        camera.SetViewportSize(kViewportWidth, kViewportHeight);
        // CameraPose names a target rather than angles because that is how the
        // poses read; convert here. Positive pitch tilts the view DOWN.
        const glm::vec3 forward = glm::normalize(pose.m_Target - pose.m_Eye);
        const f32 yaw = std::atan2(forward.x, -forward.z);
        const f32 pitch = -std::asin(glm::clamp(forward.y, -1.0f, 1.0f));
        camera.SetPose(pose.m_Eye, yaw, pitch);
        return camera;
    }

    [[nodiscard]] glm::mat4 MakeViewProj(const CameraPose& pose)
    {
        return MakeCamera(pose).GetViewProjection();
    }

    // ---- the shipped water surfaces ----------------------------------------
    //
    // Copied from the scene files rather than from WaterComponent's defaults:
    // the point of the census is what the ENGINE ACTUALLY DRAWS, and both
    // scenes override most of it. Drift leaves m_TessellationEnabled at its
    // false default, so its factor is 0 and every patch draws at level 1.
    struct SurfaceConfig
    {
        const char* m_Name;
        f32 m_WorldSize;
        u32 m_GridResolution;
        bool m_TessellationEnabled;
        f32 m_TessFactor;
        f32 m_TessMinDistance;
        f32 m_TessMaxDistance;
        f32 m_WaveAmplitude;
        f32 m_WaveFrequency;
        glm::vec2 m_WaveDir0;
        f32 m_Steepness0;
        f32 m_Wavelength0;
        glm::vec2 m_WaveDir1;
        f32 m_Steepness1;
        f32 m_Wavelength1;

        [[nodiscard]] f32 VertexSpacing() const
        {
            return m_WorldSize / static_cast<f32>(m_GridResolution);
        }

        [[nodiscard]] glm::vec4 TessParams() const
        {
            // Scene.cpp's build, verbatim: a disabled toggle sends 0, not the factor.
            return { m_TessellationEnabled ? m_TessFactor : 0.0f, m_TessMinDistance, m_TessMaxDistance, 1.0f };
        }

        [[nodiscard]] glm::vec4 WaveParams() const
        {
            return { 0.0f, 1.0f, m_WaveAmplitude, m_WaveFrequency };
        }

        [[nodiscard]] glm::vec4 WaveDir0() const
        {
            return { m_WaveDir0.x, m_WaveDir0.y, m_Steepness0, m_Wavelength0 };
        }

        [[nodiscard]] glm::vec4 WaveDir1() const
        {
            return { m_WaveDir1.x, m_WaveDir1.y, m_Steepness1, m_Wavelength1 };
        }
    };

    // WaterShowcase.olo as authored BEFORE #1035 — the only shipped scene that
    // had water tessellation on. The scene itself has since opted into the
    // projected grid, but this configuration is kept verbatim because it is the
    // thing being measured: the census is a statement about the world-space
    // grid, and it stops meaning anything if it drifts along with the scene.
    constexpr SurfaceConfig kWaterShowcase{
        "WaterShowcase", 1000.0f, 512, true, 8.0f, 10.0f, 200.0f, 0.5f, 1.0f, { 1.0f, 0.3f }, 0.3f, 30.0f, { -0.4f, 0.9f }, 0.25f, 50.0f
    };

    // Drift.olo — the game. 1.6 km of sea at 2.5 m quads, tessellation off, so
    // every triangle it draws is base grid.
    constexpr SurfaceConfig kDrift{
        "Drift", 1600.0f, 640, false, 8.0f, 10.0f, 200.0f, 0.12f, 0.55f, { 1.0f, 0.15f }, 0.25f, 10.0f, { 0.6f, 0.8f }, 0.15f, 15.0f
    };

    // ---- the band-limited Gerstner ladder, as a SLOPE field ----------------
    //
    // sumGerstnerWaves sums h = sum_i a_i sin(k_i (d_i . x) - w_i t + p_i), so
    // dh/dx picks up a_i k_i d_i.x cos(...). The census only needs the spread of
    // |grad h| over the surface, so the per-octave time and phase offsets are
    // dropped: they translate each octave, and a translation cannot change the
    // distribution of a stationary field sampled over 250k points. The domain
    // warp is dropped for the same reason — it moves WHERE an octave is sampled,
    // not how steep it is.
    //
    // What is NOT dropped is octaveMeshWeight (#943): it is the whole reason the
    // roughness spread is narrow, so leaving it out would manufacture crest
    // detail the engine does not draw and point the profile at the wrong design.
    struct Octave
    {
        glm::vec2 m_Direction;
        f32 m_Wavelength;
        f32 m_Steepness;
        f32 m_AmplitudeWeight;
    };

    [[nodiscard]] f32 OctaveMeshWeight(f32 wavelength, f32 vertexSpacing)
    {
        if (vertexSpacing <= 0.0f)
            return 1.0f;
        const f32 t = std::clamp((wavelength / vertexSpacing - 3.0f) / 3.0f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t); // smoothstep(3, 6, samplesPerWave)
    }

    [[nodiscard]] std::vector<Octave> BuildOctaveLadder(const SurfaceConfig& cfg)
    {
        const f32 freq = std::max(cfg.m_WaveFrequency, 0.01f);
        const f32 wl0 = std::max(cfg.m_Wavelength0, 0.1f) / freq;
        const f32 wl1 = std::max(cfg.m_Wavelength1, 0.1f) / freq;
        const glm::vec2 d0 = glm::normalize(cfg.m_WaveDir0);
        const glm::vec2 d1 = glm::normalize(cfg.m_WaveDir1);
        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSteepness = (cfg.m_Steepness0 + cfg.m_Steepness1) * 0.5f;
        const f32 baseAngle = std::atan2(d0.y, d0.x);
        constexpr f32 kGoldenAngle = 2.39996f;

        std::vector<Octave> ladder{
            { d0, wl0, cfg.m_Steepness0, 0.55f },
            { d1, wl1, cfg.m_Steepness1, 0.55f },
        };
        // WaterCommon.glsl's six detail octaves: wavelength ratio, steepness
        // ratio, global amplitude weight.
        constexpr f32 kDetail[6][3] = {
            { 0.85f, 0.5f, 0.5f },
            { 0.6f, 0.45f, 0.4f },
            { 0.4f, 0.38f, 0.3f },
            { 0.25f, 0.3f, 0.22f },
            { 0.15f, 0.22f, 0.15f },
            { 0.09f, 0.15f, 0.1f },
        };
        for (i32 i = 0; i < 6; ++i)
        {
            const f32 angle = baseAngle + kGoldenAngle * static_cast<f32>(i + 1);
            ladder.push_back({ { std::cos(angle), std::sin(angle) },
                               avgWL * kDetail[i][0],
                               avgSteepness * kDetail[i][1],
                               kDetail[i][2] });
        }
        return ladder;
    }

    [[nodiscard]] f32 SurfaceSlope(const std::vector<Octave>& ladder, const SurfaceConfig& cfg,
                                   f32 vertexSpacing, const glm::vec2& worldXZ)
    {
        constexpr f32 kTwoPi = 6.28318530f;
        f32 dhdx = 0.0f;
        f32 dhdz = 0.0f;
        for (const auto& o : ladder)
        {
            const f32 meshWeight = OctaveMeshWeight(o.m_Wavelength, vertexSpacing);
            if (meshWeight <= 1e-6f)
                continue;
            const f32 k = kTwoPi / std::max(o.m_Wavelength, 0.001f);
            // gerstnerWave derives its amplitude as steepness / k, then the
            // caller scales displacement AND steepness by the same
            // amplitude * weight — see WaterCommon.glsl's note on that pairing.
            const f32 amplitude = (o.m_Steepness / k) * cfg.m_WaveAmplitude * o.m_AmplitudeWeight * meshWeight;
            const f32 slopeTerm = amplitude * k * std::cos(k * glm::dot(worldXZ, o.m_Direction));
            dhdx += o.m_Direction.x * slopeTerm;
            dhdz += o.m_Direction.y * slopeTerm;
        }
        return std::sqrt(dhdx * dhdx + dhdz * dhdz);
    }

    // ---- the census ---------------------------------------------------------

    struct Census
    {
        u64 m_BasePatches = 0;
        u64 m_CulledPatches = 0;
        u64 m_DrawnPatches = 0;
        u64 m_VertexInvocations = 0;  ///< paid before the cull can reject anything
        u64 m_GeneratedTriangles = 0; ///< tessellator output over the drawn patches
        u64 m_GeneratedVertices = 0;
        u64 m_SubPixelTriangles = 0; ///< generated triangles covering < 1 px
        f32 m_SlopeP5 = 0.0f;
        f32 m_SlopeMedian = 0.0f;
        f32 m_SlopeP95 = 0.0f;
        /// Share of generated geometry that landed on the roughest decile of
        /// drawn patches. A rule blind to roughness scores ~10%.
        f32 m_GeometryOnRoughestDecile = 0.0f;

        [[nodiscard]] f32 CulledFraction() const
        {
            return m_BasePatches > 0 ? static_cast<f32>(m_CulledPatches) / static_cast<f32>(m_BasePatches) : 0.0f;
        }

        [[nodiscard]] f32 SubPixelFraction() const
        {
            return m_GeneratedTriangles > 0
                       ? static_cast<f32>(m_SubPixelTriangles) / static_cast<f32>(m_GeneratedTriangles)
                       : 0.0f;
        }
    };

    /// Screen-space area of a world-space triangle, in pixels. NaN when any
    /// corner is at or behind the eye, where the projection has no meaning.
    [[nodiscard]] f32 ScreenAreaPixels(const glm::mat4& viewProj, const glm::vec3& p0,
                                       const glm::vec3& p1, const glm::vec3& p2)
    {
        const glm::vec3 corners[3] = { p0, p1, p2 };
        glm::vec2 screen[3]{};
        for (i32 i = 0; i < 3; ++i)
        {
            const glm::vec4 clip = viewProj * glm::vec4(corners[i], 1.0f);
            if (!(clip.w > 1e-6f))
                return std::numeric_limits<f32>::quiet_NaN();
            screen[i] = { clip.x / clip.w * kViewportWidth * 0.5f, clip.y / clip.w * kViewportHeight * 0.5f };
        }
        const glm::vec2 e1 = screen[1] - screen[0];
        const glm::vec2 e2 = screen[2] - screen[0];
        return std::abs(e1.x * e2.y - e2.x * e1.y) * 0.5f;
    }

    /// Replay the tess-control stage over the whole world-space grid.
    [[nodiscard]] Census ProfileCensus(const SurfaceConfig& cfg, const CameraPose& pose)
    {
        const glm::mat4 viewProj = MakeViewProj(pose);
        const glm::vec4 tessParams = cfg.TessParams();
        const f32 margin =
            WaterSurfaceLod::MaxWaveDisplacement(cfg.WaveParams(), cfg.WaveDir0(), cfg.WaveDir1());
        const std::vector<Octave> ladder = BuildOctaveLadder(cfg);
        const f32 spacing = cfg.VertexSpacing();
        const f32 half = cfg.m_WorldSize * 0.5f;
        const f32 step = cfg.m_WorldSize / static_cast<f32>(cfg.m_GridResolution);

        struct DrawnPatch
        {
            f32 m_Slope;
            u32 m_Triangles;
        };
        std::vector<DrawnPatch> drawn;
        // Two triangle patches per quad, and at a grazing angle roughly half of
        // them survive the cull — reserving the full patch count keeps the walk
        // free of reallocation without materially over-allocating.
        drawn.reserve(static_cast<sizet>(cfg.m_GridResolution) * cfg.m_GridResolution * 2u);

        Census census;
        // MeshPrimitives::CreateWaterGrid's layout: two triangles per quad,
        // (topLeft, bottomLeft, topRight) and (topRight, bottomLeft, bottomRight).
        for (u32 z = 0; z < cfg.m_GridResolution; ++z)
        {
            const f32 z0 = -half + static_cast<f32>(z) * step;
            const f32 z1 = z0 + step;
            for (u32 x = 0; x < cfg.m_GridResolution; ++x)
            {
                const f32 x0 = -half + static_cast<f32>(x) * step;
                const f32 x1 = x0 + step;
                const glm::vec3 topLeft(x0, 0.0f, z0);
                const glm::vec3 topRight(x1, 0.0f, z0);
                const glm::vec3 bottomLeft(x0, 0.0f, z1);
                const glm::vec3 bottomRight(x1, 0.0f, z1);
                const glm::vec3 patches[2][3] = {
                    { topLeft, bottomLeft, topRight },
                    { topRight, bottomLeft, bottomRight },
                };

                for (const auto& p : patches)
                {
                    ++census.m_BasePatches;
                    census.m_VertexInvocations += 3;

                    if (WaterSurfaceLod::IsPatchOutsideFrustum(p[0], p[1], p[2], margin, viewProj))
                    {
                        ++census.m_CulledPatches;
                        continue;
                    }
                    ++census.m_DrawnPatches;

                    const f32 e0 = WaterSurfaceLod::CalcTessLevel(p[1], p[2], pose.m_Eye, tessParams);
                    const f32 e1 = WaterSurfaceLod::CalcTessLevel(p[2], p[0], pose.m_Eye, tessParams);
                    const f32 e2 = WaterSurfaceLod::CalcTessLevel(p[0], p[1], pose.m_Eye, tessParams);
                    // equal_spacing rounds every level UP to an integer; a
                    // triangle patch at level n yields n^2 triangles and
                    // (n+1)(n+2)/2 vertices. The three outer levels differ from
                    // the inner one by well under 1% across a patch this small,
                    // so the stitch ring is not modelled.
                    const u32 level = static_cast<u32>(std::ceil((e0 + e1 + e2) / 3.0f));
                    const u32 triangles = level * level;
                    census.m_GeneratedTriangles += triangles;
                    census.m_GeneratedVertices += (level + 1) * (level + 2) / 2;

                    const f32 areaPx = ScreenAreaPixels(viewProj, p[0], p[1], p[2]);
                    if (std::isfinite(areaPx) && areaPx < static_cast<f32>(triangles))
                    {
                        // Every generated triangle in this patch averages under
                        // a pixel. Counting per patch rather than per generated
                        // triangle is the conservative direction: a patch that
                        // straddles the threshold is not counted at all.
                        census.m_SubPixelTriangles += triangles;
                    }

                    const glm::vec2 centre((p[0].x + p[1].x + p[2].x) / 3.0f, (p[0].z + p[1].z + p[2].z) / 3.0f);
                    drawn.push_back({ SurfaceSlope(ladder, cfg, spacing, centre), triangles });
                }
            }
        }

        if (!drawn.empty())
        {
            std::vector<f32> slopes;
            slopes.reserve(drawn.size());
            for (const auto& d : drawn)
                slopes.push_back(d.m_Slope);
            std::sort(slopes.begin(), slopes.end());
            const auto pick = [&slopes](f64 q)
            {
                const sizet i = static_cast<sizet>(q * static_cast<f64>(slopes.size() - 1));
                return slopes[i];
            };
            census.m_SlopeP5 = pick(0.05);
            census.m_SlopeMedian = pick(0.5);
            census.m_SlopeP95 = pick(0.95);

            const f32 decileThreshold = pick(0.9);
            u64 roughGeometry = 0;
            for (const auto& d : drawn)
            {
                if (d.m_Slope >= decileThreshold)
                    roughGeometry += d.m_Triangles;
            }
            census.m_GeometryOnRoughestDecile =
                census.m_GeneratedTriangles > 0
                    ? static_cast<f32>(roughGeometry) / static_cast<f32>(census.m_GeneratedTriangles)
                    : 0.0f;
        }
        return census;
    }

    /// ProfileCensus walks 0.5-0.8 M patches and evaluates an eight-octave wave
    /// ladder at each one; six tests below want the same four results. Computing
    /// it once per (surface, pose) keeps the whole file inside a second or two
    /// of a Debug run instead of a minute of it. Deterministic by construction —
    /// there is no clock and no randomness anywhere in the walk — so caching
    /// cannot hide a difference between calls.
    [[nodiscard]] const Census& CachedCensus(const SurfaceConfig& cfg, const CameraPose& pose)
    {
        struct Entry
        {
            const char* m_Surface;
            const char* m_Pose;
            Census m_Census;
        };
        // A deque, not a vector: this returns a REFERENCE into the cache, and
        // deque keeps references to existing elements valid across a push_back
        // where vector would invalidate every one of them on a reallocation.
        static std::deque<Entry> cache;
        for (const auto& e : cache)
        {
            if (e.m_Surface == cfg.m_Name && e.m_Pose == pose.m_Label)
                return e.m_Census;
        }
        cache.push_back({ cfg.m_Name, pose.m_Label, ProfileCensus(cfg, pose) });
        return cache.back().m_Census;
    }

    /// What a projected grid actually puts on screen. Laid out exactly as the
    /// vertex stage does — over the NDC rectangle, ray-cast onto the plane,
    /// clamped into the surface rect — then reprojected and measured, because a
    /// share derived from arithmetic is a guess and this is the number the
    /// acceptance criterion is about.
    struct ProjectedGridMeasurement
    {
        u32 m_OnScreenTriangles = 0;
        f32 m_MedianOnScreenAreaPx = 0.0f;
        f32 m_SubPixelShare = 0.0f;
    };

    [[nodiscard]] ProjectedGridMeasurement MeasureProjectedGrid(const SurfaceConfig& cfg,
                                                                const CameraPose& pose,
                                                                u32 gridX, u32 gridY)
    {
        const glm::mat4 viewProj = MakeViewProj(pose);
        const glm::mat4 invViewProj = glm::inverse(viewProj);
        const glm::vec3 planePoint(0.0f);
        const glm::vec3 planeNormal(0.0f, 1.0f, 0.0f);
        const f32 half = cfg.m_WorldSize * 0.5f;
        const f32 rimRadius = 2.0f * std::sqrt(half * half + half * half);
        const f32 margin =
            WaterSurfaceLod::MaxSurfaceDisplacement(cfg.WaveParams(), cfg.WaveDir0(), cfg.WaveDir1());
        const WaterSurfaceLod::NdcBounds bounds =
            WaterSurfaceLod::ComputeNdcBounds(viewProj, pose.m_Eye, planePoint, planeNormal, margin);

        // Place every grid vertex, exactly as WaterVertexStage.glsl does.
        std::vector<glm::vec3> positions;
        positions.reserve(static_cast<sizet>(gridX + 1) * (gridY + 1));
        for (u32 j = 0; j <= gridY; ++j)
        {
            const f32 v = static_cast<f32>(j) / static_cast<f32>(gridY);
            for (u32 i = 0; i <= gridX; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(gridX);
                // v = 1 on the near edge, exactly as the vertex stage maps it.
                const glm::vec2 ndc(glm::mix(bounds.m_Min.x, bounds.m_Max.x, u),
                                    glm::mix(bounds.m_FarEdgeY, bounds.m_NearEdgeY, v));
                const glm::vec3 hit =
                    WaterSurfaceLod::ProjectGridVertex(invViewProj, pose.m_Eye, ndc, planePoint, planeNormal, rimRadius);
                const glm::vec2 clamped = WaterSurfaceLod::ClampToRect({ hit.x, hit.z }, half, half);
                positions.push_back({ clamped.x, planePoint.y, clamped.y });
            }
        }

        const auto at = [&](u32 i, u32 j)
        { return positions[static_cast<sizet>(j) * (gridX + 1) + i]; };
        const auto onScreen = [&](const glm::vec3& p)
        {
            const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
            if (!(clip.w > 1e-6f))
                return false;
            const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
            return std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f;
        };

        std::vector<f32> areas;
        for (u32 j = 0; j < gridY; ++j)
        {
            for (u32 i = 0; i < gridX; ++i)
            {
                const glm::vec3 p0 = at(i, j);
                const glm::vec3 p1 = at(i + 1, j);
                const glm::vec3 p2 = at(i, j + 1);
                // A triangle counts as on screen when any corner is; a corner
                // test alone would drop the ones straddling an edge, which are
                // exactly the ones the layout margin exists for.
                if (!onScreen(p0) && !onScreen(p1) && !onScreen(p2))
                    continue;
                const f32 area = ScreenAreaPixels(viewProj, p0, p1, p2);
                // Zero area is a row collapsed onto the surface rim — real
                // output, but it covers nothing and would drag the median down
                // without describing anything the viewer sees.
                if (std::isfinite(area) && area > 1e-6f)
                    areas.push_back(area);
            }
        }

        ProjectedGridMeasurement out;
        if (areas.empty())
            return out;
        std::sort(areas.begin(), areas.end());
        out.m_OnScreenTriangles = static_cast<u32>(areas.size());
        out.m_MedianOnScreenAreaPx = areas[areas.size() / 2];
        const auto subPixel = std::lower_bound(areas.begin(), areas.end(), 1.0f);
        out.m_SubPixelShare =
            static_cast<f32>(subPixel - areas.begin()) / static_cast<f32>(areas.size());
        return out;
    }

    void ReportCensus(const SurfaceConfig& cfg, const CameraPose& pose, const Census& c)
    {
        std::cout << "[  PROFILE ] " << cfg.m_Name << " @ " << pose.m_Label << "\n"
                  << "[  PROFILE ]   base patches " << c.m_BasePatches
                  << " | culled " << c.m_CulledPatches << " (" << (100.0f * c.CulledFraction()) << "%)"
                  << " | drawn " << c.m_DrawnPatches << "\n"
                  << "[  PROFILE ]   vertex-shader invocations " << c.m_VertexInvocations
                  << " (paid before the cull) | generated triangles " << c.m_GeneratedTriangles
                  << " | generated vertices " << c.m_GeneratedVertices << "\n"
                  << "[  PROFILE ]   sub-pixel geometry " << (100.0f * c.SubPixelFraction()) << "%"
                  << " | |grad h| p5 " << c.m_SlopeP5 << " median " << c.m_SlopeMedian
                  << " p95 " << c.m_SlopeP95
                  << " | geometry on roughest decile " << (100.0f * c.m_GeometryOnRoughestDecile) << "%"
                  << std::endl;
    }
} // namespace

// =============================================================================
// The profile (#1035 acceptance criterion 1)
// =============================================================================

TEST(WaterGeometryLodProfile, CensusReportsBothShippedSurfacesAtBothPoses)
{
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        for (const auto& pose : { kLowGrazing, kHighOverhead })
        {
            const Census& c = CachedCensus(cfg, pose);
            ReportCensus(cfg, pose, c);
            EXPECT_GT(c.m_BasePatches, 0u);
            EXPECT_EQ(c.m_CulledPatches + c.m_DrawnPatches, c.m_BasePatches);
            EXPECT_EQ(c.m_VertexInvocations, c.m_BasePatches * 3)
                << "Every base patch costs its vertex invocations whether or not the "
                   "tess-control stage then rejects it — that is the cost no tessellation "
                   "rule can reach, and the reason the projected grid was chosen.";
        }
    }
}

TEST(WaterGeometryLodProfile, CensusGrazingAngleIsDominatedBySubPixelGeometry)
{
    // The finding that picked the design. At a grazing angle a world-space grid
    // foreshortens to far below the raster's resolution while still paying for
    // every triangle. If this ever drops below half, the premise of the
    // projected grid has changed and #1035's choice deserves re-reading.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const Census& c = CachedCensus(cfg, kLowGrazing);
        EXPECT_GT(c.SubPixelFraction(), 0.5f)
            << cfg.m_Name << ": sub-pixel share was " << (100.0f * c.SubPixelFraction()) << "%";
    }
}

TEST(WaterGeometryLodProfile, CensusOverheadPoseWastesAlmostEveryPatchOnTheCull)
{
    // The complementary half: looking down, the geometry that survives is
    // well-sized (nothing sub-pixel), and the waste has moved entirely into
    // patches that are shaded and then thrown away.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const Census& c = CachedCensus(cfg, kHighOverhead);
        EXPECT_GT(c.CulledFraction(), 0.8f)
            << cfg.m_Name << ": culled share was " << (100.0f * c.CulledFraction()) << "%";
        EXPECT_LT(c.SubPixelFraction(), 0.05f)
            << cfg.m_Name << ": overhead geometry should be comfortably larger than a pixel";
    }
}

TEST(WaterGeometryLodProfile, CensusRoughnessSpreadIsNarrow)
{
    // The case AGAINST gradient-adaptive tessellation, measured rather than
    // asserted. The band-limited surface has no near-flat water next to
    // near-vertical crests: |grad h| stays inside a single order of magnitude,
    // so there is no reservoir of wasted subdivision on calm patches to move.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const Census& c = CachedCensus(cfg, kLowGrazing);
        ASSERT_GT(c.m_SlopeP5, 0.0f) << cfg.m_Name;
        EXPECT_LT(c.m_SlopeP95 / c.m_SlopeP5, 10.0f)
            << cfg.m_Name << ": p95/p5 of |grad h| was " << (c.m_SlopeP95 / c.m_SlopeP5);
        EXPECT_LT(c.m_SlopeP95, 0.5f)
            << cfg.m_Name << ": the steepest decile is still a gentle slope, not a breaking crest";
    }
}

TEST(WaterGeometryLodProfile, CensusDistanceTessIsUncorrelatedWithRoughness)
{
    // The distance rule spends a UNIFORM share of its geometry on the roughest
    // decile of patches — which is the point. It is blind to roughness, and
    // making it see roughness would move a share this small.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const Census& c = CachedCensus(cfg, kLowGrazing);
        EXPECT_NEAR(c.m_GeometryOnRoughestDecile, 0.10f, 0.05f)
            << cfg.m_Name << ": share on the roughest decile was "
            << (100.0f * c.m_GeometryOnRoughestDecile) << "%";
    }
}

TEST(WaterGeometryLodProfile, ProjectedGridCostsFarLessThanTheWorldGridItReplaces)
{
    // The comparison that makes "equal or lower vertex count" checkable: a
    // screen-space grid's patch count is its resolution, full stop — the same
    // number at every camera pose, with no patch off screen and none of it
    // sub-pixel by construction.
    //
    // The resolution WaterShowcase.olo actually opts into. Not 192x108 (the
    // count that would tile 1080p exactly), because part of the grid is laid out
    // OUTSIDE the frame so a crest can lift water into the bottom edge.
    constexpr u32 kProjectedX = 256;
    constexpr u32 kProjectedY = 144;
    const u64 projectedPatches = static_cast<u64>(kProjectedX) * kProjectedY * 2;
    const u64 projectedVertices = static_cast<u64>(kProjectedX + 1) * (kProjectedY + 1);

    for (const auto& pose : { kLowGrazing, kHighOverhead })
    {
        const ProjectedGridMeasurement m =
            MeasureProjectedGrid(kWaterShowcase, pose, kProjectedX, kProjectedY);

        std::cout << "[  PROFILE ] projected grid " << kProjectedX << "x" << kProjectedY << " @ "
                  << pose.m_Label << ": " << projectedPatches << " patches, " << projectedVertices
                  << " vertices, " << m.m_OnScreenTriangles << " triangles on screen at "
                  << m.m_MedianOnScreenAreaPx << " px each (median), " << (100.0f * m.m_SubPixelShare)
                  << "% sub-pixel" << std::endl;

        EXPECT_GT(m.m_OnScreenTriangles, 0u) << pose.m_Label;
        // The whole point: no sub-pixel geometry at either pose, where the world
        // grid produced 80% of it at the grazing one.
        EXPECT_LT(m.m_SubPixelShare, 0.01f)
            << pose.m_Label << ": the projected grid is still producing sub-pixel triangles";
        EXPECT_GT(m.m_MedianOnScreenAreaPx, 4.0f) << pose.m_Label;
    }

    // "Equal or lower vertex count", stated exactly. The counts below are the
    // grid's own dimensions, so they hold at EVERY camera pose rather than at
    // the one that happens to be measured.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const Census& c = CachedCensus(cfg, kLowGrazing);
        EXPECT_LT(projectedPatches, c.m_GeneratedTriangles)
            << cfg.m_Name << ": generated " << c.m_GeneratedTriangles << " triangles";
        EXPECT_LT(projectedVertices, c.m_VertexInvocations)
            << cfg.m_Name << ": paid " << c.m_VertexInvocations << " vertex invocations";
    }
}

// =============================================================================
// Projected-grid contract (water-ocean.md §4.1)
// =============================================================================

namespace
{
    // A plain y-up water plane at the origin, and the surface half-extent used
    // as the rim radius.
    constexpr glm::vec3 kPlanePoint{ 0.0f, 0.0f, 0.0f };
    constexpr glm::vec3 kPlaneNormal{ 0.0f, 1.0f, 0.0f };
    constexpr f32 kRimRadius = 800.0f;
} // namespace

TEST(WaterGeometryLodProfile, UnprojectionDepthConventionIsNotAssumed)
{
    // The assumption that broke the first implementation, written down as a
    // test: which NDC depth unprojects in FRONT of the camera is a property of
    // the projection, not a constant, and the projected grid must not depend on
    // it. Print both so the convention is visible rather than inferred.
    const EditorCamera camera = MakeCamera(kLowGrazing);
    const glm::mat4 viewProj = camera.GetViewProjection();
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    for (const f32 ndcZ : { -1.0f, 1.0f })
    {
        const glm::vec4 h = invViewProj * glm::vec4(0.0f, 0.0f, ndcZ, 1.0f);
        ASSERT_GT(std::abs(h.w), 1e-9f);
        const glm::vec3 world(glm::vec3(h) / h.w);
        const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        std::cout << "[  PROFILE ] NDC z " << ndcZ << " -> world (" << world.x << ", " << world.y
                  << ", " << world.z << "), distance from eye "
                  << glm::distance(world, kLowGrazing.m_Eye) << " m, clip w " << clip.w << std::endl;
    }
}

TEST(WaterGeometryLodProfile, ProjectedGridVertexLandsOnThePlane)
{
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    // Bottom of the screen looks down at the water in front of the camera.
    for (const f32 ndcX : { -0.9f, 0.0f, 0.9f })
    {
        const glm::vec3 hit = WaterSurfaceLod::ProjectGridVertex(
            invViewProj, kLowGrazing.m_Eye, { ndcX, -0.9f }, kPlanePoint, kPlaneNormal, kRimRadius);
        EXPECT_NEAR(hit.y, kPlanePoint.y, 1e-3f) << "ndcX " << ndcX;
        EXPECT_TRUE(std::isfinite(hit.x) && std::isfinite(hit.z));
    }
}

// The displacement bound a projected grid is laid out against, for
// WaterShowcase's waves. The TIGHT one, not the tess-control cull's — see
// MaxSurfaceDisplacement's header for why the two differ and why it matters
// here and not there.
const f32 kDisplacementMargin =
    WaterSurfaceLod::MaxSurfaceDisplacement(kWaterShowcase.WaveParams(), kWaterShowcase.WaveDir0(),
                                            kWaterShowcase.WaveDir1());

TEST(WaterGeometryLodProfile, ProjectedGridVertexIsUniformInScreenSpace)
{
    // The property the whole design rests on: equal steps in the grid's own
    // (u, v) are equal steps ON SCREEN, however unequal they are in the world.
    // Reproject the hits and check the NDC spacing is preserved — that is
    // "uniform pixel density" stated as something a test can fail.
    //
    // Measured with a ZERO displacement margin, because that is the statement:
    // the mapping is uniform. The margin then extends the rectangle past the
    // screen edge without changing that it is uniform over it.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const WaterSurfaceLod::NdcBounds bounds =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, 0.0f);
    ASSERT_TRUE(bounds.m_Visible);

    // Sampled over the lower 80% of the band, not all of it. The top of the
    // rectangle is the horizon edge, widened by one probe pitch on purpose, and
    // rows up there do not reach the plane at all — they collapse to a single
    // point by design, which is not a spacing to measure. Uniformity is a claim
    // about the rows that land on the water.
    constexpr i32 kRows = 16;
    constexpr f32 kBandFraction = 0.8f;
    f32 previousNdcY = 0.0f;
    f32 minGap = std::numeric_limits<f32>::max();
    f32 maxGap = 0.0f;
    for (i32 i = 0; i < kRows; ++i)
    {
        const f32 v = kBandFraction * static_cast<f32>(i) / static_cast<f32>(kRows - 1);
        const f32 ndcY = glm::mix(bounds.m_Min.y, bounds.m_Max.y, v);
        const glm::vec3 hit =
            WaterSurfaceLod::ProjectGridVertex(invViewProj, kLowGrazing.m_Eye, { 0.0f, ndcY }, kPlanePoint, kPlaneNormal, kRimRadius);
        const glm::vec4 clip = viewProj * glm::vec4(hit, 1.0f);
        ASSERT_GT(clip.w, 0.0f) << "row " << i << " projected behind the eye";
        const f32 reprojected = clip.y / clip.w;
        EXPECT_NEAR(reprojected, ndcY, 1e-2f) << "row " << i;
        if (i > 0)
        {
            const f32 gap = std::abs(reprojected - previousNdcY);
            // A zero gap is a pair of rows that both collapsed, which happens by
            // DESIGN at the horizon end: rows past the reachable band land on
            // the rim and coincide. Uniformity is a claim about the rows that
            // reach the plane, so those are what is measured.
            if (gap > 1e-5f)
            {
                minGap = std::min(minGap, gap);
                maxGap = std::max(maxGap, gap);
            }
        }
        previousNdcY = reprojected;
    }
    // World-space spacing between these same rows spans three orders of
    // magnitude at a grazing angle; on screen they stay within a few percent.
    ASSERT_LT(minGap, std::numeric_limits<f32>::max()) << "every row collapsed";
    EXPECT_LT(maxGap / minGap, 1.1f)
        << "screen-space row spacing drifted: min " << minGap << " max " << maxGap;
}

TEST(WaterGeometryLodProfile, ProjectedGridWorldSpacingGrowsWithDistance)
{
    // The other half of the same statement, and the one that explains the cost
    // saving: uniform on screen means the world-space step grows without bound
    // toward the horizon, which is exactly the geometry the world-space grid
    // was spending and the raster was throwing away.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const WaterSurfaceLod::NdcBounds bounds =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, 0.0f);
    ASSERT_TRUE(bounds.m_Visible);

    const auto rowAt = [&](f32 v)
    {
        return WaterSurfaceLod::ProjectGridVertex(
            invViewProj, kLowGrazing.m_Eye, { 0.0f, glm::mix(bounds.m_Min.y, bounds.m_Max.y, v) },
            kPlanePoint, kPlaneNormal, kRimRadius);
    };

    const f32 nearGap = glm::distance(rowAt(0.0f), rowAt(0.05f));
    const f32 farGap = glm::distance(rowAt(0.90f), rowAt(0.95f));
    EXPECT_GT(farGap, nearGap * 10.0f)
        << "near step " << nearGap << " m, far step " << farGap << " m";
}

TEST(WaterGeometryLodProfile, NdcBoundsExcludeTheSkyAtAGrazingAngle)
{
    // "No vertices past the horizon", measured. At eye height 3 m looking level,
    // roughly the top half of the frame is sky; the rectangle must stop short of
    // it, and every row inside it must still reach the plane.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const WaterSurfaceLod::NdcBounds bounds =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, 0.0f);

    ASSERT_TRUE(bounds.m_Visible);
    EXPECT_LT(bounds.m_Max.y, 0.5f) << "the rectangle should stop well below the top of the frame";

    constexpr i32 kRows = 32;
    for (i32 i = 1; i + 1 < kRows; ++i)
    {
        const f32 v = static_cast<f32>(i) / static_cast<f32>(kRows - 1);
        const glm::vec3 hit = WaterSurfaceLod::ProjectGridVertex(
            invViewProj, kLowGrazing.m_Eye, { 0.0f, glm::mix(bounds.m_Min.y, bounds.m_Max.y, v) },
            kPlanePoint, kPlaneNormal, kRimRadius);
        EXPECT_NEAR(hit.y, kPlanePoint.y, 1e-2f) << "row " << i << " missed the plane";
    }
}

TEST(WaterGeometryLodProfile, NdcBoundsExtendPastTheNearEdgeToCoverDisplacement)
{
    // The defect this margin exists for, stated as a test. The grid is placed on
    // the RESTING plane and displaced afterwards, so a vertex sitting exactly on
    // the bottom of the frame is lifted OUT of the frame by a crest — from a 3 m
    // eye a 1.5 m crest moves it up by a quarter of the vertical field of view,
    // which is a band of missing water across the nearest part of the picture.
    //
    // With a margin the rectangle has to reach BELOW the screen, far enough that
    // whatever lands at the bottom edge after displacement had somewhere to come
    // from.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);

    const WaterSurfaceLod::NdcBounds tight =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, 0.0f);
    const WaterSurfaceLod::NdcBounds covered =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);

    ASSERT_TRUE(tight.m_Visible);
    ASSERT_TRUE(covered.m_Visible);
    ASSERT_GT(kDisplacementMargin, 0.0f);

    std::cout << "[  PROFILE ] grazing NDC rectangle: tight y [" << tight.m_Min.y << ", " << tight.m_Max.y
              << "], with a " << kDisplacementMargin << " m displacement margin y [" << covered.m_Min.y
              << ", " << covered.m_Max.y << "] -- skirt below the screen = "
              << (100.0f * (-1.0f - covered.m_Min.y) / (covered.m_Max.y - covered.m_Min.y))
              << "% of the rows" << std::endl;

    EXPECT_LT(covered.m_Min.y, -1.0f)
        << "the rectangle must reach past the bottom of the frame, or a lifted crest "
           "leaves a band of missing water behind it";
    EXPECT_LE(covered.m_Min.y, tight.m_Min.y);
    EXPECT_GE(covered.m_Max.y, tight.m_Max.y);
    EXPECT_GE(covered.m_Min.y, -WaterSurfaceLod::kNdcBoundsCap);
    EXPECT_LE(covered.m_Max.y, WaterSurfaceLod::kNdcBoundsCap);
}

TEST(WaterGeometryLodProfile, NdcBoundsCoverEveryDisplacedVertexAtAGrazingAngle)
{
    // The property that margin is FOR, checked directly rather than by the sign
    // of an inequality: take the bottom row of the screen, raise the surface by
    // the margin, and confirm the rectangle contains the base position whose
    // displaced form lands there.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const WaterSurfaceLod::NdcBounds bounds =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);
    ASSERT_TRUE(bounds.m_Visible);

    const glm::vec3 raisedPlane = kPlanePoint + kPlaneNormal * kDisplacementMargin;
    const glm::vec3 raisedHit =
        WaterSurfaceLod::ProjectGridVertex(invViewProj, kLowGrazing.m_Eye, { 0.0f, -1.0f }, raisedPlane, kPlaneNormal, kRimRadius);
    const glm::vec3 basePosition(raisedHit.x, kPlanePoint.y, raisedHit.z);

    const glm::vec4 clip = viewProj * glm::vec4(basePosition, 1.0f);
    ASSERT_GT(clip.w, 0.0f);
    const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);

    EXPECT_GE(ndc.y, bounds.m_Min.y)
        << "the base position a crest lifts onto the bottom row (" << ndc.y
        << ") falls outside the grid (" << bounds.m_Min.y << ")";
    EXPECT_GE(ndc.x, bounds.m_Min.x);
    EXPECT_LE(ndc.x, bounds.m_Max.x);
}

TEST(WaterGeometryLodProfile, NdcBoundsNearEdgeIsDecidedByGeometry)
{
    // The bottom of the screen is NDC y = -1 on GL and +1 under the Vulkan
    // seam's row flip. The grid puts v = 1 on the NEAR edge to keep the mesh
    // winding front-facing, so ComputeNdcBounds must report which edge that is
    // from where the hits are — a flip hard-coded for GL leaves Vulkan with no
    // water at all, which is how this was first shipped.
    const EditorCamera camera = MakeCamera(kLowGrazing);
    const glm::mat4 glViewProj = camera.GetViewProjection();

    // WITH the displacement margin: it widens the rectangle on the near side
    // only, past the probed [-1, 1], which is the case that fooled the first
    // version of this decision (it compared against the rectangle's midpoint).
    const WaterSurfaceLod::NdcBounds gl = WaterSurfaceLod::ComputeNdcBounds(
        glViewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);
    ASSERT_TRUE(gl.m_Visible);
    EXPECT_FLOAT_EQ(gl.m_NearEdgeY, gl.m_Min.y) << "GL: the near water is at the bottom, y = -1";
    EXPECT_FLOAT_EQ(gl.m_FarEdgeY, gl.m_Max.y);

    // The Vulkan seam, applied by hand: negate clip y (RHIProjectionSeam.h's
    // row flip; the z remap does not move a hit).
    glm::mat4 flip(1.0f);
    flip[1][1] = -1.0f;
    const WaterSurfaceLod::NdcBounds vk = WaterSurfaceLod::ComputeNdcBounds(
        flip * glViewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);
    ASSERT_TRUE(vk.m_Visible);
    EXPECT_FLOAT_EQ(vk.m_NearEdgeY, vk.m_Max.y) << "flipped: the near water is at the TOP, y = +1";
    EXPECT_FLOAT_EQ(vk.m_FarEdgeY, vk.m_Min.y);
    // Same rectangle, mirrored.
    EXPECT_NEAR(vk.m_Min.y, -gl.m_Max.y, 1e-4f);
    EXPECT_NEAR(vk.m_Max.y, -gl.m_Min.y, 1e-4f);
}

TEST(WaterGeometryLodProfile, NdcBoundsCoverTheWholeFrameFromOverhead)
{
    // Looking steeply down, the plane fills the frame and the rectangle must not
    // shrink inside it: cropping would delete water the camera can see.
    const WaterSurfaceLod::NdcBounds bounds = WaterSurfaceLod::ComputeNdcBounds(
        MakeViewProj(kHighOverhead), kHighOverhead.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);

    ASSERT_TRUE(bounds.m_Visible);
    EXPECT_LE(bounds.m_Min.x, -1.0f);
    EXPECT_LE(bounds.m_Min.y, -1.0f);
    EXPECT_GE(bounds.m_Max.x, 1.0f);
    EXPECT_GE(bounds.m_Max.y, 1.0f);
}

TEST(WaterGeometryLodProfile, NdcBoundsReportNotVisibleWhenLookingAwayFromTheWater)
{
    // Camera above the plane looking up. Nothing hits, the rectangle stays the
    // full frame, and m_Visible says so — the caller keeps [-1, 1]^2 and every
    // vertex clamps onto the rim rather than the shader growing a special case.
    const CameraPose lookingUp{ "looking up", { 0.0f, 20.0f, 0.0f }, { 0.0f, 120.0f, -60.0f } };
    const WaterSurfaceLod::NdcBounds bounds = WaterSurfaceLod::ComputeNdcBounds(
        MakeViewProj(lookingUp), lookingUp.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);

    EXPECT_FALSE(bounds.m_Visible);
    EXPECT_EQ(bounds.m_Min, glm::vec2(-1.0f, -1.0f));
    EXPECT_EQ(bounds.m_Max, glm::vec2(1.0f, 1.0f));
}

TEST(WaterGeometryLodProfile, CullBoundDominatesTheGridBound)
{
    // Two bounds on the same displacement exist on purpose — the cull's is
    // loose because over-estimating is free there, the grid's is tight because
    // over-estimating costs rows off screen. What must never invert is their
    // ORDER: the tess-control stage rejects patches using the loose one, so if
    // it were ever the SMALLER of the two the cull would discard a patch the
    // grid had legitimately placed, and the water would develop a hole that
    // moves with the camera.
    for (const auto& cfg : { kWaterShowcase, kDrift })
    {
        const f32 cullBound =
            WaterSurfaceLod::MaxWaveDisplacement(cfg.WaveParams(), cfg.WaveDir0(), cfg.WaveDir1());
        const f32 gridBound =
            WaterSurfaceLod::MaxSurfaceDisplacement(cfg.WaveParams(), cfg.WaveDir0(), cfg.WaveDir1());

        std::cout << "[  PROFILE ] " << cfg.m_Name << " displacement: cull bound " << cullBound
                  << " m, grid bound " << gridBound << " m (" << (cullBound / gridBound) << "x)"
                  << std::endl;

        EXPECT_GT(gridBound, 0.0f) << cfg.m_Name;
        EXPECT_GE(cullBound, gridBound)
            << cfg.m_Name << ": the tess-control cull must never be tighter than the grid layout";
    }
}

TEST(WaterGeometryLodProfile, GridBoundMatchesTheLadderItClaimsToSum)
{
    // MaxSurfaceDisplacement is a hand-summed mirror of WaterCommon.glsl's
    // octave ladder, so it can drift from it silently. Re-derive the same sum
    // here from the constants written out independently and require agreement —
    // the same shape of pin WaterRenderingTest puts on the culling bound.
    const SurfaceConfig& cfg = kWaterShowcase;
    constexpr f32 kTwoPi = 6.28318530f;
    const f32 freq = std::max(cfg.m_WaveFrequency, 0.01f);
    const f32 wl0 = std::max(cfg.m_Wavelength0, 0.1f) / freq;
    const f32 wl1 = std::max(cfg.m_Wavelength1, 0.1f) / freq;
    const auto amplitude = [&](f32 steepness, f32 wavelength, f32 weight)
    {
        return (steepness / (kTwoPi / wavelength)) * cfg.m_WaveAmplitude * weight;
    };

    f32 expected = amplitude(cfg.m_Steepness0, wl0, 0.55f) + amplitude(cfg.m_Steepness1, wl1, 0.55f);
    const f32 avgWL = (wl0 + wl1) * 0.5f;
    const f32 avgSteepness = (cfg.m_Steepness0 + cfg.m_Steepness1) * 0.5f;
    expected += amplitude(avgSteepness * 0.5f, avgWL * 0.85f, 0.5f);
    expected += amplitude(avgSteepness * 0.45f, avgWL * 0.6f, 0.4f);
    expected += amplitude(avgSteepness * 0.38f, avgWL * 0.4f, 0.3f);
    expected += amplitude(avgSteepness * 0.3f, avgWL * 0.25f, 0.22f);
    expected += amplitude(avgSteepness * 0.22f, avgWL * 0.15f, 0.15f);
    expected += amplitude(avgSteepness * 0.15f, avgWL * 0.09f, 0.1f);

    EXPECT_NEAR(WaterSurfaceLod::MaxSurfaceDisplacement(cfg.WaveParams(), cfg.WaveDir0(), cfg.WaveDir1()),
                expected, 1e-4f);
}

TEST(WaterGeometryLodProfile, ProjectedGridSpacingIsContinuousAndGrowsWithDistance)
{
    // The band-limit spacing a projected vertex is sampled at. It has to be a
    // function of the VERTEX (its ray distance and incidence) rather than of the
    // patch, because two patches share every edge vertex: derived per patch,
    // the two sides disagreed about the octave weights and the surface tore
    // along every edge — sixteen pinholes of seabed per frame from overhead.
    const EditorCamera camera = MakeCamera(kLowGrazing);
    const glm::mat4 viewProj = camera.GetViewProjection();
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const WaterSurfaceLod::NdcBounds bounds =
        WaterSurfaceLod::ComputeNdcBounds(viewProj, kLowGrazing.m_Eye, kPlanePoint, kPlaneNormal, kDisplacementMargin);
    ASSERT_TRUE(bounds.m_Visible);

    // GL is identity for the backend adjustment, so the camera's own projection
    // is what the vertex stage sees.
    const f32 perMetre = WaterSurfaceLod::SpacingPerMetre(bounds, camera.GetProjection(), 256u, 144u);
    ASSERT_GT(perMetre, 0.0f);

    // Walk one column from the near skirt toward the horizon. Spacing must grow
    // monotonically with distance and stay finite, and the near rows must be
    // sampled far finer than the world grid's 1.95 m ever was.
    f32 previousSpacing = -1.0f;
    f32 previousT = -1.0f;
    i32 rowsChecked = 0;
    for (i32 i = 0; i < 64; ++i)
    {
        const f32 v = 0.9f * static_cast<f32>(i) / 63.0f;
        const glm::vec2 ndc(0.0f, glm::mix(bounds.m_Min.y, bounds.m_Max.y, v));
        const glm::vec3 hit = WaterSurfaceLod::ProjectGridVertex(invViewProj, kLowGrazing.m_Eye, ndc,
                                                                 kPlanePoint, kPlaneNormal, kRimRadius);
        const glm::vec3 toHit = hit - kLowGrazing.m_Eye;
        const f32 t = glm::length(toHit);
        if (!(t > 0.0f) || std::abs(hit.y - kPlanePoint.y) > 1e-2f)
            continue; // a missed row, which the rim handles
        const f32 dirDotNormal = glm::dot(toHit / t, kPlaneNormal);
        const f32 spacing = WaterSurfaceLod::ProjectedGridSpacing(perMetre, t, dirDotNormal);
        ASSERT_TRUE(std::isfinite(spacing)) << "row " << i;
        if (previousSpacing >= 0.0f && t > previousT)
        {
            EXPECT_GE(spacing, previousSpacing) << "row " << i << ": spacing shrank with distance";
        }
        previousSpacing = spacing;
        previousT = t;
        ++rowsChecked;
        if (i == 0)
        {
            EXPECT_LT(spacing, 0.5f) << "the nearest row is sampled at " << spacing << " m";
        }
    }
    EXPECT_GT(rowsChecked, 40);
}

TEST(WaterGeometryLodProfile, ProjectedGridMissedRayLandsBeyondTheRim)
{
    // A row above the horizon has to go SOMEWHERE finite. It is pushed out
    // along its own horizontal direction past the rim radius, so the caller's
    // rect clamp puts it on the surface edge and the patch collapses to zero
    // area instead of extending the ocean into the sky.
    const glm::mat4 viewProj = MakeViewProj(kLowGrazing);
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    const glm::vec3 skyward = WaterSurfaceLod::ProjectGridVertex(
        invViewProj, kLowGrazing.m_Eye, { 0.0f, 0.99f }, kPlanePoint, kPlaneNormal, kRimRadius);

    ASSERT_TRUE(std::isfinite(skyward.x) && std::isfinite(skyward.y) && std::isfinite(skyward.z));
    EXPECT_NEAR(skyward.y, kPlanePoint.y, 1e-3f) << "a missed ray still lands ON the plane";
    const f32 radius = std::sqrt(skyward.x * skyward.x + skyward.z * skyward.z);
    EXPECT_GT(radius, kRimRadius * 0.5f)
        << "a missed ray must land outside any plausible surface rect, radius was " << radius;
}

TEST(WaterGeometryLodProfile, ClampToRectKeepsAFiniteSurfaceFinite)
{
    constexpr f32 kHalfX = 500.0f;
    constexpr f32 kHalfZ = 300.0f;
    EXPECT_EQ(WaterSurfaceLod::ClampToRect({ 0.0f, 0.0f }, kHalfX, kHalfZ), glm::vec2(0.0f, 0.0f));
    EXPECT_EQ(WaterSurfaceLod::ClampToRect({ 9000.0f, -9000.0f }, kHalfX, kHalfZ), glm::vec2(kHalfX, -kHalfZ));
    EXPECT_EQ(WaterSurfaceLod::ClampToRect({ -1.0f, 400.0f }, kHalfX, kHalfZ), glm::vec2(-1.0f, kHalfZ));
}

TEST(WaterGeometryLodProfile, TessLevelFloorSurvivesADisabledToggle)
{
    // The regression this whole family exists next to: a level below 1 discards
    // the patch, and "tessellation off" sends factor 0. Every projected-grid
    // change keeps the floor.
    const glm::vec4 disabled(0.0f, 10.0f, 200.0f, 1.0f);
    EXPECT_GE(WaterSurfaceLod::CalcTessLevel({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                                             { 0.0f, 2.0f, 0.0f }, disabled),
              1.0f);
    EXPECT_GE(WaterSurfaceLod::CalcTessLevel({ 0.0f, 0.0f, -900.0f }, { 1.0f, 0.0f, -900.0f },
                                             { 0.0f, 2.0f, 0.0f }, disabled),
              1.0f);
}

// =============================================================================
// The two invariants a projected-grid change is most likely to break silently
// =============================================================================

namespace
{
    namespace fs = std::filesystem;

    // The repo's "source-tree path, independent of the binary's cwd" idiom
    // (ADR 0003); the cwd walk is the fallback for a standalone harness that
    // does not define the macro.
    [[nodiscard]] fs::path FindShader(const std::string& relative)
    {
#ifdef OLO_TEST_EDITOR_ROOT
        if (const fs::path fromRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / relative;
            fs::exists(fromRoot))
        {
            return fromRoot;
        }
#endif
        fs::path candidate = fs::current_path();
        for (i32 depth = 0; depth < 6; ++depth)
        {
            for (const std::string prefix : { std::string("assets/shaders/"),
                                              std::string("OloEditor/assets/shaders/") })
            {
                if (fs::exists(candidate / (prefix + relative)))
                    return candidate / (prefix + relative);
            }
            if (!candidate.has_parent_path() || candidate == candidate.parent_path())
                break;
            candidate = candidate.parent_path();
        }
        return {};
    }

    [[nodiscard]] std::string ReadShader(const std::string& relative)
    {
        const fs::path path = FindShader(relative);
        if (path.empty())
            return {};
        std::ifstream file(path);
        if (!file.is_open())
            return {};
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    /// The WaterParams block as one file declares it, stripped of comments and
    /// whitespace so only the member list is compared.
    [[nodiscard]] std::string ExtractWaterParamsMembers(const std::string& source)
    {
        const sizet blockStart = source.find("uniform WaterParams");
        if (blockStart == std::string::npos)
            return {};
        const sizet open = source.find('{', blockStart);
        const sizet close = source.find("};", open);
        if (open == std::string::npos || close == std::string::npos)
            return {};

        std::string out;
        const std::string body = source.substr(open + 1, close - open - 1);
        for (sizet i = 0; i < body.size(); ++i)
        {
            if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/')
            {
                while (i < body.size() && body[i] != '\n')
                    ++i;
                continue;
            }
            if (!std::isspace(static_cast<unsigned char>(body[i])))
                out.push_back(body[i]);
        }
        return out;
    }

    constexpr const char* kWaterShaderFiles[] = {
        "Water.glsl",
        "Water_Depth.glsl",
        "include/WaterVertexStage.glsl",
        "include/WaterTessControlStage.glsl",
        "include/WaterTessEvalStage.glsl",
    };
} // namespace

TEST(WaterGeometryLodProfile, WaterParamsBlockIsIdenticalInEveryWaterShader)
{
    // GL requires a uniform block shared across a program's stages to be
    // declared the same way in each, so a field appended to four of these five
    // files is a LINK error — but a field appended to the C++ struct and to
    // NONE of them is worse: the block is then a valid prefix, every shader
    // links, and the new fields silently read as whatever the previous upload
    // left. ShaderUBOSizeConsistencyTest only rejects GLSL that is LARGER than
    // the C++ side, so that case has no other guard.
    std::string reference;
    std::string referenceFile;
    for (const char* file : kWaterShaderFiles)
    {
        const std::string source = ReadShader(file);
        ASSERT_FALSE(source.empty()) << "could not read " << file << " from " << fs::current_path();
        const std::string members = ExtractWaterParamsMembers(source);
        ASSERT_FALSE(members.empty()) << file << " has no WaterParams block";
        if (reference.empty())
        {
            reference = members;
            referenceFile = file;
            continue;
        }
        EXPECT_EQ(members, reference)
            << file << " declares WaterParams differently from " << referenceFile;
    }
    EXPECT_NE(reference.find("u_ProjectedGridParams;"), std::string::npos)
        << "the projected-grid params are missing from the shared block";
}

TEST(WaterGeometryLodProfile, DepthCaptureReplaysTheSameDisplacementChain)
{
    // #1035's third acceptance criterion. The surface-depth capture feeds the
    // underwater fog the height of the surface that was DRAWN; if it places its
    // grid differently the fog sits at a different height than the water.
    //
    // The dispatch makes this structural — the capture is the same draw command
    // with only the fragment program swapped (CommandDispatch.cpp, "the same
    // VS/TCS/TES displacement chain with a no-color-output fragment stage") —
    // so the only way to break it is to fork the stage sources. This asserts
    // they are not forked.
    const std::string colorPass = ReadShader("Water.glsl");
    const std::string depthPass = ReadShader("Water_Depth.glsl");
    ASSERT_FALSE(colorPass.empty());
    ASSERT_FALSE(depthPass.empty());

    for (const char* stage : { "include/WaterVertexStage.glsl",
                               "include/WaterTessControlStage.glsl",
                               "include/WaterTessEvalStage.glsl" })
    {
        const std::string directive = std::string("#include \"") + stage + "\"";
        EXPECT_NE(colorPass.find(directive), std::string::npos)
            << "Water.glsl no longer includes " << stage;
        EXPECT_NE(depthPass.find(directive), std::string::npos)
            << "Water_Depth.glsl no longer includes " << stage
            << " — the depth capture would stop replaying the colour pass's displacement chain";
    }
}
