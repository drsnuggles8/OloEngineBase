// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// DriftLegWeatherAndSeaStateTest — Functional Test.
//
// Cross-subsystem seam under test (issue #882, part of the #878 Drift epic):
//   Scene tick × LuaScriptEngine × the SHIPPED
//   `Assets/Scripts/LuaScripts/DriftWeatherDirector.lua` × WeatherSystem's
//   preset cross-blend × the scene-level WindSettings the blend writes ×
//   WaterComponent's Gerstner sea-state fields × TimeOfDayComponent's clock.
//
// This test runs the REAL script file out of the sandbox project, not a
// re-implementation of it. That is deliberate. The failure this guards against
// is the two-mirrors one: a C++ mirror of the wind→sea curve would keep passing
// while the shipped Lua drifted away from it, and a Lua test with its own inline
// copy of the curve would only ever prove the copy consistent with itself. The
// content is the thing under test, so the content is what gets loaded.
//
// What this asserts:
//   1. The leg machine advances on LANDFALL, not on a timer: the boat has to
//      clear the departure radius (arm) and then close the island before the
//      weather retargets. A leg change with the boat parked offshore would mean
//      the arming half is dead.
//   2. Each leg drives a DIFFERENT weather state, and the state settles.
//   3. The sea tracks the wind, in the right DIRECTION and monotonically:
//      Clear (2 m/s) < Overcast (5 m/s) < Storm (14 m/s) on wave amplitude and
//      wave speed, and the foam threshold moves the opposite way (whitecaps
//      start lower down the wave as it builds). This is the "sea state visibly
//      tracks wind" acceptance criterion, in the only form a headless test can
//      hold: the numbers the water shader reads.
//   4. The sea EASES rather than snapping — one tick after the weather has been
//      retargeted to Storm the sea is still nearer its old state than its new
//      one. A coupling written as a direct assignment would pass (3) and fail
//      this, and it is the difference between a transition and a cut.
//   5. The clock warps FORWARD toward the leg's hour, and a leg whose hour
//      equals the current one does not warp a whole day (the off-by-24 that a
//      naive "always positive" wrap produces on leg 1).
//
// Headless: no GL context, no physics bodies. Everything asserted here is a
// CPU-observable component field. The frame this produces is verified
// separately, and visually, by DriftWeatherVisualEvidenceTest.
// =============================================================================

#include "DriftScenePresets.h"
#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>
#include <filesystem>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    namespace fs = std::filesystem;

    // Mirrors Drift.olo: the terrain tile's origin is a CORNER, so its centre
    // is translation + half the world size (480 / 2).
    constexpr f32 kIslandHalfSize = 240.0f;
    constexpr glm::vec3 kIslandOrigin{ -240.0f, -70.0f, 60.0f };
    const glm::vec3 kIslandCentre{ kIslandOrigin.x + kIslandHalfSize, 0.0f,
                                   kIslandOrigin.z + kIslandHalfSize };

    // The script's own radii. Offshore is beyond kDepartureRadius (300);
    // landfall is inside kLandfallRadius (210).
    const glm::vec3 kOffshore{ kIslandCentre.x, 0.0f, kIslandCentre.z - 550.0f };
    const glm::vec3 kAtIsland{ kIslandCentre.x, 0.0f, kIslandCentre.z - 40.0f };

    // Coarse steps: nothing here is physics, and the script's slowest constant
    // is the 25 s sea-state ease, so a 20 Hz tick resolves every transition it
    // has with three orders of magnitude to spare.
    constexpr f32 kDt = 0.05f;
    // Weather cross-blend (14 s) + several sea-state time constants (25 s).
    constexpr f32 kSettleSeconds = 120.0f;
} // namespace

class DriftLegWeatherAndSeaStateTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        Scene& scene = GetScene();

        // ── The sea ──
        m_Sea = scene.CreateEntity("Sea");
        auto& water = m_Sea.AddComponent<WaterComponent>();
        // Drift.olo's authored (moderate) values: the script must move these,
        // and starting from the middle anchor means a wrong-direction coupling
        // shows up as a failure in BOTH directions rather than one.
        water.m_WaveAmplitude = 0.12f;
        water.m_WaveSpeed = 1.0f;
        water.m_FoamHeightStart = 0.16f;
        water.m_FoamBrightness = 1.1f;

        // ── The island and the boat: the leg machine's only two inputs ──
        m_Island = scene.CreateEntity("Island");
        m_Island.GetComponent<TransformComponent>().Translation = kIslandOrigin;

        m_Boat = scene.CreateEntity("Boat");
        m_Boat.GetComponent<TransformComponent>().Translation = kOffshore;

        // ── The atmosphere director ──
        m_Atmosphere = scene.CreateEntity("Atmosphere");
        auto& tod = m_Atmosphere.AddComponent<TimeOfDayComponent>();
        tod.m_TimeOfDayHours = 5.2f; // Drift.olo's authored clock == leg 1's hour
        tod.m_DayLengthMinutes = 30.0f;
        auto& weather = m_Atmosphere.AddComponent<WeatherStateComponent>();
        weather.m_CurrentState = WeatherStateId::Clear;
        weather.m_TargetState = WeatherStateId::Clear;
        // Drift.olo's authored presets, from the shared table — the wind
        // speeds in it are what the shipped script maps to a sea state, so
        // this test and the golden captures must read the same numbers.
        OloEngine::Tests::ApplyDriftWeatherPresets(weather);
        m_Atmosphere.AddComponent<CloudscapeComponent>();

        EnableLua();
        RegisterLuaScript(m_Atmosphere, ScriptPath());
    }

    [[nodiscard]] static fs::path ScriptPath()
    {
        return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets" / "Scripts" /
               "LuaScripts" / "DriftWeatherDirector.lua";
    }

    [[nodiscard]] WaterComponent& Water()
    {
        return m_Sea.GetComponent<WaterComponent>();
    }

    [[nodiscard]] WeatherStateComponent& Weather()
    {
        return m_Atmosphere.GetComponent<WeatherStateComponent>();
    }

    [[nodiscard]] TimeOfDayComponent& Clock()
    {
        return m_Atmosphere.GetComponent<TimeOfDayComponent>();
    }

    // Sail out to the island: arm the leg machine offshore, then close it.
    void MakeLandfall()
    {
        m_Boat.GetComponent<TransformComponent>().Translation = kOffshore;
        TickFor(0.5f, kDt); // a few ticks beyond the departure radius: armed
        m_Boat.GetComponent<TransformComponent>().Translation = kAtIsland;
        TickFor(0.2f, kDt); // inside the landfall radius: the leg ends
    }

    Entity m_Atmosphere;
    Entity m_Sea;
    Entity m_Island;
    Entity m_Boat;
};

TEST_F(DriftLegWeatherAndSeaStateTest, LegsRetargetWeatherAndTheSeaFollowsTheWind)
{
    ASSERT_TRUE(fs::exists(ScriptPath()))
        << "the shipped director script is missing: " << ScriptPath().string();

    // ── Leg 1: Clear ────────────────────────────────────────────────────────
    TickFor(kSettleSeconds, kDt);
    EXPECT_EQ(Weather().m_TargetState, WeatherStateId::Clear);
    EXPECT_EQ(Weather().m_CurrentState, WeatherStateId::Clear)
        << "leg 1's transition never settled";

    // (5) Leg 1's hour equals the authored clock (both 5.2), so the warp must
    // be a no-op rather than a 24 h sweep. 120 s of a 30-minute game day is 96
    // game minutes of ordinary clock drift; a wrapped warp would have run the
    // clock through a whole day inside the first 18 s and landed somewhere else
    // entirely. Keep this constant in step with kLegs[1].hour in the script —
    // they are the same number for the same reason.
    EXPECT_NEAR(Clock().m_TimeOfDayHours, 5.2f + 120.0f / 1800.0f * 24.0f, 0.35f)
        << "leg 1 warped the clock instead of leaving it to drift — the "
           "equal-hour case wrapped a full day";

    const f32 clearAmplitude = Water().m_WaveAmplitude;
    const f32 clearWaveSpeed = Water().m_WaveSpeed;
    const f32 clearFoamStart = Water().m_FoamHeightStart;
    const f32 clearFoamBrightness = Water().m_FoamBrightness;
    const f32 clearSpecular = Water().m_SpecularIntensity;

    // The Clear preset's 2 m/s is the calm anchor, so the sea should have come
    // DOWN from the moderate state the scene authors.
    EXPECT_LT(clearAmplitude, 0.12f)
        << "the sea did not fall toward the calm anchor under a 2 m/s wind";
    EXPECT_NEAR(clearAmplitude, 0.05f, 0.01f);

    // ── Leg 2: Overcast ─────────────────────────────────────────────────────
    MakeLandfall();
    EXPECT_EQ(Weather().m_TargetState, WeatherStateId::Overcast)
        << "landfall did not advance the leg";

    TickFor(kSettleSeconds, kDt);
    const f32 overcastAmplitude = Water().m_WaveAmplitude;
    const f32 overcastWaveSpeed = Water().m_WaveSpeed;
    const f32 overcastFoamStart = Water().m_FoamHeightStart;

    // ── Leg 3: Storm ────────────────────────────────────────────────────────
    MakeLandfall();
    EXPECT_EQ(Weather().m_TargetState, WeatherStateId::Storm);

    // (4) The sea must EASE. One tick past the retarget the wind has barely
    // begun to build (the blend itself takes 14 s) and the sea lags it by
    // another 25 s, so the surface is still far nearer where it was than where
    // it is going. A direct wind→amplitude assignment would already be moving
    // hard here.
    const f32 stormFirstTick = Water().m_WaveAmplitude;
    EXPECT_LT(std::abs(stormFirstTick - overcastAmplitude), 0.02f)
        << "the sea jumped on the leg change instead of easing (was "
        << overcastAmplitude << ", one tick later " << stormFirstTick << ")";

    TickFor(kSettleSeconds, kDt);
    const f32 stormAmplitude = Water().m_WaveAmplitude;
    const f32 stormWaveSpeed = Water().m_WaveSpeed;
    const f32 stormFoamStart = Water().m_FoamHeightStart;
    const f32 stormFoamBrightness = Water().m_FoamBrightness;
    const f32 stormSpecular = Water().m_SpecularIntensity;

    // (3) Monotonic in the right direction across all three states.
    EXPECT_LT(clearAmplitude, overcastAmplitude);
    EXPECT_LT(overcastAmplitude, stormAmplitude);
    EXPECT_LT(clearWaveSpeed, overcastWaveSpeed);
    EXPECT_LT(overcastWaveSpeed, stormWaveSpeed);

    // Whitecaps: the threshold falls as the sea builds, so foam appears lower
    // down the wave. Opposite sign to the amplitude — a coupling that drove
    // every field through the same lerp with the same sign would fail here.
    EXPECT_GT(clearFoamStart, overcastFoamStart);
    EXPECT_GT(overcastFoamStart, stormFoamStart);

    // The other two fields the coupling drives, and the two Lua bindings added
    // for it: whitecaps burn brighter and the specular track breaks up as the
    // sea builds. Asserted here because these bindings have no other coverage —
    // a setter that silently dropped the write (a value outside its clamp, a
    // typo'd property name that Sol2 would happily create as a new table key)
    // would leave the shipped script quietly doing nothing.
    EXPECT_GT(stormFoamBrightness, clearFoamBrightness)
        << "foamBrightness did not rise with the sea state";
    EXPECT_LT(stormSpecular, clearSpecular)
        << "specularIntensity did not fall with the sea state";

    // The rough anchor, reached.
    EXPECT_NEAR(stormAmplitude, 0.22f, 0.02f);
    EXPECT_GT(GetScene().GetWindSettings().Speed, 10.0f)
        << "the storm preset's wind never reached the scene settings";

    // (2) Three legs, three distinct states, and the storm one actually rains.
    EXPECT_TRUE(GetScene().GetPrecipitationSettings().Enabled)
        << "the storm state produced no precipitation";
}

TEST_F(DriftLegWeatherAndSeaStateTest, ALegWhoseHourHasAlreadyPassedDoesNotWarpAWholeDay)
{
    // The regression this exists for: the clock free-runs during a leg, so by
    // the time a leg ENDS the clock can already be a little PAST the next
    // leg's target hour. "Forward around a 24 h clock" then means all the way
    // round, and the director would sweep the sun through a whole day in the
    // 18 s warp — the single most jarring thing it could do, in the one place
    // nobody would think to look. A guard that only special-cases an EXACT
    // match (which is what leg 1 needs) sails straight past this.
    //
    // Set the clock just past leg 2's 12.0 h, then trigger the landfall that
    // selects leg 2.
    TickFor(1.0f, kDt);
    Clock().m_TimeOfDayHours = 12.05f;
    MakeLandfall();
    ASSERT_EQ(Weather().m_TargetState, WeatherStateId::Overcast);

    // Half a warp in. With the bug the clock is ~12 h ahead of where it should
    // be (smoothstep(0.5) x 23.95 h added, wrapping to just past midnight);
    // without it the clock has only drifted at its ordinary rate.
    TickFor(9.0f, kDt);
    const f32 expectedDrift = 12.05f + 10.7f / 1800.0f * 24.0f; // ~12.19 h
    EXPECT_NEAR(Clock().m_TimeOfDayHours, expectedDrift, 0.2f)
        << "the clock warped toward a target it had already passed, sweeping "
           "a full day (clock is "
        << Clock().m_TimeOfDayHours << " h)";
}

TEST_F(DriftLegWeatherAndSeaStateTest, LegDoesNotAdvanceWhileTheBoatStaysOffshore)
{
    // (1) The arming half. The boat never closes the island, so however long
    // this runs the leg must not turn over — the failsafe timeout is 300 s and
    // is deliberately not exercised here.
    TickFor(200.0f, kDt);
    EXPECT_EQ(Weather().m_TargetState, WeatherStateId::Clear)
        << "the leg advanced without a landfall — the distance gate is not "
           "actually gating";
}
