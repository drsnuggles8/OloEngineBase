#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::WaterRain
{
    // =========================================================================
    // THE RAIN-RIPPLE CONTRACT (issue #1034, §7.3)
    //
    // Rain stipples the water with small expanding rings. This header is the
    // ONE place that field is written down; two consumers mirror it and
    // neither re-derives it:
    //
    //   * include/WaterRainCommon.glsl — evaluates it per fragment in Water.glsl;
    //   * WaterRainRippleTest.cpp      — pins this side, and the hash parity.
    //
    // ---- 1. Why this is procedural and NOT the disturbance field ------------
    //
    // The obvious reading of §7.3 is "drive WaterDisturbanceSystem from the
    // precipitation system" — it is already "inject a decaying world-anchored
    // disturbance". Two hard numbers rule that out, and both are in
    // WaterDisturbanceField.h:
    //
    //   * the field is 0.5 m PER TEXEL. A rain ring is ~0.2 m across at its
    //     widest, so every ripple would land inside a single texel and the
    //     whole feature would render as noise on a half-metre grid;
    //   * the splat queue is 96 PER FRAME, shared with the boat wake. Rain
    //     covers the whole visible sea; at any usable density it would starve
    //     the wake queue every frame and drop most of its own impacts.
    //
    // So the rings are evaluated analytically at shading time from a
    // world-anchored cell hash. That also gives the acceptance criterion "no
    // cost when rain is off" STRUCTURALLY: `strength <= 0` returns before the
    // cell loop, and strength comes from a uniform, so the branch is uniform
    // and the loop is never entered.
    //
    // ---- 2. The lattice -----------------------------------------------------
    //
    // Ripples live on a square cell grid anchored at the WORLD ORIGIN, one
    // potential impact per cell per cycle. Cell `c` covers absolute world XZ
    // [c * cellSize, (c+1) * cellSize). Anchoring at the origin (rather than at
    // the camera or the water mesh) is what stops the stipple sliding when the
    // camera moves or the surface re-tessellates — the same reason the
    // disturbance field is world-anchored.
    //
    // The impact point is jittered inside its cell and re-jittered every cycle,
    // so a cell does not fire on the same spot forever. Because a jittered
    // centre plus kRingMaxRadiusMetres can cross a cell boundary, evaluation
    // walks the 3x3 neighbourhood.
    //
    // ---- 3. Why an integer hash --------------------------------------------
    //
    // The cycle index grows without bound with the clock (at a 0.9 s lifetime,
    // an hour of play is ~4000 cycles), and the usual `fract(sin(dot(p, k)) * c)`
    // hash degenerates into visible structure once its argument gets large —
    // a defect that only appears after the scene has been running for a while,
    // i.e. never in a test. The lowbias32 integer finalizer below has no such
    // regime, and it is EXACT on both sides: u32 wrapping arithmetic is the
    // same in C++ and in GLSL, so WaterRainRippleTest compares the two hashes
    // value-for-value instead of comparing pictures.
    // =========================================================================

    /// Metres per ripple cell. One potential impact per cell per cycle, so this
    /// is the mean impact spacing at full density. 0.55 m reads as rain from a
    /// standing camera and keeps the neighbourhood walk at 9 cells.
    inline constexpr f32 kCellSizeMetres = 0.55f;

    /// Seconds from a ring's birth to its disappearance.
    inline constexpr f32 kRingLifetimeSeconds = 0.9f;

    /// Radius the ring crest reaches at the end of its life, in metres.
    /// Expansion is linear in age — real ripples decelerate slightly, but the
    /// difference over 0.9 s and 0.22 m is well under a pixel.
    inline constexpr f32 kRingMaxRadiusMetres = 0.22f;

    /// Crest height of a ring at full amplitude, in metres.
    ///
    /// THE LENGTH SCALE, and the reason it is a named constant rather than
    /// folded into the profile: RingProfileSlope is dimensionless (it is the
    /// derivative of sin(pi x) exp(-x^2), whose amplitude is 1), and the slope
    /// it feeds is divided by a width in METRES. Without a height in metres to
    /// multiply back in, the profile's implicit amplitude is one metre — an
    /// 80 cm raindrop ripple — and the resulting surface slope peaks near 41,
    /// i.e. a surface tilted 88 degrees. That renders: it tips the shading
    /// normal past horizontal and the sea fills with black speckle that reads
    /// as a shader bug rather than as a units bug.
    ///
    /// 1.5 mm over a ~4.5 cm wavelet is the real thing, and it puts the peak
    /// slope near 0.07 — a few degrees, which on a specular surface is plenty.
    /// SlopeStaysBoundedAtFullStrength is the pin.
    inline constexpr f32 kRingHeightMetres = 0.0015f;

    /// Half-width of the ring's wavelet at birth, in metres. It widens with age
    /// (see RingWidth) so an old ring never becomes sharper than the pixel that
    /// samples it — the Nyquist rule in docs/agent-rules/water-shading-nyquist.md
    /// §3 applies to a procedural signal exactly as it does to a normal map.
    inline constexpr f32 kRingWidthMetres = 0.045f;

    /// Ring widths beyond the crest at which a contribution is dropped.
    /// exp(-2.5^2) is ~0.002, below an 8-bit quantum.
    inline constexpr f32 kRingCutoffWidths = 2.5f;

    /// Lower corner and extent, as a fraction of the cell, that the jittered
    /// impact point is confined to. Keeping impacts off the cell boundary means
    /// the 3x3 walk always contains the whole ring.
    inline constexpr f32 kJitterOrigin = 0.15f;
    inline constexpr f32 kJitterExtent = 0.70f;

    /// Density at full precipitation intensity. Below 1 on purpose: every cell
    /// firing every cycle is a uniform boil, not rain.
    inline constexpr f32 kMaxDensity = 0.85f;

    /// Precipitation intensity below which ripples switch OFF outright rather
    /// than merely scaling down.
    ///
    /// Load-bearing for the acceptance criterion, not a look tweak.
    /// PrecipitationSystem smooths its intensity with a lerp toward the target,
    /// which is an exponential decay that never actually reaches zero — so
    /// after the first shower the strength would sit at some tiny positive
    /// value forever, the shader's `params.x <= 0` early-out would never fire
    /// again, and "no cost when rain is off" would silently become "no cost
    /// until it has rained once". At 0.01 the deepest ring is ~1 % of a
    /// millimetre of slope: below anything an 8-bit frame can show.
    inline constexpr f32 kMinIntensity = 0.01f;

    // -------------------------------------------------------------------------
    // Hashing — bit-exact with waterRainHash* in WaterRainCommon.glsl
    // -------------------------------------------------------------------------

    /// lowbias32 (Chris Wellons). u32 wrapping arithmetic, so this is
    /// bit-identical in C++ and GLSL.
    [[nodiscard("the hashed value is the only effect")]]
    inline u32 HashU32(u32 x) noexcept
    {
        x ^= x >> 16u;
        x *= 0x7feb352du;
        x ^= x >> 15u;
        x *= 0x846ca68bu;
        x ^= x >> 16u;
        return x;
    }

    /// Hash of one (cell, cycle, stream) tuple. `stream` separates the several
    /// independent values a single cell needs (jitter x, jitter y, existence)
    /// without correlating them, which re-scaling one seed would not do.
    [[nodiscard("the hashed value is the only effect")]]
    inline u32 HashCell(glm::ivec2 cell, i32 cycle, u32 stream) noexcept
    {
        u32 h = HashU32(static_cast<u32>(cell.x) * 0x9e3779b9u);
        h = HashU32(h ^ (static_cast<u32>(cell.y) * 0x85ebca6bu));
        h = HashU32(h ^ (static_cast<u32>(cycle) * 0xc2b2ae35u));
        return HashU32(h ^ (stream * 0x27d4eb2fu));
    }

    /// Hash mapped to [0, 1). 24 bits — exactly an f32 mantissa, so the
    /// conversion is lossless and the GLSL twin cannot round differently.
    [[nodiscard("the unit value is the only effect")]]
    inline f32 UnitFromHash(u32 h) noexcept
    {
        return static_cast<f32>(h >> 8u) * (1.0f / 16777216.0f);
    }

    // -------------------------------------------------------------------------
    // One ring
    // -------------------------------------------------------------------------

    /// Ring crest radius (metres) at normalised age `age` in [0, 1).
    [[nodiscard("the radius is the only effect")]]
    inline f32 RingRadius(f32 age) noexcept
    {
        return kRingMaxRadiusMetres * age;
    }

    /// Wavelet half-width (metres) at normalised age `age`. Widening with age
    /// is an anti-alias term, not a look choice: the crest travels outward, so
    /// its screen-space footprint shrinks exactly as the ring gets faint.
    [[nodiscard("the width is the only effect")]]
    inline f32 RingWidth(f32 age) noexcept
    {
        return kRingWidthMetres * (1.0f + 2.0f * age);
    }

    /// Amplitude envelope over a ring's life. Ramps in over the first sixth so
    /// a ring does not pop into existence at full strength, then falls linearly
    /// to zero at age 1 — which is what makes the field continuous in time and
    /// so free of the per-cycle flicker a hard cutoff would give.
    [[nodiscard("the amplitude is the only effect")]]
    inline f32 RingAmplitude(f32 age) noexcept
    {
        return (1.0f - age) * glm::min(age * 6.0f, 1.0f);
    }

    /// d/dx of the ring's UNIT height profile h(x) = sin(pi x) * exp(-x^2),
    /// where x is the signed distance from the crest measured in ring widths.
    ///
    /// Dimensionless — kRingHeightMetres supplies the length scale, and the
    /// division by the ring width supplies the other half of the slope's units.
    /// Read that constant's comment before touching either.
    ///
    /// The DERIVATIVE is the primitive rather than the height because nothing
    /// consumes the height: §7.3 is explicitly normal-map-only, so the surface
    /// is never displaced and only the slope is ever asked for. Differencing a
    /// height to get a slope would be a second expression of the same function
    /// and a second chance for the two sides to disagree.
    [[nodiscard("the profile derivative is the only effect")]]
    inline f32 RingProfileSlope(f32 x) noexcept
    {
        constexpr f32 kPi = 3.14159265358979323846f;
        if (glm::abs(x) >= kRingCutoffWidths)
            return 0.0f;
        const f32 gauss = std::exp(-x * x);
        return (kPi * std::cos(kPi * x) - 2.0f * x * std::sin(kPi * x)) * gauss;
    }

    // -------------------------------------------------------------------------
    // The field
    // -------------------------------------------------------------------------

    /// Surface slope (dh/dWorldX, dh/dWorldZ) contributed by rain ripples at
    /// ABSOLUTE world XZ.
    ///
    /// `strength` is the already-combined artist gain x precipitation
    /// intensity; `density` in [0, 1] is the fraction of cells that fire in a
    /// given cycle. `strength <= 0` returns zero WITHOUT walking the
    /// neighbourhood — that early-out is the "no cost when rain is off"
    /// acceptance criterion, and it is a uniform branch on the GPU side.
    ///
    /// Expression-for-expression identical to `waterRainRippleSlope` in
    /// include/WaterRainCommon.glsl.
    [[nodiscard("the slope is the only effect")]]
    inline glm::vec2 RippleSlope(glm::vec2 absoluteWorldXZ, f32 timeSeconds, f32 strength,
                                 f32 density, f32 cellSizeMetres) noexcept
    {
        if (!(strength > 0.0f))
            return glm::vec2(0.0f);
        if (!std::isfinite(absoluteWorldXZ.x) || !std::isfinite(absoluteWorldXZ.y) ||
            !std::isfinite(timeSeconds))
            return glm::vec2(0.0f);

        const f32 cellSize = glm::max(cellSizeMetres, 1.0e-3f);
        const glm::vec2 gridPos = absoluteWorldXZ / cellSize;
        const glm::ivec2 baseCell{ static_cast<i32>(std::floor(gridPos.x)),
                                   static_cast<i32>(std::floor(gridPos.y)) };
        const f32 clampedDensity = glm::clamp(density, 0.0f, 1.0f);
        const f32 invLifetime = 1.0f / kRingLifetimeSeconds;

        glm::vec2 slope(0.0f);
        for (i32 dz = -1; dz <= 1; ++dz)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                const glm::ivec2 cell = baseCell + glm::ivec2(dx, dz);

                // Per-cell phase offset, so neighbouring cells do not all fire
                // on the same beat.
                const f32 phase = UnitFromHash(HashCell(cell, 0, 0u));
                const f32 cycleT = timeSeconds * invLifetime + phase;
                const f32 cycleFloor = std::floor(cycleT);
                const i32 cycle = static_cast<i32>(cycleFloor);
                const f32 age = cycleT - cycleFloor;

                // Existence is re-rolled every cycle, so light rain is sparse in
                // TIME as well as in space rather than being a fixed subset of
                // cells that always rains and a fixed subset that never does.
                if (UnitFromHash(HashCell(cell, cycle, 3u)) >= clampedDensity)
                    continue;

                const glm::vec2 jitter{ UnitFromHash(HashCell(cell, cycle, 1u)),
                                        UnitFromHash(HashCell(cell, cycle, 2u)) };
                const glm::vec2 centre =
                    (glm::vec2(cell) + kJitterOrigin + kJitterExtent * jitter) * cellSize;

                const glm::vec2 delta = absoluteWorldXZ - centre;
                const f32 dist = std::sqrt(glm::dot(delta, delta));
                if (dist < 1.0e-6f)
                    continue; // at the exact centre the radial direction is undefined

                const f32 width = RingWidth(age);
                const f32 x = (dist - RingRadius(age)) / width;
                const f32 profile = RingProfileSlope(x);
                if (profile == 0.0f)
                    continue;

                // metres (kRingHeightMetres) x envelope x gain, divided by a
                // width in metres — so the result is a dimensionless slope.
                const f32 amplitude = kRingHeightMetres * RingAmplitude(age) * strength;
                slope += (amplitude * profile / width) * (delta / dist);
            }
        }
        return slope;
    }

    /// Ripple density for a precipitation intensity in [0, 1].
    ///
    /// Deliberately NOT the identity: a tenth of the cells firing at intensity
    /// 0.1 is too sparse to read as rain at all, and the square root is the
    /// cheap curve that lifts light rain into view while keeping heavy rain
    /// short of the uniform boil kMaxDensity exists to avoid.
    [[nodiscard("the density is the only effect")]]
    inline f32 DensityForIntensity(f32 intensity) noexcept
    {
        if (!std::isfinite(intensity) || intensity <= 0.0f)
            return 0.0f;
        return glm::clamp(std::sqrt(glm::min(intensity, 1.0f)) * kMaxDensity, 0.0f, kMaxDensity);
    }

    /// Scene-level rain-ripple controls, published each frame by
    /// Scene::ProcessScene3DSharedLogic from the dominant WaterComponent and
    /// combined with the live precipitation state by WaterRainRippleSystem.
    struct WaterRainSettings
    {
        bool m_Enabled = false;
        /// Artist gain on the ripple slope, multiplied by the live
        /// precipitation intensity before it reaches the shader.
        f32 m_Strength = 1.0f;
        /// Camera distances over which the ripples fade out. The cell grid is
        /// sub-metre, so it is undersampled long before the horizon — see
        /// docs/agent-rules/water-shading-nyquist.md §3.
        f32 m_FadeStartMetres = 18.0f;
        f32 m_FadeEndMetres = 45.0f;
    };
} // namespace OloEngine::WaterRain
