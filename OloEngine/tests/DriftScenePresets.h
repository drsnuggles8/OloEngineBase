#pragma once

// =============================================================================
// DriftScenePresets.h — the weather presets Drift.olo authors, in one place.
//
// TWO test fixtures need this table and they need it for opposite reasons:
//
//   * DriftWeatherVisualEvidenceTest (L8) renders it, so its goldens depict the
//     weather the scene actually produces;
//   * DriftLegWeatherAndSeaStateTest (Functional) asserts against the WIND
//     SPEEDS in it, because those are what the shipped
//     DriftWeatherDirector.lua maps to a sea state.
//
// They had a copy each. That is the two-mirrors failure this repo keeps
// re-learning: the copies can disagree, and when they do NOTHING fails — the
// goldens still match their own captures and the functional assertions still
// match their own numbers, while the two tests quietly describe different
// weather and neither is checking the scene any more. Flagged by review on
// #922; one definition now.
//
// This is DUPLICATED FROM Drift.olo and nothing enforces the pairing, so it is
// still a mirror — just a single one. The rationale for each value lives in the
// scene's own comments (why Clear has no fog, why Storm's density is 0.0034 and
// not the engine preset's 0.009, why Overcast is deliberately dry); this file
// deliberately does not repeat it, because a comment copied twice rots twice.
//
// NOT the sea-state anchors. Those stay quoted separately inside the visual
// test on purpose — they are the OUTPUT of the Lua curve, and a shared constant
// would let a bug in the curve bless its own golden. This file holds authored
// INPUT data, which has no curve to guard.
// =============================================================================

#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    // Mirrors the WeatherStateComponent block on Drift.olo's Atmosphere entity.
    // Only the three states Drift actually targets are authored — Rain, Snow and
    // FogBank keep their engine defaults, exactly as the scene leaves them.
    inline void ApplyDriftWeatherPresets(WeatherStateComponent& w)
    {
        w.m_TransitionDuration = 14.0f;
        w.m_WetnessRiseRate = 0.2f;
        w.m_WetnessDryRate = 0.03f;

        auto& clear = w.m_PresetClear;
        clear.CloudCoverage = 0.18f;
        clear.CloudDensity = 0.5f;
        clear.CloudTypeBlend = 0.8f;
        clear.CloudWetness = 0.0f;
        clear.FogEnabled = false;
        clear.FogDensity = 0.0f;
        clear.FogColor = glm::vec3(0.66f, 0.76f, 0.86f);
        clear.FogHeightFalloff = 0.0f;
        clear.FogMaxOpacity = 0.85f;
        clear.WindSpeed = 2.0f;
        clear.WindGustStrength = 0.1f;
        clear.WindTurbulence = 0.2f;
        clear.PrecipitationEnabled = false;
        clear.PrecipitationKind = WeatherPrecipitationType::Rain;
        clear.PrecipitationIntensity = 0.0f;
        clear.SnowAccumulationEnabled = false;
        clear.SnowAccumulationRate = 0.0f;
        clear.SunDimming = 0.0f;
        clear.WetnessTarget = 0.0f;

        auto& overcast = w.m_PresetOvercast;
        overcast.CloudCoverage = 0.8f;
        overcast.CloudDensity = 0.75f;
        overcast.CloudTypeBlend = 0.2f;
        overcast.CloudWetness = 0.25f;
        overcast.FogEnabled = true;
        overcast.FogDensity = 0.0016f;
        overcast.FogColor = glm::vec3(0.62f, 0.66f, 0.72f);
        overcast.FogHeightFalloff = 0.0f;
        overcast.FogMaxOpacity = 0.9f;
        overcast.WindSpeed = 5.0f;
        overcast.WindGustStrength = 0.28f;
        overcast.WindTurbulence = 0.4f;
        overcast.PrecipitationEnabled = false;
        overcast.PrecipitationKind = WeatherPrecipitationType::Rain;
        overcast.PrecipitationIntensity = 0.0f;
        overcast.SnowAccumulationEnabled = false;
        overcast.SnowAccumulationRate = 0.0f;
        overcast.SunDimming = 0.4f;
        overcast.WetnessTarget = 0.15f;

        auto& storm = w.m_PresetStorm;
        storm.CloudCoverage = 0.97f;
        storm.CloudDensity = 1.0f;
        storm.CloudTypeBlend = 0.85f;
        storm.CloudWetness = 0.9f;
        storm.FogEnabled = true;
        storm.FogDensity = 0.0034f;
        storm.FogColor = glm::vec3(0.4f, 0.44f, 0.5f);
        storm.FogHeightFalloff = 0.0f;
        storm.FogMaxOpacity = 0.92f;
        storm.WindSpeed = 14.0f;
        storm.WindGustStrength = 0.85f;
        storm.WindTurbulence = 0.9f;
        // Explicit, though MakeDefaultWeatherPreset(Storm) already sets both.
        // DriftLegWeatherAndSeaStateTest asserts that the storm leg precipitates,
        // and an assertion that passes only because an unrelated engine default
        // happens to agree is one that starts failing for a reason nobody can
        // trace back to this file.
        storm.PrecipitationEnabled = true;
        storm.PrecipitationKind = WeatherPrecipitationType::Rain;
        storm.PrecipitationIntensity = 1.0f;
        storm.SnowAccumulationEnabled = false;
        storm.SnowAccumulationRate = 0.0f;
        storm.SunDimming = 0.75f;
        storm.WetnessTarget = 1.0f;
    }

    // Mirrors the hull, buoyancy and rig blocks on Drift.olo's Boat entity
    // (issue #899). Same one-definition rule, and the same caveat, as the
    // weather table above: nothing enforces the pairing with the scene, so the
    // rationale for each number stays in the scene's own comments and this file
    // carries only the values.
    //
    // What it is FOR: the three numbers that decide whether Drift is playable
    // under sail - the hull's ForwardDrag, BuoyancyComponent's LinearDrag and
    // SailComponent's SailArea - are a single ratio, and each of the three is a
    // plausible-looking thing to tidy up in isolation. SailTest's Drift cases
    // sail this exact rig in the exact wind speeds the weather table above
    // authors, so a tidy-up that becalms the game fails a test instead of being
    // discovered by playing it.
    namespace DriftBoat
    {
        inline constexpr f32 kMass = 2600.0f;
        inline constexpr glm::vec3 kColliderHalfExtents{ 1.0f, 0.4f, 2.5f };
    } // namespace DriftBoat

    inline void ApplyDriftBuoyancy(BuoyancyComponent& b)
    {
        b.m_Enabled = true;
        b.m_ProbeExtents = glm::vec3(1.35f, 0.45f, 2.5f);
        b.m_FluidDensity = 1000.0f;
        b.m_BuoyancyScale = 1.0f;
        b.m_LinearDrag = 0.35f;
        b.m_AngularDrag = 2.2f;
        b.m_SubmergenceRamp = 0.8f;
    }

    inline void ApplyDriftHull(BoatComponent& c)
    {
        c.m_Enabled = true;
        c.m_MaxThrust = 900.0f;
        c.m_ThrustOffsetZ = -2.2f;
        c.m_ThrustOffsetY = -0.55f;
        c.m_MaxRudderTorque = 5400.0f;
        c.m_RudderAuthoritySpeed = 7.0f;
        c.m_LateralDrag = 3.5f;
        c.m_ForwardDrag = 0.12f;
        c.m_YawDrag = 2.2f;
        c.m_ImmersionDepth = 0.3f;
        c.m_ThrottleInput = 0.0f;
        c.m_SteerInput = 0.0f;
    }

    inline void ApplyDriftRig(SailComponent& s)
    {
        s.m_Enabled = true;
        s.m_SailArea = 260.0f;
        s.m_AirDensity = 1.225f;
        s.m_MaxNormalCoefficient = 1.5f;
        s.m_MaxYardAngleDeg = 45.0f;
        s.m_TrimRateDeg = 28.0f;
        s.m_CentreOfEffortY = 2.0f;
        s.m_CentreOfEffortZ = -0.36f;
        s.m_AutoTrim = true;
        s.m_TrimInput = 0.0f;
        s.m_SailSetInput = 1.0f;
    }
} // namespace OloEngine::Tests
