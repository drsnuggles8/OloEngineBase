#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ProceduralSky.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class EnvironmentMap;

    // Inputs for the combined day/night atmosphere sky bake (issue #633,
    // Pillar B). One bake pass renders: the Preetham day sky, a procedural
    // starfield, the moon disk (+ glow), and the twilight cross-fade between
    // them — into ONE cubemap + IBL set, so PBR ambient / SSR / water
    // reflections track the time of day with no extra runtime blending.
    //
    // Driven by TimeOfDaySystem through Scene::LoadAndRenderSkybox: the sun
    // and moon directions arrive already QUANTIZED to the component's rebake
    // quantum (Ephemeris::QuantizeTimeHours) so HashParameters only moves on
    // those steps — never a per-frame rebake.
    struct AtmosphereSkyParameters
    {
        // Day half — the scene's ProceduralSkyComponent params with the
        // ephemeris' quantized toward-sun direction and the day↔night-lerped
        // exposure substituted in.
        PreethamParameters Day;

        // Night half.
        glm::vec3 MoonDirection = glm::vec3(0.0f, -1.0f, 0.0f); // toward moon (unit)
        f32 MoonDiskSize = 1.0f;                                // multiplier on the nominal lunar angular radius
        f32 MoonIlluminatedFraction = 0.5f;                     // [0,1] synodic illumination
        f32 MoonIntensity = 1.0f;                               // moon disk / glow brightness
        f32 StarIntensity = 1.0f;
        f32 NightBlendFactor = 0.0f;    // 0 = day, 1 = night (Ephemeris::NightBlend)
        f32 NightBrightness = 0.35f;    // scales the whole night layer (component m_SkyExposureNight)
        f32 StarRotationRadians = 0.0f; // sidereal-ish dome rotation (from time of day)

        // Bake-time cloud tint so IBL/reflections see the cloudscape
        // (CloudscapeComponent::m_AffectIBL). 0 coverage = off. Coverage
        // arrives bucketed (0.05 steps) so it hash-gates cleanly.
        f32 CloudCoverage = 0.0f;
        f32 CloudWetness = 0.0f;
    };

    // Packed std140 layout the AtmosphereSky.glsl bake shader consumes at
    // binding UBO_ATMOSPHERE_SKY. Day coefficients first (identical layout to
    // PreethamCoefficientsUBO), then the night/cloud lanes.
    struct AtmosphereSkyUBO
    {
        PreethamCoefficientsUBO Day; // 8 x vec4
        glm::vec4 MoonDirection;     // xyz = toward moon (unit), w = cos(angular radius * disk size)
        glm::vec4 NightParams;       // x = night blend, y = star intensity, z = moon intensity, w = night brightness
        glm::vec4 NightParams2;      // x = star rotation (rad), y = moon illuminated fraction,
                                     // z = cloud coverage, w = cloud wetness
    };
    static_assert(sizeof(AtmosphereSkyUBO) == 11 * 16,
                  "AtmosphereSkyUBO must remain std140-friendly (11x vec4)");

    class AtmosphereSky
    {
      public:
        // Pure math: day coefficients via ProceduralSky::ComputeCoefficients
        // plus sanitized night lanes. Pinned by AtmosphereSkyMathTest.cpp.
        [[nodiscard]] static AtmosphereSkyUBO ComputeUBO(const AtmosphereSkyParameters& params);

        // Bake into a fresh cubemap + IBL set (same shape as
        // ProceduralSky::Generate / StarNestSky::Generate; UseDiskCache off).
        [[nodiscard]] static Ref<EnvironmentMap> Generate(const AtmosphereSkyParameters& params,
                                                          u32 resolution = 256);

        // FNV-1a dirtiness hash over every parameter + resolution — the
        // caller feeds quantized directions/time, making this the rebake gate.
        [[nodiscard]] static u64 HashParameters(const AtmosphereSkyParameters& params, u32 resolution);

        // CPU mirror of the shader for headless tests (structurally
        // identical, not bit-exact — same contract as StarNestSky).
        [[nodiscard]] static glm::vec3 EvaluateAtDirection(const AtmosphereSkyUBO& ubo,
                                                           const glm::vec3& viewDir);

        // Nominal lunar angular radius — the moon's apparent half-angle is
        // almost exactly the sun's (total eclipses exist), so reuse the value.
        static constexpr f32 MoonNominalAngularRadius = ProceduralSky::SunNominalAngularRadius;
    };
} // namespace OloEngine
