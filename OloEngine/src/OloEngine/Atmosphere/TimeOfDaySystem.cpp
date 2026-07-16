#include "OloEnginePCH.h"
#include "OloEngine/Atmosphere/TimeOfDaySystem.h"

#include "OloEngine/Atmosphere/Ephemeris.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kRadToDeg = 180.0f / glm::pi<f32>();

        // Advance the component clock by `dt` real seconds, honouring day
        // length and time scale; wraps hours into [0,24) and increments the
        // day of year on wrap.
        void AdvanceComponentClock(TimeOfDayComponent& tod, f32 dt)
        {
            if (tod.m_Paused)
                return;
            if (!std::isfinite(dt) || dt <= 0.0f)
                return;

            const f32 dayLengthMinutes = std::isfinite(tod.m_DayLengthMinutes)
                                             ? std::max(tod.m_DayLengthMinutes, 0.1f)
                                             : 24.0f;
            const f32 timeScale = std::isfinite(tod.m_TimeScale)
                                      ? std::clamp(tod.m_TimeScale, 0.0f, 1000.0f)
                                      : 1.0f;

            // 24 game hours span dayLengthMinutes real minutes.
            const f32 gameHoursPerRealSecond = 24.0f / (dayLengthMinutes * 60.0f);
            f32 hours = tod.m_TimeOfDayHours;
            if (!std::isfinite(hours))
                hours = 12.0f;
            hours += dt * gameHoursPerRealSecond * timeScale;

            while (hours >= 24.0f)
            {
                hours -= 24.0f;
                tod.m_DayOfYear = (tod.m_DayOfYear % 365) + 1;
            }
            tod.m_TimeOfDayHours = std::max(hours, 0.0f);
        }
    } // namespace

    void TimeOfDaySystem::AdvanceClock(Scene& scene, Timestep ts)
    {
        OLO_PROFILE_FUNCTION();

        auto view = scene.GetAllEntitiesWith<TimeOfDayComponent>();
        for (auto entity : view)
        {
            auto& tod = view.get<TimeOfDayComponent>(entity);
            if (!tod.m_Enabled)
                continue;
            AdvanceComponentClock(tod, ts.GetSeconds());
            return; // one clock drives the scene
        }
    }

    void TimeOfDaySystem::AdvanceClockInEditMode(Scene& scene, Timestep ts)
    {
        auto view = scene.GetAllEntitiesWith<TimeOfDayComponent>();
        for (auto entity : view)
        {
            auto& tod = view.get<TimeOfDayComponent>(entity);
            if (!tod.m_Enabled || !tod.m_AdvanceInEditMode)
                continue;
            AdvanceComponentClock(tod, ts.GetSeconds());
            return;
        }
    }

    void TimeOfDaySystem::Apply(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        auto view = scene.GetAllEntitiesWith<TimeOfDayComponent>();
        for (auto entity : view)
        {
            auto& tod = view.get<TimeOfDayComponent>(entity);
            if (!tod.m_Enabled)
                continue;

            EphemerisInputs inputs;
            inputs.TimeOfDayHours = tod.m_TimeOfDayHours;
            inputs.DayOfYear = tod.m_DayOfYear;
            inputs.LatitudeDegrees = tod.m_LatitudeDegrees;
            inputs.NorthOffsetDegrees = tod.m_NorthOffsetDegrees;
            inputs.MoonPhase = tod.m_MoonPhase;
            const SunMoonState state = Ephemeris::ComputeSunMoon(inputs);

            // Derived script-readable outputs.
            tod.m_SunDirection = state.SunDirection;
            tod.m_MoonDirection = state.MoonDirection;
            tod.m_SunElevationDegrees = state.SunElevationRadians * kRadToDeg;
            tod.m_IsNight = Ephemeris::NightBlend(state.SunElevationRadians) > 0.5f;

            // Weather director's global sun dimming (cloud cover), if present.
            f32 dimming = 0.0f;
            auto weatherView = scene.GetAllEntitiesWith<WeatherStateComponent>();
            for (auto weatherEntity : weatherView)
            {
                if (const auto& weather = weatherView.get<WeatherStateComponent>(weatherEntity);
                    weather.m_Enabled)
                {
                    dimming = std::clamp(weather.m_Blended.SunDimming, 0.0f, 1.0f);
                    break;
                }
            }
            const f32 dimFactor = 1.0f - dimming;

            // Sun vs moon: whichever contributes more light drives the single
            // directional light this frame. Both ramp to ~0 through the
            // horizon, so the swap is pop-free.
            const f32 sunIntensity = Ephemeris::SunIntensityFactor(state.SunElevationRadians) *
                                     std::max(tod.m_SunIntensityMax, 0.0f) * dimFactor;
            const f32 moonIntensity =
                Ephemeris::MoonIntensityFactor(state.MoonElevationRadians, state.MoonIlluminatedFraction) *
                std::max(tod.m_MoonIntensityMax, 0.0f) * dimFactor;

            auto lightView = scene.GetAllEntitiesWith<DirectionalLightComponent>();
            if (auto it = lightView.begin(); it != lightView.end())
            {
                auto& light = lightView.get<DirectionalLightComponent>(*it);
                if (sunIntensity >= moonIntensity)
                {
                    light.m_Direction = -state.SunDirection;
                    light.m_Color = Ephemeris::SunColorForElevation(state.SunElevationRadians);
                    light.m_Intensity = sunIntensity;
                }
                else
                {
                    light.m_Direction = -state.MoonDirection;
                    light.m_Color = Ephemeris::MoonColor();
                    light.m_Intensity = moonIntensity;
                }
            }
            return; // one clock drives the scene
        }
    }
} // namespace OloEngine
