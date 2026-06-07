#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // CPU mirror of the underwater refraction-distortion (§7.2 bullet 2) and
    // caustics (§7.1) math in PostProcess_ToneMap.glsl. Both sides MUST stay in
    // sync so the WaterRendering contract tests can pin the shader path against
    // this header — the same arrangement UnderwaterFog.h has for the fog falloff.
    //
    // Everything here is analytic (sums of sines, abs, integer powers) precisely
    // so CPU and GLSL produce the same value for the same inputs; no hashing /
    // fract(sin(...)) noise that would drift between the two float pipelines.
    namespace UnderwaterCaustics
    {
        // Animated screen-space UV offset for the submerged refraction wobble.
        // Two phase-shifted trig layers scrolled by time displace the scene-colour
        // sample (result is in UV units). The amplitude is hard-capped so a bad
        // param can never tear the image apart. Returns (0,0) when disabled.
        // MUST match `underwaterRefractionOffset` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline glm::vec2 RefractionOffset(glm::vec2 uv, f32 time, f32 strength,
                                                        f32 scale, f32 speed) noexcept
        {
            if (!std::isfinite(strength) || strength <= 0.0f)
                return glm::vec2(0.0f);
            if (!std::isfinite(time))
                time = 0.0f;
            if (!std::isfinite(scale))
                scale = 0.0f;
            if (!std::isfinite(speed))
                speed = 0.0f;
            const f32 s = std::clamp(strength, 0.0f, 0.1f); // hard cap (UV units)
            const f32 phase = time * speed;
            const f32 ox = std::sin(uv.y * scale + phase);
            const f32 oy = std::cos(uv.x * scale + phase * 0.9f + 1.3f);
            return glm::vec2(ox, oy) * s;
        }

        // One octave of the caustic web: a thin wavy ridge that's bright along the
        // zero-contour of a pair of drifting sines, sharpened with a cube so the
        // line stays narrow. MUST match `underwaterCausticOctave` in the shader.
        [[nodiscard]] inline f32 CausticOctave(f32 x, f32 y, f32 t) noexcept
        {
            const f32 s = (std::sin(x + t) + std::sin(y * 1.2f - t * 0.8f)) * 0.5f; // in [-1,1]
            f32 r = 1.0f - std::abs(s);                                             // bright where s ~ 0
            if (r < 0.0f)
                r = 0.0f;
            return r * r * r; // sharpen into a thin line
        }

        // Procedural caustic pattern in [0,1] sampled at a world-space XZ point.
        // Two octaves of wavy ridge lines (the finer one offset + counter-drifting)
        // unioned with max() into a caustic web — sparser, higher-contrast filaments
        // than a single broad field. Fully analytic so CPU and GLSL agree.
        // MUST match `underwaterCausticPattern` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline f32 CausticPattern(glm::vec2 worldXZ, f32 time, f32 scale, f32 speed) noexcept
        {
            if (!std::isfinite(worldXZ.x) || !std::isfinite(worldXZ.y))
                return 0.0f;
            if (!std::isfinite(time))
                time = 0.0f;
            if (!std::isfinite(scale) || scale <= 0.0f)
                scale = 1.0f;
            if (!std::isfinite(speed))
                speed = 0.0f;
            const glm::vec2 q = worldXZ * scale;
            const f32 t = time * speed;
            const f32 o1 = CausticOctave(q.x, q.y, t);
            const f32 o2 = CausticOctave(q.x * 1.7f + 5.0f, q.y * 1.7f - 3.0f, -t * 1.1f);
            return std::max(o1, o2);
        }

        // Depth fade: caustics are strongest at the surface and vanish by
        // maxDepth. Returns 0 at/above the surface (depthBelowSurface <= 0).
        // MUST match `underwaterCausticDepthFade` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline f32 CausticDepthFade(f32 depthBelowSurface, f32 maxDepth) noexcept
        {
            if (!std::isfinite(depthBelowSurface) || depthBelowSurface <= 0.0f)
                return 0.0f;
            if (!std::isfinite(maxDepth) || maxDepth <= 0.0f)
                return 0.0f;
            const f32 f = 1.0f - depthBelowSurface / maxDepth;
            return std::clamp(f, 0.0f, 1.0f);
        }
    } // namespace UnderwaterCaustics
} // namespace OloEngine
