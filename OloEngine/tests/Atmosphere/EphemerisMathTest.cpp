// OLO_TEST_LAYER: L1
// =============================================================================
// EphemerisMathTest.cpp
//
// Pins the simplified sun/moon ephemeris (OloEngine/Atmosphere/Ephemeris.{h,cpp})
// that drives the time-of-day system (issue #633, Pillar B). Pure CPU math —
// no GPU context — mirroring how ProceduralSkyMathTest pins the Preetham
// model these directions ultimately feed.
//
// What gets pinned:
//   - Solar declination hits the solstice/equinox reference values (Cooper's
//     formula, ±0.6 deg — the formula itself is a ~1 deg approximation).
//   - Noon elevation = 90 - |latitude - declination| at the equinox.
//   - Elevation is monotonic through the morning; azimuth walks east → south
//     → west in the northern hemisphere (and faces north in the southern).
//   - Polar summer produces a midnight sun; mid-latitude midnight is night.
//   - The full moon opposes the sun (up at midnight, down at noon); the
//     synodic illumination curve is 0 at new, 1 at full.
//   - Time quantization is stable within a bucket and moves across buckets
//     (the sky-bake hash gate — the "no per-frame rebake" criterion).
//   - The lighting-drive curves (intensity ramp, night blend, sun colour)
//     are bounded, finite, and monotonic where the design says they are.
//   - Adversarial inputs (NaN/inf/huge) are sanitized to finite unit vectors.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Atmosphere/Ephemeris.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kDegToRad = glm::pi<f32>() / 180.0f;
        constexpr f32 kRadToDeg = 180.0f / glm::pi<f32>();

        EphemerisInputs MidLatitudeNoon()
        {
            EphemerisInputs in;
            in.TimeOfDayHours = 12.0f;
            in.DayOfYear = 80; // ~March equinox
            in.LatitudeDegrees = 48.0f;
            in.NorthOffsetDegrees = 0.0f;
            in.MoonPhase = 0.5f;
            return in;
        }

        void ExpectUnitFinite(const glm::vec3& v)
        {
            EXPECT_TRUE(std::isfinite(v.x));
            EXPECT_TRUE(std::isfinite(v.y));
            EXPECT_TRUE(std::isfinite(v.z));
            EXPECT_NEAR(glm::length(v), 1.0f, 1e-4f);
        }
    } // namespace

    // ---------- Solar declination (Cooper 1969) ----------

    TEST(EphemerisMath, DeclinationHitsSolsticesAndEquinox)
    {
        // June solstice (~day 172): +23.44 deg. December (~day 355): -23.44.
        // March equinox (~day 80): ~0. Formula accuracy ~1 deg → 0.6 deg
        // tolerance at the extremes, 1 deg at the zero crossing.
        EXPECT_NEAR(Ephemeris::SolarDeclination(172) * kRadToDeg, 23.44f, 0.6f);
        EXPECT_NEAR(Ephemeris::SolarDeclination(355) * kRadToDeg, -23.44f, 0.6f);
        EXPECT_NEAR(Ephemeris::SolarDeclination(80) * kRadToDeg, 0.0f, 1.0f);
    }

    TEST(EphemerisMath, DeclinationWrapsDayOfYear)
    {
        EXPECT_FLOAT_EQ(Ephemeris::SolarDeclination(1), Ephemeris::SolarDeclination(366));
        EXPECT_FLOAT_EQ(Ephemeris::SolarDeclination(365), Ephemeris::SolarDeclination(0));
    }

    // ---------- Solar position ----------

    TEST(EphemerisMath, NoonElevationMatchesLatitudeAtEquinox)
    {
        // At the equinox, noon elevation = 90 - latitude.
        const auto state = Ephemeris::ComputeSunMoon(MidLatitudeNoon());
        EXPECT_NEAR(state.SunElevationRadians * kRadToDeg, 90.0f - 48.0f, 1.5f);
        ExpectUnitFinite(state.SunDirection);
    }

    TEST(EphemerisMath, NoonSunIsDueSouthInNorthernHemisphere)
    {
        const auto state = Ephemeris::ComputeSunMoon(MidLatitudeNoon());
        // Azimuth convention: 0 = +Z (north), pi = south. Direction z < 0.
        EXPECT_NEAR(std::abs(state.SunAzimuthRadians), glm::pi<f32>(), 0.05f);
        EXPECT_LT(state.SunDirection.z, 0.0f);
        EXPECT_NEAR(state.SunDirection.x, 0.0f, 0.05f);
    }

    TEST(EphemerisMath, NoonSunIsDueNorthInSouthernHemisphere)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.LatitudeDegrees = -48.0f;
        const auto state = Ephemeris::ComputeSunMoon(in);
        EXPECT_NEAR(state.SunAzimuthRadians, 0.0f, 0.05f);
        EXPECT_GT(state.SunDirection.z, 0.0f);
    }

    TEST(EphemerisMath, ElevationIsMonotonicThroughTheMorning)
    {
        EphemerisInputs in = MidLatitudeNoon();
        f32 prev = -std::numeric_limits<f32>::infinity();
        for (f32 hour = 6.0f; hour <= 12.0f; hour += 0.5f)
        {
            in.TimeOfDayHours = hour;
            const auto state = Ephemeris::ComputeSunMoon(in);
            EXPECT_GT(state.SunElevationRadians, prev)
                << "elevation must rise through the morning (hour " << hour << ")";
            prev = state.SunElevationRadians;
        }
    }

    TEST(EphemerisMath, MorningSunEastAfternoonSunWest)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.TimeOfDayHours = 9.0f;
        const auto morning = Ephemeris::ComputeSunMoon(in);
        EXPECT_GT(morning.SunDirection.x, 0.0f) << "9am sun must be east (+X)";

        in.TimeOfDayHours = 15.0f;
        const auto afternoon = Ephemeris::ComputeSunMoon(in);
        EXPECT_LT(afternoon.SunDirection.x, 0.0f) << "3pm sun must be west (-X)";
    }

    TEST(EphemerisMath, MidnightIsNightAtMidLatitudeButMidnightSunAtThePole)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.TimeOfDayHours = 0.0f;
        in.DayOfYear = 172;
        EXPECT_LT(Ephemeris::ComputeSunMoon(in).SunElevationRadians, 0.0f);

        in.LatitudeDegrees = 80.0f; // inside the arctic circle at the solstice
        EXPECT_GT(Ephemeris::ComputeSunMoon(in).SunElevationRadians, 0.0f)
            << "polar summer must produce a midnight sun";
    }

    TEST(EphemerisMath, NorthOffsetRotatesAroundYOnly)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.TimeOfDayHours = 9.0f;
        const auto base = Ephemeris::ComputeSunMoon(in);

        in.NorthOffsetDegrees = 90.0f;
        const auto rotated = Ephemeris::ComputeSunMoon(in);

        // Elevation (y) unchanged; horizontal component rotated by 90 deg.
        EXPECT_NEAR(base.SunDirection.y, rotated.SunDirection.y, 1e-4f);
        // Rotating azimuth by +90 deg: x' = cos(el)sin(az+90) = cos(el)cos(az) = z,
        // z' = cos(el)cos(az+90) = -cos(el)sin(az) = -x.
        EXPECT_NEAR(rotated.SunDirection.x, base.SunDirection.z, 1e-3f);
        EXPECT_NEAR(rotated.SunDirection.z, -base.SunDirection.x, 1e-3f);
    }

    // ---------- Moon ----------

    TEST(EphemerisMath, FullMoonOpposesTheSun)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.MoonPhase = 0.5f;

        // Noon, full moon: sun high, moon below the horizon.
        const auto noon = Ephemeris::ComputeSunMoon(in);
        EXPECT_GT(noon.SunElevationRadians, 0.0f);
        EXPECT_LT(noon.MoonElevationRadians, 0.0f);

        // Midnight, full moon: moon up, sun down.
        in.TimeOfDayHours = 0.0f;
        const auto midnight = Ephemeris::ComputeSunMoon(in);
        EXPECT_LT(midnight.SunElevationRadians, 0.0f);
        EXPECT_GT(midnight.MoonElevationRadians, 0.0f);
        ExpectUnitFinite(midnight.MoonDirection);
    }

    TEST(EphemerisMath, NewMoonRidesWithTheSun)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.MoonPhase = 0.0f;
        const auto state = Ephemeris::ComputeSunMoon(in);
        // Same hour angle and declination → same position as the sun.
        EXPECT_NEAR(state.MoonElevationRadians, state.SunElevationRadians, 0.02f);
        EXPECT_NEAR(glm::dot(state.MoonDirection, state.SunDirection), 1.0f, 1e-3f);
    }

    TEST(EphemerisMath, MoonIlluminationFollowsSynodicPhase)
    {
        EphemerisInputs in = MidLatitudeNoon();
        in.MoonPhase = 0.0f;
        EXPECT_NEAR(Ephemeris::ComputeSunMoon(in).MoonIlluminatedFraction, 0.0f, 1e-4f);
        in.MoonPhase = 0.5f;
        EXPECT_NEAR(Ephemeris::ComputeSunMoon(in).MoonIlluminatedFraction, 1.0f, 1e-4f);
        in.MoonPhase = 0.25f;
        EXPECT_NEAR(Ephemeris::ComputeSunMoon(in).MoonIlluminatedFraction, 0.5f, 1e-3f);
    }

    // ---------- Time quantization (sky-bake hash gate) ----------

    TEST(EphemerisMath, QuantizeIsStableWithinABucketAndMovesAcross)
    {
        // 5-minute buckets: 10:01 and 10:04 share one; 10:06 is the next.
        const f32 a = Ephemeris::QuantizeTimeHours(10.0f + 1.0f / 60.0f, 5.0f);
        const f32 b = Ephemeris::QuantizeTimeHours(10.0f + 4.0f / 60.0f, 5.0f);
        const f32 c = Ephemeris::QuantizeTimeHours(10.0f + 6.0f / 60.0f, 5.0f);
        EXPECT_FLOAT_EQ(a, b);
        EXPECT_NE(a, c);
        EXPECT_NEAR(a, 10.0f, 1e-4f);
        EXPECT_NEAR(c, 10.0f + 5.0f / 60.0f, 1e-4f);
    }

    TEST(EphemerisMath, QuantizeWrapsAndSurvivesBadInput)
    {
        const f32 wrapped = Ephemeris::QuantizeTimeHours(25.5f, 5.0f);
        EXPECT_GE(wrapped, 0.0f);
        EXPECT_LT(wrapped, 24.0f);
        EXPECT_NEAR(wrapped, 1.5f, 0.1f);

        // Non-finite hours / non-positive quantum fall back to sane values.
        EXPECT_TRUE(std::isfinite(Ephemeris::QuantizeTimeHours(std::numeric_limits<f32>::quiet_NaN(), 5.0f)));
        EXPECT_TRUE(std::isfinite(Ephemeris::QuantizeTimeHours(10.0f, 0.0f)));
        EXPECT_TRUE(std::isfinite(Ephemeris::QuantizeTimeHours(10.0f, -3.0f)));
    }

    // ---------- Lighting-drive curves ----------

    TEST(EphemerisMath, SunIntensityFactorRampsMonotonicallyAndIsBounded)
    {
        f32 prev = -1.0f;
        for (f32 deg = -10.0f; deg <= 90.0f; deg += 2.0f)
        {
            const f32 f = Ephemeris::SunIntensityFactor(deg * kDegToRad);
            EXPECT_GE(f, 0.0f);
            EXPECT_LE(f, 1.0f);
            EXPECT_GE(f, prev - 1e-5f) << "intensity must not decrease with elevation (deg " << deg << ")";
            prev = f;
        }
        EXPECT_FLOAT_EQ(Ephemeris::SunIntensityFactor(-10.0f * kDegToRad), 0.0f);
        EXPECT_GT(Ephemeris::SunIntensityFactor(90.0f * kDegToRad), 0.95f);
    }

    TEST(EphemerisMath, NightBlendIsZeroByDayOneByNightAndMonotonic)
    {
        EXPECT_FLOAT_EQ(Ephemeris::NightBlend(30.0f * kDegToRad), 0.0f);
        EXPECT_FLOAT_EQ(Ephemeris::NightBlend(-20.0f * kDegToRad), 1.0f);
        f32 prev = 1.1f;
        for (f32 deg = -15.0f; deg <= 10.0f; deg += 1.0f)
        {
            const f32 blend = Ephemeris::NightBlend(deg * kDegToRad);
            EXPECT_LE(blend, prev + 1e-5f) << "night blend must fall as the sun rises";
            EXPECT_GE(blend, 0.0f);
            EXPECT_LE(blend, 1.0f);
            prev = blend;
        }
    }

    TEST(EphemerisMath, SunColorIsWarmerAtTheHorizonThanAtNoon)
    {
        const glm::vec3 horizon = Ephemeris::SunColorForElevation(2.0f * kDegToRad);
        const glm::vec3 noon = Ephemeris::SunColorForElevation(60.0f * kDegToRad);
        // Warmth = blue deficit relative to red. The horizon sun must be
        // markedly warmer; noon must be near-white.
        EXPECT_LT(horizon.b / horizon.r, noon.b / noon.r);
        EXPECT_GT(noon.b, 0.9f);
        EXPECT_LT(horizon.b, 0.5f);
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(std::isfinite(horizon[i]));
            EXPECT_TRUE(std::isfinite(noon[i]));
            EXPECT_GE(horizon[i], 0.0f);
            EXPECT_LE(noon[i], 1.0f);
        }
    }

    TEST(EphemerisMath, MoonIntensityScalesWithIlluminationAndElevation)
    {
        const f32 high = 30.0f * kDegToRad;
        EXPECT_FLOAT_EQ(Ephemeris::MoonIntensityFactor(high, 0.0f), 0.0f);
        EXPECT_GT(Ephemeris::MoonIntensityFactor(high, 1.0f), 0.9f);
        EXPECT_FLOAT_EQ(Ephemeris::MoonIntensityFactor(-10.0f * kDegToRad, 1.0f), 0.0f);
        EXPECT_LT(Ephemeris::MoonIntensityFactor(high, 0.3f),
                  Ephemeris::MoonIntensityFactor(high, 0.9f));
    }

    // ---------- Adversarial inputs ----------

    TEST(EphemerisMath, AdversarialInputsAreSanitized)
    {
        EphemerisInputs in;
        in.TimeOfDayHours = std::numeric_limits<f32>::quiet_NaN();
        in.DayOfYear = -12345;
        in.LatitudeDegrees = std::numeric_limits<f32>::infinity();
        in.NorthOffsetDegrees = -std::numeric_limits<f32>::infinity();
        in.MoonPhase = 1.0e30f;

        const auto state = Ephemeris::ComputeSunMoon(in);
        ExpectUnitFinite(state.SunDirection);
        ExpectUnitFinite(state.MoonDirection);
        EXPECT_TRUE(std::isfinite(state.SunElevationRadians));
        EXPECT_TRUE(std::isfinite(state.MoonElevationRadians));
        EXPECT_GE(state.MoonIlluminatedFraction, 0.0f);
        EXPECT_LE(state.MoonIlluminatedFraction, 1.0f);

        // Exact poles must not produce NaN azimuth.
        EphemerisInputs pole = MidLatitudeNoon();
        pole.LatitudeDegrees = 90.0f;
        ExpectUnitFinite(Ephemeris::ComputeSunMoon(pole).SunDirection);
        pole.LatitudeDegrees = -90.0f;
        ExpectUnitFinite(Ephemeris::ComputeSunMoon(pole).SunDirection);
    }
} // namespace OloEngine::Tests
