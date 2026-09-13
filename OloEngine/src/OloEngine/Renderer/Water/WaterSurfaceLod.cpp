#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterSurfaceLod.h"

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

        // WaterCommon.glsl's six detail octaves: wavelength ratio, steepness
        // ratio, global amplitude weight — the real constants, not the
        // largest-octave fall-back the culling bound uses.
        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSteepness = (waveDir0.z + waveDir1.z) * 0.5f;
        constexpr f32 kDetail[6][3] = {
            { 0.85f, 0.5f, 0.5f },
            { 0.6f, 0.45f, 0.4f },
            { 0.4f, 0.38f, 0.3f },
            { 0.25f, 0.3f, 0.22f },
            { 0.15f, 0.22f, 0.15f },
            { 0.09f, 0.15f, 0.1f },
        };
        for (const auto& detail : kDetail)
        {
            total += octaveAmplitude(avgSteepness * detail[1], avgWL * detail[0], detail[2], amp);
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
        const f32 slabOffsets[3] = { margin, 0.0f, -margin };
        constexpr f32 kStep = 2.0f / static_cast<f32>(kNdcProbeSteps - 1);

        for (i32 iy = 0; iy < kNdcProbeSteps; ++iy)
        {
            const f32 y = -1.0f + static_cast<f32>(iy) * kStep;
            for (i32 ix = 0; ix < kNdcProbeSteps; ++ix)
            {
                const f32 x = -1.0f + static_cast<f32>(ix) * kStep;
                for (const f32 offset : slabOffsets)
                {
                    const glm::vec3 slabPoint = planePoint + n * offset;
                    // Explicitly zeroed, not `{}`: glm is built here WITHOUT
                    // GLM_FORCE_CTOR_INIT, so a default-constructed vec is not
                    // a documented zero — and RayPlaneHit can return false
                    // before it writes either of these.
                    glm::vec3 rayOrigin(0.0f);
                    glm::vec3 rayDir(0.0f);
                    f32 t = 0.0f;
                    if (!RayPlaneHit(invViewProj, cameraPos, { x, y }, slabPoint, n, rayOrigin,
                                     rayDir, t))
                        continue;

                    // Flatten the hit onto the BASE plane: the grid is laid out
                    // there, and this is the base position whose displaced form
                    // can reach this pixel.
                    const glm::vec3 hit = rayOrigin + rayDir * t;
                    const glm::vec3 flat = hit - n * glm::dot(hit - planePoint, n);

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
