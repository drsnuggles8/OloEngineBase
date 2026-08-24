#include "OloEnginePCH.h"

// =============================================================================
// SailTest — Functional Test.
//
// OLO_TEST_LAYER: Functional
//
// Cross-subsystem seam under test:
//   SailComponent (authored ECS data) x SailSystem's aerodynamic model x the
//   scene-level WindSettings (through WindSystem's analytical CPU query) x
//   BuoyancySystem x Jolt, all driven through a real Scene::OnUpdateRuntime.
//
// Issue #899: Drift's boat was a sailing ship pushed by an invisible engine.
// BoatSystem owns the water; this slice adds the air, so the wind is what moves
// the boat and the sail is what the wind acts on.
//
// docs/agent-rules/force-model-vehicles.md opens with the reason this file is
// long: EVERY bug in a force model leaves the suite green unless the scenario is
// built to discriminate it. So each assertion below is paired with the baseline
// it is measured against, and the negative cases are first-class:
//
//   * a becalmed boat drifts < 0.3 m, which is the yardstick every "the sail
//     drove it" margin is quoted against;
//   * a beam reach drives the hull along its OWN forward axis by metres;
//   * HEAD TO WIND it does not — that is the no-go zone, and it is the single
//     assertion that separates a real sail from "always AddForce along +Z";
//   * the yard braces to LEEWARD, with the sign flipping when the wind side
//     does. A mirrored trim model reproduces every magnitude in here and fails
//     only this;
//   * the hull HEELS to leeward, where an unpowered hull stays upright. This is
//     what Drift's script-side roll fake existed to imitate;
//   * apparent wind FALLS as the boat runs off downwind, which is what stops a
//     boat outrunning the breeze;
//   * a yawed hull sails along its own axis, not a hard-coded world one.
//
// The wave clock is frozen wherever a deterministic surface matters, and the
// wind's gust modulation is switched off in the fixture for the same reason —
// mirroring WaterBuoyancyTest / BoatTest.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include "DriftScenePresets.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // The same 2 x 1 x 4 m, 2000 kg hull BoatTest uses, so the two files'
    // margins are directly comparable and the buoyancy behaviour is already
    // characterised: the probe box matches the collider, displacing 8000 kg at
    // full immersion, so the hull settles around quarter-immersed with its
    // origin near the waterline.
    constexpr glm::vec3 kHullHalfExtents{ 1.0f, 0.5f, 2.0f };
    constexpr f32 kHullMass = 2000.0f;
    constexpr f32 kWaterPlaneY = 0.0f;

    // The rig on that hull. Big enough that the travel margins below are metres
    // rather than centimetres — this file tests the MODEL, and a rig tuned for a
    // particular game's pacing would only make every threshold a judgement call.
    // Drift's own numbers live in Drift.olo, where they belong.
    //
    // But NOT arbitrarily big, and the first draft's 260 m^2 is worth recording
    // as the trap it was. A rig that overpowers the hull drives it straight past
    // the yard limit's speed cap in under a second, after which the sail is
    // ABACK and braking hard — so the over-canvassed boat covered LESS ground
    // than a reefed one over the same window, and the reefing test failed while
    // the model was behaving correctly. 70 m^2 settles this hull comfortably
    // below the cap, which is the regime every margin here assumes.
    constexpr f32 kSailArea = 70.0f;
    // Deliberately a tall rig: heel scales with this, and the point of the heel
    // assertions is to see it clearly.
    constexpr f32 kCentreOfEffortY = 4.0f;
    constexpr f32 kTrueWindSpeed = 8.0f;
} // namespace

class SailTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}

    void TearDown() override
    {
        Time::ClearMockTime();
        FunctionalTest::TearDown();
    }

    /// A flat, still water tile so rest height and immersion are deterministic
    /// and the assertions are about the WIND rather than about which wave crest
    /// the hull happened to be on.
    ///
    /// SIZED SO A FAST BOAT CANNOT SAIL OFF IT. At 800 m across, a boat making
    /// 9 m/s in the storm case ran out of sea inside one 45 s measurement window,
    /// and what happens then is not a crash: BoatSystem finds no water volume so
    /// the keel and rudder quietly stop existing, buoyancy stops holding her up,
    /// and the run reports a speed of nearly zero. Read off the assertion that
    /// failed, that says "the gale is slower than the breeze" — which is a
    /// statement about the RIG, and points nowhere near the edge of the map.
    /// The surface is analytic, so a big one costs nothing.
    Entity SpawnFlatWater()
    {
        Entity water = GetScene().CreateEntity("Water");
        water.GetComponent<TransformComponent>().Translation = { 0.0f, kWaterPlaneY, 0.0f };
        auto& wc = water.AddComponent<WaterComponent>();
        wc.m_Enabled = true;
        wc.m_WorldSizeX = 6000.0f;
        wc.m_WorldSizeZ = 6000.0f;
        wc.m_WaveAmplitude = 0.0f;
        return water;
    }

    /// Steady wind blowing TOWARD `direction` at `speed`. Gusts and turbulence
    /// are off: a sine-modulated wind would make every margin in this file a
    /// function of where in the gust cycle the window happened to close.
    void SetWind(const glm::vec3& direction, f32 speed = kTrueWindSpeed)
    {
        WindSettings& wind = GetScene().GetWindSettings();
        wind.Enabled = true;
        wind.Direction = glm::normalize(direction);
        wind.Speed = speed;
        wind.GustStrength = 0.0f;
        wind.GustFrequency = 0.0f;
        wind.TurbulenceIntensity = 0.0f;
    }

    void SetNoWind()
    {
        WindSettings& wind = GetScene().GetWindSettings();
        wind.Enabled = false;
        wind.Speed = 0.0f;
        wind.GustStrength = 0.0f;
    }

    /// A floating hull carrying a rig. `yawDegrees` rotates it about world up so
    /// a test can prove the drive follows the HULL's own forward axis rather
    /// than a hard-coded world one.
    Entity SpawnSailingBoat(const glm::vec3& pos, f32 yawDegrees = 0.0f)
    {
        Entity boat = GetScene().CreateEntity("Boat");
        auto& tc = boat.GetComponent<TransformComponent>();
        tc.Translation = pos;
        tc.SetRotationEuler(glm::vec3(0.0f, glm::radians(yawDegrees), 0.0f));

        auto& rb = boat.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = kHullMass;
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;

        boat.AddComponent<BoxCollider3DComponent>().m_HalfExtents = kHullHalfExtents;

        auto& buoyancy = boat.AddComponent<BuoyancyComponent>();
        buoyancy.m_ProbeExtents = kHullHalfExtents;
        buoyancy.m_FluidDensity = 1000.0f;
        buoyancy.m_SubmergenceRamp = 1.0f;
        buoyancy.m_LinearDrag = 0.4f;
        buoyancy.m_AngularDrag = 2.0f;

        auto& sail = boat.AddComponent<SailComponent>();
        sail.m_SailArea = kSailArea;
        sail.m_CentreOfEffortY = kCentreOfEffortY;
        sail.m_CentreOfEffortZ = 0.0f; // no weather helm: keep the heading fixed
        sail.m_TrimRateDeg = 180.0f;   // settle the trim inside a short window
        return boat;
    }

    /// A keel. Without one the sail just shoves the hull bodily downwind — the
    /// lateral resistance is what turns side force into forward travel, exactly
    /// as on a real boat. BoatComponent already models it, so use it, with the
    /// propeller and rudder authored to zero so the SAIL is the only thing in
    /// the scene that can move the boat.
    static void AddKeel(Entity boat)
    {
        auto& hull = boat.AddComponent<BoatComponent>();
        hull.m_MaxThrust = 0.0f;
        hull.m_MaxRudderTorque = 0.0f;
        hull.m_LateralDrag = 4.0f;
        hull.m_ForwardDrag = 0.15f;
        hull.m_YawDrag = 2.0f;
        hull.m_ThrottleInput = 0.0f;
        hull.m_SteerInput = 0.0f;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    static glm::quat Rot(Entity e)
    {
        return e.GetComponent<TransformComponent>().GetRotation();
    }

    /// The hull's own horizontal forward axis (local +Z), normalized.
    static glm::vec3 ForwardDir(Entity e)
    {
        const glm::vec3 fwd = Rot(e) * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 flat(fwd.x, 0.0f, fwd.z);
        const f32 len = glm::length(flat);
        return (len > 1.0e-4f) ? flat / len : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    /// World-space starboard beam: forward x up, i.e. local -X for a +Z-forward
    /// hull. Reading it the other way round is exactly the bug issue #897 fixed.
    static glm::vec3 StarboardDir(Entity e)
    {
        const glm::vec3 f = ForwardDir(e);
        return { -f.z, 0.0f, f.x };
    }

    /// How far the mast leans toward `beam`. Positive means the masthead has
    /// gone that way, so the boat is heeled the OTHER way — a boat heeled to
    /// port has its mast over to starboard.
    static f32 MastLeanToward(Entity e, const glm::vec3& beam)
    {
        return glm::dot(Rot(e) * glm::vec3(0.0f, 1.0f, 0.0f), beam);
    }

    /// Let buoyancy bring the hull to its floating equilibrium before any wind
    /// is applied, so measured travel is propulsion and not the tail of the
    /// initial drop. The wind is held off during the settle for the same reason.
    void Settle()
    {
        Time::SetMockTime(0.0f); // frozen wave clock => a deterministic surface
        const bool wasEnabled = GetScene().GetWindSettings().Enabled;
        GetScene().GetWindSettings().Enabled = false;
        TickFor(3.0f);
        GetScene().GetWindSettings().Enabled = wasEnabled;
    }
};

// -----------------------------------------------------------------------------
// The baseline every margin below is quoted against: with the wind switched off
// the boat just floats. If a becalmed boat crept on its own, "the sail drove it"
// would prove nothing.
// -----------------------------------------------------------------------------
TEST_F(SailTest, BecalmedBoatDoesNotMove)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetNoWind();
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(4.0f);
    const glm::vec3 end = Pos(boat);

    ASSERT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_LT(glm::length(glm::vec2(end.x - start.x, end.z - start.z)), 0.3f)
        << "a becalmed boat drifted across the water";

    const auto& sail = boat.GetComponent<SailComponent>();
    EXPECT_TRUE(sail.m_Luffing) << "the sail claimed to be driving in a flat calm";
    EXPECT_FLOAT_EQ(sail.m_DriveForce, 0.0f);
    EXPECT_FLOAT_EQ(sail.m_HeelForce, 0.0f);
}

// -----------------------------------------------------------------------------
// A beam reach — wind square on the side — is the point of sail this whole
// model exists for: the drive is not the wind pushing the boat along, it is the
// sail's side force turned forward by the yard angle and held there by the keel.
// Measured against the < 0.3 m becalmed baseline above, metres of travel can
// only have come from SailSystem.
// -----------------------------------------------------------------------------
TEST_F(SailTest, BeamReachDrivesTheBoatForward)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    // Hull points along +Z; wind blows toward -X, so it comes FROM the port
    // beam (+X is port for a +Z-forward hull).
    SetWind({ -1.0f, 0.0f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    const glm::vec3 forwardAtStart = ForwardDir(boat);
    const glm::vec3 starboardAtStart = StarboardDir(boat);

    TickFor(8.0f);

    const glm::vec3 delta = Pos(boat) - start;
    ASSERT_TRUE(std::isfinite(delta.x) && std::isfinite(delta.y) && std::isfinite(delta.z));

    const f32 alongHull = glm::dot(delta, forwardAtStart);
    const f32 acrossHull = glm::dot(delta, starboardAtStart);
    GTEST_LOG_(INFO) << "beam reach over 8 s: headway=" << alongHull
                     << " m leeway=" << acrossHull << " m";

    EXPECT_GT(alongHull, 3.0f)
        << "a beam reach did not drive the boat forward; along=" << alongHull;
    // Some leeway is correct — a real boat makes some. What must not happen is
    // the boat being blown bodily downwind, which is what an implementation
    // that ignored the yard angle (force straight along the apparent wind)
    // would produce.
    EXPECT_GT(alongHull, std::abs(acrossHull))
        << "the boat made more leeway than headway; along=" << alongHull
        << " across=" << acrossHull;
    EXPECT_NEAR(Pos(boat).y, kWaterPlaneY, 1.0f)
        << "the sail drove the hull out of the water; y=" << Pos(boat).y;
}

// -----------------------------------------------------------------------------
// THE NO-GO ZONE. Head to wind there is no brace angle that produces drive, and
// the model has to say so by arithmetic rather than by a special case. Paired
// with the beam reach above: the same rig, the same wind speed, the same
// window, and the only difference is where the wind is coming from.
//
// This is the assertion an "always push along +Z" implementation fails and
// nothing else in this file would catch.
// -----------------------------------------------------------------------------
TEST_F(SailTest, HeadToWindTheSailCannotDrive)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    // Wind blowing toward -Z: dead on the bow of a +Z-forward hull.
    SetWind({ 0.0f, 0.0f, -1.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    const glm::vec3 forwardAtStart = ForwardDir(boat);

    TickFor(8.0f);

    const f32 alongHull = glm::dot(Pos(boat) - start, forwardAtStart);
    EXPECT_LT(alongHull, 0.5f) << "the boat sailed dead upwind; along=" << alongHull;

    const auto& sail = boat.GetComponent<SailComponent>();
    EXPECT_TRUE(sail.m_Luffing) << "the sail claimed to be driving head to wind";
    EXPECT_LE(sail.m_DriveForce, 0.0f) << "positive drive head to wind";
    // ...and the apparent wind is reported as coming from ahead, which is what
    // a "you are in irons" cue would key off.
    EXPECT_LT(std::abs(glm::degrees(sail.m_ApparentWindAngle)), 25.0f)
        << "apparent wind was not reported as being on the bow; deg="
        << glm::degrees(sail.m_ApparentWindAngle);
}

// -----------------------------------------------------------------------------
// The yard trims to the TACK, and the sign flips with the wind side. A model
// that mirrored the trim would reproduce every travel magnitude in this file
// exactly — the sail is symmetric — and fail only here. Same reason
// BoatTest.StarboardHelmMovesTheBoatToStarboardNotToPort exists.
//
// The invariant, and the only one worth reasoning from, is that the sail's
// NORMAL tilts to LEEWARD: n = cos(yard)*forward + sin(yard)*port. So wind from
// STARBOARD (leeward = port) needs sin(yard) > 0, i.e. a POSITIVE yard angle.
//
// Do not restate that as "braced to port" or "braced to starboard". A square
// yard has two ends and one of them is always going forward while the other
// goes aft, so the phrase has no agreed referent — which is exactly how the
// first draft of this test came to assert the mirror image of the truth while
// its own comment sounded authoritative. Issue #897 is the same mistake on the
// rudder. Assert the leeward-normal rule, name nothing else.
// -----------------------------------------------------------------------------
TEST_F(SailTest, TheYardTrimsToTheTackAndFlipsWithIt)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    // Wind blowing toward +X: from -X, which is the STARBOARD beam.
    SetWind({ 1.0f, 0.0f, 0.0f });
    EnablePhysics3D();

    Settle();
    TickFor(2.0f);

    {
        const auto& sail = boat.GetComponent<SailComponent>();
        GTEST_LOG_(INFO) << "starboard tack: yard(rad)=" << sail.m_YardAngle
                         << " apparent(deg)=" << glm::degrees(sail.m_ApparentWindAngle);
        EXPECT_GT(sail.m_YardAngle, 0.1f)
            << "wind from starboard did not tilt the sail's normal to leeward; yard(rad)="
            << sail.m_YardAngle;
        EXPECT_GT(sail.m_ApparentWindAngle, 0.0f)
            << "wind on the starboard beam was not reported as being to starboard";
    }

    // Now put the wind on the other side and the yard must swing the other way.
    SetWind({ -1.0f, 0.0f, 0.0f });
    TickFor(2.0f);

    {
        const auto& sail = boat.GetComponent<SailComponent>();
        GTEST_LOG_(INFO) << "port tack: yard(rad)=" << sail.m_YardAngle
                         << " apparent(deg)=" << glm::degrees(sail.m_ApparentWindAngle);
        EXPECT_LT(sail.m_YardAngle, -0.1f)
            << "wind from port did not tilt the sail's normal to leeward; yard(rad)="
            << sail.m_YardAngle;
        EXPECT_LT(sail.m_ApparentWindAngle, 0.0f)
            << "wind on the port beam was not reported as being to port";
    }
}

// -----------------------------------------------------------------------------
// The yard is hauled round, not teleported. Over a known window it may not move
// further than m_TrimRateDeg allows — which is what makes a tack read as a
// manoeuvre instead of the sail snapping to a solved value.
// -----------------------------------------------------------------------------
TEST_F(SailTest, TheYardSlewsAtItsAuthoredRate)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetWind({ 1.0f, 0.0f, 0.0f });
    boat.GetComponent<SailComponent>().m_TrimRateDeg = 10.0f;
    EnablePhysics3D();

    Settle();

    constexpr f32 kDt = 1.0f / 60.0f;
    constexpr f32 kWindow = 0.5f;
    TickFor(kWindow, kDt);

    const f32 yard = std::abs(boat.GetComponent<SailComponent>().m_YardAngle);
    // Ticks are quantised, so allow one extra step of slack rather than
    // asserting an exact product — the point is the BOUND, not the arithmetic.
    const f32 allowed = glm::radians(10.0f) * (kWindow + kDt);
    EXPECT_GT(yard, 0.0f) << "the yard did not move at all";
    EXPECT_LE(yard, allowed)
        << "the yard slewed faster than its authored rate; yard(rad)=" << yard
        << " allowed=" << allowed;
}

// -----------------------------------------------------------------------------
// HEEL, and it comes from wind pressure on a rig ABOVE the centre of mass —
// the thing Drift's script-side roll fake was standing in for. Paired with the
// same hull under no wind, which must stay upright: without the pair, "it is
// tilted" could just be the hull settling.
//
// Wind from starboard pushes the boat to PORT, and heeling is the whole boat
// rotating — mast included — so the masthead goes to PORT too. (The first draft
// of this test asserted starboard, on the strength of a sentence that sounded
// like naval terminology and was simply wrong. A masthead force to port about a
// centre of mass below it is a torque of -2.5*F_port about +Z, and the masthead
// moves the way it is pushed.)
//
// Asserting the SIGNED lean, not its magnitude, is what makes this catch a
// centre of effort that ended up below the waterline: that boat heels to
// WINDWARD and every magnitude-only assertion passes.
// -----------------------------------------------------------------------------
TEST_F(SailTest, WindPressureHeelsTheHullToLeeward)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetWind({ 1.0f, 0.0f, 0.0f }); // from the starboard beam
    EnablePhysics3D();

    Settle();
    const glm::vec3 starboardAtStart = StarboardDir(boat);
    const f32 leanBefore = MastLeanToward(boat, starboardAtStart);
    TickFor(6.0f);
    const f32 leanAfter = MastLeanToward(boat, starboardAtStart);

    ASSERT_TRUE(std::isfinite(leanAfter));
    GTEST_LOG_(INFO) << "mast lean toward starboard: before=" << leanBefore
                     << " after=" << leanAfter;
    EXPECT_LT(std::abs(leanBefore), 0.01f) << "the hull was not upright at the start";
    EXPECT_LT(leanAfter, -0.02f)
        << "wind on the starboard beam did not heel the boat to port; lean=" << leanAfter;
    // ...and the reported heel force agrees about the side: the boat is being
    // pushed to PORT, so m_HeelForce (positive = to starboard) is negative.
    EXPECT_LT(boat.GetComponent<SailComponent>().m_HeelForce, 0.0f);
}

TEST_F(SailTest, WithoutWindTheHullStaysUpright)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetNoWind();
    EnablePhysics3D();

    Settle();
    const glm::vec3 starboardAtStart = StarboardDir(boat);
    TickFor(6.0f);

    const f32 lean = MastLeanToward(boat, starboardAtStart);
    GTEST_LOG_(INFO) << "becalmed mast lean: " << lean;
    EXPECT_LT(std::abs(lean), 0.01f)
        << "a becalmed hull heeled on its own, so the heel test above proves nothing; lean="
        << lean;
}

// -----------------------------------------------------------------------------
// Apparent wind falls as the boat runs off downwind — the term that stops a
// boat outrunning the breeze. Measured against the boat's own first tick, when
// it is still stationary and the apparent wind is therefore the true wind.
// -----------------------------------------------------------------------------
TEST_F(SailTest, RunningDownwindReducesTheApparentWind)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    // Blowing toward +Z: straight up the stern of a +Z-forward hull.
    SetWind({ 0.0f, 0.0f, 1.0f });
    EnablePhysics3D();

    Settle();
    TickFor(1.0f / 60.0f);
    const f32 atRest = boat.GetComponent<SailComponent>().m_ApparentWindSpeed;
    EXPECT_NEAR(atRest, kTrueWindSpeed, 0.6f)
        << "a stationary boat did not feel the true wind; apparent=" << atRest;

    const glm::vec3 start = Pos(boat);
    TickFor(8.0f);

    const auto& sail = boat.GetComponent<SailComponent>();
    EXPECT_GT(Pos(boat).z - start.z, 3.0f) << "the boat did not run off downwind";
    EXPECT_LT(sail.m_ApparentWindSpeed, atRest - 1.0f)
        << "apparent wind did not fall as the boat ran downwind; apparent="
        << sail.m_ApparentWindSpeed << " at rest=" << atRest;
    EXPECT_GT(sail.m_ApparentWindSpeed, 0.0f);
    // Running dead downwind the yard is square, or nearly so.
    EXPECT_LT(std::abs(glm::degrees(sail.m_YardAngle)), 20.0f)
        << "the yard was not squared on a dead run; deg=" << glm::degrees(sail.m_YardAngle);
}

// -----------------------------------------------------------------------------
// The drive follows the HULL, not the world. Yawed +90 deg the boat's local +Z
// points along world +X, and a beam reach must move it in X. A model that
// pushed along a hard-coded world axis passes every unrotated test in this file
// and fails this one.
// -----------------------------------------------------------------------------
TEST_F(SailTest, DriveFollowsTheHullHeading)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f }, 90.0f);
    AddKeel(boat);
    // The hull now points along +X, so a wind blowing toward +Z is on its beam.
    SetWind({ 0.0f, 0.0f, 1.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(8.0f);

    const glm::vec3 delta = Pos(boat) - start;
    EXPECT_GT(delta.x, 3.0f)
        << "a boat yawed 90 deg did not sail along world +X; dx=" << delta.x;
    EXPECT_GT(delta.x, std::abs(delta.z))
        << "it made more leeway than headway; dx=" << delta.x << " dz=" << delta.z;
}

// -----------------------------------------------------------------------------
// The off switches, each of which must produce EXACTLY nothing. Cheap, and what
// stops a "disabled" sail from quietly still pushing — the sort of thing only
// noticed months later, as a boat that will not stop.
// -----------------------------------------------------------------------------
TEST_F(SailTest, DisabledSailProducesNoDrive)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetWind({ -1.0f, 0.0f, 0.0f });
    boat.GetComponent<SailComponent>().m_Enabled = false;
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(6.0f);

    EXPECT_LT(glm::length(glm::vec2(Pos(boat).x - start.x, Pos(boat).z - start.z)), 0.3f)
        << "a disabled sail still drove the boat";
    EXPECT_FLOAT_EQ(boat.GetComponent<SailComponent>().m_DriveForce, 0.0f);
}

TEST_F(SailTest, FullyReefedSailProducesNoDrive)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetWind({ -1.0f, 0.0f, 0.0f });
    boat.GetComponent<SailComponent>().m_SailSetInput = 0.0f; // bare poles
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(6.0f);

    EXPECT_LT(glm::length(glm::vec2(Pos(boat).x - start.x, Pos(boat).z - start.z)), 0.3f)
        << "a boat under bare poles still sailed";
    EXPECT_FLOAT_EQ(boat.GetComponent<SailComponent>().m_DriveForce, 0.0f);
}

// -----------------------------------------------------------------------------
// Reefing is a DIAL, not a switch: a third of the canvas must make appreciably
// less drive than all of it, over the same window and the same wind. Without
// this, m_SailSetInput could be treated as a bool and both tests above would
// still pass.
// -----------------------------------------------------------------------------
TEST_F(SailTest, ShorteningSailReducesDrive)
{
    SpawnFlatWater();
    Entity full = SpawnSailingBoat({ -60.0f, 0.5f, 0.0f });
    Entity reefed = SpawnSailingBoat({ 60.0f, 0.5f, 0.0f });
    AddKeel(full);
    AddKeel(reefed);
    reefed.GetComponent<SailComponent>().m_SailSetInput = 0.35f;
    SetWind({ -1.0f, 0.0f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 fullStart = Pos(full);
    const glm::vec3 reefedStart = Pos(reefed);
    TickFor(8.0f);

    const f32 fullRun = glm::dot(Pos(full) - fullStart, glm::vec3(0.0f, 0.0f, 1.0f));
    const f32 reefedRun = glm::dot(Pos(reefed) - reefedStart, glm::vec3(0.0f, 0.0f, 1.0f));

    GTEST_LOG_(INFO) << "full sail ran " << fullRun << " m, reefed ran " << reefedRun << " m";
    EXPECT_GT(fullRun, 3.0f) << "the full-sail control did not sail";
    EXPECT_GT(reefedRun, 0.0f) << "a reefed boat should still make way";
    EXPECT_LT(reefedRun, fullRun * 0.8f)
        << "shortening sail barely changed the drive; full=" << fullRun
        << " reefed=" << reefedRun;
}

// -----------------------------------------------------------------------------
// Manual trim is genuinely worse than auto trim when it is wrong — which is what
// makes the manual mode a skill mechanic rather than a cosmetic toggle. A yard
// braced hard the WRONG way on a beam reach must not out-sail the solved angle.
// -----------------------------------------------------------------------------
TEST_F(SailTest, BadlyTrimmedSailIsSlowerThanAutoTrim)
{
    SpawnFlatWater();
    Entity trimmed = SpawnSailingBoat({ -60.0f, 0.5f, 0.0f });
    Entity mistrimmed = SpawnSailingBoat({ 60.0f, 0.5f, 0.0f });
    AddKeel(trimmed);
    AddKeel(mistrimmed);
    // Wind from the port beam solves to a NEGATIVE yard angle (the sail's normal
    // has to tilt to starboard, which is leeward here). Brace it hard the other
    // way instead, which puts the wind on the back of the sail.
    auto& bad = mistrimmed.GetComponent<SailComponent>();
    bad.m_AutoTrim = false;
    bad.m_TrimInput = 1.0f;
    SetWind({ -1.0f, 0.0f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 trimmedStart = Pos(trimmed);
    const glm::vec3 mistrimmedStart = Pos(mistrimmed);
    TickFor(8.0f);

    const f32 goodRun = glm::dot(Pos(trimmed) - trimmedStart, glm::vec3(0.0f, 0.0f, 1.0f));
    const f32 badRun = glm::dot(Pos(mistrimmed) - mistrimmedStart, glm::vec3(0.0f, 0.0f, 1.0f));

    GTEST_LOG_(INFO) << "auto-trimmed ran " << goodRun << " m, mistrimmed ran " << badRun << " m";
    EXPECT_GT(goodRun, 3.0f) << "the auto-trimmed control did not sail";
    EXPECT_LT(badRun, goodRun)
        << "a badly braced yard sailed as well as a solved one; good=" << goodRun
        << " bad=" << badRun;
}

// -----------------------------------------------------------------------------
// Garbage in the authored fields must not reach Jolt. Every one of these can
// arrive from a corrupt scene file or a scripted write, and a NaN that gets as
// far as AddForce takes the body out of the world permanently.
// -----------------------------------------------------------------------------
TEST_F(SailTest, NonFiniteTunablesDoNotCorruptTheHull)
{
    SpawnFlatWater();
    Entity boat = SpawnSailingBoat({ 0.0f, 0.5f, 0.0f });
    AddKeel(boat);
    SetWind({ -1.0f, 0.0f, 0.0f });
    EnablePhysics3D();

    Settle();

    auto& sail = boat.GetComponent<SailComponent>();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    sail.m_SailArea = nan;
    sail.m_AirDensity = inf;
    sail.m_MaxNormalCoefficient = -inf;
    sail.m_MaxYardAngleDeg = nan;
    sail.m_TrimRateDeg = nan;
    sail.m_CentreOfEffortY = nan;
    sail.m_CentreOfEffortZ = inf;
    sail.m_TrimInput = nan;
    sail.m_SailSetInput = nan;
    sail.m_YardAngle = nan;

    TickFor(2.0f);

    const glm::vec3 p = Pos(boat);
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
        << "a non-finite sail tunable put the hull at a non-finite position";
    EXPECT_TRUE(std::isfinite(sail.m_YardAngle)) << "the yard angle stayed non-finite";
    EXPECT_TRUE(std::isfinite(sail.m_DriveForce) && std::isfinite(sail.m_HeelForce));
    EXPECT_NEAR(p.y, kWaterPlaneY, 2.0f) << "the hull left the water; y=" << p.y;
}

// =============================================================================
// Persistence. The failure both of these guard is the one
// docs/agent-rules/force-model-vehicles.md §10 describes: a dropped field
// SIMULATES PERFECTLY, it just simulates a differently-tuned boat, which reads
// as a design choice rather than as data loss.
//
// The runtime readouts are deliberately not persisted by either path, so they
// are not asserted here — SailSystem rewrites them on the first step after a
// load.
// =============================================================================

namespace
{
    void AuthorEveryField(SailComponent& s)
    {
        s.m_Enabled = false;
        s.m_SailArea = 123.5f;
        s.m_AirDensity = 1.1f;
        s.m_MaxNormalCoefficient = 1.75f;
        s.m_MaxYardAngleDeg = 62.0f;
        s.m_TrimRateDeg = 21.0f;
        s.m_CentreOfEffortY = 3.25f;
        s.m_CentreOfEffortZ = -0.75f;
        s.m_AutoTrim = false;
        s.m_TrimInput = -0.4f;
        s.m_SailSetInput = 0.6f;
    }

    void ExpectEveryField(const SailComponent& s)
    {
        constexpr f32 kEps = 1e-4f;
        EXPECT_FALSE(s.m_Enabled);
        EXPECT_NEAR(s.m_SailArea, 123.5f, kEps);
        EXPECT_NEAR(s.m_AirDensity, 1.1f, kEps);
        EXPECT_NEAR(s.m_MaxNormalCoefficient, 1.75f, kEps);
        EXPECT_NEAR(s.m_MaxYardAngleDeg, 62.0f, kEps);
        EXPECT_NEAR(s.m_TrimRateDeg, 21.0f, kEps);
        EXPECT_NEAR(s.m_CentreOfEffortY, 3.25f, kEps);
        EXPECT_NEAR(s.m_CentreOfEffortZ, -0.75f, kEps);
        EXPECT_FALSE(s.m_AutoTrim);
        EXPECT_NEAR(s.m_TrimInput, -0.4f, kEps);
        EXPECT_NEAR(s.m_SailSetInput, 0.6f, kEps);
    }
} // namespace

TEST_F(SailTest, SailComponentSurvivesASaveGameRoundTrip)
{
    Entity e = GetScene().CreateEntity("SailSaveGame");
    AuthorEveryField(e.AddComponent<SailComponent>());

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("SailSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<SailComponent>())
        << "SailComponent dropped by the save-game round-trip";
    ExpectEveryField(re.GetComponent<SailComponent>());
}

// The scene-YAML half. SailComponent is all-trivial, so OloHeaderTool generates
// both blocks — which means this test is really asking "did the generator see
// it at all?". A component the scan skipped would round-trip to defaults here
// while every behavioural test above still passed.
TEST_F(SailTest, SailComponentSurvivesASceneYamlRoundTrip)
{
    std::string yaml;
    {
        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);
        Entity e = scene->CreateEntity("SailYaml");
        AuthorEveryField(e.AddComponent<SailComponent>());
        yaml = SceneSerializer(scene).SerializeToYAML();
    }
    ASSERT_FALSE(yaml.empty());

    Ref<Scene> reloaded = Scene::Create();
    reloaded->SetRenderingEnabled(false);
    ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
        << "the scene carrying a SailComponent failed to deserialize";

    Entity re = reloaded->FindEntityByName("SailYaml");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<SailComponent>())
        << "SailComponent dropped by the scene-YAML round-trip";
    ExpectEveryField(re.GetComponent<SailComponent>());
}

// =============================================================================
// DRIFT'S OWN RIG (issue #899).
//
// Everything above tests the MODEL with numbers chosen to make the margins
// obvious. These cases test the GAME: the exact hull, buoyancy and rig
// Drift.olo authors, in the exact wind speeds its weather director produces,
// because that tuning is one ratio spread across three components and each
// third of it looks like an independent knob.
//
// The failure they exist to catch is not a crash. It is somebody lowering the
// sail area because 260 m^2 looks absurd for a 5 m boat (it is; the hull's drag
// is equally unphysical, and the pair was tuned together), or restoring
// BuoyancyComponent's LinearDrag to 0.8 because it reads as a bobbing damper
// and not as the surge damper it also is. Either one leaves every test above
// green and turns Drift into a boat that cannot cross its own bay.
//
// The thresholds are deliberately loose FRACTIONS of the true wind rather than
// absolute speeds: what must hold is that each weather state is a distinctly
// different boat and that all three are sailable, not that any of them hits a
// particular number.
// =============================================================================

namespace
{
    // The three wind speeds Drift's weather states blow at, from the same
    // one-definition table DriftLegWeatherAndSeaStateTest asserts against.
    [[nodiscard]] f32 DriftWindSpeed(WeatherStateId state)
    {
        WeatherStateComponent w;
        Tests::ApplyDriftWeatherPresets(w);
        switch (state)
        {
            case WeatherStateId::Overcast:
                return w.m_PresetOvercast.WindSpeed;
            case WeatherStateId::Storm:
                return w.m_PresetStorm.WindSpeed;
            default:
                return w.m_PresetClear.WindSpeed;
        }
    }
} // namespace

class DriftSailingTest : public SailTest
{
  protected:
    /// Drift's boat, built from the shared tuning table rather than from numbers
    /// retyped here. Spawned with the sail FURLED: a test measures one boat at a
    /// time and the others must not be sailing in the meantime.
    Entity SpawnDriftBoat(const glm::vec3& pos)
    {
        Entity boat = GetScene().CreateEntity("Boat");
        boat.GetComponent<TransformComponent>().Translation = pos;

        auto& rb = boat.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = Tests::DriftBoat::kMass;
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;

        boat.AddComponent<BoxCollider3DComponent>().m_HalfExtents =
            Tests::DriftBoat::kColliderHalfExtents;
        Tests::ApplyDriftBuoyancy(boat.AddComponent<BuoyancyComponent>());
        Tests::ApplyDriftHull(boat.AddComponent<BoatComponent>());
        Tests::ApplyDriftRig(boat.AddComponent<SailComponent>());
        boat.GetComponent<SailComponent>().m_Enabled = false;
        return boat;
    }

    /// Tick with SOMEONE AT THE HELM, holding `course`.
    ///
    /// This is not decoration. Drift's rig sits aft of the centre of mass, so a
    /// loaded sail gives the boat WEATHER HELM — the bow rounds up into the wind
    /// — and an unsteered boat therefore luffs, stalls, and reports a beam-reach
    /// speed of nearly zero. The first version of these cases measured exactly
    /// that and read as "the rig is far too weak"; worse, a BIGGER sail rounded
    /// the boat up FASTER and measured SLOWER, which points at the opposite of
    /// the truth. The boat was behaving correctly and the test was asking it to
    /// sail a course with nobody steering.
    ///
    /// A plain PD controller on the heading error is enough, and is what the
    /// player's hands do. The error term is sin(angle off course), taken as the
    /// component of the target heading along the hull's starboard beam, so a
    /// target to starboard asks for starboard helm — the sign convention
    /// BoatComponent documents.
    void HoldCourse(Entity boat, const glm::vec3& course, f32 seconds)
    {
        constexpr f32 kDt = 1.0f / 60.0f;
        constexpr f32 kP = 4.0f;
        constexpr f32 kD = 1.2f;
        const auto steps = static_cast<u32>(seconds / kDt);
        f32 previousError = 0.0f;
        for (u32 i = 0; i < steps; ++i)
        {
            const f32 error = glm::dot(course, StarboardDir(boat));
            const f32 rate = (i == 0) ? 0.0f : (error - previousError) / kDt;
            previousError = error;
            boat.GetComponent<BoatComponent>().m_SteerInput =
                std::clamp(kP * error + kD * rate, -1.0f, 1.0f);
            TickFor(kDt, kDt);
        }
    }

    /// Sail ONE boat on a beam reach and report the mean speed made good.
    ///
    /// Every measurement takes a FRESH boat, and that is load-bearing. Reusing one
    /// hull for three winds in a row cost several rounds of misdiagnosis: the
    /// third leg reported 0 m/s in a gale and read as a dead rig, when what had
    /// actually happened was that she left the previous leg with way on and a few
    /// degrees of heading error, rounded up under her own weather helm, and parked
    /// on the edge of the no-go zone — where a 45 deg yard limit makes the forward
    /// drive identically zero, and where the rudder cannot bear her away again
    /// because rudder authority needs forward speed she no longer has. All of that
    /// is the model being RIGHT. An attempt to reset the hull between legs instead
    /// of re-spawning it was worse than useless: the rotation write did not reach
    /// the transform (measured: heading -5.42 deg before and after), so the reset
    /// silently did nothing while reading as though it had.
    ///
    /// The course is DERIVED FROM THE WIND, never read off the hull: the wind blows
    /// toward -X, so +Z is square across it, which is what makes this a beam reach
    /// rather than whatever point of sail the hull happens to be lying on.
    f32 BeamReachSpeed(Entity boat, f32 windSpeed, f32 seconds = 45.0f)
    {
        constexpr glm::vec3 kCourse{ 0.0f, 0.0f, 1.0f };
        SetWind({ -1.0f, 0.0f, 0.0f }, windSpeed); // from the port beam
        boat.GetComponent<SailComponent>().m_Enabled = true;
        HoldCourse(boat, kCourse, seconds * 0.5f); // work up to terminal speed
        const glm::vec3 start = Pos(boat);
        HoldCourse(boat, kCourse, seconds * 0.5f);
        const f32 speed = glm::dot(Pos(boat) - start, kCourse) / (seconds * 0.5f);
        boat.GetComponent<SailComponent>().m_Enabled = false;

        // Fail with the RIGHT sentence when a run stops measuring sails. BOTH of
        // these have happened here and BOTH reported themselves as "the boat made
        // no speed", which reads as a weak rig and points nowhere:
        //   * sailing off the water tile — the keel, the rudder and buoyancy all
        //     silently stop existing, there being no water volume to find;
        //   * CAPSIZING — and an inverted hull still floats at the waterline, so a
        //     height check alone passes happily while the sail correctly makes no
        //     force at all, its own heel factor having gone to zero.
        const glm::vec3 up = Rot(boat) * glm::vec3(0.0f, 1.0f, 0.0f);
        GTEST_LOG_(INFO) << "  " << windSpeed << " m/s wind -> " << speed
                         << " m/s; y=" << Pos(boat).y << " upY=" << up.y
                         << " apparent(deg)="
                         << glm::degrees(boat.GetComponent<SailComponent>().m_ApparentWindAngle);
        EXPECT_NEAR(Pos(boat).y, kWaterPlaneY, 2.0f)
            << "the hull is not at the waterline — most likely it sailed off the "
            << "water tile; y=" << Pos(boat).y;
        EXPECT_GT(up.y, 0.5f)
            << "the hull capsized during the run, so this speed measures nothing; upY="
            << up.y;
        return speed;
    }
};

// Each weather state is a genuinely different boat, and every one of them sails.
// Thresholds are RATIOS between the legs rather than three absolute numbers,
// because what has to hold is that the wind is what decides the boat's speed.
//
// Measured on the shipped tuning at the time of writing: 0.63 / 2.6 / 9.6 m/s for
// Clear / Overcast / Storm. The gates sit well clear of those — this guards
// against the tuning being gutted, and is not a golden.
TEST_F(DriftSailingTest, EveryWeatherStateIsSailableAndTheyDiffer)
{
    SpawnFlatWater();
    Entity inCalm = SpawnDriftBoat({ -400.0f, 0.5f, 0.0f });
    Entity inBreeze = SpawnDriftBoat({ 0.0f, 0.5f, 0.0f });
    Entity inGale = SpawnDriftBoat({ 400.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    const f32 clearWind = DriftWindSpeed(WeatherStateId::Clear);
    const f32 breezeWind = DriftWindSpeed(WeatherStateId::Overcast);
    const f32 galeWind = DriftWindSpeed(WeatherStateId::Storm);

    const f32 calm = BeamReachSpeed(inCalm, clearWind);
    const f32 breeze = BeamReachSpeed(inBreeze, breezeWind);
    const f32 gale = BeamReachSpeed(inGale, galeWind);

    EXPECT_GT(calm, 0.0f) << "she could not move at all in the Clear leg's air";
    EXPECT_GT(breeze, calm * 2.0f)
        << "the Overcast breeze is barely different from a flat calm; calm=" << calm
        << " breeze=" << breeze;
    EXPECT_GT(gale, breeze * 2.0f)
        << "the Storm is barely different from the breeze; breeze=" << breeze
        << " gale=" << gale;

    // The yard's 45 deg brace limit is the speed governor: on a reach the drive
    // dies once the apparent wind draws forward of the braced yard, which caps her
    // near the true wind speed. Well past it means the governor is gone.
    EXPECT_LT(gale, galeWind * 1.4f)
        << "she outran the wind by more than the yard limit allows; " << gale
        << " m/s in " << galeWind << " m/s";
}

// The leg the game spends most of its time on has to be worth sailing.
TEST_F(DriftSailingTest, TheWorkingBreezeIsWorthSailing)
{
    SpawnFlatWater();
    Entity boat = SpawnDriftBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    const f32 breeze = BeamReachSpeed(boat, DriftWindSpeed(WeatherStateId::Overcast));
    EXPECT_GT(breeze, 1.5f)
        << "she is becalmed in the weather state the game spends most of its time "
        << "in; " << breeze << " m/s";
}

// The storm must be dangerous, not fatal — and it has to LOOK like wind pressure,
// which means lying over further in a gale than in a breeze.
//
// This pair is what the buoyancy probe's beam is pinned by. At a half-beam of 1.0
// (narrower than the drawn hull actually is) a rig big enough to move her rolled
// her clean over: measured upY = -1.0, floating inverted, drive zero — which shows
// up on the SPEED tests as "the rig is far too weak" rather than as a capsize.
// Hence the explicit upright assertion: a magnitude-only heel check passes happily
// on a boat that is upside down.
TEST_F(DriftSailingTest, TheGaleHeelsHerFurtherThanTheBreezeAndNeverRollsHerOver)
{
    SpawnFlatWater();
    Entity inBreeze = SpawnDriftBoat({ -400.0f, 0.5f, 0.0f });
    Entity inGale = SpawnDriftBoat({ 400.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    constexpr glm::vec3 kCourse{ 0.0f, 0.0f, 1.0f };
    const auto heelIn = [&](Entity boat, f32 wind)
    {
        SetWind({ 1.0f, 0.0f, 0.0f }, wind); // from the starboard beam
        boat.GetComponent<SailComponent>().m_Enabled = true;
        HoldCourse(boat, kCourse, 25.0f);
        const f32 lean = MastLeanToward(boat, StarboardDir(boat));
        boat.GetComponent<SailComponent>().m_Enabled = false;
        return lean;
    };

    const f32 breeze = heelIn(inBreeze, DriftWindSpeed(WeatherStateId::Overcast));
    const f32 gale = heelIn(inGale, DriftWindSpeed(WeatherStateId::Storm));
    GTEST_LOG_(INFO) << "Drift mast lean toward starboard: breeze=" << breeze
                     << " gale=" << gale;

    // Wind from starboard pushes her to port, so she heels to PORT and the mast
    // goes with her — a negative lean toward starboard.
    EXPECT_LT(gale, -0.03f) << "a storm on the beam did not heel her; lean=" << gale;
    EXPECT_LT(gale, breeze)
        << "she lies over no further in a gale than in a breeze, so the heel is not "
        << "reading as wind pressure; breeze=" << breeze << " gale=" << gale;
    EXPECT_GT((Rot(inGale) * glm::vec3(0.0f, 1.0f, 0.0f)).y, 0.7f)
        << "the hull went over under sail";
}

// And the auxiliary is genuinely auxiliary: under engine alone in a flat calm she
// still moves, but nowhere near as fast as the same boat under sail in a working
// breeze. If this ever inverts, the wind has stopped being the game.
TEST_F(DriftSailingTest, TheEngineIsSlowerThanTheSail)
{
    SpawnFlatWater();
    Entity underSailBoat = SpawnDriftBoat({ -400.0f, 0.5f, 0.0f });
    Entity underEngineBoat = SpawnDriftBoat({ 400.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    const f32 underSail = BeamReachSpeed(underSailBoat, DriftWindSpeed(WeatherStateId::Overcast));

    constexpr glm::vec3 kCourse{ 0.0f, 0.0f, 1.0f };
    SetNoWind();
    underEngineBoat.GetComponent<BoatComponent>().m_ThrottleInput = 1.0f;
    HoldCourse(underEngineBoat, kCourse, 25.0f);
    const glm::vec3 start = Pos(underEngineBoat);
    HoldCourse(underEngineBoat, kCourse, 20.0f);
    const f32 underEngine = glm::dot(Pos(underEngineBoat) - start, kCourse) / 20.0f;

    GTEST_LOG_(INFO) << "Drift under sail: " << underSail
                     << " m/s, under auxiliary engine: " << underEngine << " m/s";
    EXPECT_GT(underEngine, 0.5f) << "the auxiliary cannot move her at all";
    EXPECT_LT(underEngine, underSail)
        << "the auxiliary engine out-sails the rig, so the wind is decoration again; "
        << "engine=" << underEngine << " sail=" << underSail;
}
