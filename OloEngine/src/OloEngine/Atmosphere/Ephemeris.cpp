#include "OloEnginePCH.h"
#include "OloEngine/Atmosphere/Ephemeris.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kPi = glm::pi<f32>();
        constexpr f32 kTwoPi = 2.0f * glm::pi<f32>();
        constexpr f32 kDegToRad = glm::pi<f32>() / 180.0f;

        // Earth's axial tilt, the amplitude of the declination swing.
        constexpr f32 kAxialTiltDeg = 23.44f;

        [[nodiscard]] f32 WrapHours(f32 hours)
        {
            f32 wrapped = std::fmod(hours, 24.0f);
            if (wrapped < 0.0f)
                wrapped += 24.0f;
            return wrapped;
        }

        [[nodiscard]] i32 WrapDayOfYear(i32 day)
        {
            i32 wrapped = ((day - 1) % 365 + 365) % 365 + 1;
            return wrapped;
        }

        [[nodiscard]] f32 SmoothStep01(f32 edge0, f32 edge1, f32 x)
        {
            const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    } // namespace

    f32 Ephemeris::SolarDeclination(i32 dayOfYear)
    {
        const i32 day = WrapDayOfYear(dayOfYear);
        // Cooper (1969): delta = -23.44 deg * cos(2*pi * (N + 10) / 365).
        // N + 10 places the December solstice (min declination) near day 355.
        return -kAxialTiltDeg * kDegToRad *
               std::cos(kTwoPi * (static_cast<f32>(day) + 10.0f) / 365.0f);
    }

    glm::vec2 Ephemeris::ElevationAzimuth(f32 declinationRadians, f32 latitudeRadians,
                                          f32 hourAngleRadians)
    {
        // Standard spherical-astronomy transform (declination/hour angle ->
        // horizontal coordinates), e.g. Meeus, "Astronomical Algorithms" ch. 13.
        const f32 sinDecl = std::sin(declinationRadians);
        const f32 cosDecl = std::cos(declinationRadians);
        const f32 sinLat = std::sin(latitudeRadians);
        const f32 cosLat = std::cos(latitudeRadians);
        const f32 cosHour = std::cos(hourAngleRadians);

        const f32 sinElev = std::clamp(sinDecl * sinLat + cosDecl * cosLat * cosHour, -1.0f, 1.0f);
        const f32 elevation = std::asin(sinElev);

        // Azimuth measured from north (+Z), positive toward east (+X).
        // atan2 form avoids the acos quadrant ambiguity.
        const f32 azimuth = std::atan2(std::sin(hourAngleRadians) * cosDecl,
                                       cosHour * cosDecl * sinLat - sinDecl * cosLat);
        // Meeus' convention above measures from SOUTH; rotate so 0 = north and
        // morning sun (negative hour angle) sits in the east.
        f32 azimuthFromNorth = azimuth + kPi;
        if (azimuthFromNorth > kPi)
            azimuthFromNorth -= kTwoPi;

        return { elevation, azimuthFromNorth };
    }

    glm::vec3 Ephemeris::DirectionFromElevationAzimuth(f32 elevationRadians, f32 azimuthRadians,
                                                       f32 northOffsetRadians)
    {
        const f32 azimuth = azimuthRadians + northOffsetRadians;
        const f32 cosElev = std::cos(elevationRadians);
        // Azimuth 0 = +Z (north), pi/2 = +X (east) — matches the MCP
        // SunDirectionFromAngles convention this feature retires.
        return glm::normalize(glm::vec3(cosElev * std::sin(azimuth),
                                        std::sin(elevationRadians),
                                        cosElev * std::cos(azimuth)));
    }

    f32 Ephemeris::QuantizeTimeHours(f32 hours, f32 quantumGameMinutes)
    {
        const f32 wrapped = WrapHours(std::isfinite(hours) ? hours : 12.0f);
        const f32 quantum = (std::isfinite(quantumGameMinutes) && quantumGameMinutes > 0.0f)
                                ? quantumGameMinutes
                                : 5.0f;
        const f32 minutes = wrapped * 60.0f;
        const f32 quantized = std::floor(minutes / quantum) * quantum;
        return WrapHours(quantized / 60.0f);
    }

    SunMoonState Ephemeris::ComputeSunMoon(const EphemerisInputs& inputs)
    {
        // Sanitize: every field can arrive from YAML / scripts / network.
        const f32 hours = WrapHours(std::isfinite(inputs.TimeOfDayHours) ? inputs.TimeOfDayHours : 12.0f);
        const i32 day = WrapDayOfYear(inputs.DayOfYear);
        f32 latitudeDeg = std::isfinite(inputs.LatitudeDegrees) ? inputs.LatitudeDegrees : 48.0f;
        // Exact poles make azimuth degenerate; nudge inside.
        latitudeDeg = std::clamp(latitudeDeg, -89.9f, 89.9f);
        const f32 northOffset = (std::isfinite(inputs.NorthOffsetDegrees) ? inputs.NorthOffsetDegrees : 0.0f) * kDegToRad;
        f32 phase = std::isfinite(inputs.MoonPhase) ? inputs.MoonPhase : 0.5f;
        phase = phase - std::floor(phase); // wrap to [0,1)

        const f32 latitude = latitudeDeg * kDegToRad;

        SunMoonState state;

        // ── Sun ──
        const f32 sunDecl = SolarDeclination(day);
        const f32 sunHourAngle = (hours - 12.0f) * 15.0f * kDegToRad;
        const glm::vec2 sunEA = ElevationAzimuth(sunDecl, latitude, sunHourAngle);
        state.SunElevationRadians = sunEA.x;
        state.SunAzimuthRadians = sunEA.y;
        state.SunDirection = DirectionFromElevationAzimuth(sunEA.x, sunEA.y, northOffset);

        // ── Moon (approximation) ──
        // Hour angle lags the sun by the synodic phase (full moon = opposite:
        // rises at sunset). Declination borrows the sun's curve half a year
        // ahead at full phase, so a winter full moon rides high — the
        // qualitatively correct behaviour without a lunar orbital solution.
        const f32 moonHourAngle = sunHourAngle - phase * kTwoPi;
        const i32 moonDeclDay = WrapDayOfYear(day + static_cast<i32>(std::round(phase * 365.0f)));
        const f32 moonDecl = SolarDeclination(moonDeclDay);
        const glm::vec2 moonEA = ElevationAzimuth(moonDecl, latitude, moonHourAngle);
        state.MoonElevationRadians = moonEA.x;
        state.MoonAzimuthRadians = moonEA.y;
        state.MoonDirection = DirectionFromElevationAzimuth(moonEA.x, moonEA.y, northOffset);

        // Illuminated fraction of the disk: 0 at new, 1 at full.
        state.MoonIlluminatedFraction = 0.5f * (1.0f - std::cos(kTwoPi * phase));

        return state;
    }

    glm::vec3 Ephemeris::SunColorForElevation(f32 sunElevationRadians)
    {
        // Three-band ramp pinned by EphemerisMathTest: deep amber at/below the
        // horizon, golden through the first ~10 deg, near-white above ~25 deg.
        // Values chosen against reference golden-hour photography, not a
        // radiative-transfer model.
        constexpr glm::vec3 kHorizon = { 1.0f, 0.45f, 0.20f };
        constexpr glm::vec3 kGolden = { 1.0f, 0.80f, 0.55f };
        constexpr glm::vec3 kNoon = { 1.0f, 0.98f, 0.95f };

        const f32 elevDeg = sunElevationRadians / kDegToRad;
        const f32 lowBlend = SmoothStep01(0.0f, 10.0f, elevDeg);
        const f32 highBlend = SmoothStep01(10.0f, 25.0f, elevDeg);
        const glm::vec3 low = kHorizon + (kGolden - kHorizon) * lowBlend;
        return low + (kNoon - low) * highBlend;
    }

    f32 Ephemeris::SunIntensityFactor(f32 sunElevationRadians)
    {
        const f32 elevDeg = sunElevationRadians / kDegToRad;
        // Ramp on through sunrise (-3 deg → +6 deg), then grow gently with
        // altitude (crude air-mass attenuation shape).
        const f32 horizonRamp = SmoothStep01(-3.0f, 6.0f, elevDeg);
        const f32 altitude = std::clamp(std::sin(std::max(sunElevationRadians, 0.0f)), 0.0f, 1.0f);
        return horizonRamp * (0.35f + 0.65f * altitude);
    }

    f32 Ephemeris::NightBlend(f32 sunElevationRadians)
    {
        const f32 elevDeg = sunElevationRadians / kDegToRad;
        // 0 while the sun is >= +2 deg (Preetham's validity floor), 1 once it
        // drops below -10 deg (past civil twilight); smooth between.
        return 1.0f - SmoothStep01(-10.0f, 2.0f, elevDeg);
    }

    glm::vec3 Ephemeris::MoonColor()
    {
        return { 0.62f, 0.72f, 0.90f };
    }

    f32 Ephemeris::MoonIntensityFactor(f32 moonElevationRadians, f32 illuminatedFraction)
    {
        const f32 elevDeg = moonElevationRadians / kDegToRad;
        const f32 horizonRamp = SmoothStep01(-3.0f, 6.0f, elevDeg);
        return horizonRamp * std::clamp(illuminatedFraction, 0.0f, 1.0f);
    }
} // namespace OloEngine
