// OLO_TEST_LAYER: unit
// =============================================================================
// WeatherBlendTest.cpp
//
// Pins the weather director's pure blending helpers (issue #633, Pillar A):
// WeatherSystem::BlendPresets / EaseSmoothstep / AdvanceWetness / PresetFor.
// Headless — no Scene tick, no GPU. The cross-subsystem application path
// (scene settings + renderer globals) is covered by the Functional
// WeatherDirector test instead.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Atmosphere/WeatherSystem.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        WeatherPreset ClearPreset()
        {
            return MakeDefaultWeatherPreset(WeatherStateId::Clear);
        }
        WeatherPreset StormPreset()
        {
            return MakeDefaultWeatherPreset(WeatherStateId::Storm);
        }
    } // namespace

    // ---------- Easing ----------

    TEST(WeatherBlend, EaseIsBoundedMonotoneAndHitsEndpoints)
    {
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(1.0f), 1.0f);
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(0.5f), 0.5f);
        f32 prev = -0.1f;
        for (f32 t = 0.0f; t <= 1.0f; t += 0.05f)
        {
            const f32 e = WeatherSystem::EaseSmoothstep(t);
            EXPECT_GE(e, prev);
            EXPECT_GE(e, 0.0f);
            EXPECT_LE(e, 1.0f);
            prev = e;
        }
        // Out-of-range / non-finite input saturates.
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(-3.0f), 0.0f);
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(7.0f), 1.0f);
        EXPECT_FLOAT_EQ(WeatherSystem::EaseSmoothstep(std::numeric_limits<f32>::quiet_NaN()), 0.0f);
    }

    // ---------- Preset blending ----------

    TEST(WeatherBlend, EndpointsReproduceTheSourcePresets)
    {
        const WeatherPreset clear = ClearPreset();
        const WeatherPreset storm = StormPreset();

        const WeatherPreset at0 = WeatherSystem::BlendPresets(clear, storm, 0.0f);
        EXPECT_FLOAT_EQ(at0.CloudCoverage, clear.CloudCoverage);
        EXPECT_FLOAT_EQ(at0.WindSpeed, clear.WindSpeed);
        EXPECT_EQ(at0.PrecipitationEnabled, false); // clear has no precip
        EXPECT_FLOAT_EQ(at0.SunDimming, clear.SunDimming);

        const WeatherPreset at1 = WeatherSystem::BlendPresets(clear, storm, 1.0f);
        EXPECT_FLOAT_EQ(at1.CloudCoverage, storm.CloudCoverage);
        EXPECT_FLOAT_EQ(at1.WindSpeed, storm.WindSpeed);
        EXPECT_TRUE(at1.PrecipitationEnabled);
        EXPECT_FLOAT_EQ(at1.PrecipitationIntensity, storm.PrecipitationIntensity);
        EXPECT_EQ(at1.PrecipitationKind, storm.PrecipitationKind);
    }

    TEST(WeatherBlend, MidpointLerpsContinuousFields)
    {
        const WeatherPreset clear = ClearPreset();
        const WeatherPreset storm = StormPreset();
        const WeatherPreset mid = WeatherSystem::BlendPresets(clear, storm, 0.5f);

        EXPECT_NEAR(mid.CloudCoverage, 0.5f * (clear.CloudCoverage + storm.CloudCoverage), 1e-5f);
        EXPECT_NEAR(mid.WindSpeed, 0.5f * (clear.WindSpeed + storm.WindSpeed), 1e-5f);
        EXPECT_NEAR(mid.SunDimming, 0.5f * (clear.SunDimming + storm.SunDimming), 1e-5f);
        // Storm-only precipitation ramps from zero contribution.
        EXPECT_NEAR(mid.PrecipitationIntensity, 0.5f * storm.PrecipitationIntensity, 1e-5f);
        EXPECT_TRUE(mid.PrecipitationEnabled);
    }

    TEST(WeatherBlend, DisabledEndpointFogRampsFromZeroNotFromItsAuthoredDensity)
    {
        const WeatherPreset clear = ClearPreset(); // fog disabled, authored density irrelevant
        const WeatherPreset storm = StormPreset(); // fog enabled

        const WeatherPreset early = WeatherSystem::BlendPresets(clear, storm, 0.1f);
        EXPECT_NEAR(early.FogDensity, 0.1f * storm.FogDensity, 1e-6f);
        EXPECT_TRUE(early.FogEnabled);
        // Colour comes from the enabled endpoint, not a lerp toward a colour
        // no fog would ever show.
        EXPECT_FLOAT_EQ(early.FogColor.r, storm.FogColor.r);

        const WeatherPreset none = WeatherSystem::BlendPresets(clear, clear, 0.7f);
        EXPECT_FALSE(none.FogEnabled);
        EXPECT_NEAR(none.FogDensity, 0.0f, 1e-6f);
    }

    TEST(WeatherBlend, PrecipitationKindSwitchesAtTheMidpoint)
    {
        WeatherPreset rain = MakeDefaultWeatherPreset(WeatherStateId::Rain);
        WeatherPreset snow = MakeDefaultWeatherPreset(WeatherStateId::Snow);

        EXPECT_EQ(WeatherSystem::BlendPresets(rain, snow, 0.49f).PrecipitationKind,
                  WeatherPrecipitationType::Rain);
        EXPECT_EQ(WeatherSystem::BlendPresets(rain, snow, 0.5f).PrecipitationKind,
                  WeatherPrecipitationType::Snow);
    }

    TEST(WeatherBlend, SnowAccumulationRampsThroughDisabledEndpoint)
    {
        const WeatherPreset clear = ClearPreset();
        const WeatherPreset snow = MakeDefaultWeatherPreset(WeatherStateId::Snow);

        const WeatherPreset mid = WeatherSystem::BlendPresets(clear, snow, 0.5f);
        EXPECT_TRUE(mid.SnowAccumulationEnabled);
        EXPECT_NEAR(mid.SnowAccumulationRate, 0.5f * snow.SnowAccumulationRate, 1e-6f);

        const WeatherPreset start = WeatherSystem::BlendPresets(clear, snow, 0.0f);
        EXPECT_FALSE(start.SnowAccumulationEnabled);
    }

    TEST(WeatherBlend, BlendSaturatesOutOfRangeT)
    {
        const WeatherPreset clear = ClearPreset();
        const WeatherPreset storm = StormPreset();
        const WeatherPreset below = WeatherSystem::BlendPresets(clear, storm, -1.0f);
        EXPECT_FLOAT_EQ(below.CloudCoverage, clear.CloudCoverage);
        const WeatherPreset above = WeatherSystem::BlendPresets(clear, storm, 2.0f);
        EXPECT_FLOAT_EQ(above.CloudCoverage, storm.CloudCoverage);
        const WeatherPreset nan = WeatherSystem::BlendPresets(clear, storm,
                                                              std::numeric_limits<f32>::quiet_NaN());
        EXPECT_TRUE(std::isfinite(nan.CloudCoverage));
    }

    // ---------- Wetness dynamics ----------

    TEST(WeatherBlend, WetnessRisesWhilePrecipitatingAndDriesAfter)
    {
        // Rising: 0 → 0.85 target at 0.15/s.
        f32 w = 0.0f;
        w = WeatherSystem::AdvanceWetness(w, 0.85f, /*precipitating=*/true, 0.15f, 0.02f, 1.0f);
        EXPECT_NEAR(w, 0.15f, 1e-5f);
        // Never overshoots the target.
        for (int i = 0; i < 20; ++i)
            w = WeatherSystem::AdvanceWetness(w, 0.85f, true, 0.15f, 0.02f, 1.0f);
        EXPECT_NEAR(w, 0.85f, 1e-5f);

        // Drying is slower (asymmetry is the design point).
        const f32 afterOneSecondDry = WeatherSystem::AdvanceWetness(w, 0.0f, false, 0.15f, 0.02f, 1.0f);
        EXPECT_NEAR(afterOneSecondDry, 0.85f - 0.02f, 1e-5f);
        // And never undershoots.
        f32 dry = 0.01f;
        dry = WeatherSystem::AdvanceWetness(dry, 0.0f, false, 0.15f, 0.02f, 5.0f);
        EXPECT_FLOAT_EQ(dry, 0.0f);
    }

    TEST(WeatherBlend, WetnessSurvivesBadInput)
    {
        EXPECT_TRUE(std::isfinite(WeatherSystem::AdvanceWetness(
            std::numeric_limits<f32>::quiet_NaN(), 0.5f, true, 0.15f, 0.02f, 1.0f)));
        // Zero / negative / NaN dt leaves wetness unchanged.
        EXPECT_FLOAT_EQ(WeatherSystem::AdvanceWetness(0.4f, 1.0f, true, 0.15f, 0.02f, 0.0f), 0.4f);
        EXPECT_FLOAT_EQ(WeatherSystem::AdvanceWetness(0.4f, 1.0f, true, 0.15f, 0.02f,
                                                      std::numeric_limits<f32>::quiet_NaN()),
                        0.4f);
        // Result always lands in [0,1].
        const f32 w = WeatherSystem::AdvanceWetness(0.9f, 5.0f, true, 100.0f, 0.02f, 10.0f);
        EXPECT_GE(w, 0.0f);
        EXPECT_LE(w, 1.0f);
    }

    // ---------- Preset lookup ----------

    TEST(WeatherBlend, PresetForReturnsTheMatchingAuthoredPreset)
    {
        WeatherStateComponent comp;
        comp.m_PresetStorm.WindSpeed = 99.0f;
        EXPECT_FLOAT_EQ(WeatherSystem::PresetFor(comp, WeatherStateId::Storm).WindSpeed, 99.0f);
        EXPECT_FLOAT_EQ(WeatherSystem::PresetFor(comp, WeatherStateId::Clear).WindSpeed,
                        comp.m_PresetClear.WindSpeed);
        // Unknown id falls back to Clear instead of reading out of bounds.
        EXPECT_FLOAT_EQ(WeatherSystem::PresetFor(comp, static_cast<WeatherStateId>(42)).WindSpeed,
                        comp.m_PresetClear.WindSpeed);
    }

    // ---------- Component defaults sanity ----------

    TEST(WeatherBlend, DefaultPresetsAreOrderedBySeverity)
    {
        // The authored defaults must keep the qualitative ordering the states
        // promise — storms are windier, cloudier, and dimmer than clear skies.
        const WeatherPreset clear = ClearPreset();
        const WeatherPreset overcast = MakeDefaultWeatherPreset(WeatherStateId::Overcast);
        const WeatherPreset storm = StormPreset();
        EXPECT_LT(clear.CloudCoverage, overcast.CloudCoverage);
        EXPECT_LT(overcast.CloudCoverage, storm.CloudCoverage);
        EXPECT_LT(clear.WindSpeed, storm.WindSpeed);
        EXPECT_LT(clear.SunDimming, overcast.SunDimming);
        EXPECT_LT(overcast.SunDimming, storm.SunDimming);
        EXPECT_FALSE(clear.PrecipitationEnabled);
        EXPECT_TRUE(storm.PrecipitationEnabled);
        // Snow state accumulates snow; rain state wets surfaces more than snow.
        EXPECT_TRUE(MakeDefaultWeatherPreset(WeatherStateId::Snow).SnowAccumulationEnabled);
        EXPECT_GT(MakeDefaultWeatherPreset(WeatherStateId::Rain).WetnessTarget,
                  MakeDefaultWeatherPreset(WeatherStateId::Snow).WetnessTarget);
        // Fog bank is the fog-density outlier.
        EXPECT_GT(MakeDefaultWeatherPreset(WeatherStateId::FogBank).FogDensity,
                  storm.FogDensity);
    }
} // namespace OloEngine::Tests
