#pragma once

// =============================================================================
// Distance-impostor reflection probes (issue #705) — CPU half of the contract.
//
// ENCODING CONTRACT (the GLSL raymarch in include/ReflectionProbes.glsl and
// the bake in ReflectionProbeBaker.cpp both mirror this file; change them
// together):
//
//  - A probe's distance cubemap stores, per texel direction u, THREE LINEAR
//    RADIAL DISTANCES in world units from the probe centre (issue #1804):
//      Front  (d0) — the first surface along u (any facing),
//      Back   (d1) — the first surface beyond d0: the far side of the first
//                    occluder for a closed mesh,
//      Behind (d2) — the first surface beyond d1: what that occluder hides.
//    Probe space = world space translated so the probe centre is the
//    origin; no rotation, no scale, so distances are world-metric.
//    A ray point at radius r along u is SOLID iff it lies in [d0, d1] or at
//    or beyond d2; the GAP (d1, d2) is free space hidden from the probe.
//    With nothing behind the first occluder (a room's wall, the sky) the
//    bake stores d2 = d1: no gap, everything beyond d0 is solid, which is
//    the single-distance encoding this extends (a closed room, an open
//    plane). The gap exists so a reflection ray that passes behind an
//    occluder keeps going instead of reporting a hit in the occluder's
//    radial shadow, which stretched its silhouette (#1804).
//  - Fixed face resolution kProbeDistanceResolution (all probes share one
//    cubemap array, so the resolution cannot be per-probe). Format RGBA16F
//    on the GPU (r = d0, g = d1, b = d2, a = 0; half precision is 1/2048
//    relative, far inside the 1% inside bias), kProbeDistanceChannels f32 per
//    texel on the CPU. Face order and orientation match
//    GL_TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z exactly as captured by
//    ReflectionProbeBaker's face target/up tables.
//  - MISS SENTINEL: texels with no geometry keep the capture clear value
//    kProbeDistanceFar (== the capture far plane) in all three layers. Any
//    stored distance >= kProbeDistanceMissThreshold means "no surface in this
//    direction" (sky). The sentinel is deliberately the far plane and not a
//    huge float, and "no gap" is d2 = d1 and not d2 = sentinel, so any
//    consumer that does interpolate (a mip, a debug view) blends toward
//    values the march's tMax already excludes.
//  - SAMPLING: the march reads mip 0 at the CENTRE of the texel covering a
//    direction (oloProbeTexelCentre in the GLSL), never a bilinear blend
//    (#1804). A blend across a silhouette mixes an occluder's layers with the
//    wall's behind it into a solid shell at a distance where neither surface
//    is; a ray passing behind the occluder hits that shell and the silhouette
//    stretches. A centre sample returns the texel whatever the sampler's
//    filter, on both backends; the CPU reference (SampleLayersNearest) reads
//    the same texel.
//  - MIP CHAIN: strict 2x2 MAX-downsample per level and per layer
//    (BuildNextMaxMip), never a box average and never
//    glGenerateTextureMipmap. The only mip consumer is the cheap visibility
//    reject, which reads d0 and needs an UPPER bound of distance over the
//    footprint: "probe sees point iff |x-o| <= dist + margin" with a
//    max-filtered dist can only over-admit (wasted march), never
//    over-reject (missing reflection). The raymarch itself samples mip 0.
//  - MaxFiniteDistance (dMax) = max over mip-0 d0 and d2 values strictly
//    below the miss threshold (far plane when every texel is sky). Any
//    surface crossing of a ray starting at probe-space p0 satisfies
//    t <= |p0| + dMax, which bounds the coarse march.
//
// RAYMARCH (Szirmay-Kalos et al., "Approximate Ray-Tracing on the GPU with
// Distance Impostors", Eurographics 2005 — re-derived, not transcribed, and
// extended with the two peeled layers above): march the reflection ray
// p(t) = p0 + t*dir in kProbeMarchSteps uniform steps over (0, tMax]; the
// first free->solid transition brackets the surface; kProbeRefineSteps
// bisection steps refine it. Two consecutive FREE samples of different kinds
// (one in front of d0, one in the gap) may straddle a thin occluder a coarse
// step jumped over, so kProbeGapRefineSteps bisection steps look for a solid
// sample between them. No transition within tMax = miss (the ray leaves
// through sky). The refined direction normalize(p(tHit)) is the
// parallax-corrected lookup direction — unless the hit lies on the d2
// surface, which the probe cannot see: its radiance in that direction is the
// occluder's. Such a hit takes the radiance of the last point the march saw
// IN FRONT of d0 (the edge of the occluder's shadow the ray entered
// through), and is a miss when the ray never saw one.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

#include <span>
#include <type_traits>
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    // ---- Contract constants (mirrored by include/ReflectionProbes.glsl) ----

    // Face resolution of every probe distance cubemap (mip 0).
    inline constexpr u32 kProbeDistanceResolution = 128;

    // Channels per texel of the distance cubemap: d0, d1, d2, unused.
    inline constexpr u32 kProbeDistanceChannels = 4;

    // Capture clip planes. The far plane doubles as the miss sentinel.
    inline constexpr f32 kProbeDistanceNear = 0.05f;
    inline constexpr f32 kProbeDistanceFar = 1000.0f;

    // Stored distances at or beyond this are "no surface" (sky).
    inline constexpr f32 kProbeDistanceMissThreshold = 0.999f * kProbeDistanceFar;

    // Two peeled layers count as one surface within this distance: the bake
    // discards a fragment closer than it to the previous layer.
    inline constexpr f32 kProbePeelEpsilonAbs = 0.01f;
    inline constexpr f32 kProbePeelEpsilonRel = 0.002f;

    // Coarse march: kProbeMarchIterations loop iterations, each unrolled to
    // kProbeMarchTaps samples (the paper's reflection-quality budget).
    inline constexpr u32 kProbeMarchIterations = 8;
    inline constexpr u32 kProbeMarchTaps = 4;
    inline constexpr u32 kProbeMarchSteps = kProbeMarchIterations * kProbeMarchTaps;

    // Bisection refinement steps between the bracketing coarse samples.
    inline constexpr u32 kProbeRefineSteps = 6;

    // Bisection steps that look for a thin occluder between two free samples
    // of different kinds (one in front of d0, one in the d1..d2 gap).
    inline constexpr u32 kProbeGapRefineSteps = 6;

    // How far (radians) a hidden hit's radiance lookup steps back across the
    // occluder's silhouette: about three texels of a 256 radiance face, past
    // its bilinear footprint and the low prefilter mips' blur.
    inline constexpr f32 kProbeInpaintMarginRadians = 0.02f;

    // A hidden hit's radiance comes from the nearest direction around it where
    // the probe sees a surface at the hit's own distance (the same wall, just
    // outside the occluder's silhouette): kProbeInpaintRingCount rings of
    // kProbeInpaintAzimuths directions, at kProbeInpaintFirstRing * 2^i
    // radians, matched within max(abs, distance * (rel + slope * angle)): the
    // slope term lets an oblique wall's distance change with the angle, the
    // tight rel keeps the floor at a similar distance from matching. Only
    // when no ring matches does the visible-point rule above apply.
    inline constexpr u32 kProbeInpaintRingCount = 4;
    inline constexpr u32 kProbeInpaintAzimuths = 8;
    inline constexpr f32 kProbeInpaintFirstRing = 0.03f;
    inline constexpr f32 kProbeInpaintMatchAbs = 0.1f;
    inline constexpr f32 kProbeInpaintMatchRel = 0.03f;
    inline constexpr f32 kProbeInpaintMatchSlope = 0.5f;

    // "Inside the environment surface" bias: |p| < dist + max(abs, rel*|p|).
    // Absorbs capture quantisation so a shading point sitting exactly ON the
    // captured surface does not read as already-outside at the march start
    // (the classic self-hit -> surface reflects itself artifact).
    inline constexpr f32 kProbeInsideBiasAbs = 0.02f;
    inline constexpr f32 kProbeInsideBiasRel = 0.01f;

    // March-range slack. The plain triangle-inequality bound t <= |p0| + dMax
    // holds for |p(t)| = dist, but the crossing TEST is |p| > dist + bias —
    // the crossing sits a bias past that bound, and a head-on ray across a
    // room then never samples outside and silently misses (caught by
    // ReflectionProbeRaymarch.ConvergesToTheAnalyticHitAcrossASphereRoom).
    // The slack covers the bias plus boundary-exactness at the last sample.
    inline constexpr f32 kProbeMarchSlackRel = 1.05f;
    inline constexpr f32 kProbeMarchSlackAbs = 0.1f;

    // Cheap visibility reject margins (see ProbeCanSeePoint). Relative slack
    // covers bilinear interpolation between max-filtered texels; absolute
    // slack covers capture quantisation near the probe.
    inline constexpr f32 kProbeRejectRelMargin = 1.05f;
    inline constexpr f32 kProbeRejectAbsMargin = 0.2f;

    // Mip the cheap reject samples (16x16 faces for a 128 base).
    inline constexpr u32 kProbeRejectMip = 3;

    // Hard cap on simultaneously shaded probes: one bit per probe in the
    // per-cluster u32 visibility mask.
    inline constexpr u32 kMaxReflectionProbes = 32;

    // The three distances stored per direction (see the contract above).
    struct ProbeDistanceLayers
    {
        f32 Front = kProbeDistanceFar;  // d0
        f32 Back = kProbeDistanceFar;   // d1
        f32 Behind = kProbeDistanceFar; // d2
    };

    // @brief CPU-resident layered radial-distance cubemap for one reflection
    // probe, with its conservative max-mip chain. Produced by the bake
    // (ReflectionProbeBaker::BakeProbe), stored on the probe's
    // EnvironmentMap, uploaded verbatim into a layer of the shared RGBA16F
    // cubemap array by ReflectionProbeArrayPass.
    //
    // Layout per mip: 6 faces contiguous in GL cubemap face order, each face
    // row-major (texel = face * res * res + y * res + x), kProbeDistanceChannels
    // floats per texel — exactly the glTextureSubImage3D layout for a
    // zoffset=0, depth=6 RGBA float upload.
    class ReflectionProbeDistanceField : public RefCounted
    {
      public:
        // Builds the max-mip chain (down to 1x1) and MaxFiniteDistance from
        // the mip-0 data. `mip0` must hold resolution*resolution*6 texels of
        // kProbeDistanceChannels floats in the face-major layout above;
        // `resolution` must be a power of two. Returns nullptr on malformed
        // input.
        static Ref<ReflectionProbeDistanceField> CreateLayered(TArray<f32>&& mip0, u32 resolution);

        // A single-distance field (one float per texel, d0 only): every
        // direction gets d1 = d2 = d0, i.e. no gap. Analytic test
        // environments use it.
        static Ref<ReflectionProbeDistanceField> Create(TArray<f32>&& mip0, u32 resolution);

        [[nodiscard]] u32 GetResolution() const
        {
            return m_Resolution;
        }
        [[nodiscard]] u32 GetMipCount() const
        {
            return static_cast<u32>(m_Mips.Num());
        }
        [[nodiscard]] f32 GetMaxFiniteDistance() const
        {
            return m_MaxFiniteDistance;
        }

        // Face-major texel data for one mip, kProbeDistanceChannels floats per
        // texel (see layout comment above).
        [[nodiscard]] std::span<const f32> GetMip(u32 mip) const;

        // Nearest-neighbour sample along `direction` (need not be normalised,
        // must be non-zero). CPU mirror of the GLSL cubemap lookup — used by
        // contract tests and the CPU reference raymarch over baked data.
        [[nodiscard]] ProbeDistanceLayers SampleLayersNearest(const glm::vec3& direction, u32 mip) const;

        // d0 only.
        [[nodiscard]] f32 SampleNearest(const glm::vec3& direction, u32 mip) const
        {
            return SampleLayersNearest(direction, mip).Front;
        }

      private:
        u32 m_Resolution = 0;
        f32 m_MaxFiniteDistance = 0.0f;
        TArray<TArray<f32>> m_Mips; // [mip][(face * res * res + y * res + x) * kProbeDistanceChannels + layer]
    };

    // ---- Pure helpers (unit-tested headlessly) ----

    // 2x2 MAX-downsample of one face-major mip level (6 faces), per channel.
    // `resolution` is the SOURCE mip's face size (must be >= 2 and even).
    // Returns the next mip in the same layout at half resolution.
    [[nodiscard]] TArray<f32> BuildNextMaxMip(std::span<const f32> source, u32 resolution, u32 channels = 1);

    // Max over values strictly below the miss threshold; kProbeDistanceFar
    // when every value is sky (a probe floating in empty sky still gets a
    // valid, if useless, march bound). Every value passed counts.
    [[nodiscard]] f32 ComputeMaxFiniteProbeDistance(std::span<const f32> values);

    // The bake's post-pass for one texel's three peeled distances: clamps
    // each into the contract range (non-finite reads as the sentinel), and
    // collapses "nothing behind the first occluder" to d2 = d1 (see the
    // contract), so the result is ordered d0 <= d1 <= d2.
    [[nodiscard]] ProbeDistanceLayers NormalizeProbeDistanceLayers(f32 front, f32 back, f32 behind);

    // GL cubemap addressing: direction -> (face, texel x, texel y) at the
    // given face resolution. Mirrors the GL 4.6 spec's major-axis selection
    // (table 8.19) so CPU samples agree with GLSL `texture(samplerCube...)`
    // up to filtering. Exposed for tests.
    struct CubeFaceTexel
    {
        u32 Face;
        u32 X;
        u32 Y;
    };
    [[nodiscard]] CubeFaceTexel DirectionToCubeFaceTexel(const glm::vec3& direction, u32 resolution);

    // Cheap visibility reject shared by CPU tests and the GLSL shading path:
    // can the probe see a point at distance `pointDistance` from its centre,
    // given the max-filtered low-mip distance sample toward that point?
    [[nodiscard]] constexpr bool ProbeCanSeePoint(f32 pointDistance, f32 coneMaxDistance)
    {
        return pointDistance <= coneMaxDistance * kProbeRejectRelMargin + kProbeRejectAbsMargin;
    }

    // What a ray point at radius r along a direction is, given that
    // direction's layers. Front and Gap are free space. GLSL twin:
    // oloProbeClassify in include/ReflectionProbes.glsl.
    enum class ProbeRayPointKind : u8
    {
        Front, // in front of d0: free, seen by the probe
        Gap,   // between d1 and d2: free, hidden from the probe
        Solid,
    };
    [[nodiscard]] inline ProbeRayPointKind ClassifyProbeRayPoint(f32 radius, const ProbeDistanceLayers& layers)
    {
        f32 const bias = glm::max(kProbeInsideBiasAbs, radius * kProbeInsideBiasRel);
        if (radius < layers.Front + bias)
        {
            return ProbeRayPointKind::Front;
        }
        if (radius > layers.Back + bias && radius < layers.Behind + bias)
        {
            return ProbeRayPointKind::Gap;
        }
        return ProbeRayPointKind::Solid;
    }

    // Result of the reference raymarch.
    struct ProbeRaymarchResult
    {
        bool Hit = false;
        f32 HitT = 0.0f;                // ray parameter of the refined crossing
        glm::vec3 HitDirection{ 0.0f }; // normalised probe-space direction of the hit point
        // Where to read the radiance cubemap: HitDirection, or for a hit on a
        // surface hidden from the probe (Occluded), the direction of the last
        // point the march saw in front of d0.
        glm::vec3 RadianceDirection{ 0.0f };
        bool Occluded = false;
    };

    // CPU reference implementation of the distance-impostor raymarch. The
    // GLSL in include/ReflectionProbes.glsl mirrors this expression-for-
    // expression; contract tests drive THIS version against analytic
    // environments (sphere / box rooms, an occluder in a room) where the true
    // intersection is known in closed form.
    //
    // `originProbeSpace` — shading point relative to the probe centre
    //     (callers apply any normal-offset bias BEFORE this call).
    // `direction`        — unit reflection direction.
    // `maxFiniteDistance`— the probe's dMax (bounds the march).
    // `sampleLayers`     — ProbeDistanceLayers along a unit direction, or a
    //     plain f32 d0 for a single-distance environment (no gap).
    template<typename SampleFn>
    [[nodiscard]] ProbeRaymarchResult RaymarchProbeDistanceField(const glm::vec3& originProbeSpace,
                                                                 const glm::vec3& direction,
                                                                 f32 maxFiniteDistance,
                                                                 SampleFn&& sampleLayers)
    {
        ProbeRaymarchResult result;

        auto layersAlong = [&](const glm::vec3& unit) -> ProbeDistanceLayers
        {
            if constexpr (std::is_convertible_v<decltype(sampleLayers(unit)), f32>)
            {
                f32 const d = static_cast<f32>(sampleLayers(unit));
                return { d, d, d };
            }
            else
            {
                return sampleLayers(unit);
            }
        };
        auto pointAt = [&](f32 t) -> glm::vec3
        {
            return originProbeSpace + direction * t;
        };
        auto classifyAt = [&](f32 t) -> ProbeRayPointKind
        {
            glm::vec3 const p = pointAt(t);
            f32 const radius = glm::length(p);
            if (radius <= 0.0f)
            {
                return ProbeRayPointKind::Front; // degenerate: the ray passes through the probe centre
            }
            return ClassifyProbeRayPoint(radius, layersAlong(p / radius));
        };

        // Upper bound for any crossing: |p(t)| <= dMax at the crossing, and
        // t <= |p(t)| + |p0| by the triangle inequality — plus the slack,
        // because the crossing test carries the inside bias (see
        // kProbeMarchSlackRel above).
        f32 const tMax = (maxFiniteDistance + glm::length(originProbeSpace)) * kProbeMarchSlackRel +
                         kProbeMarchSlackAbs;
        if (tMax <= 0.0f)
        {
            return result;
        }

        // t = 0 counts as free by contract (the shading point lies on the
        // captured surface; the inside bias absorbs the quantisation) — but
        // only until the samples say otherwise. A shading point the probe
        // cannot SEE, and that no gap explains, starts inside an occluder's
        // radial cone where |p| is already past the stored distance:
        // accepting the first solid sample there reports a bogus hit ON the
        // occluder's shell. So a solid sample only brackets a crossing after
        // the march has actually SEEN a free sample.
        f32 const step = tMax / static_cast<f32>(kProbeMarchSteps);
        f32 tFree = -1.0f;
        ProbeRayPointKind freeKind = ProbeRayPointKind::Front;
        f32 tVisible = -1.0f; // last t known to lie in front of d0
        f32 tSolid = -1.0f;
        if (ProbeRayPointKind const first = classifyAt(step); first != ProbeRayPointKind::Solid)
        {
            tFree = 0.0f; // the t = 0 contract holds — normal bracketing
            freeKind = first;
            if (first == ProbeRayPointKind::Front)
            {
                tVisible = 0.0f;
            }
        }
        for (u32 i = 1; i <= kProbeMarchSteps && tSolid < 0.0f; ++i)
        {
            f32 const t = step * static_cast<f32>(i);
            ProbeRayPointKind const kind = classifyAt(t);
            if (kind == ProbeRayPointKind::Solid)
            {
                if (tFree >= 0.0f)
                {
                    tSolid = t;
                }
                continue;
            }
            if (tFree >= 0.0f && kind != freeKind)
            {
                // In front of d0 on one side, in the gap on the other: the ray
                // crossed an occluder's silhouette, or went through a thin one.
                f32 lo = tFree;
                f32 hi = t;
                for (u32 j = 0; j < kProbeGapRefineSteps; ++j)
                {
                    f32 const mid = 0.5f * (lo + hi);
                    ProbeRayPointKind const midKind = classifyAt(mid);
                    if (midKind == ProbeRayPointKind::Solid)
                    {
                        tSolid = mid;
                        break;
                    }
                    if (midKind == freeKind)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                if (tSolid >= 0.0f)
                {
                    tFree = lo;
                    break;
                }
                if (freeKind == ProbeRayPointKind::Front)
                {
                    tVisible = lo; // the edge of the shadow the ray enters
                }
            }
            tFree = t;
            freeKind = kind;
            if (kind == ProbeRayPointKind::Front)
            {
                tVisible = t;
            }
        }
        if (tSolid < 0.0f)
        {
            return result; // no crossing within tMax — the ray leaves through sky
        }

        // Bisection refinement of the bracket [tFree, tSolid].
        for (u32 i = 0; i < kProbeRefineSteps; ++i)
        {
            f32 const tMid = 0.5f * (tFree + tSolid);
            ProbeRayPointKind const midKind = classifyAt(tMid);
            if (midKind == ProbeRayPointKind::Solid)
            {
                tSolid = tMid;
            }
            else
            {
                tFree = tMid;
                if (midKind == ProbeRayPointKind::Front)
                {
                    tVisible = tMid;
                }
            }
        }

        f32 const tHit = 0.5f * (tFree + tSolid);
        glm::vec3 const hitPoint = pointAt(tHit);
        f32 const hitRadius = glm::length(hitPoint);
        if (hitRadius <= 0.0f)
        {
            return result;
        }
        glm::vec3 const hitDirection = hitPoint / hitRadius;
        ProbeDistanceLayers const layers = layersAlong(hitDirection);

        // A hit past the middle of a gap is on the d2 surface, hidden from
        // the probe; otherwise it is on the d0 shell (or on the occluder's far
        // side entered from the gap, which shades like its near side).
        bool const occluded = layers.Behind > layers.Back && hitRadius > 0.5f * (layers.Back + layers.Behind);
        f32 const surface = occluded ? layers.Behind : layers.Front;

        // A crossing whose stored distance is the miss sentinel is the ray
        // leaving through SKY, not a surface hit (reachable when dMax itself
        // is sentinel-dominated — e.g. an all-sky probe). Report a miss so
        // the caller falls back to the live global sky.
        if (surface >= kProbeDistanceMissThreshold)
        {
            return result;
        }

        glm::vec3 radianceDirection = hitDirection;
        bool inpainted = false;
        if (occluded)
        {
            // The hit surface itself, just outside the occluder (see
            // kProbeInpaintRingCount).
            glm::vec3 const axis = glm::abs(hitDirection.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            glm::vec3 const tangent = glm::normalize(glm::cross(hitDirection, axis));
            glm::vec3 const bitangent = glm::cross(hitDirection, tangent);
            for (u32 ring = 0; ring < kProbeInpaintRingCount && !inpainted; ++ring)
            {
                f32 const angle = kProbeInpaintFirstRing * static_cast<f32>(1u << ring);
                f32 const tolerance =
                    glm::max(kProbeInpaintMatchAbs, hitRadius * (kProbeInpaintMatchRel + (kProbeInpaintMatchSlope * angle)));
                for (u32 k = 0; k < kProbeInpaintAzimuths; ++k)
                {
                    f32 const phi = static_cast<f32>(k) * (6.28318530718f / static_cast<f32>(kProbeInpaintAzimuths));
                    glm::vec3 const candidate = (hitDirection * glm::cos(angle)) +
                                                (((tangent * glm::cos(phi)) + (bitangent * glm::sin(phi))) * glm::sin(angle));
                    if (glm::abs(layersAlong(candidate).Front - hitRadius) <= tolerance)
                    {
                        radianceDirection = candidate;
                        inpainted = true;
                        break;
                    }
                }
            }
        }
        if (occluded && !inpainted)
        {
            if (tVisible < 0.0f)
            {
                return result; // never saw the probe's side of the occluder: nothing to show
            }
            glm::vec3 const visiblePoint = pointAt(tVisible);
            f32 const visibleRadius = glm::length(visiblePoint);
            if (visibleRadius <= 0.0f)
            {
                return result;
            }
            // The visible point sits one bisection step from the occluder's
            // silhouette, where the radiance cube's filtering and prefilter
            // blur still carry the occluder's colour. Step the lookup a fixed
            // angle back across the silhouette, against the direction the
            // ray's projection was moving when it entered the shadow.
            glm::vec3 const visibleDirection = visiblePoint / visibleRadius;
            radianceDirection = visibleDirection;
            glm::vec3 const ahead = pointAt(tVisible + (step / 64.0f));
            f32 const aheadRadius = glm::length(ahead);
            if (aheadRadius > 0.0f)
            {
                glm::vec3 const drift = (ahead / aheadRadius) - visibleDirection;
                f32 const driftLength = glm::length(drift);
                if (driftLength > 1.0e-7f)
                {
                    radianceDirection =
                        glm::normalize(visibleDirection - (drift * (kProbeInpaintMarginRadians / driftLength)));
                }
            }
        }

        result.Hit = true;
        result.HitT = tHit;
        result.HitDirection = hitDirection;
        result.RadianceDirection = radianceDirection;
        result.Occluded = occluded;
        return result;
    }
} // namespace OloEngine
