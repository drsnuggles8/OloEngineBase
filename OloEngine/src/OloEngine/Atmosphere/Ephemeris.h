#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Inputs for the simplified sun/moon ephemeris (issue #633, Pillar B).
    //
    // Accuracy target: plausible, deterministic, headless-unit-tested motion —
    // NOT an almanac. The sun follows the standard declination/hour-angle
    // spherical model (good to ~1°); the moon is an approximation that gets
    // the gameplay-visible behaviour right (full moon rises near sunset and
    // rides the opposite season's arc; new moon hugs the sun) without a true
    // lunar orbital solution.
    struct EphemerisInputs
    {
        f32 TimeOfDayHours = 12.0f;    // [0,24) local solar time
        i32 DayOfYear = 172;           // [1,365]
        f32 LatitudeDegrees = 48.0f;   // [-90,90]
        f32 NorthOffsetDegrees = 0.0f; // rotates the frame about +Y (0 = north along +Z)
        f32 MoonPhase = 0.5f;          // synodic fraction: 0 = new, 0.5 = full, 1 = new
    };

    // World-space convention: +Y is up; azimuth is measured from +Z (north)
    // toward +X (east) — the same mapping the MCP sun tools used. Directions
    // point FROM the world TOWARD the body (a "toward sun" vector, i.e. the
    // negation of the light's travel direction).
    struct SunMoonState
    {
        glm::vec3 SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 MoonDirection = glm::vec3(0.0f, -1.0f, 0.0f);
        f32 SunElevationRadians = 0.0f;
        f32 SunAzimuthRadians = 0.0f;
        f32 MoonElevationRadians = 0.0f;
        f32 MoonAzimuthRadians = 0.0f;
        f32 MoonIlluminatedFraction = 0.0f; // [0,1] from the synodic phase
    };

    // Pure math — no GPU, no Scene. Pinned by EphemerisMathTest.cpp the same
    // way ProceduralSkyMathTest pins the Preetham model.
    class Ephemeris
    {
      public:
        // Full state for one instant. Sanitizes non-finite inputs to defaults
        // and wraps/clamps ranges defensively (YAML/scripts are untrusted).
        [[nodiscard]] static SunMoonState ComputeSunMoon(const EphemerisInputs& inputs);

        // Solar declination in radians for a day of year (Cooper 1969):
        // delta = -23.44 deg * cos(2*pi * (N + 10) / 365).
        [[nodiscard]] static f32 SolarDeclination(i32 dayOfYear);

        // Elevation (output.x) and azimuth (output.y), both radians, of a body
        // with the given declination at the given local hour angle
        // (hourAngle = 0 at local solar noon, +15 deg/hour after) seen from
        // latitudeRadians. Azimuth from +Z (north) toward +X (east).
        [[nodiscard]] static glm::vec2 ElevationAzimuth(f32 declinationRadians,
                                                        f32 latitudeRadians,
                                                        f32 hourAngleRadians);

        // Unit "toward body" world vector from elevation/azimuth (+ the
        // authored north-offset yaw about +Y).
        [[nodiscard]] static glm::vec3 DirectionFromElevationAzimuth(f32 elevationRadians,
                                                                     f32 azimuthRadians,
                                                                     f32 northOffsetRadians);

        // Quantize a time-of-day to whole steps of `quantumGameMinutes` so the
        // sky-bake parameter hash only moves on those steps (the no-per-frame-
        // rebake acceptance criterion). Returns hours in [0, 24).
        [[nodiscard]] static f32 QuantizeTimeHours(f32 hours, f32 quantumGameMinutes);

        // ── Lighting-drive curves (day → golden hour → twilight → night) ──
        // All pure functions of sun elevation so the whole light curve is
        // unit-testable without a scene.

        // Linear-RGB sun colour: warm near the horizon, near-white when high.
        [[nodiscard]] static glm::vec3 SunColorForElevation(f32 sunElevationRadians);

        // [0,1] factor on the directional light's authored max intensity.
        // 0 a little below the horizon, smooth ramp through sunrise, ~1 high.
        [[nodiscard]] static f32 SunIntensityFactor(f32 sunElevationRadians);

        // 0 = full day, 1 = full night; smooth through civil twilight
        // (fades over sun elevation +2 deg → -10 deg). Drives the day↔night
        // sky cross-fade and the sun↔moon light swap.
        [[nodiscard]] static f32 NightBlend(f32 sunElevationRadians);

        // Cool moonlight tint (constant; the moon doesn't redden much in-game).
        [[nodiscard]] static glm::vec3 MoonColor();

        // [0,1] factor on the authored moon max intensity: illuminated
        // fraction x horizon ramp of the moon's own elevation.
        [[nodiscard]] static f32 MoonIntensityFactor(f32 moonElevationRadians,
                                                     f32 illuminatedFraction);
    };
} // namespace OloEngine
