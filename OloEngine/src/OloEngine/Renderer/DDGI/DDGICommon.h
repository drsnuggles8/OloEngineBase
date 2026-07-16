#pragma once

// =============================================================================
// DDGICommon.h — pure CPU mirrors of the realtime DDGI probe math (issue #632)
//
// This header is the single C++ home of the octahedral mapping, atlas layout,
// Chebyshev visibility, blend/EMA, relocation, and classification math used by
// the DDGI GPU passes. The GLSL implementation in
// OloEditor/assets/shaders/include/DDGICommon.glsl mirrors these functions
// one-for-one (each GLSL function names its C++ counterpart); the L1 contract
// tests pin THIS header, and the shaderpipe parity tests pin GLSL == C++.
//
// Everything here is header-only, allocation-free, and GL-independent so the
// contract tests run headless.
//
// References: Majercik et al., JCGT 2019 "Dynamic Diffuse Global Illumination
// with Ray-Traced Irradiance Fields"; Majercik et al., JCGT 2021 "Scaling
// Probe-Based Real-Time Dynamic Global Illumination for Production"; the
// NVIDIA RTXGI-DDGI SDK. Architecture: docs/adr/0006-ddgi-hit-point-cache-gather.md.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::DDGI
{
    // -------------------------------------------------------------------------
    // Atlas layout constants (compile-time; shared with DDGICommon.glsl, which
    // redeclares them — ShaderUBOSizeConsistencyTest-style drift is prevented
    // by the shaderpipe parity test exercising the real shader).
    // -------------------------------------------------------------------------

    // Irradiance: 6x6 interior + 1-texel bilinear-safe border = 8x8 per probe
    // (RTXGI's production layout; power-of-two tiles for SIMD occupancy).
    inline constexpr i32 kIrradianceInteriorTexels = 6;
    inline constexpr i32 kIrradianceTileTexels = kIrradianceInteriorTexels + 2;

    // Visibility (Chebyshev mean / mean^2): 14x14 interior + border = 16x16.
    inline constexpr i32 kVisibilityInteriorTexels = 14;
    inline constexpr i32 kVisibilityTileTexels = kVisibilityInteriorTexels + 2;

    // Hit-point cache angular resolutions (the cached-gather equivalent of
    // rays-per-probe). m_RaysPerProbe snaps to one of these squared.
    inline constexpr i32 kHitCacheResolutionLow = 8;     // 64 directions
    inline constexpr i32 kHitCacheResolutionMedium = 16; // 256 directions
    inline constexpr i32 kHitCacheResolutionHigh = 32;   // 1024 directions

    // Relocation: probe offsets are clamped to an ellipsoid of this fraction
    // of the per-axis grid spacing (RTXGI: dot(offsetN, offsetN) < 0.45^2).
    inline constexpr f32 kMaxProbeOffsetFraction = 0.45f;

    // Classification: a probe whose hit cache sees more than this fraction of
    // backfaces is inside geometry -> Inactive.
    inline constexpr f32 kBackfaceFraction = 0.25f;

    // Chebyshev floor (prevents a fully-shadowed corner from zeroing out) and
    // the weight-crush threshold (suppresses near-zero weights smoothly).
    inline constexpr f32 kChebyshevWeightFloor = 0.05f;
    inline constexpr f32 kWeightCrushThreshold = 0.2f;

    // Visibility distances are clamped to this multiple of the spacing
    // diagonal before mean/mean^2 blending (keeps mean^2 inside FP16 range).
    inline constexpr f32 kMaxRayDistanceSpacingScale = 1.5f;

    // Probe state stored in the probe-data texture's .w channel.
    enum class ProbeState : i32
    {
        Uncaptured = 0, // never captured — contributes nothing, capture ASAP
        Active = 1,     // captured, geometry within its cell — relight + blend
        Inactive = 2    // captured, inside geometry (backface-heavy) — skipped
    };

    // -------------------------------------------------------------------------
    // Small helpers
    // -------------------------------------------------------------------------

    // Snap an authored rays-per-probe value to a supported hit-cache
    // resolution (GLSL: n/a — CPU-side scheduling only).
    [[nodiscard]] constexpr i32 HitCacheResolutionForRayCount(i32 raysPerProbe) noexcept
    {
        if (raysPerProbe <= kHitCacheResolutionLow * kHitCacheResolutionLow)
        {
            return kHitCacheResolutionLow;
        }
        if (raysPerProbe <= kHitCacheResolutionMedium * kHitCacheResolutionMedium)
        {
            return kHitCacheResolutionMedium;
        }
        return kHitCacheResolutionHigh;
    }

    // sign() that never returns 0 — the classic octahedral-mapping fix. The
    // engine's G-buffer octEncode uses plain sign() (harmless for shading
    // normals); probe-atlas texel directions are generated on exact fold
    // seams, so DDGI deliberately uses signNotZero (RTXGI convention).
    [[nodiscard]] inline f32 SignNotZero(f32 v) noexcept
    {
        return (v >= 0.0f) ? 1.0f : -1.0f;
    }

    [[nodiscard]] inline glm::vec2 SignNotZero(const glm::vec2& v) noexcept
    {
        return { SignNotZero(v.x), SignNotZero(v.y) };
    }

    // -------------------------------------------------------------------------
    // Octahedral mapping (full sphere). GLSL mirror: ddgiOctEncode/ddgiOctDecode.
    // -------------------------------------------------------------------------

    // Unit direction -> octahedral coordinates in [-1, 1]^2.
    [[nodiscard]] inline glm::vec2 OctEncode(const glm::vec3& n) noexcept
    {
        f32 const l1 = glm::abs(n.x) + glm::abs(n.y) + glm::abs(n.z);
        glm::vec2 uv = glm::vec2(n.x, n.y) / glm::max(l1, 1e-8f);
        if (n.z < 0.0f)
        {
            uv = (1.0f - glm::abs(glm::vec2(uv.y, uv.x))) * SignNotZero(uv);
        }
        return uv;
    }

    // Octahedral coordinates in [-1, 1]^2 -> unit direction.
    [[nodiscard]] inline glm::vec3 OctDecode(const glm::vec2& f) noexcept
    {
        glm::vec3 n(f.x, f.y, 1.0f - glm::abs(f.x) - glm::abs(f.y));
        f32 const t = glm::clamp(-n.z, 0.0f, 1.0f);
        n.x += (n.x >= 0.0f) ? -t : t;
        n.y += (n.y >= 0.0f) ? -t : t;
        return glm::normalize(n);
    }

    // Center direction of an interior texel of an N x N octahedral tile.
    // GLSL mirror: ddgiTexelDirection.
    [[nodiscard]] inline glm::vec3 TexelDirection(const glm::ivec2& interiorTexel, i32 interiorResolution) noexcept
    {
        glm::vec2 const uv01 = (glm::vec2(interiorTexel) + 0.5f) / static_cast<f32>(interiorResolution);
        return OctDecode(uv01 * 2.0f - 1.0f);
    }

    // -------------------------------------------------------------------------
    // Probe grid / atlas layout. The linear index convention matches the
    // existing LightProbeVolumeComponent::GridIndex (z-major) so the baked and
    // realtime paths agree on probe identity.
    // -------------------------------------------------------------------------

    [[nodiscard]] inline i32 ProbeLinearIndex(const glm::ivec3& coord, const glm::ivec3& dims) noexcept
    {
        return coord.z * dims.y * dims.x + coord.y * dims.x + coord.x;
    }

    [[nodiscard]] inline glm::ivec3 ProbeGridCoord(i32 linearIndex, const glm::ivec3& dims) noexcept
    {
        i32 const planeSize = dims.x * dims.y;
        return { linearIndex % dims.x, (linearIndex / dims.x) % dims.y, linearIndex / planeSize };
    }

    // Probe tile within the 2D atlas: column = x, row = z * dimY + y. Atlas is
    // therefore (dims.x * tileTexels) wide and (dims.y * dims.z * tileTexels)
    // tall. GLSL mirror: ddgiProbeTileCoord.
    [[nodiscard]] inline glm::ivec2 ProbeTileCoord(i32 linearIndex, const glm::ivec3& dims) noexcept
    {
        glm::ivec3 const c = ProbeGridCoord(linearIndex, dims);
        return { c.x, c.z * dims.y + c.y };
    }

    [[nodiscard]] inline glm::ivec2 AtlasTileDimensions(const glm::ivec3& dims) noexcept
    {
        return { dims.x, dims.y * dims.z };
    }

    // Continuous atlas texel coordinate for bilinear-sampling `direction` from
    // a probe's tile (border-safe: [tileOrigin+1, tileOrigin+1+interior]).
    // Divide by atlas texel dimensions for a normalized UV.
    // GLSL mirror: ddgiProbeAtlasTexel.
    [[nodiscard]] inline glm::vec2 ProbeAtlasTexel(i32 linearIndex, const glm::ivec3& dims, const glm::vec3& direction, i32 interiorResolution) noexcept
    {
        i32 const tileTexels = interiorResolution + 2;
        glm::vec2 const tileOrigin = glm::vec2(ProbeTileCoord(linearIndex, dims) * tileTexels);
        glm::vec2 const uv01 = OctEncode(direction) * 0.5f + 0.5f;
        return tileOrigin + 1.0f + uv01 * static_cast<f32>(interiorResolution);
    }

    // Border-gutter source lookup: for a border texel of an N x N tile (local
    // coords with border, tileTexels = interior + 2), returns the interior
    // texel (local coords) whose value it must copy so cross-tile bilinear
    // taps stay inside the probe. Edges mirror one row inward; corners copy
    // the diagonally opposite interior corner (RTXGI convention).
    // Interior texels return themselves. GLSL mirror: ddgiBorderSourceTexel.
    [[nodiscard]] inline glm::ivec2 BorderSourceTexel(const glm::ivec2& localTexel, i32 tileTexels) noexcept
    {
        i32 const maxT = tileTexels - 1;
        bool const onLeft = (localTexel.x == 0);
        bool const onRight = (localTexel.x == maxT);
        bool const onBottom = (localTexel.y == 0);
        bool const onTop = (localTexel.y == maxT);

        bool const corner = (onLeft || onRight) && (onBottom || onTop);
        if (corner)
        {
            // Diagonally opposite interior corner.
            return { onLeft ? (maxT - 1) : 1, onBottom ? (maxT - 1) : 1 };
        }
        if (onBottom || onTop)
        {
            // Horizontal edge: mirror x across the tile, one row inward.
            return { maxT - localTexel.x, onBottom ? 1 : (maxT - 1) };
        }
        if (onLeft || onRight)
        {
            // Vertical edge: mirror y across the tile, one column inward.
            return { onLeft ? 1 : (maxT - 1), maxT - localTexel.y };
        }
        return localTexel;
    }

    // -------------------------------------------------------------------------
    // Probe placement. Corner-anchored lattice matching the baked path
    // (LightProbeBaker) and LightProbeSampling.glsl::worldToProbeGrid, with a
    // res==1 guard the baker lacks (probe sits at the min corner; the sampler
    // collapses that axis to grid coordinate 0 regardless).
    // -------------------------------------------------------------------------

    [[nodiscard]] inline glm::vec3 ProbeSpacing(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims) noexcept
    {
        glm::vec3 const extent = glm::max(boundsMax - boundsMin, glm::vec3(1e-6f));
        return extent / glm::vec3(glm::max(dims - glm::ivec3(1), glm::ivec3(1)));
    }

    [[nodiscard]] inline glm::vec3 ProbeGridPosition(const glm::ivec3& coord, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims) noexcept
    {
        return boundsMin + ProbeSpacing(boundsMin, boundsMax, dims) * glm::vec3(coord);
    }

    // World position including the relocation offset (offset is stored
    // normalized by per-axis spacing, RTXGI convention).
    [[nodiscard]] inline glm::vec3 ProbeWorldPosition(const glm::ivec3& coord, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims, const glm::vec3& offsetNormalized) noexcept
    {
        return ProbeGridPosition(coord, boundsMin, boundsMax, dims) + offsetNormalized * ProbeSpacing(boundsMin, boundsMax, dims);
    }

    // -------------------------------------------------------------------------
    // Sampler weights (the leak fix). GLSL mirror: ddgiChebyshevWeight,
    // ddgiWrapShadingWeight, ddgiCrushWeight, ddgiSelfShadowBias.
    // -------------------------------------------------------------------------

    // Chebyshev upper-bound visibility: probability the surface at distance r
    // is unoccluded from the probe, given the distance distribution's mean and
    // mean^2 along the sample direction. 1 when r <= mean (closer than the
    // average occluder). Cubed to sharpen, floored to keep a minimum bleed.
    [[nodiscard]] inline f32 ChebyshevWeight(f32 mean, f32 meanSquared, f32 r) noexcept
    {
        if (r <= mean)
        {
            return 1.0f;
        }
        f32 const variance = glm::max(meanSquared - mean * mean, 1e-6f);
        f32 const d = r - mean;
        f32 const p = variance / (variance + d * d);
        return glm::max(p * p * p, kChebyshevWeightFloor);
    }

    // Wrapped backface weight: smoothly de-weights probes behind the shading
    // surface without the hard cut that causes seams (JCGT 2019 eq. via
    // RTXGI: ((dot+1)/2)^2 + 0.2).
    [[nodiscard]] inline f32 WrapShadingWeight(const glm::vec3& dirToProbe, const glm::vec3& normal) noexcept
    {
        f32 const wrapped = (glm::dot(dirToProbe, normal) + 1.0f) * 0.5f;
        return wrapped * wrapped + 0.2f;
    }

    // Crush tiny weights smoothly to zero (suppresses variance amplification
    // when normalizing a near-zero total weight; RTXGI convention).
    [[nodiscard]] inline f32 CrushWeight(f32 w) noexcept
    {
        if (w < kWeightCrushThreshold)
        {
            return w * (w * w) / (kWeightCrushThreshold * kWeightCrushThreshold);
        }
        return w;
    }

    // Self-shadow bias applied to the shading point before probe lookups
    // (JCGT 2021 unified form): offset along a blend of surface normal and
    // the direction back to the camera, scaled by grid spacing.
    [[nodiscard]] inline glm::vec3 SelfShadowBias(const glm::vec3& normal, const glm::vec3& viewDir, f32 minAxialSpacing, f32 biasScale) noexcept
    {
        return (0.2f * normal + 0.8f * viewDir) * (0.75f * minAxialSpacing) * biasScale;
    }

    // -------------------------------------------------------------------------
    // Blend pass math. GLSL mirror: DDGIProbeBlend.comp.
    // -------------------------------------------------------------------------

    // Cosine weight of a cached hit direction for an irradiance texel
    // direction (power 1: Lambertian irradiance).
    [[nodiscard]] inline f32 IrradianceBlendWeight(const glm::vec3& texelDir, const glm::vec3& hitDir) noexcept
    {
        return glm::max(0.0f, glm::dot(texelDir, hitDir));
    }

    // Power-cosine weight for the visibility texels (sharper lobe so the
    // distance estimate is directional; RTXGI probeDistanceExponent default).
    inline constexpr f32 kDistanceBlendExponent = 50.0f;

    [[nodiscard]] inline f32 DistanceBlendWeight(const glm::vec3& texelDir, const glm::vec3& hitDir, f32 exponent = kDistanceBlendExponent) noexcept
    {
        return glm::pow(glm::max(0.0f, glm::dot(texelDir, hitDir)), exponent);
    }

    // Temporal EMA: hysteresis is the fraction of HISTORY kept.
    [[nodiscard]] inline glm::vec3 BlendEMA(const glm::vec3& newValue, const glm::vec3& oldValue, f32 hysteresis) noexcept
    {
        return glm::mix(newValue, oldValue, hysteresis);
    }

    // Big-change response: when the new estimate departs strongly from
    // history, cut hysteresis so lights snapping on/off do not smear (JCGT
    // 2021 thresholds: >25% of full range -> reduce, >80% -> drop history).
    [[nodiscard]] inline f32 AdjustHysteresis(f32 hysteresis, const glm::vec3& newValue, const glm::vec3& oldValue) noexcept
    {
        glm::vec3 const delta = glm::abs(newValue - oldValue);
        f32 const maxComponentDelta = glm::max(glm::max(delta.x, delta.y), delta.z);
        if (maxComponentDelta > 0.8f)
        {
            return 0.0f;
        }
        if (maxComponentDelta > 0.25f)
        {
            return glm::max(hysteresis - 0.15f, 0.0f);
        }
        return hysteresis;
    }

    // -------------------------------------------------------------------------
    // Relocation + classification (RTXGI three-path algorithm, computed from
    // hit-cache aggregates). GLSL mirror: DDGIProbeCaptureResample.comp.
    // -------------------------------------------------------------------------

    struct ProbeHitAggregates
    {
        f32 BackfaceFraction = 0.0f;           // backface hits / total directions
        glm::vec3 ClosestBackfaceDir{ 0.0f };  // unit dir of nearest backface hit
        f32 ClosestBackfaceDist = -1.0f;       // < 0 = none
        glm::vec3 ClosestFrontfaceDir{ 0.0f }; // unit dir of nearest frontface hit
        f32 ClosestFrontfaceDist = -1.0f;      // < 0 = none
        glm::vec3 FarthestFrontfaceDir{ 0.0f };
        f32 FarthestFrontfaceDist = -1.0f;
        bool AnyHitWithinCell = false; // any frontface hit within one cell's reach
    };

    // Returns the new offset, normalized by per-axis spacing. minFrontfaceDistance
    // is in world units (RTXGI default 1.0, scaled to scene by callers).
    [[nodiscard]] inline glm::vec3 RelocateProbe(const glm::vec3& currentOffsetN, const ProbeHitAggregates& agg, const glm::vec3& spacing, f32 minFrontfaceDistance) noexcept
    {
        glm::vec3 offsetWorld = currentOffsetN * spacing;

        if (agg.BackfaceFraction > kBackfaceFraction && agg.ClosestBackfaceDist >= 0.0f)
        {
            // Inside geometry: push through the closest backface plus margin.
            offsetWorld += agg.ClosestBackfaceDir * (agg.ClosestBackfaceDist + 0.5f * minFrontfaceDistance);
        }
        else if (agg.ClosestFrontfaceDist >= 0.0f && agg.ClosestFrontfaceDist < minFrontfaceDistance)
        {
            // Uncomfortably close to a wall: slide along the farthest
            // frontface direction, but only when it doesn't fight the
            // closest one (oscillation guard).
            if (agg.FarthestFrontfaceDist >= 0.0f && glm::dot(agg.ClosestFrontfaceDir, agg.FarthestFrontfaceDir) <= 0.0f)
            {
                glm::vec3 const dir = agg.FarthestFrontfaceDir;
                offsetWorld += dir * glm::min(agg.FarthestFrontfaceDist, 1.0f);
            }
        }
        else if (agg.ClosestFrontfaceDist >= minFrontfaceDistance || agg.ClosestFrontfaceDist < 0.0f)
        {
            // Comfortable: decay back toward the grid point.
            f32 const moveBack = glm::length(offsetWorld);
            if (moveBack > 1e-6f)
            {
                f32 const headroom = (agg.ClosestFrontfaceDist < 0.0f)
                                         ? moveBack
                                         : glm::min(agg.ClosestFrontfaceDist - minFrontfaceDistance, moveBack);
                offsetWorld -= glm::normalize(offsetWorld) * glm::max(headroom, 0.0f);
            }
        }

        glm::vec3 const offsetN = offsetWorld / glm::max(spacing, glm::vec3(1e-6f));
        // Accept only inside the 45%-of-cell ellipsoid; otherwise keep the
        // previous offset (RTXGI convention).
        if (glm::dot(offsetN, offsetN) < kMaxProbeOffsetFraction * kMaxProbeOffsetFraction)
        {
            return offsetN;
        }
        return currentOffsetN;
    }

    [[nodiscard]] inline ProbeState ClassifyProbe(const ProbeHitAggregates& agg) noexcept
    {
        if (agg.BackfaceFraction > kBackfaceFraction)
        {
            return ProbeState::Inactive;
        }
        return ProbeState::Active;
    }
} // namespace OloEngine::DDGI
