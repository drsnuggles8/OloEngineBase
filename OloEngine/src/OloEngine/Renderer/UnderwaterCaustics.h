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

        // Volumetric light shafts ("god rays", §3.3). The shader marches from each
        // pixel toward the sun's screen position over `samples` steps and, at each
        // step, adds `decay^i` when the depth sample is open water toward the sun
        // (occlusion radial blur). It divides that accumulation by the sum of all
        // `decay^i` so the result is a BOUNDED [0,1] "openness" — sample-count
        // independent, which is what keeps the effect from blowing out. This
        // normaliser is that denominator: sum_{i=1..N} decay^i (a finite geometric
        // series). CPU and GLSL must compute it identically or the shaft brightness
        // scale drifts between the two pipelines, so the loop is run (not the closed
        // form). MUST match the `norm` accumulator in `underwaterGodRays`.
        [[nodiscard]] inline f32 GodRayDecaySum(i32 samples, f32 decay) noexcept
        {
            if (samples <= 0 || !std::isfinite(decay))
                return 0.0f;
            f32 illum = 1.0f;
            f32 sum = 0.0f;
            for (i32 i = 0; i < samples; ++i)
            {
                illum *= decay; // matches the shader: illum decays BEFORE the add
                sum += illum;
            }
            return sum;
        }

        // Broad animated wave pattern in [0,1] (sum of sines, ~half-duty) used to
        // dapple the god-ray shafts with the surface waves — deliberately smoother
        // than the sparse seabed caustic web so the modulation survives. Analytic
        // (sums of sines) so CPU and GLSL agree for the same inputs. MUST match
        // `underwaterGodRayDapple` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline f32 GodRayDapple(glm::vec2 worldXZ, f32 time, f32 scale, f32 speed) noexcept
        {
            if (!std::isfinite(worldXZ.x) || !std::isfinite(worldXZ.y))
                return 0.5f; // neutral (no dapple) on bad input
            if (!std::isfinite(time))
                time = 0.0f;
            if (!std::isfinite(scale))
                scale = 0.0f;
            if (!std::isfinite(speed))
                speed = 0.0f;
            const glm::vec2 q = worldXZ * scale;
            const f32 t = time * speed;
            const f32 a = std::sin(q.x + t) + std::sin(q.y * 1.3f - t * 0.8f) + std::sin((q.x + q.y) * 0.7f + t * 1.1f);
            return std::clamp(a * (1.0f / 3.0f) * 0.5f + 0.5f, 0.0f, 1.0f);
        }

        // Project a directional sun to its screen-space UV vanishing point using
        // the frame's view-projection. The sun sits opposite the light-travel
        // direction `sunDir`, infinitely far, so it projects as the point at
        // infinity `viewProjection * vec4(-sunDir, 0)`. Returns false (god rays
        // should be skipped) when the sun is behind the camera or the projection is
        // degenerate; otherwise writes the [0,1] screen UV to `outUV`. Scene.cpp
        // packs the same value into the UBO so the shader needs no extra matrix.
        [[nodiscard]] inline bool GodRaySunScreenUV(const glm::mat4& viewProjection, glm::vec3 sunDir,
                                                    glm::vec2& outUV) noexcept
        {
            if (!std::isfinite(sunDir.x) || !std::isfinite(sunDir.y) || !std::isfinite(sunDir.z))
                return false;
            if (glm::dot(sunDir, sunDir) < 1e-12f)
                return false;
            const glm::vec4 clip = viewProjection * glm::vec4(-sunDir, 0.0f);
            // w <= 0 means the sun is at/behind the camera plane: no shafts.
            if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w <= 1e-6f)
                return false;
            const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
            if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
                return false;
            outUV = ndc * 0.5f + 0.5f;
            return true;
        }
    } // namespace UnderwaterCaustics
} // namespace OloEngine
