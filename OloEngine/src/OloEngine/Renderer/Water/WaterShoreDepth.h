#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::WaterShore
{
    // =========================================================================
    // THE SHORE-WAVE CONTRACT (issue #1033)
    //
    // Deep-water waves have the same shape everywhere. Approaching a shallow
    // shore a real wave slows, bunches up, grows, turns toward the beach and
    // eventually breaks. All four fall out of ONE input the engine did not have:
    // the water depth under each surface point. This header is the single place
    // that input is defined, and the single place the wave transform driven by
    // it is written down.
    //
    // Four consumers mirror this file and none of them re-derives it:
    //
    //   * WaterShoreDepthSystem.cpp            — bakes the field from the scene's
    //                                            terrain, sizes/uploads the texture,
    //                                            fills the params;
    //   * include/WaterShoreCommon.glsl        — samples it, and applies the wave
    //                                            transform in the vertex/tess chain;
    //   * WaterSurface.cpp                     — the CPU mirror buoyancy floats on;
    //   * WaterShoreWaveTest.cpp               — pins every relation below.
    //
    // ---- 1. WHERE THE DEPTH COMES FROM --------------------------------------
    //
    // The seabed is the terrain the scene already has. Drift's islands are six
    // TerrainComponents whose height fields are generated with an island-falloff
    // mask that drives every tile border to a base height 24 m UNDER the water
    // plane (Drift.olo, ShapingIslandFalloff) — so the terrain IS the sea floor
    // for the whole approach to every island, all the way out to where the tile
    // ends. Nothing had to be authored for this; the shoreline is already
    // "wherever the masked field crosses y = 0".
    //
    // The alternative the issue names — a per-island authored depth texture —
    // was rejected for a reason worth recording, because it looks like the safer
    // option: an authored texture is a SECOND source of truth for a shape that
    // already exists, and the two would drift apart silently the first time
    // anyone moved an island or re-rolled a procedural seed. The waves would
    // then shoal against a coastline that is not the one drawn.
    //
    // What the terrain route costs, measured rather than assumed, is one bake:
    // see WaterShoreDepthSystem. It is NOT a per-vertex sample of six separate
    // height maps — that would need six transforms and six samplers in the
    // vertex stage, and would still have nothing to say about the water between
    // the islands. It is one square R/G/B field resampled from all of them.
    //
    // ---- 2. THE LATTICE -----------------------------------------------------
    //
    // A square window of `kResolution^2` texels covering the WATER SURFACE's own
    // world-XZ extent — the tile the WaterComponent already declares
    // (m_WorldSizeX/Z about its transform). Not the camera, not a terrain tile:
    // the sea floor does not move, so neither does the field, and no scrolling
    // window (and none of WaterDisturbanceField's toroidal machinery) is needed.
    //
    //     uv = (worldXZ - centreXZ) * invExtent + 0.5
    //
    // outside [0,1] the sample reports `kDeepSentinelMetres`, which every
    // relation below reduces to exactly its deep-water form at. That is what
    // makes "open ocean is byte-identical to before" a structural property
    // rather than a tolerance.
    //
    // Storage is RGBA16F: r = depth in metres, gb = d(depth)/d(worldXZ), a
    // reserved. The gradient is BAKED rather than finite-differenced in the
    // shader — three extra taps per vertex would cost more, and a central
    // difference taken over the baked field is smoother than one taken over its
    // bilinear reconstruction, which matters because the gradient is what
    // decides which way a wave turns. A wrong gradient does not look noisy; it
    // aims the whole wave train at the wrong heading.
    //
    // ---- 3. WHAT THE DEPTH DOES TO A WAVE -----------------------------------
    //
    // One quantity is conserved as a wave train runs into shallow water: its
    // angular frequency w. Everything else follows from the dispersion relation
    //
    //     w^2 = g k tanh(k h)
    //
    // solved for the LOCAL wavenumber k at depth h (LocalWavenumber below).
    // As h falls, k rises: the crests bunch up (shoaling) and the phase speed
    // c = w/k falls. Because w is what is held fixed and not k, the time term of
    // every wave's phase is unchanged by depth — which is why this cannot
    // shimmer or beat when the camera moves over a slope. It is the SPATIAL
    // term that compresses.
    //
    // Amplitude follows from energy flux conservation: a * c_g is constant along
    // a ray, so a grows as the group velocity falls (ShoalingCoefficient), and
    // the ray tube narrows or widens as the ray turns (RefractionCoefficient).
    //
    // Direction follows from Snell's law with c in place of the refractive
    // index: sin(theta) / c is constant, so a wave crossing into slower water
    // turns TOWARD the shore normal. That is the whole reason waves arrive
    // roughly parallel to a beach whatever their offshore heading.
    //
    // This is the WKB / ray-theory approximation, evaluated pointwise instead of
    // by integrating along a ray. Its known limitation is stated once here so it
    // is not rediscovered as a bug: the crest POSITIONS drift from a true
    // eikonal solution by the accumulated phase along the path, so this gets the
    // spacing, the heading, the height and the breaking right, and does not
    // claim to place an individual crest where a wave tank would. Nothing in the
    // shot depends on that, and the alternative is a per-direction path integral
    // baked per octave.
    //
    // ---- 4. BREAKING IS THE JACOBIAN, NOT A SECOND SIGNAL -------------------
    //
    // The engine already detects a folded surface for FFT foam: the Jacobian of
    // the horizontal displacement map, negative where the surface has folded
    // over itself. The Gerstner sum has the SAME quantity available analytically
    // and, as it happens, already accumulated — see WaterCommon.glsl, where the
    // tangent/binormal terms gerstnerWaveNormal() sums are exactly the
    // off-identity entries of that Jacobian. So breaking here reads the same
    // signal the FFT path reads, computed rather than sampled, and no second
    // steepness heuristic was invented.
    //
    // What the depth adds is the LIMIT: a wave cannot grow past roughly
    // kBreakerIndex * h in amplitude — it breaks instead. Clamping the shoaled
    // amplitude there is what makes the surf zone decay toward the waterline
    // rather than growing without bound into a wall of water, and the clamped-off
    // excess is what says how hard it is breaking.
    // =========================================================================

    /// Standard gravity. The same 9.81 WaterCommon.glsl's gerstnerWave() uses —
    /// a different constant here would put the CPU and GPU surfaces at different
    /// heights, which is the failure WaterSurface.cpp exists to prevent.
    inline constexpr f32 kGravity = 9.81f;

    /// Texels per axis of the baked field. 512 over Drift's 1600 m sea is 3.1 m
    /// per texel, against wavelengths of 18-27 m: a wave responds to depth
    /// averaged over something like its own length, so resolving the depth far
    /// finer than that buys nothing and costs the bake quadratically.
    inline constexpr u32 kResolution = 512;

    /// Depth reported outside the field window, and the ceiling every sampled
    /// depth is clamped to.
    ///
    /// The number is chosen so tanh(k h) is EXACTLY 1.0f here, not merely close:
    /// that is what makes every relation below return its deep-water value
    /// bitwise, and it is the whole basis of "the open ocean is unchanged". In
    /// f32 that needs k*h >= about 9, i.e. a depth of ~1.43 wavelengths — so
    /// 1000 m covers every wavelength up to ~700 m, comfortably past the 500 m
    /// ceiling WaterComponent clamps m_Wavelength0/1 to.
    ///
    /// It is not "a big number": at 200 m, a 500 m wave still gives k*h = 2.5,
    /// where tanh is 0.987 — the deep ocean would have picked up a slow drift
    /// with nothing visibly wrong. WaterShoreWaveTest pins this.
    inline constexpr f32 kDeepSentinelMetres = 1000.0f;

    /// Below this the column is treated as dry land rather than as water. The
    /// dispersion relation has no solution at h = 0 (k -> infinity), and the
    /// surface is not drawn there anyway.
    inline constexpr f32 kMinDepthMetres = 0.05f;

    /// Depth-limited breaking. A solitary wave breaks at a height of about
    /// 0.78 h (McCowan); Gerstner amplitude is half the crest-to-trough height,
    /// so the amplitude limit is half of that.
    inline constexpr f32 kBreakerIndex = 0.39f;

    /// Hard ceiling on a single octave's steepness (Q = a k). A Gerstner wave
    /// self-intersects at Q = 1 regardless of depth; stopping just short keeps
    /// the surface a function even where the breaker limit above has not bitten.
    inline constexpr f32 kMaxSteepness = 0.9f;

    /// Fixed-point iterations used to invert the dispersion relation. Four is
    /// not a tuning knob: it is a mirrored constant, and the CPU and GLSL sides
    /// must run the SAME count or the two surfaces differ by the residual.
    inline constexpr i32 kDispersionIterations = 4;

    /// What the surface knows about the seabed under one point — the CPU twin
    /// of WaterShoreCommon.glsl's `WaterShoreSample`.
    struct Sample
    {
        f32 Depth = kDeepSentinelMetres; ///< metres of water
        glm::vec2 Gradient{ 0.0f };      ///< d(depth)/d(worldXZ), toward DEEPER water
        bool Enabled = false;            ///< false disables every transform below
    };

    /// The disabled sample, spelled once. Every relation below returns its
    /// deep-water form at this, which is what makes "no field means the
    /// pre-#1033 surface" structural rather than a tolerance.
    [[nodiscard]] inline Sample DisabledSample()
    {
        return {};
    }

    /// Local wavenumber k solving `w^2 = g k tanh(k h)`, given the DEEP-water
    /// wavenumber k0 (from which w^2 = g k0) and the depth h.
    ///
    /// Solved in the dimensionless form `(kh) tanh(kh) = x`, x = k0 h, by NEWTON
    /// from an Eckart seed. Both halves of that are load-bearing and the obvious
    /// simpler scheme is wrong:
    ///
    ///   * the natural-looking fixed point `k <- k0 / tanh(k h)` does NOT
    ///     converge in shallow water. There tanh(kh) -> kh, so the map is
    ///     k -> k0/(k h), whose derivative at the root is exactly -1: it
    ///     oscillates about the answer forever instead of closing on it. A fixed
    ///     iteration count then returns whichever side of the root it happened to
    ///     land on — which is a wave with the wrong length, in exactly the water
    ///     this whole feature is about. (WaterShoreWaveTest asserts the RESIDUAL
    ///     rather than the iteration for this reason: a non-converging scheme
    ///     still returns a plausible-looking number.)
    ///   * Newton alone needs a seed that is already close, or it can step past
    ///     zero. Eckart's `kh = x / sqrt(tanh x)` is within about 5% over the
    ///     whole range and is exact in both limits, which makes four Newton steps
    ///     machine precision everywhere.
    ///
    /// In deep water tanh(x) is exactly 1 in f32, the seed is exactly x, Newton's
    /// residual is exactly zero, and this returns k0 bitwise.
    [[nodiscard]] inline f32 LocalWavenumber(f32 k0, f32 depth)
    {
        const f32 h = glm::max(depth, kMinDepthMetres);
        const f32 x = glm::max(k0 * h, 1e-8f);

        f32 kh = x / glm::max(std::sqrt(std::tanh(x)), 1e-6f); // Eckart seed
        for (i32 i = 0; i < kDispersionIterations; ++i)
        {
            const f32 t = std::tanh(kh);
            const f32 f = kh * t - x;
            const f32 df = t + kh * (1.0f - t * t); // d/d(kh) of kh tanh(kh)
            kh -= f / glm::max(df, 1e-6f);
        }
        return glm::max(kh, 1e-6f) / h;
    }

    /// The `n` of `c_g = n c` — the ratio of group to phase velocity at depth.
    /// 0.5 in deep water, rising to 1 in shallow water.
    [[nodiscard]] inline f32 GroupVelocityRatio(f32 k, f32 depth)
    {
        const f32 kh = k * glm::max(depth, kMinDepthMetres);
        // sinh(2kh) overflows f16/f32 long before the term stops mattering; at
        // 2kh = 20 the correction is 4e-8, so the deep value is exact there.
        if (kh > 10.0f)
            return 0.5f;
        const f32 twoKh = 2.0f * kh;
        return 0.5f * (1.0f + twoKh / std::sinh(twoKh));
    }

    /// Green's-law shoaling coefficient: the amplitude gain from energy flux
    /// a * c_g being conserved along a ray. 1 in deep water.
    [[nodiscard]] inline f32 ShoalingCoefficient(f32 k0, f32 k, f32 depth)
    {
        // c_g0 = 0.5 * c0 = 0.5 * w/k0 ; c_g = n * w/k. w cancels.
        const f32 cg0 = 0.5f / glm::max(k0, 1e-6f);
        const f32 cg = GroupVelocityRatio(k, depth) / glm::max(k, 1e-6f);
        return std::sqrt(glm::max(cg0 / glm::max(cg, 1e-9f), 0.0f));
    }

    /// The outcome of refracting one wave train at one point.
    struct Refraction
    {
        glm::vec2 Direction{ 1.0f, 0.0f }; ///< unit heading after turning toward the shore
        f32 Coefficient = 1.0f;            ///< amplitude gain from the ray tube narrowing/widening
    };

    /// Snell's law for water waves. `deepDir` is the offshore (unit) heading,
    /// `offshoreNormal` the unit direction of increasing depth (the normalised
    /// depth gradient), and `speedRatio` = c / c0 at this depth (<= 1).
    ///
    /// A zero-length `offshoreNormal` means the seabed is flat here and there is
    /// nothing to refract against; the wave passes through unturned.
    [[nodiscard]] inline Refraction Refract(glm::vec2 deepDir, glm::vec2 offshoreNormal, f32 speedRatio)
    {
        Refraction result{ deepDir, 1.0f };
        const f32 nLen2 = glm::dot(offshoreNormal, offshoreNormal);
        if (!(nLen2 > 1e-8f))
            return result;

        const glm::vec2 s = offshoreNormal / std::sqrt(nLen2); // toward deeper water
        const glm::vec2 t{ -s.y, s.x };                        // along the depth contour

        // Incidence measured from the shore normal, in DEEP water. cosDeep keeps
        // its sign so a wave running offshore stays offshore.
        const f32 sinDeep = glm::dot(deepDir, t);
        const f32 cosDeep = glm::dot(deepDir, s);

        // sin(theta) / c is conserved, and c <= c0, so |sinLocal| <= |sinDeep|:
        // the ray always turns TOWARD the normal, never away.
        const f32 sinLocal = glm::clamp(sinDeep * speedRatio, -1.0f, 1.0f);
        const f32 cosMag = std::sqrt(glm::max(1.0f - sinLocal * sinLocal, 0.0f));
        const f32 cosLocal = (cosDeep < 0.0f) ? -cosMag : cosMag;

        result.Direction = t * sinLocal + s * cosLocal;
        // Kr = sqrt(cos(theta0) / cos(theta)) — the ray tube widens as the ray
        // straightens, so this is <= 1 and offsets part of the shoaling gain.
        result.Coefficient = std::sqrt(glm::max(std::abs(cosDeep), 0.0f) / glm::max(cosMag, 1e-3f));
        return result;
    }

    /// One octave's deep-water description, transformed for the local depth.
    struct Octave
    {
        glm::vec2 Direction{ 1.0f, 0.0f }; ///< heading after refraction
        f32 Wavelength = 10.0f;            ///< 2*pi/k at this depth (shorter than deep)
        f32 Steepness = 0.5f;              ///< Q = a k after shoaling, refraction and the breaker clamp
        f32 PhaseSpeed = 1.0f;             ///< c = w/k. NOT sqrt(g/k) once the depth bites.
        f32 Breaking = 0.0f;               ///< 0 offshore; the fraction of amplitude the breaker limit removed
    };

    /// Transform one deep-water octave for the depth under it.
    ///
    /// `amplitudeScale` is everything the CALLER will multiply this octave's
    /// displacement by afterwards (WaveAmplitude times the octave weight times
    /// the mesh band-limit weight). It has to be passed in rather than applied
    /// afterwards because the breaker limit is a statement about METRES of water
    /// — `a <= breakerIndex * h` — and applying it to an amplitude 15x larger
    /// than the one actually rendered puts the surf zone 15x too far offshore.
    /// The returned Steepness is still the PRE-scale value, so call sites keep
    /// multiplying by the same factor they always did.
    ///
    /// `breakerIndex` is the a/h limit; kBreakerIndex is the physical value.
    ///
    /// `depth` <= 0 (dry land) is clamped to kMinDepthMetres; at or beyond
    /// kDeepSentinelMetres this returns the inputs unchanged with PhaseSpeed =
    /// sqrt(g/k0), which is exactly what gerstnerWave() computes internally —
    /// so a scene with no shore field renders the pre-#1033 surface bitwise.
    [[nodiscard]] inline Octave TransformOctave(glm::vec2 deepDir, f32 deepWavelength, f32 deepSteepness,
                                                f32 amplitudeScale, f32 depth, glm::vec2 depthGradient,
                                                f32 breakerIndex = kBreakerIndex)
    {
        constexpr f32 kTwoPi = 6.28318530718f;
        const f32 wl0 = glm::max(deepWavelength, 0.001f);
        const f32 k0 = kTwoPi / wl0;
        const f32 c0 = std::sqrt(kGravity / k0);

        Octave out{ deepDir, wl0, deepSteepness, c0, 0.0f };

        const f32 h = glm::clamp(depth, kMinDepthMetres, kDeepSentinelMetres);
        const f32 k = LocalWavenumber(k0, h);
        const f32 omega = std::sqrt(kGravity * k0);
        const f32 c = omega / glm::max(k, 1e-6f);

        out.Wavelength = kTwoPi / glm::max(k, 1e-6f);
        out.PhaseSpeed = c;

        const Refraction refr = Refract(deepDir, depthGradient, glm::min(c / c0, 1.0f));
        out.Direction = refr.Direction;

        // Amplitude gains, applied to the amplitude the surface will ACTUALLY
        // be displaced by (see the amplitudeScale note above).
        const f32 scale = glm::max(amplitudeScale, 1e-6f);
        const f32 a0 = (deepSteepness / k0) * scale;
        const f32 aGrown = a0 * ShoalingCoefficient(k0, k, h) * refr.Coefficient;

        // Depth-limited breaking. Past breakerIndex * h the wave cannot stand up
        // any further, so the excess is taken off the surface and reported as
        // the breaking strength instead. This is what makes the surf zone decay
        // toward the waterline rather than growing into a wall of water.
        const f32 aLimit = glm::max(breakerIndex, 1e-4f) * h;
        f32 aFinal = aGrown;
        if (aGrown > aLimit)
        {
            aFinal = aLimit;
            out.Breaking = glm::clamp((aGrown - aLimit) / glm::max(aGrown, 1e-6f), 0.0f, 1.0f);
        }

        // Back out of the caller's scale, then guard the Gerstner fold: a single
        // octave at Q >= 1 self-intersects whatever the depth says.
        f32 steepness = (aFinal / scale) * k;
        if (steepness * scale > kMaxSteepness)
        {
            steepness = kMaxSteepness / scale;
            out.Breaking = 1.0f;
        }
        out.Steepness = steepness;
        return out;
    }
} // namespace OloEngine::WaterShore
