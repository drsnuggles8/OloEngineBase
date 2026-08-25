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
// NVIDIA RTXGI-DDGI SDK. Architecture: docs/adr/0007-ddgi-hit-point-cache-gather.md.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <utility> // std::to_underlying, used by CaptureScore

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

    // Issue #751. How far PAST the volume bounds the INFINITE-BOUNCE feedback
    // path may still gather irradiance, in probe spacings per axis.
    //
    // The bounce term samples the previous frame's irradiance at cached hit
    // points, and every hit point is ON A SURFACE — so a volume fitted to a
    // room's interior air (the natural authoring) excludes every wall, floor
    // and ceiling, and a hard inside-test silently zeroes the entire feedback
    // term. One spacing is the principled reach: the gather's trilinear lookup
    // already clamps to the boundary probe layer for a position outside the
    // bounds, and a probe field cannot resolve variation finer than its own
    // spacing, so extrapolating by one spacing is exactly as accurate as the
    // interpolation inside the volume. Beyond that the constant extrapolation
    // is no longer justified by the sample density, so the weight is zero.
    //
    // The LIT path keeps margin 0 (a surface outside the volume must fall
    // through to the ambient ladder, not be shaded from a clamped boundary
    // probe). See docs/adr/0007-ddgi-hit-point-cache-gather.md.
    inline constexpr f32 kBounceMarginSpacingScale = 1.0f;

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
    [[nodiscard("the snapped hit-cache resolution is the only effect")]] constexpr i32 HitCacheResolutionForRayCount(i32 raysPerProbe) noexcept
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
    [[nodiscard("pure sign computation — the result is the only effect")]] inline f32 SignNotZero(f32 v) noexcept
    {
        return (v >= 0.0f) ? 1.0f : -1.0f;
    }

    [[nodiscard("pure sign computation — the result is the only effect")]] inline glm::vec2 SignNotZero(const glm::vec2& v) noexcept
    {
        return { SignNotZero(v.x), SignNotZero(v.y) };
    }

    // -------------------------------------------------------------------------
    // Octahedral mapping (full sphere). GLSL mirror: ddgiOctEncode/ddgiOctDecode.
    // -------------------------------------------------------------------------

    // Unit direction -> octahedral coordinates in [-1, 1]^2.
    [[nodiscard("the octahedral encoding is the only effect")]] inline glm::vec2 OctEncode(const glm::vec3& n) noexcept
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
    [[nodiscard("the decoded unit direction is the only effect")]] inline glm::vec3 OctDecode(const glm::vec2& f) noexcept
    {
        glm::vec3 n(f.x, f.y, 1.0f - glm::abs(f.x) - glm::abs(f.y));
        f32 const t = glm::clamp(-n.z, 0.0f, 1.0f);
        n.x += (n.x >= 0.0f) ? -t : t;
        n.y += (n.y >= 0.0f) ? -t : t;
        return glm::normalize(n);
    }

    // Center direction of an interior texel of an N x N octahedral tile.
    // GLSL mirror: ddgiTexelDirection.
    [[nodiscard("the texel direction is the only effect")]] inline glm::vec3 TexelDirection(const glm::ivec2& interiorTexel, i32 interiorResolution) noexcept
    {
        glm::vec2 const uv01 = (glm::vec2(interiorTexel) + 0.5f) / static_cast<f32>(interiorResolution);
        return OctDecode(uv01 * 2.0f - 1.0f);
    }

    // -------------------------------------------------------------------------
    // Probe grid / atlas layout. The linear index convention matches the
    // existing LightProbeVolumeComponent::GridIndex (z-major) so the baked and
    // realtime paths agree on probe identity.
    // -------------------------------------------------------------------------

    [[nodiscard("the linear probe index is the only effect")]] inline i32 ProbeLinearIndex(const glm::ivec3& coord, const glm::ivec3& dims) noexcept
    {
        return coord.z * dims.y * dims.x + coord.y * dims.x + coord.x;
    }

    [[nodiscard("the grid coordinate is the only effect")]] inline glm::ivec3 ProbeGridCoord(i32 linearIndex, const glm::ivec3& dims) noexcept
    {
        i32 const planeSize = dims.x * dims.y;
        return { linearIndex % dims.x, (linearIndex / dims.x) % dims.y, linearIndex / planeSize };
    }

    // Probe tile within the 2D atlas: column = x, row = z * dimY + y. Atlas is
    // therefore (dims.x * tileTexels) wide and (dims.y * dims.z * tileTexels)
    // tall. GLSL mirror: ddgiProbeTileCoord.
    [[nodiscard("the atlas tile coordinate is the only effect")]] inline glm::ivec2 ProbeTileCoord(i32 linearIndex, const glm::ivec3& dims) noexcept
    {
        glm::ivec3 const c = ProbeGridCoord(linearIndex, dims);
        return { c.x, c.z * dims.y + c.y };
    }

    [[nodiscard("the atlas tile dimensions are the only effect")]] inline glm::ivec2 AtlasTileDimensions(const glm::ivec3& dims) noexcept
    {
        return { dims.x, dims.y * dims.z };
    }

    // Continuous atlas texel coordinate for bilinear-sampling `direction` from
    // a probe's tile (border-safe: [tileOrigin+1, tileOrigin+1+interior]).
    // Divide by atlas texel dimensions for a normalized UV.
    // GLSL mirror: ddgiProbeAtlasTexel.
    [[nodiscard("the atlas texel coordinate is the only effect")]] inline glm::vec2 ProbeAtlasTexel(i32 linearIndex, const glm::ivec3& dims, const glm::vec3& direction, i32 interiorResolution) noexcept
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
    [[nodiscard("the border-gutter source texel is the only effect")]] inline glm::ivec2 BorderSourceTexel(const glm::ivec2& localTexel, i32 tileTexels) noexcept
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

    [[nodiscard("the per-axis spacing is the only effect")]] inline glm::vec3 ProbeSpacing(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims) noexcept
    {
        glm::vec3 const extent = glm::max(boundsMax - boundsMin, glm::vec3(1e-6f));
        return extent / glm::vec3(glm::max(dims - glm::ivec3(1), glm::ivec3(1)));
    }

    [[nodiscard("the probe grid position is the only effect")]] inline glm::vec3 ProbeGridPosition(const glm::ivec3& coord, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims) noexcept
    {
        return boundsMin + ProbeSpacing(boundsMin, boundsMax, dims) * glm::vec3(coord);
    }

    // World position including the relocation offset (offset is stored
    // normalized by per-axis spacing, RTXGI convention).
    [[nodiscard("the relocated probe position is the only effect")]] inline glm::vec3 ProbeWorldPosition(const glm::ivec3& coord, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims, const glm::vec3& offsetNormalized) noexcept
    {
        return ProbeGridPosition(coord, boundsMin, boundsMax, dims) + offsetNormalized * ProbeSpacing(boundsMin, boundsMax, dims);
    }

    // Per-axis margin the bounce path gathers with. GLSL mirror:
    // ddgiBounceMargin (which reads the precomputed spacing from the UBO).
    //
    // On an axis with resolution 1, ProbeSpacing returns the whole extent
    // (extent / max(dims - 1, 1)), so the margin there is the extent too. That
    // is the rule applied consistently rather than a special case: with one
    // probe layer the volume's INTERIOR is already served by constant
    // extrapolation from that layer across the whole extent, so extending it
    // by one more extent is exactly as justified as the interior is. It does
    // mean a deliberately flat volume reaches further than a reader might
    // expect, which is why it is written down here and pinned by
    // DDGIMath.BounceMarginOnASingleProbeAxisIsTheWholeExtent.
    [[nodiscard("the bounce margin is the only effect")]] inline glm::vec3 BounceMargin(const glm::vec3& spacing) noexcept
    {
        return spacing * kBounceMarginSpacingScale;
    }

    // -------------------------------------------------------------------------
    // Volume membership. GLSL mirror: ddgiIsInsideVolume, ddgiVolumeWeight.
    // -------------------------------------------------------------------------

    [[nodiscard("the membership test is the only effect")]] inline bool IsInsideVolume(const glm::vec3& worldPos, const glm::vec3& boundsMin, const glm::vec3& boundsMax) noexcept
    {
        return glm::all(glm::greaterThanEqual(worldPos, boundsMin)) && glm::all(glm::lessThanEqual(worldPos, boundsMax));
    }

    // Smooth membership of `worldPos` in the volume grown by a per-axis
    // `margin`: exactly 1 inside the bounds, smoothstep down to 0 across the
    // margin band, exactly 0 beyond it.
    //
    // margin == 0 reproduces IsInsideVolume EXACTLY (1 inside, 0 outside,
    // boundary inclusive), which is what lets the one gather serve both the
    // lit path (margin 0, behaviour unchanged) and the bounce path
    // (margin = BounceMargin(spacing), issue #751).
    [[nodiscard("the volume weight must be applied to the gathered irradiance")]] inline f32 VolumeWeight(const glm::vec3& worldPos, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::vec3& margin) noexcept
    {
        glm::vec3 const outside = glm::max(glm::max(boundsMin - worldPos, worldPos - boundsMax), glm::vec3(0.0f));
        if (glm::dot(outside, outside) <= 0.0f)
        {
            return 1.0f;
        }
        glm::vec3 const safeMargin = glm::max(margin, glm::vec3(0.0f));
        // Beyond the margin on ANY axis -> no contribution. This also covers
        // margin == 0, so the division below never divides a positive distance
        // by zero.
        if (glm::any(glm::greaterThan(outside, safeMargin)))
        {
            return 0.0f;
        }
        f32 const t = glm::clamp(glm::length(outside / glm::max(safeMargin, glm::vec3(1e-20f))), 0.0f, 1.0f);
        f32 const w = 1.0f - t * t * (3.0f - 2.0f * t);
        // Positive test on purpose: a non-finite worldPos must read as
        // "outside" (w is NaN, `w > 0.0f` is false), which is what the hard
        // inside-test did for free. See the GLSL mirror for why that matters
        // on the shader side.
        return (w > 0.0f) ? w : 0.0f;
    }

    // -------------------------------------------------------------------------
    // Probe cascades / clipmaps (issue #707).
    //
    // A cascade is a probe LATTICE, not a box: an infinite axis-aligned grid of
    // spacing `Spacing` anchored at `Origin`, of which the storage holds a
    // `Dims`-sized window starting at integer lattice coordinate `LatticeMin`.
    // Storage is TOROIDAL — storage coordinate s holds the lattice point
    // congruent to s modulo Dims that lies inside the window — so moving the
    // window by one cell reassigns exactly one slab of probes and leaves every
    // other probe's cached data valid at its existing storage address.
    //
    // That property is the whole reason cascades are affordable here. Our
    // capture is a RASTERIZED cube-face mini-G-buffer (ADR 0007), not a ray
    // trace: recapturing a probe costs six draws over the nearby casters, so a
    // camera-following grid that invalidated every probe on every one-cell
    // shift could never converge. With toroidal addressing a one-cell shift
    // costs one slab, and sparsity (issue #707's second upgrade) means only the
    // requested probes in that slab are ever paid for.
    //
    // The AUTHORED single-volume path is the same structure with LatticeMin = 0
    // and Origin = BoundsMin, which makes StorageCoordForLattice the identity
    // and reproduces the pre-#707 layout texel for texel. Every legacy helper
    // above stays exactly as it was, and the L1 tests that pin them still pass
    // unchanged — that is deliberate, not incidental.
    // -------------------------------------------------------------------------

    // Hard ceiling on cascade count. Mirrored by DDGI_MAX_CASCADES in
    // DDGICommon.glsl and by the fixed-size arrays in DDGIVolumeUBO, so it
    // cannot be raised on one side alone.
    inline constexpr i32 kMaxCascades = 8;

    // Defaults for the camera-centred (unauthored) path.
    inline constexpr i32 kDefaultCascadeCount = 4;
    inline constexpr f32 kDefaultBaseProbeSpacing = 1.5f;
    inline constexpr glm::ivec3 kDefaultCascadeResolution{ 16, 16, 16 };

    // Upper bound on the per-cascade probe count per axis. Dense storage is
    // CascadeCount * Resolution^3 probes, so this is a MEMORY guard, not a
    // quality one: at 8 cascades, 64^3 would be 2M probes (~9 GB of atlases).
    // The editor slider stops at 32, but a settings file, a script or a quality
    // preset can reach SubmitVolume without passing through the slider, so the
    // bound belongs at the submission boundary too.
    inline constexpr i32 kMaxCascadeResolution = 64;

    // Compute dispatch geometry, shared with the GLSL that must agree with it.
    //
    // GL guarantees only 65535 work groups per dimension and a 6-cascade 32^3
    // field is ~196k probes, so the one-group-per-probe request dispatch is 2D
    // and both sides must use the SAME stride — a mismatch silently processes
    // the wrong probes rather than failing. Same for the screen-request
    // subsample. Pinned against the shader source by
    // BindlessShaderPipeline.DDGIShaderConstantsMatchTheCppMirrors.
    inline constexpr u32 kProbeDispatchStride = 32768u;
    inline constexpr u32 kScreenRequestStride = 8u;

    // Fraction of a cascade's half-extent, per axis, over which its
    // contribution cross-fades into the next coarser cascade. 0 disables the
    // band entirely and restores the pre-#707 hard volume test — which is what
    // the AUTHORED path passes, so its behaviour is bit-identical.
    inline constexpr f32 kDefaultCascadeBlendBand = 0.2f;

    // @brief One cascade's probe lattice + toroidal storage window.
    struct CascadeGrid
    {
        glm::vec3 Origin{ 0.0f };   // world position of lattice coordinate (0,0,0)
        glm::vec3 Spacing{ 1.0f };  // per-axis lattice spacing
        glm::ivec3 LatticeMin{ 0 }; // lattice coordinate stored at the window's low corner
        glm::ivec3 Dims{ 1 };       // storage window size in probes

        bool operator==(const CascadeGrid&) const = default;
    };

    // Cascade `level`'s spacing: each level doubles the previous one's, so
    // cascade N covers 2^N times the extent at 2^N times the probe spacing.
    [[nodiscard("the cascade spacing is the only effect")]] inline glm::vec3 CascadeSpacing(const glm::vec3& baseSpacing, i32 level) noexcept
    {
        // Unsigned shift: `1 << level` on a signed left operand is UB the moment
        // the clamp is ever widened past 30, and there is no signedness to want
        // here -- the result is a positive power of two on its way to f32.
        return baseSpacing * static_cast<f32>(1u << static_cast<u32>(glm::clamp(level, 0, 30)));
    }

    // Camera-centred cascade. The window is placed so the camera sits as close
    // to its centre as the lattice allows; because the lattice itself is
    // anchored at Origin and never moves, every probe position is a fixed world
    // point and re-centring only changes WHICH lattice points are stored.
    [[nodiscard("the cascade grid is the only effect")]] inline CascadeGrid MakeCameraCascade(i32 level, const glm::vec3& cameraWorldPos, const glm::vec3& baseSpacing, const glm::ivec3& dims, const glm::vec3& origin = glm::vec3(0.0f)) noexcept
    {
        CascadeGrid grid;
        grid.Origin = origin;
        grid.Spacing = glm::max(CascadeSpacing(baseSpacing, level), glm::vec3(1e-4f));
        grid.Dims = glm::max(dims, glm::ivec3(1));
        const glm::vec3 latticeF = (cameraWorldPos - origin) / grid.Spacing;
        const glm::ivec3 centre{ static_cast<i32>(glm::floor(latticeF.x + 0.5f)),
                                 static_cast<i32>(glm::floor(latticeF.y + 0.5f)),
                                 static_cast<i32>(glm::floor(latticeF.z + 0.5f)) };
        grid.LatticeMin = centre - grid.Dims / 2;
        return grid;
    }

    // Authored-volume cascade: the pre-#707 corner-anchored lattice, expressed
    // in the same structure. Origin = BoundsMin and LatticeMin = 0, so storage
    // coordinate s holds lattice point s and CascadeProbeGridPosition reduces
    // to ProbeGridPosition exactly.
    [[nodiscard("the cascade grid is the only effect")]] inline CascadeGrid MakeAuthoredCascade(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& dims) noexcept
    {
        CascadeGrid grid;
        grid.Dims = glm::max(dims, glm::ivec3(1));
        grid.Origin = boundsMin;
        grid.Spacing = ProbeSpacing(boundsMin, boundsMax, grid.Dims);
        grid.LatticeMin = glm::ivec3(0);
        return grid;
    }

    // Euclidean modulo — GLSL's `%` and C++'s both truncate toward zero, which
    // maps a negative lattice coordinate to a NEGATIVE storage index. Cascades
    // routinely go negative (the camera moves toward -x), so this is the single
    // most load-bearing line in the toroidal scheme. Forwards to the shared
    // implementation in Math::WrapIndex (also used by the terrain ring-buffer
    // chunk window) — kept as a same-named alias here so call sites in this
    // file and DDGIMathTest.cpp don't need to change.
    [[nodiscard("the wrapped index is the only effect")]] inline i32 WrapIndex(i32 value, i32 modulus) noexcept
    {
        return Math::WrapIndex(value, modulus);
    }

    [[nodiscard("the storage coordinate is the only effect")]] inline glm::ivec3 StorageCoordForLattice(const glm::ivec3& lattice, const glm::ivec3& dims) noexcept
    {
        return { WrapIndex(lattice.x, dims.x), WrapIndex(lattice.y, dims.y), WrapIndex(lattice.z, dims.z) };
    }

    // Inverse of StorageCoordForLattice within the window: the unique lattice
    // coordinate congruent to `storage` (mod Dims) inside [LatticeMin,
    // LatticeMin + Dims).
    [[nodiscard("the lattice coordinate is the only effect")]] inline glm::ivec3 LatticeForStorageCoord(const glm::ivec3& storage, const CascadeGrid& grid) noexcept
    {
        glm::ivec3 lattice{ 0 };
        for (i32 axis = 0; axis < 3; ++axis)
        {
            const i32 dim = glm::max(grid.Dims[axis], 1);
            const i32 base = grid.LatticeMin[axis];
            lattice[axis] = base + WrapIndex(storage[axis] - base, dim);
        }
        return lattice;
    }

    // Un-relocated world position of a storage-addressed probe.
    [[nodiscard("the probe grid position is the only effect")]] inline glm::vec3 CascadeProbeGridPosition(const glm::ivec3& storage, const CascadeGrid& grid) noexcept
    {
        return grid.Origin + glm::vec3(LatticeForStorageCoord(storage, grid)) * grid.Spacing;
    }

    [[nodiscard("the relocated probe position is the only effect")]] inline glm::vec3 CascadeProbeWorldPosition(const glm::ivec3& storage, const CascadeGrid& grid, const glm::vec3& offsetNormalized) noexcept
    {
        return CascadeProbeGridPosition(storage, grid) + offsetNormalized * grid.Spacing;
    }

    // World-space corners of the cascade's stored window (the low corner probe
    // and the high corner probe).
    [[nodiscard("the bounds are the only effect")]] inline glm::vec3 CascadeBoundsMin(const CascadeGrid& grid) noexcept
    {
        return grid.Origin + glm::vec3(grid.LatticeMin) * grid.Spacing;
    }

    [[nodiscard("the bounds are the only effect")]] inline glm::vec3 CascadeBoundsMax(const CascadeGrid& grid) noexcept
    {
        return CascadeBoundsMin(grid) + glm::vec3(glm::max(grid.Dims - glm::ivec3(1), glm::ivec3(0))) * grid.Spacing;
    }

    // Continuous lattice coordinates of a world position (the cascade
    // equivalent of ddgiWorldToProbeGrid).
    [[nodiscard("the lattice coordinates are the only effect")]] inline glm::vec3 WorldToCascadeLattice(const glm::vec3& worldPos, const CascadeGrid& grid) noexcept
    {
        return (worldPos - grid.Origin) / glm::max(grid.Spacing, glm::vec3(1e-6f));
    }

    // How strongly this cascade owns `worldPos`: 1 well inside the window,
    // smoothstepping to 0 across the outer `bandFraction` of the half-extent,
    // 0 outside. bandFraction <= 0 reproduces the hard membership test (1
    // inside the bounds, 0 outside), which is exactly what the authored path
    // wants — see kDefaultCascadeBlendBand.
    //
    // MIRROR NOTE. The GLSL twin (ddgiCascadeWeight) takes ONE EXTRA parameter
    // this does not: a `marginScale` that grows the window by that many probe
    // spacings before the band ramp, which is how the #751 infinite-bounce
    // margin survives cascades. With marginScale = 0 the two are identical
    // expressions — that is the whole relationship, and it is stated here
    // because "one-for-one mirror" is otherwise a claim this pair breaks.
    //
    // The margin has no CPU consumer: the only thing that samples with a margin
    // is the bounce gather, which is a shader. Adding an unused parameter here
    // to make the signatures match would be worse than writing down why they
    // do not.
    [[nodiscard("the cascade weight must be applied to the gathered irradiance")]] inline f32 CascadeInteriorWeight(const glm::vec3& worldPos, const CascadeGrid& grid, f32 bandFraction) noexcept
    {
        const glm::vec3 boundsMin = CascadeBoundsMin(grid);
        const glm::vec3 boundsMax = CascadeBoundsMax(grid);
        if (bandFraction <= 0.0f)
        {
            return IsInsideVolume(worldPos, boundsMin, boundsMax) ? 1.0f : 0.0f;
        }
        const glm::vec3 centre = (boundsMin + boundsMax) * 0.5f;
        const glm::vec3 halfExtent = glm::max((boundsMax - boundsMin) * 0.5f, glm::vec3(1e-6f));
        // Chebyshev (box) distance in units of the half-extent: 0 at the
        // centre, 1 exactly on the window boundary.
        const glm::vec3 n = glm::abs(worldPos - centre) / halfExtent;
        const f32 t = glm::max(glm::max(n.x, n.y), n.z);
        const f32 bandStart = glm::clamp(1.0f - bandFraction, 0.0f, 0.999f);
        if (t <= bandStart)
        {
            return 1.0f;
        }
        if (t >= 1.0f)
        {
            return 0.0f;
        }
        const f32 u = (t - bandStart) / (1.0f - bandStart);
        // 1 - smoothstep(0, 1, u). Written out so the GLSL mirror is textual.
        return 1.0f - (u * u * (3.0f - 2.0f * u));
    }

    // Finest cascade that still owns `worldPos` (weight > 0), or -1 when the
    // position is outside every cascade. Cascade 0 is the finest.
    [[nodiscard("the selected cascade is the only effect")]] inline i32 SelectCascade(const glm::vec3& worldPos, const CascadeGrid* grids, i32 count, f32 bandFraction) noexcept
    {
        if (grids == nullptr)
        {
            return -1;
        }
        for (i32 level = 0; level < count; ++level)
        {
            if (CascadeInteriorWeight(worldPos, grids[level], bandFraction) > 0.0f)
            {
                return level;
            }
        }
        return -1;
    }

    // -------------------------------------------------------------------------
    // Cascaded probe identity / atlas layout.
    //
    // Global probe index = level * probesPerCascade + z-major local index, and
    // the atlas grows in X by cascade: tile column = level * Dims.x + x, tile
    // row = z * Dims.y + y. For a single cascade both reduce to the pre-#707
    // ProbeLinearIndex / ProbeTileCoord / AtlasTileDimensions exactly, so an
    // authored volume produces a byte-identical atlas.
    // -------------------------------------------------------------------------

    [[nodiscard("the probe count is the only effect")]] inline i32 ProbesPerCascade(const glm::ivec3& dims) noexcept
    {
        return glm::max(dims.x, 1) * glm::max(dims.y, 1) * glm::max(dims.z, 1);
    }

    [[nodiscard("the global probe index is the only effect")]] inline i32 CascadedProbeIndex(i32 level, const glm::ivec3& storage, const glm::ivec3& dims) noexcept
    {
        return level * ProbesPerCascade(dims) + ProbeLinearIndex(storage, dims);
    }

    [[nodiscard("the cascade level is the only effect")]] inline i32 CascadeOfProbeIndex(i32 globalIndex, const glm::ivec3& dims) noexcept
    {
        return globalIndex / ProbesPerCascade(dims);
    }

    [[nodiscard("the storage coordinate is the only effect")]] inline glm::ivec3 StorageCoordOfProbeIndex(i32 globalIndex, const glm::ivec3& dims) noexcept
    {
        return ProbeGridCoord(globalIndex % ProbesPerCascade(dims), dims);
    }

    [[nodiscard("the atlas tile coordinate is the only effect")]] inline glm::ivec2 CascadedProbeTileCoord(i32 level, const glm::ivec3& storage, const glm::ivec3& dims) noexcept
    {
        return { level * glm::max(dims.x, 1) + storage.x, storage.z * glm::max(dims.y, 1) + storage.y };
    }

    [[nodiscard("the atlas tile coordinate is the only effect")]] inline glm::ivec2 CascadedProbeTileCoord(i32 globalIndex, const glm::ivec3& dims) noexcept
    {
        return CascadedProbeTileCoord(CascadeOfProbeIndex(globalIndex, dims), StorageCoordOfProbeIndex(globalIndex, dims), dims);
    }

    [[nodiscard("the atlas tile dimensions are the only effect")]] inline glm::ivec2 CascadedAtlasTileDimensions(const glm::ivec3& dims, i32 cascadeCount) noexcept
    {
        return { glm::max(dims.x, 1) * glm::max(cascadeCount, 1), glm::max(dims.y, 1) * glm::max(dims.z, 1) };
    }

    // -------------------------------------------------------------------------
    // Sparsity + variable update rate (issue #707 upgrades 2 and 3).
    // -------------------------------------------------------------------------

    // How many frames a probe stays live after the last request. Long enough
    // that a probe grazed by one frame's shading does not flicker in and out
    // with sub-pixel camera motion; short enough that the set follows the view.
    inline constexpr u32 kProbeRequestLifetimeFrames = 16u;

    // Supported per-frame relight fractions. PGI defaults to 1-in-8.
    enum class ProbeUpdateRate : i32
    {
        EveryFrame = 1,
        OneInTwo = 2,
        OneInEight = 8,
        OneInSixteen = 16,
        OneInThirtyTwo = 32,
        OneInSixtyFour = 64
    };

    inline constexpr ProbeUpdateRate kDefaultProbeUpdateRate = ProbeUpdateRate::OneInEight;

    [[nodiscard("the divisor is the only effect")]] constexpr i32 UpdateRateDivisor(ProbeUpdateRate rate) noexcept
    {
        return glm::max(static_cast<i32>(rate), 1);
    }

    // Snap an arbitrary integer to the nearest supported rate at or below it
    // (so an out-of-range authored value degrades to more work, never less).
    [[nodiscard("the snapped rate is the only effect")]] constexpr ProbeUpdateRate SnapUpdateRate(i32 divisor) noexcept
    {
        if (divisor >= 64)
        {
            return ProbeUpdateRate::OneInSixtyFour;
        }
        if (divisor >= 32)
        {
            return ProbeUpdateRate::OneInThirtyTwo;
        }
        if (divisor >= 16)
        {
            return ProbeUpdateRate::OneInSixteen;
        }
        if (divisor >= 8)
        {
            return ProbeUpdateRate::OneInEight;
        }
        if (divisor >= 2)
        {
            return ProbeUpdateRate::OneInTwo;
        }
        return ProbeUpdateRate::EveryFrame;
    }

    // Round-robin phase test. Deliberately (index + frame) % divisor rather
    // than a hash: it guarantees EXACTLY one frame in `divisor` per probe and
    // that adjacent probes update on adjacent frames, so a probe's neighbours
    // are never all stale at once — which is what a hash would allow and what
    // makes a hashed schedule read as blotchy convergence.
    // GLSL mirror: ddgiProbeUpdatesThisFrame.
    [[nodiscard("the schedule decision is the only effect")]] inline bool ProbeUpdatesThisFrame(i32 probeIndex, u32 frameIndex, i32 divisor) noexcept
    {
        const i32 d = glm::max(divisor, 1);
        if (d == 1)
        {
            return true;
        }
        return WrapIndex(probeIndex + static_cast<i32>(frameIndex % static_cast<u32>(d)), d) == 0;
    }

    // A probe is live while a shaded pixel (or another live probe's hit point)
    // requested it within the lifetime window. `lastRequestFrame` is the frame
    // counter atomicMax'd into the request buffer; 0 means "never requested",
    // which is why frame counters START AT 1 (see DDGIProbeUpdatePass).
    [[nodiscard("the liveness decision is the only effect")]] inline bool IsProbeLive(u32 lastRequestFrame, u32 frameIndex, u32 lifetime) noexcept
    {
        if (lastRequestFrame == 0u)
        {
            return false;
        }
        return (frameIndex <= lastRequestFrame) || ((frameIndex - lastRequestFrame) <= lifetime);
    }

    // -------------------------------------------------------------------------
    // Capture priority (issue #707). CPU-only — capture is rasterization, so the
    // CPU picks the set; there is no GLSL mirror.
    //
    // This lives here, as a pure function with its own contract test, because
    // getting the ORDER wrong is invisible: every tier still gets captured
    // eventually, so coverage still reaches 100% and every DDGI test still
    // passes — just many frames later. Putting refinement ahead of first
    // capture multiplied the time to full coverage by the refinement count and
    // took the wide-volume parity rig from 12 frames to 56 against its 60-frame
    // budget. That is a green test one tuning change away from red, and a
    // comment is not a mechanism for keeping it green.
    // -------------------------------------------------------------------------

    enum class CaptureTier : i32
    {
        // Contributes NOTHING to the gather until captured. Always first.
        NeverCaptured = 0,
        // Already contributing, just from a slightly wrong position — the
        // relocation spring wants another look.
        RelocationRefinement = 1,
        // Correct, merely stale. Healed at a throttled rate.
        PeriodicRefresh = 2
    };

    // Separation between tiers. Intra-tier ranks are clamped strictly below it,
    // so the tier ordering is EXACT rather than emergent — a probe 2 km away in
    // cascade 7 still cannot outrank a nearer probe in a more urgent tier,
    // which an unclamped `distance + tier * bias` does not guarantee.
    inline constexpr f32 kCaptureTierBias = 1.0e6f;
    // Weight on the cascade level inside a tier: finer cascades first, since
    // they are what the viewer is actually standing in.
    inline constexpr f32 kCaptureCascadePenalty = 1.0e3f;

    // Lower is more urgent. `framesSinceCapture` is only consulted for
    // PeriodicRefresh (oldest first); the other tiers order by CASCADE LEVEL
    // FIRST and distance second — kCaptureCascadePenalty is 1e3, so any coarser
    // cascade loses to any finer one inside a kilometre, which is the intended
    // priority (the finest cascade is what the viewer is standing in) and not
    // merely a tie-break. Callers break remaining ties on the probe index so the
    // schedule is reproducible frame to frame — a temporal algorithm whose
    // capture order wobbles cannot be converged against.
    [[nodiscard("the capture score is the only effect")]] inline f32 CaptureScore(CaptureTier tier, f32 distanceToCamera, i32 cascadeLevel, u32 framesSinceCapture) noexcept
    {
        constexpr f32 kMaxIntra = kCaptureTierBias - 1.0f;

        f32 intra = 0.0f;
        if (tier == CaptureTier::PeriodicRefresh)
        {
            // Oldest first: a probe not recaptured for a long time is the one
            // most likely to be showing moved geometry. Distance deliberately
            // does not enter — a stale far probe is just as wrong as a stale
            // near one, and letting distance win would starve the far field
            // forever.
            const f32 age = glm::min(static_cast<f32>(framesSinceCapture), kMaxIntra);
            intra = kMaxIntra - age;
        }
        else
        {
            const f32 safeDistance = (distanceToCamera > 0.0f) ? distanceToCamera : 0.0f;
            intra = safeDistance + static_cast<f32>(glm::max(cascadeLevel, 0)) * kCaptureCascadePenalty;
        }

        return static_cast<f32>(std::to_underlying(tier)) * kCaptureTierBias +
               glm::clamp(intra, 0.0f, kMaxIntra);
    }

    // -------------------------------------------------------------------------
    // Sampler weights (the leak fix). GLSL mirror: ddgiChebyshevWeight,
    // ddgiWrapShadingWeight, ddgiCrushWeight, ddgiSelfShadowBias.
    // -------------------------------------------------------------------------

    // Chebyshev upper-bound visibility: probability the surface at distance r
    // is unoccluded from the probe, given the distance distribution's mean and
    // mean^2 along the sample direction. 1 when r <= mean (closer than the
    // average occluder). Cubed to sharpen, floored to keep a minimum bleed.
    [[nodiscard("the visibility weight must be applied to the probe contribution")]] inline f32 ChebyshevWeight(f32 mean, f32 meanSquared, f32 r) noexcept
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
    [[nodiscard("the backface weight must be applied to the probe contribution")]] inline f32 WrapShadingWeight(const glm::vec3& dirToProbe, const glm::vec3& normal) noexcept
    {
        f32 const wrapped = (glm::dot(dirToProbe, normal) + 1.0f) * 0.5f;
        return wrapped * wrapped + 0.2f;
    }

    // Crush tiny weights smoothly to zero (suppresses variance amplification
    // when normalizing a near-zero total weight; RTXGI convention).
    [[nodiscard("the crushed weight must replace the input weight")]] inline f32 CrushWeight(f32 w) noexcept
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
    [[nodiscard("the bias offset must be applied to the shading point")]] inline glm::vec3 SelfShadowBias(const glm::vec3& normal, const glm::vec3& viewDir, f32 minAxialSpacing, f32 biasScale) noexcept
    {
        return (0.2f * normal + 0.8f * viewDir) * (0.75f * minAxialSpacing) * biasScale;
    }

    // -------------------------------------------------------------------------
    // Blend pass math. GLSL mirror: DDGIProbeBlend.comp.
    // -------------------------------------------------------------------------

    // Cosine weight of a cached hit direction for an irradiance texel
    // direction (power 1: Lambertian irradiance).
    [[nodiscard("the cosine blend weight must be applied to the ray radiance")]] inline f32 IrradianceBlendWeight(const glm::vec3& texelDir, const glm::vec3& hitDir) noexcept
    {
        return glm::max(0.0f, glm::dot(texelDir, hitDir));
    }

    // Power-cosine weight for the visibility texels (sharper lobe so the
    // distance estimate is directional; RTXGI probeDistanceExponent default).
    inline constexpr f32 kDistanceBlendExponent = 50.0f;

    [[nodiscard("the power-cosine blend weight must be applied to the hit distance")]] inline f32 DistanceBlendWeight(const glm::vec3& texelDir, const glm::vec3& hitDir, f32 exponent = kDistanceBlendExponent) noexcept
    {
        return glm::pow(glm::max(0.0f, glm::dot(texelDir, hitDir)), exponent);
    }

    // Temporal EMA: hysteresis is the fraction of HISTORY kept.
    [[nodiscard("the blended value must be written back to the atlas texel")]] inline glm::vec3 BlendEMA(const glm::vec3& newValue, const glm::vec3& oldValue, f32 hysteresis) noexcept
    {
        return glm::mix(newValue, oldValue, hysteresis);
    }

    // Big-change response: when the new estimate departs strongly from
    // history, cut hysteresis so lights snapping on/off do not smear (JCGT
    // 2021 thresholds: >25% of full range -> reduce, >80% -> drop history).
    [[nodiscard("the adjusted hysteresis must replace the input hysteresis")]] inline f32 AdjustHysteresis(f32 hysteresis, const glm::vec3& newValue, const glm::vec3& oldValue) noexcept
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

        // Issue #707, upgrade 4. The two aggregates the SPRING relocation adds
        // over RTXGI's closest-back/front-face rule.
        //
        // FreeDirectionSum: sum of hit directions weighted by how UNOCCLUDED
        // they are (a sky miss counts 1, a hit at the max ray distance counts
        // ~1, a hit right against the probe counts 0). Its normalized value is
        // the "average free direction" — where the open space is, considered
        // over the whole sphere rather than from the single nearest surface.
        //
        // CrowdingSum: sum over CLOSE hits of dir * (1 - dist/minDist), the
        // repulsion term. Both are accumulated over the same hit-cache sweep
        // that produces the fields above, so the spring costs no extra taps.
        glm::vec3 FreeDirectionSum{ 0.0f };
        f32 FreeDirectionWeight = 0.0f;
        glm::vec3 CrowdingSum{ 0.0f };
        f32 CrowdingWeight = 0.0f;
    };

    // Spring-force relocation tuning (issue #707). Deliberately gentle: the
    // spring runs EVERY time a probe is captured and its output is fed back as
    // the next capture's position, so a step size near 1 turns a mild crowding
    // signal into an oscillation between two cells. 0.35 settles in three to
    // four captures on the bring-up scenes without overshoot.
    inline constexpr f32 kSpringStepScale = 0.35f;
    // Pull back toward the undistorted lattice point, in units of the current
    // offset per step. This is the "grid distortion" half: without it a probe
    // that finds ANY open direction keeps sliding, and a field of probes that
    // have all slid loses the regular spacing the trilinear gather assumes.
    inline constexpr f32 kSpringGridRestoreScale = 0.25f;
    // How strongly the average free direction pulls, relative to crowding.
    // Crowding is the safety term (never be inside a wall); free direction is
    // the quality term (see as much of the room as possible), so crowding wins.
    inline constexpr f32 kSpringFreeDirectionScale = 0.35f;
    inline constexpr f32 kSpringCrowdingScale = 1.0f;

    // Returns the new offset, normalized by per-axis spacing. minFrontfaceDistance
    // is in world units (RTXGI default 1.0, scaled to scene by callers).
    //
    // SUPERSEDED BY RelocateProbeSpring SINCE #707, and kept deliberately. It
    // is no longer on any render path — the GPU relocation compute evaluates
    // the spring — but it is the stock RTXGI three-case rule the spring is
    // defined as a departure FROM, it is what the spring's documentation and
    // the agent-rules postmortem compare against, and its existing L1 tests are
    // the record of what the pre-#707 behaviour was. Deleting it would leave
    // "the spring replaces RTXGI's closest-face rule" as a claim with nothing
    // in the tree to check it against.
    [[nodiscard("relocation has no side effects — the new offset must be stored")]] inline glm::vec3 RelocateProbe(const glm::vec3& currentOffsetN, const ProbeHitAggregates& agg, const glm::vec3& spacing, f32 minFrontfaceDistance) noexcept
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

    [[nodiscard("classification has no side effects — the state must be stored")]] inline ProbeState ClassifyProbe(const ProbeHitAggregates& agg) noexcept
    {
        if (agg.BackfaceFraction > kBackfaceFraction)
        {
            return ProbeState::Inactive;
        }
        return ProbeState::Active;
    }

    // -------------------------------------------------------------------------
    // Spring-force relocation (issue #707, upgrade 4 — PGI's quality delta over
    // stock DDGI). GLSL mirror: ddgiRelocateProbeSpring in DDGICommon.glsl,
    // evaluated by compute/DDGI_Relocate.comp.
    //
    // Stock RTXGI relocation (RelocateProbe above) is a THREE-CASE rule driven
    // by the single closest back/front face. It fixes probes that are strictly
    // inside geometry, and it is what this engine shipped from #632. What it
    // does NOT fix is the case PGI's notes call out as the visible one: a probe
    // in open air but pressed against a wall, or wedged in a corner, where the
    // closest-face rule either does nothing (the probe is not inside anything)
    // or slides along one face and stops. Those probes see a hemisphere of wall
    // and become the "blind spots" that leave dark patches in Sponza-like
    // scenes.
    //
    // The spring form replaces the two non-degenerate cases with a force
    // balance over the WHOLE hit cache:
    //
    //   crowding    — every hit closer than minFrontfaceDistance pushes the
    //                 probe away, weighted by how close it is. A corner
    //                 therefore pushes along the diagonal, which no
    //                 single-closest-face rule can express.
    //   free        — the openness-weighted mean hit direction pulls the probe
    //                 toward where it can actually see, so a probe against a
    //                 wall drifts into the room rather than along the wall.
    //   grid        — a restoring pull toward the undistorted lattice point, so
    //                 the field keeps the regular spacing the trilinear gather
    //                 assumes and probes do not all migrate into the same
    //                 pocket.
    //
    // The strictly-inside-geometry escape is KEPT from the stock rule: a spring
    // starting inside a wall has to climb out through the crowding term alone,
    // which is slow and can stall in a symmetric cavity. Pushing straight
    // through the closest backface is both faster and better-conditioned, and
    // it is the one case where the closest-face signal is unambiguous.
    //
    // Returns the new offset normalized by per-axis spacing, clamped to the
    // same 45%-of-cell ellipsoid as RelocateProbe (an offset that leaves the
    // cell breaks the gather's assumption that probe i is near lattice point i,
    // and RTXGI's constant is the one the Chebyshev weights were tuned for).
    [[nodiscard("relocation has no side effects — the new offset must be stored")]] inline glm::vec3 RelocateProbeSpring(const glm::vec3& currentOffsetN, const ProbeHitAggregates& agg, const glm::vec3& spacing, f32 minFrontfaceDistance) noexcept
    {
        const glm::vec3 safeSpacing = glm::max(spacing, glm::vec3(1e-6f));
        glm::vec3 offsetWorld = currentOffsetN * safeSpacing;

        if (agg.BackfaceFraction > kBackfaceFraction && agg.ClosestBackfaceDist >= 0.0f)
        {
            // Inside geometry — the stock escape, unchanged.
            offsetWorld += agg.ClosestBackfaceDir * (agg.ClosestBackfaceDist + 0.5f * minFrontfaceDistance);
        }
        else
        {
            glm::vec3 force(0.0f);

            if (agg.CrowdingWeight > 0.0f)
            {
                // Mean repulsion direction x mean crowding magnitude. Scaled by
                // minFrontfaceDistance so the force is in world units and a
                // fully-crowded probe moves at most one comfort distance.
                const glm::vec3 mean = agg.CrowdingSum / agg.CrowdingWeight;
                force -= mean * (kSpringCrowdingScale * minFrontfaceDistance);
            }

            if (agg.FreeDirectionWeight > 0.0f)
            {
                const glm::vec3 mean = agg.FreeDirectionSum / agg.FreeDirectionWeight;
                const f32 len = glm::length(mean);
                if (len > 1e-4f)
                {
                    // The mean SHRINKS toward zero when free space is isotropic
                    // (nothing to move toward) and approaches a unit vector when
                    // one hemisphere is open — so its length is exactly the
                    // "how lopsided is my visibility" signal, and using it
                    // unnormalized is deliberate.
                    force += mean * (kSpringFreeDirectionScale * minFrontfaceDistance);
                }
            }

            // Grid restore: always present, so an unconstrained probe returns
            // to its lattice point instead of holding whatever offset it last
            // acquired.
            force -= offsetWorld * kSpringGridRestoreScale;

            offsetWorld += force * kSpringStepScale;
        }

        const glm::vec3 offsetN = offsetWorld / safeSpacing;
        const f32 lenSq = glm::dot(offsetN, offsetN);
        if (lenSq < kMaxProbeOffsetFraction * kMaxProbeOffsetFraction)
        {
            return offsetN;
        }
        // Outside the ellipsoid: PROJECT back onto it rather than rejecting the
        // whole step the way RelocateProbe does. Rejection is right for the
        // three-case rule (its steps are large and discrete, so a rejected step
        // means "that move was wrong"), but wrong for a spring: the spring's
        // steps are small and continuous, and rejecting them leaves a probe
        // pinned at the boundary with a permanently unsatisfied force, which
        // reads as a probe that never converges.
        if (lenSq <= 0.0f)
        {
            return glm::vec3(0.0f);
        }
        return offsetN * (kMaxProbeOffsetFraction * 0.999f / glm::sqrt(lenSq));
    }
} // namespace OloEngine::DDGI
