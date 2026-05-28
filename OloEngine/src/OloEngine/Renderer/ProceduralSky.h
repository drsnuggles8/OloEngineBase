#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class EnvironmentMap;

    // Inputs for the Preetham (1999) analytic daylight model.
    //
    // Reference: A. J. Preetham, P. Shirley, B. Smits,
    // "A Practical Analytic Model for Daylight", SIGGRAPH 1999.
    //
    // Turbidity describes haze; 2 = very clear (mountain air), 3 = clear sky,
    // 6 = hazy, 10 = thick haze. Values outside [1.7, 10] are extrapolation
    // and the model degrades. The sun direction is the unit vector that
    // points *from* the world *toward* the sun (i.e. the direction of
    // incoming light, negated). Elevation below the horizon is clamped a
    // few degrees above horizontal to avoid singular Preetham values.
    struct PreethamParameters
    {
        glm::vec3 SunDirection = glm::vec3(0.0f, 1.0f, 0.0f); // Toward sun (world space, unit)
        f32 Turbidity = 2.5f;
        f32 Exposure = 0.1f;     // Rate in the luminance tonemap 1-exp(-Exposure*Y); higher = brighter sky
        f32 SunIntensity = 1.0f; // Disk brightness multiplier
        f32 SunDiskSize = 1.0f;  // Multiplier on the nominal solar angular radius (~0.265 deg)
        bool ShowSunDisk = true;
    };

    // Packed std140 layout that the ProceduralSky.glsl shader consumes.
    // Each glm::vec4 occupies 16 bytes; total = 7 * 16 = 112 bytes.
    // Validate any future field additions stay 16-byte aligned per std140.
    struct PreethamCoefficientsUBO
    {
        glm::vec4 SunDirection; // xyz = toward sun (unit), w = cos(angular radius * SunDiskSize)
        glm::vec4 ZenithXYY;    // x = chromaticity x, y = chromaticity y, z = luminance Y, w = unused
        // Perez F coefficients packed as (Fx, Fy, FY, _) per quantity.
        glm::vec4 A;
        glm::vec4 B;
        glm::vec4 C;
        glm::vec4 D;
        glm::vec4 E;
        glm::vec4 Params; // x = exposure, y = sun intensity, z = show sun (0/1), w = unused
    };
    static_assert(sizeof(PreethamCoefficientsUBO) == 8 * 16,
                  "PreethamCoefficientsUBO must remain std140-friendly (8x vec4)");

    class ProceduralSky
    {
      public:
        // Pure-math: compute the Preetham distribution coefficients and zenith
        // chromaticity/luminance for the given parameters. No GPU work — this
        // is the hot path for tests and for detecting parameter drift between
        // frames.  Pinned by ProceduralSkyMathTest.cpp.
        [[nodiscard]] static PreethamCoefficientsUBO ComputeCoefficients(const PreethamParameters& params);

        // Bake Preetham sky into a fresh cubemap and wrap it in an
        // EnvironmentMap with IBL textures generated. Requires the IBL
        // system to be initialised (EnvironmentMap::InitializeIBLSystem).
        // Falls back to nullptr if the shader library isn't yet up.
        //
        // Cost: one full Preetham coefficient compute + six face renders +
        // irradiance / prefilter / BRDF generation. Designed to be invoked
        // when sky parameters change, *not* every frame.
        [[nodiscard]] static Ref<EnvironmentMap> Generate(const PreethamParameters& params,
                                                          u32 resolution = 256);

        // Convert a sky parameter set to its serialisable hash so callers
        // can detect dirtiness without keeping a full copy of the previous
        // PreethamParameters around.  Cheap; intended for per-frame checks.
        [[nodiscard]] static u64 HashParameters(const PreethamParameters& params, u32 resolution);

        // Convenience: evaluate Preetham at one world direction on the CPU.
        // Returns linear RGB (the same colour-space the shader emits).
        // Used by tests to validate the analytic chain end-to-end without
        // standing up GPU resources.
        [[nodiscard]] static glm::vec3 EvaluateAtDirection(const PreethamCoefficientsUBO& coeffs,
                                                           const glm::vec3& viewDir);

        // Nominal solar angular radius (sun's apparent half-angle as seen
        // from Earth's surface).  Used by ComputeCoefficients to bake the
        // sun disk size into UBO.SunDirection.w.
        static constexpr f32 SunNominalAngularRadius = 0.00465f; // ~0.266 degrees
    };
} // namespace OloEngine
