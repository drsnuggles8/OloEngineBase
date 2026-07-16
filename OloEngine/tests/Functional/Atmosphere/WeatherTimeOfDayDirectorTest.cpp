// OLO_TEST_LAYER: Functional
// =============================================================================
// WeatherTimeOfDayDirectorTest.cpp — Weather × TimeOfDay × Scene seam
// (issue #633).
//
// Drives real Scene::OnUpdateRuntime ticks and pins the cross-subsystem
// contracts of the two new gameplay systems:
//   - The weather director cross-blends Clear → Storm over the authored
//     duration, writing the blended weather-facing subset into the
//     scene-level Wind/Fog/Precipitation settings AND their Renderer3D
//     global twins each tick (the seam the render pipeline consumes).
//   - Wetness rises while storm rain falls and dries out after a switch
//     back to Clear (asymmetric rates).
//   - The time-of-day clock advances with simulated time (day length ×
//     time scale), pauses when told to, and TimeOfDaySystem::Apply drives
//     the scene's directional light (sun by day, moon at night, warm sun
//     at the horizon) — Apply is called directly here because the headless
//     harness never renders, and Apply lives on the render path.
// =============================================================================

#include "OloEnginePCH.h"

#include "../FunctionalTest.h"

#include "OloEngine/Atmosphere/Ephemeris.h"
#include "OloEngine/Atmosphere/TimeOfDaySystem.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>

namespace OloEngine::Functional
{
    class WeatherTimeOfDayDirectorTest : public FunctionalTest
    {
      protected:
        void BuildScene() override
        {
            Entity director = GetScene().CreateEntity("AtmosphereDirector");
            auto& weather = director.AddComponent<WeatherStateComponent>();
            weather.m_TransitionDuration = 1.0f; // fast blend for the test
            auto& tod = director.AddComponent<TimeOfDayComponent>();
            tod.m_DayLengthMinutes = 1.0f; // 1 real minute per game day
            tod.m_TimeOfDayHours = 12.0f;
            tod.m_DayOfYear = 80; // equinox — clean noon geometry
            tod.m_LatitudeDegrees = 48.0f;

            Entity sun = GetScene().CreateEntity("Sun");
            sun.AddComponent<DirectionalLightComponent>();

            m_Director = director;
            m_Sun = sun;
        }

        Entity m_Director;
        Entity m_Sun;
    };

    TEST_F(WeatherTimeOfDayDirectorTest, StormTransitionBlendsSceneSettingsAndRendererTwins)
    {
        auto& weather = m_Director.GetComponent<WeatherStateComponent>();
        const WeatherPreset clear = weather.m_PresetClear;
        const WeatherPreset storm = weather.m_PresetStorm;

        // Settle on Clear first (first tick initializes the blend).
        RunFrames(2);
        EXPECT_FALSE(GetScene().GetPrecipitationSettings().Enabled);
        EXPECT_NEAR(GetScene().GetWindSettings().Speed, clear.WindSpeed, 1.0e-3f);

        // Kick off the transition by editing the target (no API needed —
        // the system detects target edits).
        weather.m_TargetState = WeatherStateId::Storm;

        // Mid-transition: wind speed strictly between the two presets,
        // storm rain already ramping in.
        RunFrames(30); // 0.5 s of the 1 s blend
        {
            const f32 wind = GetScene().GetWindSettings().Speed;
            EXPECT_GT(wind, clear.WindSpeed + 0.5f);
            EXPECT_LT(wind, storm.WindSpeed - 0.5f);
            EXPECT_TRUE(GetScene().GetPrecipitationSettings().Enabled);
            EXPECT_GT(GetScene().GetPrecipitationSettings().Intensity, 0.05f);
            EXPECT_LT(GetScene().GetPrecipitationSettings().Intensity, storm.PrecipitationIntensity);
            EXPECT_GT(weather.m_TransitionProgress, 0.1f);
            EXPECT_LT(weather.m_TransitionProgress, 0.95f);
        }

        // Settled: every weather-facing field matches the storm preset, the
        // state machine reports Storm, and the Renderer3D global twins carry
        // the same values (the seam the render pipeline reads).
        RunFrames(60);
        {
            EXPECT_EQ(weather.m_CurrentState, WeatherStateId::Storm);
            EXPECT_FLOAT_EQ(weather.m_TransitionProgress, 1.0f);
            const auto& wind = GetScene().GetWindSettings();
            const auto& fog = GetScene().GetFogSettings();
            const auto& precip = GetScene().GetPrecipitationSettings();
            EXPECT_NEAR(wind.Speed, storm.WindSpeed, 1.0e-3f);
            EXPECT_NEAR(wind.GustStrength, storm.WindGustStrength, 1.0e-3f);
            EXPECT_TRUE(fog.Enabled);
            EXPECT_NEAR(fog.Density, storm.FogDensity, 1.0e-4f);
            EXPECT_TRUE(precip.Enabled);
            EXPECT_EQ(precip.Type, PrecipitationType::Rain);
            EXPECT_NEAR(precip.Intensity, storm.PrecipitationIntensity, 1.0e-3f);

            EXPECT_NEAR(Renderer3D::GetWindSettings().Speed, wind.Speed, 1.0e-5f);
            EXPECT_NEAR(Renderer3D::GetFogSettings().Density, fog.Density, 1.0e-5f);
            EXPECT_EQ(Renderer3D::GetPrecipitationSettings().Enabled, precip.Enabled);
        }
    }

    TEST_F(WeatherTimeOfDayDirectorTest, WetnessRisesInRainAndDriesSlowlyAfter)
    {
        auto& weather = m_Director.GetComponent<WeatherStateComponent>();
        weather.m_TransitionDuration = 0.1f;
        weather.m_TargetState = WeatherStateId::Storm;

        // Rain for 8 simulated seconds: wetness must climb toward the storm
        // preset's target (1.0) at ~0.15/s.
        TickFor(8.0f);
        const f32 wetAfterRain = weather.m_Wetness;
        EXPECT_GT(wetAfterRain, 0.8f);

        // Back to clear: drying is an order of magnitude slower.
        weather.m_TargetState = WeatherStateId::Clear;
        TickFor(4.0f);
        const f32 wetAfterClear = weather.m_Wetness;
        EXPECT_LT(wetAfterClear, wetAfterRain);
        EXPECT_GT(wetAfterClear, 0.5f) << "drying must be slow (dry rate 0.02/s)";
    }

    TEST_F(WeatherTimeOfDayDirectorTest, ClockAdvancesWithSimTimeAndPauses)
    {
        auto& tod = m_Director.GetComponent<TimeOfDayComponent>();
        // 1-minute day: 24 game hours per 60 real seconds → 0.4 h per second.
        TickFor(5.0f);
        EXPECT_NEAR(tod.m_TimeOfDayHours, 12.0f + 5.0f * 24.0f / 60.0f, 0.05f);

        tod.m_Paused = true;
        const f32 frozen = tod.m_TimeOfDayHours;
        TickFor(2.0f);
        EXPECT_FLOAT_EQ(tod.m_TimeOfDayHours, frozen);

        tod.m_Paused = false;
        tod.m_TimeScale = 2.0f;
        TickFor(1.0f);
        EXPECT_NEAR(tod.m_TimeOfDayHours, frozen + 2.0f * 24.0f / 60.0f, 0.05f);
    }

    TEST_F(WeatherTimeOfDayDirectorTest, ApplyDrivesTheDirectionalLightThroughTheDay)
    {
        auto& tod = m_Director.GetComponent<TimeOfDayComponent>();
        auto& light = m_Sun.GetComponent<DirectionalLightComponent>();

        // Noon: sun high — light travels downward, strong and near-white.
        tod.m_TimeOfDayHours = 12.0f;
        TimeOfDaySystem::Apply(GetScene());
        EXPECT_LT(light.m_Direction.y, -0.5f) << "noon light must point down";
        const f32 noonIntensity = light.m_Intensity;
        EXPECT_GT(noonIntensity, 1.0f);
        const f32 noonWarmth = light.m_Color.r / std::max(light.m_Color.b, 1.0e-4f);
        EXPECT_FALSE(tod.m_IsNight);

        // Golden hour: still sun-lit but weaker and warmer.
        tod.m_TimeOfDayHours = 18.0f; // near equinox sunset
        TimeOfDaySystem::Apply(GetScene());
        if (light.m_Intensity > 0.0f && !tod.m_IsNight)
        {
            EXPECT_LT(light.m_Intensity, noonIntensity);
            const f32 eveningWarmth = light.m_Color.r / std::max(light.m_Color.b, 1.0e-4f);
            EXPECT_GT(eveningWarmth, noonWarmth) << "horizon sun must be warmer than noon";
        }

        // Midnight, full moon: the single directional light swaps to
        // moonlight — cool, dim, and the component reports night.
        tod.m_TimeOfDayHours = 0.0f;
        tod.m_MoonPhase = 0.5f;
        TimeOfDaySystem::Apply(GetScene());
        EXPECT_TRUE(tod.m_IsNight);
        EXPECT_LT(light.m_Intensity, 0.3f) << "moonlight is dim";
        EXPECT_GT(light.m_Intensity, 0.0f) << "full moon at mid-latitude is up at midnight";
        EXPECT_GT(light.m_Color.b, light.m_Color.r) << "moonlight is cool";
        EXPECT_LT(light.m_Direction.y, 0.0f) << "moonlight travels downward";

        // Derived outputs are exposed for scripts.
        EXPECT_TRUE(std::isfinite(tod.m_SunElevationDegrees));
        EXPECT_LT(tod.m_SunElevationDegrees, 0.0f);
    }

    TEST_F(WeatherTimeOfDayDirectorTest, AtmosphereComponentsSurviveSaveGameRoundTrip)
    {
        // Author distinctly non-default values on all three components (a
        // mid-storm save with a scrubbed clock and a tweaked cloud layer),
        // capture, restore into a fresh scene, and assert the state survived
        // — the "save-game round-trip" acceptance criterion of issue #633.
        auto& tod = m_Director.GetComponent<TimeOfDayComponent>();
        tod.m_TimeOfDayHours = 19.25f;
        tod.m_DayOfYear = 300;
        tod.m_LatitudeDegrees = -33.5f;
        tod.m_MoonPhase = 0.75f;
        tod.m_Paused = true;

        auto& weather = m_Director.GetComponent<WeatherStateComponent>();
        weather.m_TargetState = WeatherStateId::Storm;
        weather.m_TransitionDuration = 42.0f;
        weather.m_PresetStorm.WindSpeed = 21.5f;
        RunFrames(10); // mid-transition: progress + wetness are live values

        auto& clouds = m_Director.AddComponent<CloudscapeComponent>();
        clouds.m_Coverage = 0.91f;
        clouds.m_LayerBottom = 900.0f;
        clouds.m_LayerTop = 5200.0f;
        clouds.m_CastCloudShadows = false;

        const f32 savedProgress = weather.m_TransitionProgress;
        const f32 savedWetness = weather.m_Wetness;

        auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
        ASSERT_GT(payload.size(), 0u);

        Ref<Scene> restored = Ref<Scene>::Create();
        ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

        bool found = false;
        auto view = restored->GetAllEntitiesWith<TimeOfDayComponent, WeatherStateComponent,
                                                 CloudscapeComponent>();
        for (auto entity : view)
        {
            found = true;
            const auto& rTod = view.get<TimeOfDayComponent>(entity);
            EXPECT_FLOAT_EQ(rTod.m_TimeOfDayHours, 19.25f);
            EXPECT_EQ(rTod.m_DayOfYear, 300);
            EXPECT_FLOAT_EQ(rTod.m_LatitudeDegrees, -33.5f);
            EXPECT_FLOAT_EQ(rTod.m_MoonPhase, 0.75f);
            EXPECT_TRUE(rTod.m_Paused);

            const auto& rWeather = view.get<WeatherStateComponent>(entity);
            EXPECT_EQ(rWeather.m_TargetState, WeatherStateId::Storm);
            EXPECT_FLOAT_EQ(rWeather.m_TransitionDuration, 42.0f);
            EXPECT_FLOAT_EQ(rWeather.m_PresetStorm.WindSpeed, 21.5f);
            // Save-games persist the LIVE transition + wetness (unlike scene
            // YAML, where the Skip fields reset) — a mid-storm save resumes
            // mid-storm.
            EXPECT_NEAR(rWeather.m_TransitionProgress, savedProgress, 1.0e-5f);
            EXPECT_NEAR(rWeather.m_Wetness, savedWetness, 1.0e-5f);

            const auto& rClouds = view.get<CloudscapeComponent>(entity);
            EXPECT_FLOAT_EQ(rClouds.m_Coverage, 0.91f);
            EXPECT_FLOAT_EQ(rClouds.m_LayerBottom, 900.0f);
            EXPECT_FLOAT_EQ(rClouds.m_LayerTop, 5200.0f);
            EXPECT_FALSE(rClouds.m_CastCloudShadows);
        }
        EXPECT_TRUE(found) << "restored scene lost the atmosphere director entity";
    }
} // namespace OloEngine::Functional
