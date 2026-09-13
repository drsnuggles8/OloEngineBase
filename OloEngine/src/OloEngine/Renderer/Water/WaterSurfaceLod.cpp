#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterSurfaceLod.h"
#include "OloEngine/Renderer/WaterSurface.h"

#include <limits>

namespace OloEngine::WaterSurfaceLod
{
    namespace
    {
        constexpr f32 kTwoPi = 6.28318530f;

        // A ray whose direction is this close to parallel with the plane is
        // treated as a miss. Not an epsilon guarding a division so much as the
        // horizon itself: |dot(dir, n)| falls continuously to 0 there, and the
        // intersection distance grows without bound as it does.
        constexpr f32 kParallelEpsilon = 1e-6f;

        /// Cast the view ray through one NDC coordinate and intersect it with the
        /// plane. `outOrigin` is the camera, `outDir` a UNIT direction and
        /// `outT` a distance in METRES, so the only validity test is "in front
        /// of the camera". On a miss the out-parameters still hold the ray, and
        /// ProjectGridVertex reads them in exactly that case to slide the vertex
        /// out to the rim along its own direction.
        ///
        /// The obvious alternative — unproject NDC z = -1 and z = +1 and take t
        /// as a fraction of that segment — is what this did first, and it was
        /// wrong on screen while every CPU test passed: it has to KNOW which NDC
        /// depth is the near plane, that is a per-backend, per-projection
        /// assumption (RHI/RHIProjectionSeam.h), and when it does not hold every
        /// vertex fails the `t <= 1` test and collapses onto the horizon. This
        /// form assumes nothing about the depth convention: any NDC z gives a
        /// point on the same view ray, and normalising removes the magnitude
        /// that depended on which one it was.
        bool RayPlaneHit(const glm::mat4& invViewProj, const glm::vec3& cameraPos,
                         const glm::vec2& ndc, const glm::vec3& planePoint,
                         const glm::vec3& planeNormal, glm::vec3& outOrigin, glm::vec3& outDir,
                         f32& outT)
        {
            outOrigin = cameraPos;
            outDir = glm::vec3(0.0f);
            outT = -1.0f;

            const glm::vec4 farH = invViewProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
            if (std::abs(farH.w) < kParallelEpsilon)
                return false;

            const glm::vec3 toFar = (glm::vec3(farH) / farH.w) - cameraPos;
            const f32 lenSq = glm::dot(toFar, toFar);
            if (!std::isfinite(lenSq) || lenSq < kParallelEpsilon)
                return false;
            outDir = toFar / std::sqrt(lenSq);

            const f32 denom = glm::dot(outDir, planeNormal);
            if (std::abs(denom) < kParallelEpsilon)
                return false;

            outT = glm::dot(planePoint - cameraPos, planeNormal) / denom;
            // Metres along a unit ray. Negative means the plane is behind the
            // camera; there is no upper bound, because a hit near the horizon is
            // legitimately far away and the caller's rect clamp is what bounds it.
            return outT > 0.0f && std::isfinite(outT);
        }
    } // namespace

    f32 MaxWaveDisplacement(const glm::vec4& waveParams, const glm::vec4& waveDir0,
                            const glm::vec4& waveDir1)
    {
        const f32 freq = std::max(waveParams.w, 0.01f);
        const f32 amp = waveParams.z;

        const f32 wl0 = std::max(waveDir0.w, 0.1f) / freq;
        const f32 wl1 = std::max(waveDir1.w, 0.1f) / freq;
        const f32 a0 = waveDir0.z * wl0 / kTwoPi;
        const f32 a1 = waveDir1.z * wl1 / kTwoPi;

        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSt = (waveDir0.z + waveDir1.z) * 0.5f;
        // Detail-octave amplitude weights sum to 0.5+0.4+0.3+0.22+0.15+0.1.
        const f32 octaveSum = (avgSt * avgWL / kTwoPi) * 1.67f;

        return amp * (a0 * 0.55f + a1 * 0.55f + octaveSum) * 1.5f;
    }

    f32 MaxSurfaceDisplacement(const glm::vec4& waveParams, const glm::vec4& waveDir0,
                               const glm::vec4& waveDir1)
    {
        const f32 freq = std::max(waveParams.w, 0.01f);
        const f32 amp = waveParams.z;

        const f32 wl0 = std::max(waveDir0.w, 0.1f) / freq;
        const f32 wl1 = std::max(waveDir1.w, 0.1f) / freq;

        // One octave's amplitude, exactly as gerstnerWave derives it: the
        // displacement is `steepness / k` scaled by the global amplitude and the
        // octave's weight, and k is 2*pi / wavelength.
        const auto octaveAmplitude = [](f32 steepness, f32 wavelength, f32 weight, f32 globalAmplitude)
        {
            const f32 k = kTwoPi / std::max(wavelength, 0.001f);
            return (steepness / k) * globalAmplitude * weight;
        };

        f32 total = octaveAmplitude(waveDir0.z, wl0, 0.55f, amp) + octaveAmplitude(waveDir1.z, wl1, 0.55f, amp);

        // The six detail octaves, from the one table WaterSurfaceSamplerTest
        // pins against the shader — the real constants, not the largest-octave
        // fall-back the culling bound uses.
        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSteepness = (waveDir0.z + waveDir1.z) * 0.5f;
        for (const WaterSurface::DetailOctave& o : WaterSurface::kDetailOctaves)
        {
            total += octaveAmplitude(avgSteepness * o.m_SteepnessMul, avgWL * o.m_WavelengthMul,
                                     o.m_AmplitudeWeight, amp);
        }
        return total;
    }

    bool IsPatchOutsideFrustum(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                               f32 margin, const glm::mat4& viewProj)
    {
        // GLSL is column-major and indexes vp[col][row]; glm matches, so these
        // subscripts are the shader's verbatim.
        const glm::vec4 row0(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
        const glm::vec4 row1(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
        const glm::vec4 row2(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
        const glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

        const glm::vec4 planes[6] = {
            row3 + row0, // left
            row3 - row0, // right
            row3 + row1, // bottom
            row3 - row1, // top
            row3 + row2, // near (GL [-1,1] clip-space depth)
            row3 - row2, // far
        };

        for (const auto& plane : planes)
        {
            const glm::vec3 n(plane);
            const f32 threshold = -margin * glm::length(n);
            if ((glm::dot(n, p0) + plane.w) < threshold && (glm::dot(n, p1) + plane.w) < threshold && (glm::dot(n, p2) + plane.w) < threshold)
            {
                return true;
            }
        }
        return false;
    }

    f32 CalcTessLevel(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& cameraPos,
                      const glm::vec4& tessParams)
    {
        const f32 maxFactor = std::max(tessParams.x, 1.0f);
        const f32 minDist = tessParams.y;
        const f32 maxDist = tessParams.z;

        const glm::vec3 mid = (p0 + p1) * 0.5f;
        const f32 dist = glm::distance(cameraPos, mid);

        const f32 t = std::clamp((dist - minDist) / std::max(maxDist - minDist, 0.001f), 0.0f, 1.0f);
        return std::max(glm::mix(maxFactor, 1.0f, t), 1.0f);
    }

    NdcBounds ComputeNdcBounds(const glm::mat4& viewProj, const glm::vec3& cameraPos,
                               const glm::vec3& planePoint, const glm::vec3& planeNormal,
                               f32 displacementMargin)
    {
        NdcBounds bounds;

        const f32 normalLenSq = glm::dot(planeNormal, planeNormal);
        if (!std::isfinite(normalLenSq) || normalLenSq < 1e-12f)
            return bounds;
        const glm::vec3 n = planeNormal / std::sqrt(normalLenSq);

        const f32 margin = (std::isfinite(displacementMargin) && displacementMargin > 0.0f)
                               ? displacementMargin
                               : 0.0f;

        const glm::mat4 invViewProj = glm::inverse(viewProj);
        // A singular / non-finite view-projection reaches us as NaNs here. Bail
        // to the full screen rather than letting them become vertex positions;
        // the surface then clamps to its rim and draws nothing, which is the
        // honest picture of "this camera has no valid frustum".
        for (i32 col = 0; col < 4; ++col)
        {
            for (i32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(invViewProj[col][row]))
                    return bounds;
            }
        }

        // An orthonormal basis in the plane, for the horizontal half of the
        // displacement. Which way the two axes point does not matter — both
        // signs of each are applied.
        const glm::vec3 seed = (std::abs(n.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 e1 = glm::normalize(glm::cross(seed, n));
        const glm::vec3 e2 = glm::cross(n, e1);

        glm::vec2 lo(std::numeric_limits<f32>::max());
        glm::vec2 hi(std::numeric_limits<f32>::lowest());
        bool any = false;
        // The NDC y of the closest base-plane hit, which tells us which end of
        // the rectangle is the near one without assuming a sign convention.
        f32 nearestHitDistance = std::numeric_limits<f32>::max();
        f32 nearestHitY = 0.0f;
        f32 farthestHitDistance = -1.0f;
        f32 farthestHitY = 0.0f;

        // Accumulate the NDC of one candidate base-plane position.
        const auto accumulate = [&](const glm::vec3& basePoint)
        {
            const glm::vec4 clip = viewProj * glm::vec4(basePoint, 1.0f);
            if (!(clip.w > kParallelEpsilon) || !std::isfinite(clip.x) || !std::isfinite(clip.y))
                return;
            const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
            if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
                return;
            lo = glm::min(lo, ndc);
            hi = glm::max(hi, ndc);
            any = true;
        };

        // The three slabs of the displaceable volume: the surface's highest
        // reach, its resting plane, and its lowest. A ray that misses the base
        // plane inside the depth range can still meet one of the offsets — that
        // is the whole point, and it is what pushes the rectangle past the
        // screen edge where the displacement needs the cover.
        // The three slabs of the displaceable volume: the surface's highest
        // reach, its resting plane, and its lowest. A ray that misses the base
        // plane inside the depth range can still meet one of the offsets — that
        // is the whole point, and it is what pushes the rectangle past the
        // screen edge where the displacement needs the cover. Slab 1 is the
        // base plane; it is identified by index, never by comparing the float.
        const f32 slabOffsets[3] = { margin, 0.0f, -margin };
        constexpr i32 kBaseSlab = 1;
        // With no margin all three slabs coincide; walk the base one only.
        const i32 slabCount = (margin > 0.0f) ? 3 : 1;
        constexpr f32 kStep = 2.0f / static_cast<f32>(kNdcProbeSteps - 1);

        for (i32 iy = 0; iy < kNdcProbeSteps; ++iy)
        {
            const f32 y = -1.0f + static_cast<f32>(iy) * kStep;
            for (i32 ix = 0; ix < kNdcProbeSteps; ++ix)
            {
                const f32 x = -1.0f + static_cast<f32>(ix) * kStep;
                // The ray depends on (x, y) only — cast it once, intersect it
                // with each slab.
                glm::vec3 rayOrigin(0.0f);
                glm::vec3 rayDir(0.0f);
                f32 tBase = 0.0f;
                const bool baseHit =
                    RayPlaneHit(invViewProj, cameraPos, { x, y }, planePoint, n, rayOrigin, rayDir, tBase);
                const f32 denom = glm::dot(rayDir, n);
                if (!(glm::dot(rayDir, rayDir) > 0.0f) || std::abs(denom) < kParallelEpsilon)
                    continue;

                for (i32 slab = 0; slab < slabCount; ++slab)
                {
                    const i32 slabIndex = (slabCount == 1) ? kBaseSlab : slab;
                    const glm::vec3 slabPoint = planePoint + n * slabOffsets[slabIndex];
                    const f32 t = (slabIndex == kBaseSlab)
                                      ? tBase
                                      : glm::dot(slabPoint - rayOrigin, n) / denom;
                    const bool hit = (slabIndex == kBaseSlab) ? baseHit : (t > 0.0f && std::isfinite(t));
                    if (!hit)
                        continue;

                    // Flatten the hit onto the BASE plane: the grid is laid out
                    // there, and this is the base position whose displaced form
                    // can reach this pixel.
                    const glm::vec3 hitPoint = rayOrigin + rayDir * t;
                    const glm::vec3 flat = hitPoint - n * glm::dot(hitPoint - planePoint, n);
                    if (slabIndex == kBaseSlab)
                    {
                        if (t < nearestHitDistance)
                        {
                            nearestHitDistance = t;
                            nearestHitY = y;
                        }
                        if (t > farthestHitDistance)
                        {
                            farthestHitDistance = t;
                            farthestHitY = y;
                        }
                    }

                    accumulate(flat);
                    if (margin > 0.0f)
                    {
                        // ...and the same for the horizontal half of the
                        // displacement, which slides a vertex sideways by up to
                        // the same bound (Gerstner's choppy term, the FFT's
                        // horizontal cascade).
                        accumulate(flat + e1 * margin);
                        accumulate(flat - e1 * margin);
                        accumulate(flat + e2 * margin);
                        accumulate(flat - e2 * margin);
                    }
                }
            }
        }

        if (!any)
            return bounds;

        // Widen by one probe pitch so the partially covered band outside the
        // sampled lattice stays inside the grid, then cap. The cap is not a
        // tidy-up: as the camera approaches the water plane the bottom row's
        // intersection runs off toward the horizon and the excursion has no
        // finite bound, which would spend the whole grid on a skirt.
        bounds.m_Min = glm::max(lo - glm::vec2(kStep), glm::vec2(-kNdcBoundsCap));
        bounds.m_Max = glm::min(hi + glm::vec2(kStep), glm::vec2(kNdcBoundsCap));
        // Which y edge is "near" exists to keep the mesh's authored winding
        // front-facing once its rows are re-laid along screen y, so decide it
        // by that invariant rather than by a proxy for it. The vertex stage
        // maps v = 0 -> far edge and v = 1 -> near edge, and CreateWaterGrid's
        // index order needs (dP/dv x dP/du) . n > 0. Evaluate exactly that on
        // three real hits: the nearest and farthest probe rows for dP/dv, and
        // one column step along the nearest row for dP/du. If the sign comes
        // out negative the ends are swapped. This is well-defined at every
        // pitch including straight down, where "near" and "far" are
        // equidistant and a distance rule has nothing to choose by.
        bool nearIsMax = nearestHitDistance < std::numeric_limits<f32>::max() && farthestHitDistance >= 0.0f && nearestHitY > farthestHitY;
        if (nearestHitDistance < std::numeric_limits<f32>::max() && farthestHitDistance >= 0.0f)
        {
            glm::vec3 o0(0.0f), d0(0.0f), o1(0.0f), d1(0.0f), o2(0.0f), d2(0.0f);
            f32 t0 = 0.0f, t1 = 0.0f, t2 = 0.0f;
            const f32 xMid = 0.0f;
            const bool ok = RayPlaneHit(invViewProj, cameraPos, { xMid, nearestHitY }, planePoint, n, o0, d0, t0) && RayPlaneHit(invViewProj, cameraPos, { xMid, farthestHitY }, planePoint, n, o1, d1, t1) && RayPlaneHit(invViewProj, cameraPos, { xMid + kStep, nearestHitY }, planePoint, n, o2, d2, t2);
            if (ok)
            {
                const glm::vec3 pNear = o0 + d0 * t0;
                const glm::vec3 pFar = o1 + d1 * t1;
                const glm::vec3 pNearPlusU = o2 + d2 * t2;
                // Candidate order: near = the nearest-hit row. dP/dv runs far -> near.
                const glm::vec3 dPdv = pNear - pFar;
                const glm::vec3 dPdu = pNearPlusU - pNear;
                const f32 facing = glm::dot(glm::cross(dPdv, dPdu), n);
                if (std::isfinite(facing) && std::abs(facing) > 0.0f)
                {
                    // nearestHitY is the near candidate; a negative facing means
                    // the OTHER end has to play "near" for the winding to hold.
                    const bool nearestIsMaxEnd = nearestHitY > farthestHitY;
                    nearIsMax = (facing > 0.0f) ? nearestIsMaxEnd : !nearestIsMaxEnd;
                }
            }
        }
        bounds.m_NearEdgeY = nearIsMax ? bounds.m_Max.y : bounds.m_Min.y;
        bounds.m_FarEdgeY = nearIsMax ? bounds.m_Min.y : bounds.m_Max.y;
        bounds.m_Visible = true;
        return bounds;
    }

    glm::vec3 ProjectGridVertex(const glm::mat4& invViewProj, const glm::vec3& cameraPos,
                                const glm::vec2& ndc, const glm::vec3& planePoint,
                                const glm::vec3& planeNormal, f32 rimRadius)
    {
        // See the note in ComputeNdcBounds: explicitly zeroed rather than `{}`,
        // and the miss path below genuinely reads them.
        glm::vec3 rayOrigin(0.0f);
        glm::vec3 rayDir(0.0f);
        f32 t = 0.0f;
        if (RayPlaneHit(invViewProj, cameraPos, ndc, planePoint, planeNormal, rayOrigin, rayDir, t))
            return rayOrigin + rayDir * t;

        // The ray missed: it is above the horizon, parallel to the surface, or
        // the crossing sits outside the depth range. Slide out along whatever
        // horizontal component the ray has, far enough that the caller's rect
        // clamp lands the vertex on the surface rim. A ray with no horizontal
        // component at all (looking straight down a normal) cannot miss, so the
        // degenerate fallback below is unreachable in practice and exists only
        // so the function is total.
        const glm::vec3 horizontal = rayDir - planeNormal * glm::dot(rayDir, planeNormal);
        const f32 lenSq = glm::dot(horizontal, horizontal);
        if (!std::isfinite(lenSq) || lenSq < kParallelEpsilon)
            return planePoint;

        // Start from the ray origin projected ONTO the plane rather than from
        // the plane's centre: a camera far off to one side of a small surface
        // must push its missing rows out on its own side, or the whole missed
        // band would fold back across the rect and draw water where the sky is.
        const glm::vec3 originOnPlane =
            rayOrigin - planeNormal * glm::dot(rayOrigin - planePoint, planeNormal);
        return originOnPlane + (horizontal / std::sqrt(lenSq)) * rimRadius;
    }

    bool PackProjectedGrid(const ProjectedGridInputs& in, glm::vec4& outParams, glm::vec4& outParams2)
    {
        // Disabled by default, with the NDC rectangle left as the whole screen
        // so the packed value is coherent even though the shader never reads
        // it while .x is 0.
        outParams = glm::vec4(0.0f, -1.0f, -1.0f, 0.0f);
        outParams2 = glm::vec4(in.m_HalfExtentX, in.m_HalfExtentZ, 1.0f, 1.0f);

        // The surface plane in the space `m_ViewProjection` maps from: the grid
        // is built in XZ with +Y up, so the plane normal is the transformed up
        // axis (column 1) and its centre the translation (column 3) — the same
        // derivation the planar-reflection plane uses.
        //
        // The IN-PLANE axes matter as much as the normal here, and this is the
        // one place that is true: the vertex stage inverts the model matrix to
        // clamp a projected vertex into the authored rect, so a transform that
        // is singular in x or z makes every water vertex NaN. The world-space
        // grid inverts nothing and just draws a zero-area surface, so its path
        // has no such guard.
        const glm::vec3 planeNormal(in.m_Model[1]);
        const glm::vec3 planePoint(in.m_Model[3]);
        const f32 normalLenSq = glm::dot(planeNormal, planeNormal);
        const f32 axisXLenSq = glm::dot(glm::vec3(in.m_Model[0]), glm::vec3(in.m_Model[0]));
        const f32 axisZLenSq = glm::dot(glm::vec3(in.m_Model[2]), glm::vec3(in.m_Model[2]));
        const bool usable = std::isfinite(normalLenSq) && normalLenSq > 1e-6f && std::isfinite(axisXLenSq) && axisXLenSq > 1e-6f && std::isfinite(axisZLenSq) && axisZLenSq > 1e-6f && std::isfinite(planePoint.x) && std::isfinite(planePoint.y) && std::isfinite(planePoint.z);
        if (!usable)
            return false; // the surface draws as the world-space grid it always was

        // A perspective projection writes clip w = -z_view (P[2][3] == -1, and
        // the backend seam leaves that row alone); an orthographic one writes
        // w = 1 there. The ray the vertex stage casts is a pinhole ray, so an
        // orthographic camera has to take the world-space grid instead.
        if (!std::isfinite(in.m_Projection[2][3]) || std::abs(in.m_Projection[2][3]) < 0.5f)
            return false;

        // MaxSurfaceDisplacement, NOT the tess-control cull's MaxWaveDisplacement
        // — the header explains why the two must differ. `m_VerticalExtent` is
        // the draw's own bound and the only one that describes an FFT ocean.
        //
        // Then capped below the eye's height above the plane. The layout
        // rectangle grows with the margin, and once the margin reaches the
        // camera the displaceable slab swallows the viewpoint and the rectangle
        // runs away to kNdcBoundsCap — at a 3 m eye, a 2 m margin already put
        // 92% of the grid off screen. An FFT ocean's bound is 4 m at the
        // component defaults, so without this cap a deck-height camera over FFT
        // water drew ~30 rows across the whole frame. Crests that reach above
        // the eye belong to the waterline regime a projected grid is not built
        // for, and giving up coverage of them beats giving up the grid.
        const f32 eyeHeight = std::abs(glm::dot(in.m_CameraPosition - planePoint, planeNormal)) / std::sqrt(normalLenSq);
        const f32 displacementMargin = std::min(
            std::max(MaxSurfaceDisplacement(in.m_WaveParams, in.m_WaveDir0, in.m_WaveDir1),
                     in.m_VerticalExtent),
            kLayoutMarginEyeFraction * eyeHeight);

        const NdcBounds bounds =
            ComputeNdcBounds(in.m_ViewProjection, in.m_CameraPosition, planePoint, planeNormal, displacementMargin);
        const f32 spacingPerMetre =
            SpacingPerMetre(bounds, in.m_Projection, in.m_GridResolutionX, in.m_GridResolutionZ);

        // .z / .w carry the NEAR and FAR y edges, decided by geometry — which of
        // min/max is near depends on the backend's NDC convention.
        outParams = glm::vec4(1.0f, bounds.m_Min.x, bounds.m_NearEdgeY, spacingPerMetre);
        outParams2 = glm::vec4(in.m_HalfExtentX, in.m_HalfExtentZ, bounds.m_Max.x, bounds.m_FarEdgeY);
        return true;
    }

    f32 SpacingPerMetre(const NdcBounds& bounds, const glm::mat4& gpuProjection, u32 gridResolutionX,
                        u32 gridResolutionZ)
    {
        const f32 p00 = std::abs(gpuProjection[0][0]);
        const f32 p11 = std::abs(gpuProjection[1][1]);
        if (!std::isfinite(p00) || !std::isfinite(p11) || p00 < kParallelEpsilon || p11 < kParallelEpsilon)
            return 0.0f;
        // One grid cell in NDC, divided by the focal term, is the view-space
        // angle between neighbouring vertices; that angle times the distance is
        // the world spacing. The coarser axis is the one the band-limit is
        // about.
        const f32 stepX = (bounds.m_Max.x - bounds.m_Min.x) / static_cast<f32>(std::max(gridResolutionX, 1u));
        const f32 stepY = (bounds.m_Max.y - bounds.m_Min.y) / static_cast<f32>(std::max(gridResolutionZ, 1u));
        const f32 perMetre = std::max(stepX / p00, stepY / p11);
        return std::isfinite(perMetre) ? std::max(perMetre, 0.0f) : 0.0f;
    }
} // namespace OloEngine::WaterSurfaceLod
