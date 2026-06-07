#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class EnvironmentMap;

    // Authoring parameters for the "Star Nest" raymarched volumetric nebula.
    //
    // Reference: "Star Nest" by Pablo Roman Andrioli (Kali), MIT License,
    // https://www.shadertoy.com/view/XlfGRj. Defaults reproduce the original
    // shadertoy look (clear nebula with a soft star field).
    //
    // The field is a folded fractal marched in `VolSteps` slices, each iterated
    // `Iterations` times. `Offset` positions the virtual camera inside the
    // nebula (changing it gives a completely different cloud); the two rotations
    // spin the whole sky. The remaining values are the classic Star Nest tuning
    // constants. StarNestSky bakes this across a cubemap so reflective PBR
    // surfaces, reflection probes and SSR all reflect the nebula.
    struct StarNestParameters
    {
        glm::vec3 Offset = glm::vec3(1.0f, 0.5f, 0.5f); // Camera position in the nebula field
        f32 Rotation1 = 0.5f;                           // Whole-sky rotation in the xz plane (radians)
        f32 Rotation2 = 0.8f;                           // Whole-sky rotation in the xy plane (radians)

        f32 Formuparam = 0.53f;   // The fold constant in p = abs(p)/dot(p,p) - formuparam
        f32 StepSize = 0.1f;      // March step length
        f32 Tile = 0.85f;         // Tiling-fold half period
        f32 Brightness = 0.0015f; // Per-step coloring brightness
        f32 DarkMatter = 0.3f;    // Dark-matter occlusion strength
        f32 DistFading = 0.73f;   // Per-step distance fade
        f32 Saturation = 0.85f;   // 0 = greyscale, 1 = full colour
        f32 Intensity = 1.0f;     // Overall output multiplier (drives IBL/skybox brightness)

        i32 Iterations = 17; // Inner fractal iterations  [1, 40]
        i32 VolSteps = 20;   // Volumetric march steps    [1, 40]
    };

    // Packed std140 layout consumed by StarNestSky.glsl. 4 * vec4 = 64 bytes.
    // Keep field packing in sync with the UBO block in the shader.
    struct StarNestSkyUBO
    {
        glm::vec4 Offset;  // xyz = camera position in field, w = rotation1 (xz)
        glm::vec4 Params0; // x = formuparam, y = stepsize, z = tile, w = rotation2 (xy)
        glm::vec4 Params1; // x = brightness, y = darkmatter, z = distfading, w = saturation
        glm::vec4 Params2; // x = intensity, y = iterations (float), z = volsteps (float), w = unused
    };
    static_assert(sizeof(StarNestSkyUBO) == 4 * 16,
                  "StarNestSkyUBO must remain std140-friendly (4x vec4)");

    // Hard ceilings on the marched loops. Must match MAX_ITERATIONS /
    // MAX_VOLSTEPS in StarNestSky.glsl — the shader's loops have fixed
    // compile-time trip counts and break at the runtime values, so feeding a
    // larger count than this would silently truncate.
    inline constexpr i32 kStarNestMaxIterations = 40;
    inline constexpr i32 kStarNestMaxVolSteps = 40;

    class StarNestSky
    {
      public:
        // Pure-math: clamp/sanitise the authoring parameters and pack them into
        // the std140 UBO the shader consumes. No GPU work — the hot path for
        // tests and for detecting parameter drift. Pinned by StarNestSkyMathTest.
        [[nodiscard]] static StarNestSkyUBO ComputeUBO(const StarNestParameters& params);

        // Bake the Star Nest nebula into a fresh cubemap and wrap it in an
        // EnvironmentMap with IBL textures generated, reusing the shared sky
        // cubemap bake + the EnvironmentMap IBL pipeline. Requires the renderer
        // + shader library to be live; returns nullptr if the StarNestSky shader
        // isn't loaded or the bake fails.
        //
        // Cost: six face renders (each a full raymarch) + irradiance / prefilter
        // / BRDF generation. Invoke when parameters change, not every frame.
        [[nodiscard]] static Ref<EnvironmentMap> Generate(const StarNestParameters& params,
                                                          u32 resolution = 256);

        // FNV-1a hash of the parameter set + resolution for cheap per-frame
        // "did anything change since last bake" checks.
        [[nodiscard]] static u64 HashParameters(const StarNestParameters& params, u32 resolution);

        // Convenience CPU mirror of the GLSL raymarch at one world direction.
        // Returns linear RGB in the same space the shader emits. Used by tests
        // to pin the marching formula end-to-end without a GPU. Not bit-exact to
        // the shader (float order differs) but structurally identical.
        [[nodiscard]] static glm::vec3 EvaluateAtDirection(const StarNestSkyUBO& ubo,
                                                           const glm::vec3& viewDir);
    };
} // namespace OloEngine
