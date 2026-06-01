#include "OloEnginePCH.h"
#include "OloEngine/Renderer/WaterSurface.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>

// =============================================================================
// 1:1 CPU port of WaterCommon.glsl. Every helper below mirrors a GLSL function
// of the same name; the math must stay byte-for-byte equivalent (modulo float
// determinism) so a buoyant body floats exactly where the rendered wave crest
// is. GLSL `fract(x)` == `x - floor(x)`, `mix(a,b,t)` == `a*(1-t)+b*t`.
// =============================================================================

namespace OloEngine::WaterSurface
{
    namespace
    {
        constexpr f32 kPi = 3.14159265f;
        constexpr f32 kTwoPi = 2.0f * kPi;
        constexpr f32 kGravity = 9.81f;        // dispersion relation constant (matches gerstnerWave)
        constexpr f32 kGoldenAngle = 2.39996f; // pi * (3 - sqrt(5))

        [[nodiscard]] f32 Fract(f32 x)
        {
            return x - std::floor(x);
        }
        [[nodiscard]] glm::vec2 Fract(glm::vec2 v)
        {
            return v - glm::floor(v);
        }
        [[nodiscard]] glm::vec3 Fract(glm::vec3 v)
        {
            return v - glm::floor(v);
        }

        // WaterCommon.glsl :: waveHash
        [[nodiscard]] f32 WaveHash(glm::vec2 p)
        {
            glm::vec3 p3 = Fract(glm::vec3(p.x, p.y, p.x) * 0.1031f);
            p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
            return Fract((p3.x + p3.y) * p3.z);
        }

        // WaterCommon.glsl :: waveNoise (value noise w/ smoothstep interpolation)
        [[nodiscard]] f32 WaveNoise(glm::vec2 p)
        {
            const glm::vec2 i = glm::floor(p);
            glm::vec2 f = Fract(p);
            f = f * f * (3.0f - 2.0f * f);
            const f32 a = WaveHash(i);
            const f32 b = WaveHash(i + glm::vec2(1.0f, 0.0f));
            const f32 c = WaveHash(i + glm::vec2(0.0f, 1.0f));
            const f32 d = WaveHash(i + glm::vec2(1.0f, 1.0f));
            return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
        }

        // WaterCommon.glsl :: domainWarp
        [[nodiscard]] glm::vec2 DomainWarp(glm::vec2 pos, f32 seed, f32 wavelength)
        {
            const f32 scale = 0.04f / glm::max(wavelength, 0.1f);
            const f32 warpAmount = wavelength * 0.3f;
            const f32 nx = WaveNoise(pos * scale + glm::vec2(seed, seed * 1.7f));
            const f32 ny = WaveNoise(pos * scale + glm::vec2(seed * 2.3f, seed * 0.5f));
            return (glm::vec2(nx, ny) - 0.5f) * warpAmount;
        }

        // WaterCommon.glsl :: gerstnerWave (returns the displacement vector)
        [[nodiscard]] glm::vec3 GerstnerWave(glm::vec3 position, glm::vec2 direction,
                                             f32 steepness, f32 wavelength, f32 time, f32 phase)
        {
            const f32 k = kTwoPi / glm::max(wavelength, 0.001f);
            const f32 c = std::sqrt(kGravity / k); // phase speed from the deep-water dispersion relation
            const f32 d = glm::dot(direction, glm::vec2(position.x, position.z));
            const f32 f = k * (d - c * time) + phase;
            const f32 a = steepness / k; // amplitude derived from steepness
            return glm::vec3(direction.x * a * std::cos(f),
                             a * std::sin(f),
                             direction.y * a * std::cos(f));
        }

        [[nodiscard]] glm::vec2 DirFromAngle(f32 angle)
        {
            return glm::normalize(glm::vec2(std::cos(angle), std::sin(angle)));
        }

        // Per-octave constants — must equal the inline literals in
        // WaterCommon.glsl :: sumGerstnerWaves (octaves 0..5).
        struct Octave
        {
            f32 angleMul, wlMul, stMul, phase, timeMul, ampMul, seed;
        };
        constexpr std::array<Octave, 6> kOctaves = { {
            { 1.0f, 0.85f, 0.50f, kPi * 1.7231f, 1.03f, 0.50f, 1.0f },
            { 2.0f, 0.60f, 0.45f, kPi * 3.4519f, 0.97f, 0.40f, 2.7f },
            { 3.0f, 0.40f, 0.38f, kPi * 0.8637f, 1.11f, 0.30f, 4.1f },
            { 4.0f, 0.25f, 0.30f, kPi * 5.1043f, 0.89f, 0.22f, 5.9f },
            { 5.0f, 0.15f, 0.22f, kPi * 2.6891f, 1.23f, 0.15f, 7.3f },
            { 6.0f, 0.09f, 0.15f, kPi * 4.3127f, 1.07f, 0.10f, 9.1f },
        } };

        // WaterCommon.glsl :: sumGerstnerWaves — returns only the displacement
        // delta (the shader also emits a surface normal we don't need here).
        [[nodiscard]] glm::vec3 SumGerstnerDisplacement(glm::vec3 position, f32 time,
                                                        glm::vec4 waveDir0, glm::vec4 waveDir1,
                                                        f32 waveFrequency, f32 waveAmplitude)
        {
            glm::vec3 displaced = position;

            const glm::vec2 dir0 = glm::normalize(glm::vec2(waveDir0.x, waveDir0.y) + glm::vec2(0.0001f));
            const glm::vec2 dir1 = glm::normalize(glm::vec2(waveDir1.x, waveDir1.y) + glm::vec2(0.0001f));

            const f32 wl0 = glm::max(waveDir0.w, 0.1f) / glm::max(waveFrequency, 0.01f);
            const f32 wl1 = glm::max(waveDir1.w, 0.1f) / glm::max(waveFrequency, 0.01f);

            // --- Primary waves (artist-controlled, 0.55 weight each) ---
            displaced += GerstnerWave(position, dir0, waveDir0.z, wl0, time, 0.0f) * waveAmplitude * 0.55f;
            displaced += GerstnerWave(position, dir1, waveDir1.z, wl1, time, 0.0f) * waveAmplitude * 0.55f;

            // --- Procedural detail octaves ---
            const f32 avgWL = (wl0 + wl1) * 0.5f;
            const f32 avgSteepness = (waveDir0.z + waveDir1.z) * 0.5f;
            const f32 baseAngle = std::atan2(dir0.y, dir0.x);

            for (const auto& o : kOctaves)
            {
                const f32 angle = baseAngle + kGoldenAngle * o.angleMul;
                const glm::vec2 d = DirFromAngle(angle);
                const f32 wl = avgWL * o.wlMul;
                const f32 st = avgSteepness * o.stMul;
                glm::vec3 warpedPos = position;
                const glm::vec2 warp = DomainWarp(glm::vec2(position.x, position.z), o.seed, wl);
                warpedPos.x += warp.x;
                warpedPos.z += warp.y;
                displaced += GerstnerWave(warpedPos, d, st, wl, time * o.timeMul, o.phase) * waveAmplitude * o.ampMul;
            }

            return displaced - position;
        }
    } // namespace

    glm::vec3 SampleDisplacement(const Params& params, glm::vec2 baseXZ, f32 rawTime)
    {
        // The water vertex shader evaluates `time = u_WaveParams.x * u_WaveParams.y`
        // (= Time * WaveSpeed) before calling sumGerstnerWaves — match it exactly.
        const f32 time = rawTime * params.m_WaveSpeed;
        const glm::vec3 basePos(baseXZ.x, params.m_PlaneHeight, baseXZ.y);
        return SumGerstnerDisplacement(basePos, time, params.m_WaveDir0, params.m_WaveDir1,
                                       params.m_WaveFrequency, params.m_WaveAmplitude);
    }

    f32 SampleHeight(const Params& params, glm::vec2 queryXZ, f32 rawTime)
    {
        // A Gerstner base point B maps to surface point B + horizontalDisp(B).
        // To read the height of the column *above* queryXZ we solve for the base
        // point that lands there, with a short fixed-point iteration (the shift is
        // small relative to wavelength, so a handful of steps converge fast).
        glm::vec2 base = queryXZ;
        for (int iter = 0; iter < 3; ++iter)
        {
            const glm::vec3 disp = SampleDisplacement(params, base, rawTime);
            const glm::vec2 mapped = base + glm::vec2(disp.x, disp.z);
            const glm::vec2 err = queryXZ - mapped;
            if (!std::isfinite(err.x) || !std::isfinite(err.y))
                return params.m_PlaneHeight; // fail-safe: physics must never see a NaN height
            base += err;
        }

        const f32 height = params.m_PlaneHeight + SampleDisplacement(params, base, rawTime).y;
        return std::isfinite(height) ? height : params.m_PlaneHeight;
    }
} // namespace OloEngine::WaterSurface
