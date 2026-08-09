#pragma once

// =============================================================================
// Distance-impostor reflection probes (issue #705) — CPU half of the contract.
//
// ENCODING CONTRACT (the GLSL raymarch in include/ReflectionProbes.glsl and
// the bake in ReflectionProbeBaker.cpp both mirror this file; change them
// together):
//
//  - A probe's distance cubemap stores LINEAR RADIAL DISTANCE in world units
//    from the probe centre to the first opaque surface along each texel
//    direction. Probe space = world space translated so the probe centre is
//    the origin; no rotation, no scale, so distances are world-metric.
//  - Fixed face resolution kProbeDistanceResolution (all probes share one
//    cubemap array, so the resolution cannot be per-probe). Format R32F on
//    the GPU, f32 on the CPU. Face order and orientation match
//    GL_TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z exactly as captured by
//    ReflectionProbeBaker's face target/up tables.
//  - MISS SENTINEL: texels with no geometry keep the capture clear value
//    kProbeDistanceFar (== the capture far plane). Any stored distance
//    >= kProbeDistanceMissThreshold means "no surface in this direction"
//    (sky). The sentinel is deliberately the far plane and not a huge float:
//    bilinear filtering interpolates across silhouette edges, and a finite
//    sentinel bounds the damage of a sky/wall mix to values the march's
//    tMax already excludes.
//  - MIP CHAIN: strict 2x2 MAX-downsample per level (BuildNextMaxMip), never
//    a box average and never glGenerateTextureMipmap. The only mip consumer
//    is the cheap visibility reject, which needs an UPPER bound of distance
//    over the footprint: "probe sees point iff |x-o| <= dist + margin" with
//    a max-filtered dist can only over-admit (wasted march), never
//    over-reject (missing reflection). The raymarch itself samples mip 0.
//  - MaxFiniteDistance (dMax) = max over mip-0 texels strictly below the
//    miss threshold (far plane when every texel is sky). Any surface
//    crossing of a ray starting at probe-space p0 satisfies
//    t <= |p0| + dMax, which bounds the coarse march.
//
// RAYMARCH (Szirmay-Kalos et al., "Approximate Ray-Tracing on the GPU with
// Distance Impostors", Eurographics 2005 — re-derived, not transcribed):
// treat the environment as the star-shaped surface { dist(u) * u } around
// the probe centre. A point p is INSIDE when |p| < dist(p/|p|). March the
// reflection ray p(t) = p0 + t*dir in kProbeMarchSteps uniform steps over
// (0, tMax]; the first inside->outside transition brackets the surface;
// kProbeRefineSteps bisection steps refine it. The refined direction
// normalize(p(tHit)) is the parallax-corrected cubemap lookup direction.
// No transition within tMax = miss (the ray leaves through sky).
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    // ---- Contract constants (mirrored by include/ReflectionProbes.glsl) ----

    // Face resolution of every probe distance cubemap (mip 0).
    inline constexpr u32 kProbeDistanceResolution = 128;

    // Capture clip planes. The far plane doubles as the miss sentinel.
    inline constexpr f32 kProbeDistanceNear = 0.05f;
    inline constexpr f32 kProbeDistanceFar = 1000.0f;

    // Stored distances at or beyond this are "no surface" (sky).
    inline constexpr f32 kProbeDistanceMissThreshold = 0.999f * kProbeDistanceFar;

    // Coarse march: kProbeMarchIterations loop iterations, each unrolled to
    // kProbeMarchTaps samples (the paper's reflection-quality budget).
    inline constexpr u32 kProbeMarchIterations = 8;
    inline constexpr u32 kProbeMarchTaps = 4;
    inline constexpr u32 kProbeMarchSteps = kProbeMarchIterations * kProbeMarchTaps;

    // Bisection refinement steps between the bracketing coarse samples.
    inline constexpr u32 kProbeRefineSteps = 6;

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

    // @brief CPU-resident radial-distance cubemap for one reflection probe,
    // with its conservative max-mip chain. Produced by the bake
    // (ReflectionProbeBaker::BakeProbe), stored on the probe's
    // EnvironmentMap, uploaded verbatim into a layer of the shared R32F
    // cubemap array by ReflectionProbeArrayPass.
    //
    // Layout per mip: 6 faces contiguous in GL cubemap face order, each face
    // row-major (index = face * res * res + y * res + x) — exactly the
    // glTextureSubImage3D layout for a zoffset=0, depth=6 upload.
    class ReflectionProbeDistanceField : public RefCounted
    {
      public:
        // Builds the max-mip chain (down to 1x1) and MaxFiniteDistance from
        // the mip-0 data. `mip0` must hold resolution*resolution*6 floats in
        // the face-major layout above; `resolution` must be a power of two.
        // Returns nullptr on malformed input.
        static Ref<ReflectionProbeDistanceField> Create(std::vector<f32>&& mip0, u32 resolution);

        [[nodiscard]] u32 GetResolution() const
        {
            return m_Resolution;
        }
        [[nodiscard]] u32 GetMipCount() const
        {
            return static_cast<u32>(m_Mips.size());
        }
        [[nodiscard]] f32 GetMaxFiniteDistance() const
        {
            return m_MaxFiniteDistance;
        }

        // Face-major texel data for one mip (see layout comment above).
        [[nodiscard]] std::span<const f32> GetMip(u32 mip) const;

        // Nearest-neighbour sample along `direction` (need not be normalised,
        // must be non-zero). CPU mirror of the GLSL cubemap lookup — used by
        // contract tests and the CPU reference raymarch over baked data.
        [[nodiscard]] f32 SampleNearest(const glm::vec3& direction, u32 mip) const;

      private:
        u32 m_Resolution = 0;
        f32 m_MaxFiniteDistance = 0.0f;
        std::vector<std::vector<f32>> m_Mips; // [mip][face * res * res + y * res + x]
    };

    // ---- Pure helpers (unit-tested headlessly) ----

    // 2x2 MAX-downsample of one face-major mip level (6 faces). `resolution`
    // is the SOURCE mip's face size (must be >= 2 and even). Returns the
    // next mip in the same layout at half resolution.
    [[nodiscard]] std::vector<f32> BuildNextMaxMip(std::span<const f32> source, u32 resolution);

    // Max over texels strictly below the miss threshold; kProbeDistanceFar
    // when every texel is sky (a probe floating in empty sky still gets a
    // valid, if useless, march bound).
    [[nodiscard]] f32 ComputeMaxFiniteProbeDistance(std::span<const f32> mip0);

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

    // Result of the reference raymarch.
    struct ProbeRaymarchResult
    {
        bool Hit = false;
        f32 HitT = 0.0f;                // ray parameter of the refined crossing
        glm::vec3 HitDirection{ 0.0f }; // normalised probe-space lookup direction
    };

    // CPU reference implementation of the distance-impostor raymarch. The
    // GLSL in include/ReflectionProbes.glsl mirrors this expression-for-
    // expression; contract tests drive THIS version against analytic
    // environments (sphere / box rooms) where the true intersection is
    // known in closed form.
    //
    // `originProbeSpace` — shading point relative to the probe centre
    //     (callers apply any normal-offset bias BEFORE this call).
    // `direction`        — unit reflection direction.
    // `maxFiniteDistance`— the probe's dMax (bounds the march).
    // `sampleDistance`   — dist(unit direction) of the environment surface.
    template<typename SampleFn>
    [[nodiscard]] ProbeRaymarchResult RaymarchProbeDistanceField(const glm::vec3& originProbeSpace,
                                                                 const glm::vec3& direction,
                                                                 f32 maxFiniteDistance,
                                                                 SampleFn&& sampleDistance)
    {
        ProbeRaymarchResult result;

        auto insideAt = [&](f32 t, f32& radiusOut) -> bool
        {
            glm::vec3 const p = originProbeSpace + direction * t;
            f32 const radius = glm::length(p);
            radiusOut = radius;
            if (radius <= 0.0f)
            {
                return true; // degenerate: the ray passes through the probe centre
            }
            f32 const stored = sampleDistance(p / radius);
            f32 const bias = glm::max(kProbeInsideBiasAbs, radius * kProbeInsideBiasRel);
            return radius < stored + bias;
        };

        // Upper bound for any crossing: |p(t)| <= dist(u) <= dMax at the
        // crossing, and t <= |p(t)| + |p0| by the triangle inequality — plus
        // the slack, because the crossing test carries the inside bias (see
        // kProbeMarchSlackRel above).
        f32 const tMax = (maxFiniteDistance + glm::length(originProbeSpace)) * kProbeMarchSlackRel +
                         kProbeMarchSlackAbs;
        if (tMax <= 0.0f)
        {
            return result;
        }

        // t = 0 counts as inside by contract (the shading point lies on the
        // captured surface; the inside bias absorbs the quantisation) — but
        // only until the samples say otherwise. A shading point the probe
        // cannot SEE (e.g. the underside of an object the probe looks down
        // on) starts inside that occluder's radial cone, where |p| is
        // already past the stored distance: accepting the first "outside"
        // sample there reports a bogus hit ON the occluder's shell (dark
        // smears on every probe-hidden surface). So an outside sample only
        // brackets a crossing after the march has actually SEEN an inside
        // sample; leading outside samples are skipped until the ray exits
        // the occluder cone and re-enters the visible region.
        f32 const step = tMax / static_cast<f32>(kProbeMarchSteps);
        f32 tInside = -1.0f;
        f32 tOutside = -1.0f;
        {
            f32 radius = 0.0f;
            if (insideAt(step, radius))
            {
                tInside = 0.0f; // the t = 0 contract holds — normal bracketing
            }
        }
        for (u32 i = 1; i <= kProbeMarchSteps; ++i)
        {
            f32 const t = step * static_cast<f32>(i);
            f32 radius = 0.0f;
            if (insideAt(t, radius))
            {
                tInside = t;
            }
            else if (tInside >= 0.0f)
            {
                tOutside = t;
                break;
            }
        }
        if (tOutside < 0.0f)
        {
            return result; // no crossing within tMax — the ray leaves through sky
        }

        // Bisection refinement of the bracket [tInside, tOutside].
        for (u32 i = 0; i < kProbeRefineSteps; ++i)
        {
            f32 const tMid = 0.5f * (tInside + tOutside);
            f32 radius = 0.0f;
            if (insideAt(tMid, radius))
            {
                tInside = tMid;
            }
            else
            {
                tOutside = tMid;
            }
        }

        f32 const tHit = 0.5f * (tInside + tOutside);
        glm::vec3 const hitPoint = originProbeSpace + direction * tHit;
        f32 const hitRadius = glm::length(hitPoint);
        if (hitRadius <= 0.0f)
        {
            return result;
        }
        glm::vec3 const hitDirection = hitPoint / hitRadius;

        // A crossing whose stored distance is the miss sentinel is the ray
        // leaving through SKY, not a surface hit (reachable when dMax itself
        // is sentinel-dominated — e.g. an all-sky probe). Report a miss so
        // the caller falls back to the live global sky.
        if (sampleDistance(hitDirection) >= kProbeDistanceMissThreshold)
        {
            return result;
        }

        result.Hit = true;
        result.HitT = tHit;
        result.HitDirection = hitDirection;
        return result;
    }
} // namespace OloEngine
