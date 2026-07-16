#include "OloEnginePCH.h"
#include "OloEngine/Atmosphere/WeatherSystem.h"

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // WeatherPrecipitationType (Scene/Components.h — declared there so
    // Components.h needn't include PostProcessSettings.h) must stay
    // value-pinned to the renderer's PrecipitationType.
    static_assert(static_cast<i32>(WeatherPrecipitationType::Snow) == static_cast<i32>(PrecipitationType::Snow));
    static_assert(static_cast<i32>(WeatherPrecipitationType::Rain) == static_cast<i32>(PrecipitationType::Rain));
    static_assert(static_cast<i32>(WeatherPrecipitationType::Hail) == static_cast<i32>(PrecipitationType::Hail));
    static_assert(static_cast<i32>(WeatherPrecipitationType::Sleet) == static_cast<i32>(PrecipitationType::Sleet));

    namespace
    {
        [[nodiscard]] f32 Lerp(f32 a, f32 b, f32 t)
        {
            return a + (b - a) * t;
        }

        [[nodiscard]] glm::vec3 Lerp(const glm::vec3& a, const glm::vec3& b, f32 t)
        {
            return a + (b - a) * t;
        }

        // A finite [0,1] clamp for values that may arrive from YAML/scripts.
        [[nodiscard]] f32 Saturate(f32 v)
        {
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        }
    } // namespace

    f32 WeatherSystem::EaseSmoothstep(f32 t)
    {
        t = Saturate(t);
        return t * t * (3.0f - 2.0f * t);
    }

    WeatherPreset WeatherSystem::BlendPresets(const WeatherPreset& from, const WeatherPreset& to, f32 t)
    {
        t = Saturate(t);
        WeatherPreset blended;

        // Clouds — plain lerps.
        blended.CloudCoverage = Lerp(from.CloudCoverage, to.CloudCoverage, t);
        blended.CloudDensity = Lerp(from.CloudDensity, to.CloudDensity, t);
        blended.CloudTypeBlend = Lerp(from.CloudTypeBlend, to.CloudTypeBlend, t);
        blended.CloudWetness = Lerp(from.CloudWetness, to.CloudWetness, t);

        // Fog — a disabled endpoint contributes zero density so an
        // off→on transition ramps instead of popping.
        const f32 fromFogDensity = from.FogEnabled ? from.FogDensity : 0.0f;
        const f32 toFogDensity = to.FogEnabled ? to.FogDensity : 0.0f;
        blended.FogDensity = Lerp(fromFogDensity, toFogDensity, t);
        blended.FogEnabled = blended.FogDensity > 1.0e-5f;
        if (from.FogEnabled && to.FogEnabled)
            blended.FogColor = Lerp(from.FogColor, to.FogColor, t);
        else if (to.FogEnabled)
            blended.FogColor = to.FogColor;
        else
            blended.FogColor = from.FogColor;
        blended.FogHeightFalloff = Lerp(from.FogHeightFalloff, to.FogHeightFalloff, t);
        blended.FogMaxOpacity = Lerp(from.FogMaxOpacity, to.FogMaxOpacity, t);

        // Wind — plain lerps (direction stays authored on WindSettings).
        blended.WindSpeed = Lerp(from.WindSpeed, to.WindSpeed, t);
        blended.WindGustStrength = Lerp(from.WindGustStrength, to.WindGustStrength, t);
        blended.WindTurbulence = Lerp(from.WindTurbulence, to.WindTurbulence, t);

        // Precipitation — intensity ramps through zero-contribution endpoints;
        // the kind switches at the midpoint (v1: a Rain→Snow transition swaps
        // particle type at t = 0.5 while intensity is mid-ramp).
        const f32 fromPrecip = from.PrecipitationEnabled ? from.PrecipitationIntensity : 0.0f;
        const f32 toPrecip = to.PrecipitationEnabled ? to.PrecipitationIntensity : 0.0f;
        blended.PrecipitationIntensity = Lerp(fromPrecip, toPrecip, t);
        blended.PrecipitationEnabled = blended.PrecipitationIntensity > 5.0e-3f;
        blended.PrecipitationKind = (t < 0.5f) ? from.PrecipitationKind : to.PrecipitationKind;

        // Snow accumulation — same zero-contribution ramp as fog/precip.
        const f32 fromSnowRate = from.SnowAccumulationEnabled ? from.SnowAccumulationRate : 0.0f;
        const f32 toSnowRate = to.SnowAccumulationEnabled ? to.SnowAccumulationRate : 0.0f;
        blended.SnowAccumulationRate = Lerp(fromSnowRate, toSnowRate, t);
        blended.SnowAccumulationEnabled = blended.SnowAccumulationRate > 1.0e-6f;

        // Lighting / surface.
        blended.SunDimming = Lerp(from.SunDimming, to.SunDimming, t);
        blended.WetnessTarget = Lerp(from.WetnessTarget, to.WetnessTarget, t);

        return blended;
    }

    f32 WeatherSystem::AdvanceWetness(f32 current, f32 target, bool precipitating, f32 riseRate,
                                      f32 dryRate, f32 dt)
    {
        current = Saturate(current);
        target = Saturate(target);
        if (!std::isfinite(dt) || dt <= 0.0f)
            return current;

        const f32 rate = precipitating ? std::max(riseRate, 0.0f) : std::max(dryRate, 0.0f);
        const f32 step = rate * dt;
        if (current < target)
            return std::min(current + step, target);
        return std::max(current - step, target);
    }

    const WeatherPreset& WeatherSystem::PresetFor(const WeatherStateComponent& component,
                                                  WeatherStateId id)
    {
        switch (id)
        {
            case WeatherStateId::Clear:
                return component.m_PresetClear;
            case WeatherStateId::Overcast:
                return component.m_PresetOvercast;
            case WeatherStateId::Rain:
                return component.m_PresetRain;
            case WeatherStateId::Storm:
                return component.m_PresetStorm;
            case WeatherStateId::Snow:
                return component.m_PresetSnow;
            case WeatherStateId::FogBank:
                return component.m_PresetFogBank;
            default:
                OLO_CORE_WARN("WeatherSystem::PresetFor: unknown WeatherStateId {}", static_cast<i32>(id));
                return component.m_PresetClear;
        }
    }

    void WeatherSystem::UpdateTransition(WeatherStateComponent& weather, f32 dt)
    {
        // First touch after construction / scene load: settle on the
        // (possibly deserialized) current state, then let a differing
        // TargetState from the same load kick off a fresh transition below.
        if (!weather.m_BlendedValid)
        {
            weather.m_Blended = PresetFor(weather, weather.m_CurrentState);
            weather.m_TransitionFrom = weather.m_Blended;
            weather.m_PrevTargetSeen = weather.m_CurrentState;
            weather.m_TransitionProgress = 1.0f;
            weather.m_BlendedValid = true;
        }

        // Target edited (inspector / script / MCP / deserialized): begin a
        // new transition FROM the currently-applied blend, so retargeting
        // mid-flight eases from what is on screen instead of popping.
        if (weather.m_TargetState != weather.m_PrevTargetSeen)
        {
            weather.m_TransitionFrom = weather.m_Blended;
            weather.m_TransitionProgress = 0.0f;
            weather.m_PrevTargetSeen = weather.m_TargetState;
        }

        if (weather.m_TransitionProgress < 1.0f)
        {
            if (weather.m_TransitionDuration <= 1.0e-3f)
                weather.m_TransitionProgress = 1.0f;
            else if (dt > 0.0f)
                weather.m_TransitionProgress =
                    Saturate(weather.m_TransitionProgress + dt / weather.m_TransitionDuration);

            weather.m_Blended = BlendPresets(weather.m_TransitionFrom,
                                             PresetFor(weather, weather.m_TargetState),
                                             EaseSmoothstep(weather.m_TransitionProgress));
            if (weather.m_TransitionProgress >= 1.0f)
                weather.m_CurrentState = weather.m_TargetState;
        }
        else
        {
            // Settled: follow live edits to the authored preset.
            weather.m_CurrentState = weather.m_TargetState;
            weather.m_Blended = PresetFor(weather, weather.m_CurrentState);
        }
    }

    void WeatherSystem::Tick(Scene& scene, Timestep ts)
    {
        OLO_PROFILE_FUNCTION();

        auto view = scene.GetAllEntitiesWith<WeatherStateComponent>();
        for (auto entity : view)
        {
            auto& weather = view.get<WeatherStateComponent>(entity);
            if (!weather.m_Enabled)
                continue;

            const f32 dt = ts.GetSeconds();
            UpdateTransition(weather, dt);

            // Wetness dynamics: rain/sleet wet surfaces; snow and hail read
            // cold-dry (their WetnessTarget presets are low anyway).
            const bool wetting = weather.m_Blended.PrecipitationEnabled &&
                                 (weather.m_Blended.PrecipitationKind == WeatherPrecipitationType::Rain ||
                                  weather.m_Blended.PrecipitationKind == WeatherPrecipitationType::Sleet);
            weather.m_Wetness = AdvanceWetness(weather.m_Wetness, weather.m_Blended.WetnessTarget,
                                               wetting, weather.m_WetnessRiseRate,
                                               weather.m_WetnessDryRate, dt);

            ApplyBlended(scene, weather);
            return; // one director drives the scene
        }
    }

    void WeatherSystem::ApplyImmediate(Scene& scene)
    {
        auto view = scene.GetAllEntitiesWith<WeatherStateComponent>();
        for (auto entity : view)
        {
            auto& weather = view.get<WeatherStateComponent>(entity);
            if (!weather.m_Enabled)
                continue;

            UpdateTransition(weather, 0.0f);
            ApplyBlended(scene, weather);
            return;
        }
    }

    void WeatherSystem::ApplyBlended(Scene& scene, WeatherStateComponent& component)
    {
        const WeatherPreset& b = component.m_Blended;

        // ── Wind (weather-facing subset; Direction/grid stay authored) ──
        auto& wind = scene.GetWindSettings();
        wind.Enabled = b.WindSpeed > 1.0e-2f;
        wind.Speed = std::max(b.WindSpeed, 0.0f);
        wind.GustStrength = Saturate(b.WindGustStrength);
        wind.TurbulenceIntensity = std::max(b.WindTurbulence, 0.0f);

        // ── Fog (weather-facing subset; Mode/scattering/noise stay authored) ──
        auto& fog = scene.GetFogSettings();
        fog.Enabled = b.FogEnabled;
        fog.Density = std::max(b.FogDensity, 0.0f);
        fog.Color = b.FogColor;
        fog.HeightFalloff = std::max(b.FogHeightFalloff, 0.0f);
        fog.MaxOpacity = Saturate(b.FogMaxOpacity);

        // ── Precipitation ──
        auto& precip = scene.GetPrecipitationSettings();
        const auto kind = static_cast<PrecipitationType>(b.PrecipitationKind);
        if (b.PrecipitationEnabled && precip.Type != kind)
        {
            // Type switch: take the renderer's coherent per-type parameter
            // block (particle sizes/speeds/physics), then re-apply the
            // scene-environment fields the director must not stomp.
            PrecipitationSettings defaults = PrecipitationSettings::GetDefaultsForType(kind);
            defaults.GroundY = precip.GroundY;
            defaults.GroundCollisionEnabled = precip.GroundCollisionEnabled;
            defaults.LODNearDistance = precip.LODNearDistance;
            defaults.LODFarDistance = precip.LODFarDistance;
            defaults.FrameBudgetMs = precip.FrameBudgetMs;
            defaults.MaxParticlesNearField = precip.MaxParticlesNearField;
            defaults.MaxParticlesFarField = precip.MaxParticlesFarField;
            precip = defaults;
        }
        precip.Enabled = b.PrecipitationEnabled;
        precip.Intensity = Saturate(b.PrecipitationIntensity);

        // ── Snow accumulation ──
        auto& snowAcc = scene.GetSnowAccumulationSettings();
        snowAcc.Enabled = b.SnowAccumulationEnabled;
        snowAcc.AccumulationRate = std::max(b.SnowAccumulationRate, 0.0f);

        // ── Push to the Renderer3D global twins ──
        // The render pipeline's per-frame system updates (WindSystem,
        // PrecipitationSystem, SnowAccumulationSystem, the fog UBO upload)
        // read the Renderer3D globals, which the editor only syncs from the
        // scene at load time — so the director pushes every tick. This also
        // closes the standalone-runtime gap where nothing ever synced them.
        Renderer3D::GetWindSettings() = wind;
        Renderer3D::GetFogSettings() = fog;
        Renderer3D::GetPrecipitationSettings() = precip;
        Renderer3D::GetSnowAccumulationSettings() = snowAcc;
    }
} // namespace OloEngine
