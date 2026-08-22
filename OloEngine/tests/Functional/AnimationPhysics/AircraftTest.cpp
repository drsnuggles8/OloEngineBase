#include "OloEnginePCH.h"

// =============================================================================
// AircraftTest — Functional Test.
//
// OLO_TEST_LAYER: Functional
//
// Cross-subsystem seam under test:
//   AircraftComponent (authored ECS data) × AircraftSystem's aerodynamic force
//   model × Jolt, driven through a real Scene::OnUpdateRuntime.
//
// Issue #438. Unlike the wheeled VehicleComponent this uses no Jolt constraint
// at all — the whole model is AddForce/AddTorque queued before the world step —
// so what has to be pinned is the emergent flight behaviour, not a constraint's
// configuration.
//
// The stated acceptance bar for the aircraft is STABILITY: a flight model that
// oscillates, tumbles or diverges passes any single-frame force check you could
// write for it. So the assertions here are deliberately about behaviour over
// SECONDS of simulated flight, each with an unambiguous discriminator:
//   * a wing carries the aircraft — over 4 s it loses a small fraction of the
//     ~78 m the same body free-falls with the component disabled;
//   * lift comes from AIRSPEED — the same airframe at rest just drops;
//   * pitch/roll inputs rotate the aircraft the correct way and STOP when
//     released rather than winding up (the oscillation failure mode);
//   * controls go slack at low airspeed;
//   * a wing past its stall angle stops carrying;
//   * nothing diverges to a non-finite pose or an absurd angular rate.
//
// Every threshold is a wide margin away from both the working and the broken
// case; no float `==` (see CLAUDE.md / docs/testing.md).
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // A ~1000 kg light airframe on the component's default wing.
    //
    // kCruiseSpeed is the airframe's own HANDS-OFF TRIM SPEED, not an arbitrary
    // number. With no pitch input the only pitch equilibrium is where the
    // weathervane term is zero, i.e. angle of attack 0, where the wing produces
    // exactly its zero-lift coefficient (0.2). Level flight there needs
    //     0.5 * rho * v^2 * A * Cl0 == m * g
    //     0.5 * 1.225 * v^2 * 16 * 0.2 == 1000 * 9.81   ->   v ~= 71 m/s
    // Spawning at that speed means "no input" really is level flight, so the
    // altitude assertions below are about the WING, not about how well the test
    // happened to guess a trim point.
    constexpr f32 kAircraftMass = 1000.0f;
    constexpr f32 kCruiseSpeed = 71.0f; // m/s along local +Z — the trim speed above
    constexpr f32 kSpawnAltitude = 500.0f;
    // Throttle that balances drag at kCruiseSpeed:
    //     Cd = 0.03 + 0.05 * 0.2^2 = 0.032, D = q * A * Cd ~= 1580 N
    // against the 4000 N default max thrust.
    constexpr f32 kCruiseThrottle = 0.4f;
} // namespace

class AircraftTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}

    /// An aircraft in level flight at cruise speed. High enough that a full
    /// test window of free fall still never reaches the ground, so nothing in
    /// these tests depends on a collision.
    Entity SpawnAircraft(bool enabled = true)
    {
        Entity e = GetScene().CreateEntity("Aircraft");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, kSpawnAltitude, 0.0f };

        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = kAircraftMass;
        // Zero the generic body drag: the aerodynamic model supplies its own,
        // and a hidden second drag term would mask a broken one.
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;
        rb.m_MaxLinearVelocity = 1000.0f;
        rb.m_InitialLinearVelocity = { 0.0f, 0.0f, kCruiseSpeed };

        e.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 4.0f, 0.5f, 3.0f };

        auto& ac = e.AddComponent<AircraftComponent>();
        ac.m_Enabled = enabled;
        ac.m_ThrottleInput = kCruiseThrottle;
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    static glm::quat Rot(Entity e)
    {
        return e.GetComponent<TransformComponent>().GetRotation();
    }

    /// Pitch angle in degrees: how far the nose (local +Z) sits above the
    /// horizon. Positive = nose up.
    static f32 PitchDeg(Entity e)
    {
        const glm::vec3 fwd = Rot(e) * glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
    }

    /// Bank angle in degrees, signed so that rolling to starboard (the starboard
    /// wing dropping) is positive — matching AircraftComponent::m_RollInput.
    /// The starboard wing is local -X for a +Z-forward airframe (forward x up);
    /// reading it as +X is what let issue #897's inverted roll sign pass.
    static f32 BankDeg(Entity e)
    {
        const glm::vec3 starboardWing = Rot(e) * glm::vec3(-1.0f, 0.0f, 0.0f);
        return glm::degrees(std::asin(std::clamp(-starboardWing.y, -1.0f, 1.0f)));
    }

    /// Heading in degrees about world up, signed so that yawing to starboard
    /// increases it — same convention as BoatTest::HeadingDeg.
    static f32 HeadingDeg(Entity e)
    {
        const glm::vec3 fwd = Rot(e) * glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::degrees(std::atan2(-fwd.x, fwd.z));
    }

    Ref<JoltBody> Body(Entity e)
    {
        return GetScene().GetPhysicsScene()->GetBody(e);
    }
};

// -----------------------------------------------------------------------------
// The wing carries the aircraft. The discriminator is the FREE-FALL baseline
// measured in the very next test: the same body with the component disabled
// drops ~78 m over this window. Requiring the flying one to stay within 30 m of
// its start altitude puts it unambiguously on the "wing works" side, without
// demanding a perfectly trimmed airframe.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, WingCarriesTheAircraftInsteadOfFallingLikeABrick)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();

    TickFor(4.0f);

    const glm::vec3 end = Pos(plane);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    const f32 altitudeChange = end.y - kSpawnAltitude;
    EXPECT_GT(altitudeChange, -30.0f)
        << "the aircraft fell nearly like an unpowered body — the wing produced no useful lift; dy="
        << altitudeChange;
    EXPECT_LT(altitudeChange, 60.0f)
        << "the aircraft rocketed upward — lift is wildly over-scaled; dy=" << altitudeChange;
    // It should also still be flying FORWARD, not have been turned around or
    // stopped dead by a mis-signed drag term.
    EXPECT_GT(end.z, 100.0f) << "the aircraft barely travelled forward; z=" << end.z;
    // And it must not have tumbled: still broadly upright.
    const glm::vec3 up = Rot(plane) * glm::vec3(0.0f, 1.0f, 0.0f);
    EXPECT_GT(up.y, 0.7f) << "the aircraft rolled/pitched over in level flight; up.y=" << up.y;
}

// -----------------------------------------------------------------------------
// The free-fall baseline the test above is measured against. A disabled
// component must produce a plain ballistic body — which also pins the m_Enabled
// gate. ~78 m of drop over 4 s (0.5 * 9.81 * 16) is the number that makes the
// 30 m bound above meaningful.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, DisabledAircraftFreeFalls)
{
    Entity plane = SpawnAircraft(/*enabled=*/false);
    EnablePhysics3D();

    TickFor(4.0f);

    const f32 altitudeChange = Pos(plane).y - kSpawnAltitude;
    EXPECT_LT(altitudeChange, -60.0f)
        << "a disabled AircraftComponent still produced lift; dy=" << altitudeChange;
}

// -----------------------------------------------------------------------------
// Lift comes from AIRSPEED, not from merely having the component. Spawned at a
// standstill (and with the throttle shut) the same airframe must fall: a model
// that applied a constant upward force would pass the level-flight test above
// and fail here.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, StationaryAircraftHasNoLift)
{
    Entity plane = SpawnAircraft();
    auto& rb = plane.GetComponent<Rigidbody3DComponent>();
    rb.m_InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
    plane.GetComponent<AircraftComponent>().m_ThrottleInput = 0.0f;
    EnablePhysics3D();

    TickFor(3.0f);

    const f32 altitudeChange = Pos(plane).y - kSpawnAltitude;
    EXPECT_LT(altitudeChange, -30.0f)
        << "an aircraft with no airspeed still held itself up; dy=" << altitudeChange;
}

// -----------------------------------------------------------------------------
// Throttle accelerates it. Structured as an A/B inside one flight — two seconds
// at cruise throttle (speed must hold, since kCruiseThrottle is by construction
// the drag-balancing setting) then two at full throttle (speed must climb). The
// paired baseline is what makes the threshold meaningful: raw "it got faster"
// would also pass for an aircraft that was simply diving.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, FullThrottleAcceleratesTheAircraftAboveItsCruiseSpeed)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();
    TickFor(0.5f); // let the initial velocity register

    Ref<JoltBody> body = Body(plane);
    ASSERT_TRUE(body);
    const f32 speedAtStart = glm::length(body->GetLinearVelocity());

    TickFor(2.0f); // still at cruise throttle
    const f32 speedAtCruise = glm::length(body->GetLinearVelocity());

    plane.GetComponent<AircraftComponent>().m_ThrottleInput = 1.0f;
    TickFor(2.0f);
    const f32 speedAtFull = glm::length(body->GetLinearVelocity());

    EXPECT_TRUE(std::isfinite(speedAtCruise) && std::isfinite(speedAtFull));
    EXPECT_NEAR(speedAtCruise, speedAtStart, 3.0f)
        << "cruise throttle did not hold the trim speed — the drag model is off; "
        << speedAtStart << " -> " << speedAtCruise;
    EXPECT_GT(speedAtFull, speedAtCruise + 3.0f)
        << "full throttle did not accelerate the aircraft; " << speedAtCruise << " -> " << speedAtFull;
}

// -----------------------------------------------------------------------------
// Pitch input raises the nose, and — the part that actually matters — releasing
// it settles rather than winding up. An under-damped model reaches a similar
// peak pitch and then keeps oscillating; asserting that the pitch RATE has
// collapsed after release is what separates "flyable" from "oscillator".
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, PitchInputRaisesTheNoseAndSettlesWhenReleased)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();
    TickFor(1.0f);

    const f32 pitchBefore = PitchDeg(plane);
    plane.GetComponent<AircraftComponent>().m_PitchInput = 1.0f;
    TickFor(1.5f);

    const f32 pitchAfter = PitchDeg(plane);
    EXPECT_GT(pitchAfter - pitchBefore, 5.0f)
        << "nose-up pitch input did not raise the nose; " << pitchBefore << " -> " << pitchAfter << " deg";

    // Release and let the damping + weathervane do their job.
    plane.GetComponent<AircraftComponent>().m_PitchInput = 0.0f;
    TickFor(2.0f);

    Ref<JoltBody> body = Body(plane);
    ASSERT_TRUE(body);
    const glm::vec3 angVel = body->GetAngularVelocity();
    EXPECT_TRUE(std::isfinite(angVel.x) && std::isfinite(angVel.y) && std::isfinite(angVel.z));
    EXPECT_LT(glm::length(angVel), 1.0f)
        << "the airframe kept rotating after the stick was released — under-damped / oscillating; |w|="
        << glm::length(angVel);
}

// -----------------------------------------------------------------------------
// Roll input banks the aircraft the correct way. A sign error here is the
// classic "the plane rolls left when you press right" bug, invisible to any
// magnitude-only assertion.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, RollInputBanksTheAircraftToStarboard)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();
    TickFor(1.0f);

    const f32 bankBefore = BankDeg(plane);
    plane.GetComponent<AircraftComponent>().m_RollInput = 1.0f;
    TickFor(1.5f);
    const f32 bankAfter = BankDeg(plane);

    EXPECT_GT(bankAfter - bankBefore, 10.0f)
        << "roll-starboard input did not drop the starboard wing; " << bankBefore << " -> " << bankAfter << " deg";
    EXPECT_TRUE(std::isfinite(Pos(plane).y));
}

// -----------------------------------------------------------------------------
// Rudder input yaws the nose the correct way. Same shape as the roll test above
// and the same reason it exists: issue #897 had this sign inverted too, and
// nothing caught it because the only aircraft yaw coverage was a MAGNITUDE
// check (ControlsAreSlackWithoutAirspeed) that a mirrored model satisfies
// exactly. Measured over a window short enough that atan2 cannot wrap.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, YawInputSwingsTheNoseToStarboard)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();
    TickFor(1.0f);

    const f32 headingBefore = HeadingDeg(plane);
    plane.GetComponent<AircraftComponent>().m_YawInput = 1.0f;
    TickFor(1.5f);
    const f32 turned = HeadingDeg(plane) - headingBefore;

    ASSERT_TRUE(std::isfinite(turned));
    ASSERT_LT(std::abs(turned), 180.0f) << "heading sample wrapped — shorten the window";
    EXPECT_GT(turned, 3.0f)
        << "yaw-starboard input swung the nose " << turned << " deg — negative is to PORT (issue #897)";
}

// -----------------------------------------------------------------------------
// Control surfaces need airflow. At a standstill full deflection on all three
// axes must barely move the airframe — a model with speed-independent control
// torques would spin a parked aircraft in place.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, ControlsAreSlackWithoutAirspeed)
{
    Entity plane = SpawnAircraft();
    plane.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_ThrottleInput = 0.0f;
    ac.m_PitchInput = 1.0f;
    ac.m_RollInput = 1.0f;
    ac.m_YawInput = 1.0f;
    EnablePhysics3D();

    // One tick only: after that the aircraft is falling and HAS airspeed, at
    // which point the controls legitimately start to bite.
    RunFrames(1);

    Ref<JoltBody> body = Body(plane);
    ASSERT_TRUE(body);
    const glm::vec3 angVel = body->GetAngularVelocity();
    EXPECT_LT(glm::length(angVel), 0.05f)
        << "full control deflection spun a stationary aircraft; |w|=" << glm::length(angVel);
}

// -----------------------------------------------------------------------------
// The wing stalls. Held well past the stall angle the lift curve must collapse,
// so the aircraft descends far more like a brick than like a wing. Without the
// post-stall falloff, Cl keeps growing with angle of attack and this test's
// aircraft would climb instead — the "toy flight model" failure the falloff
// exists to prevent.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, WingHeldPastTheStallAngleStopsCarrying)
{
    Entity plane = SpawnAircraft();
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_ThrottleInput = 0.0f;
    // Pin the airframe at a deep post-stall attitude: nose 60 deg up while the
    // velocity still points along world +Z, i.e. an angle of attack four times
    // the 15 deg stall angle. Zeroing the weathervane keeps the nose from being
    // pulled back onto the relative wind, so the test measures the LIFT CURVE
    // and nothing else.
    //
    // The euler X term is NEGATIVE for nose-up: a positive rotation about +X
    // takes the local +Z (nose) toward -Y, i.e. nose DOWN. Getting this
    // backwards would still stall the wing (|alpha| is what matters), so the
    // test would pass for the wrong reason — hence the explicit assertion on
    // the resulting attitude below.
    ac.m_WeathervaneStrength = 0.0f;
    plane.GetComponent<TransformComponent>().SetRotationEuler(glm::vec3(glm::radians(-60.0f), 0.0f, 0.0f));
    ASSERT_GT(PitchDeg(plane), 45.0f) << "the test airframe is not actually pitched nose-up";

    EnablePhysics3D();
    TickFor(3.0f);

    const f32 altitudeChange = Pos(plane).y - kSpawnAltitude;
    EXPECT_LT(altitudeChange, -15.0f)
        << "a wing held four times past its stall angle still carried the aircraft; dy=" << altitudeChange;
    EXPECT_TRUE(std::isfinite(altitudeChange));
}

// -----------------------------------------------------------------------------
// Long-run stability: a whole minute of simulated flight under cruise throttle,
// hands off. Nothing may diverge — no NaN, no runaway rotation, no departure
// from broadly upright flight. This is the test that would catch a slow
// oscillation the shorter windows above are too brief to reveal.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, HandsOffFlightStaysStableOverAMinute)
{
    Entity plane = SpawnAircraft();
    EnablePhysics3D();

    TickFor(60.0f);

    const glm::vec3 end = Pos(plane);
    ASSERT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z))
        << "the flight model diverged to a non-finite pose";

    Ref<JoltBody> body = Body(plane);
    ASSERT_TRUE(body);
    const glm::vec3 angVel = body->GetAngularVelocity();
    const glm::vec3 linVel = body->GetLinearVelocity();
    EXPECT_LT(glm::length(angVel), 1.0f)
        << "the airframe was still rotating after a minute hands-off; |w|=" << glm::length(angVel);
    EXPECT_LT(glm::length(linVel), 300.0f)
        << "airspeed ran away — drag is not balancing thrust; |v|=" << glm::length(linVel);

    const glm::vec3 up = Rot(plane) * glm::vec3(0.0f, 1.0f, 0.0f);
    EXPECT_GT(up.y, 0.5f) << "the aircraft ended up inverted / on its side; up.y=" << up.y;
}

// -----------------------------------------------------------------------------
// Garbage tunables must not NaN the solver. Scripts write these fields raw, so
// AircraftSystem sanitizes them; the pass condition is a finite pose and a sane
// rotation rate after several seconds with every input maxed.
// -----------------------------------------------------------------------------
TEST_F(AircraftTest, NonFiniteTunablesDoNotCorruptTheSimulation)
{
    Entity plane = SpawnAircraft();
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_MaxThrust = std::numeric_limits<f32>::quiet_NaN();
    ac.m_WingArea = std::numeric_limits<f32>::infinity();
    ac.m_LiftSlope = -12.0f;
    ac.m_StallAngleDeg = 0.0f;
    ac.m_ControlAuthoritySpeed = 0.0f;
    ac.m_PitchDamping = std::numeric_limits<f32>::quiet_NaN();
    ac.m_WeathervaneStrength = -4.0f;
    ac.m_ThrottleInput = std::numeric_limits<f32>::quiet_NaN();
    ac.m_PitchInput = 1.0f;
    ac.m_RollInput = -1.0f;
    EnablePhysics3D();

    TickFor(3.0f);

    const glm::vec3 end = Pos(plane);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z))
        << "garbage aircraft tunables produced a non-finite pose";
    Ref<JoltBody> body = Body(plane);
    ASSERT_TRUE(body);
    const glm::vec3 angVel = body->GetAngularVelocity();
    EXPECT_TRUE(std::isfinite(angVel.x) && std::isfinite(angVel.y) && std::isfinite(angVel.z));
}

// =============================================================================
// Landing gear (issue #438 follow-up).
//
// The gear exists to solve one specific problem: an air-only airframe rests on
// its fuselage collider, so it pivots about the box's REAR EDGE and the elevator
// (~2 kN·m) cannot beat the weight moment (m·g·halfLength, ~29 kN·m here). Three
// sprung ray-cast legs move the pivot to the main gear, a few tens of cm behind
// the centre of mass, which is how a real aircraft rotates.
//
// So the tests are: the gear holds the aircraft up, it rolls freely but does not
// slide sideways, the elevator can now rotate it — and the whole thing is
// opt-in, leaving a gear-less airframe exactly as it was.
// =============================================================================
class AircraftGearTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}

    /// A long static runway whose TOP face is at y = 0.
    void MakeRunway()
    {
        Entity e = GetScene().CreateEntity("Runway");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        e.AddComponent<Rigidbody3DComponent>().m_Type = BodyType3D::Static;
        e.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 60.0f, 0.5f, 600.0f };
    }

    /// An aircraft sitting just above the runway with its gear down. The gear is
    /// 1.2 m long and the fuselage half-height 0.5 m, so once it settles the belly
    /// must be clear of the deck — that gap is what the first test measures.
    Entity MakeGearedAircraft(f32 spawnY = 1.4f)
    {
        Entity e = GetScene().CreateEntity("GearAircraft");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, spawnY, 0.0f };

        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = 1000.0f;
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;
        rb.m_MaxLinearVelocity = 1000.0f;

        e.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 4.0f, 0.5f, 3.0f };

        auto& ac = e.AddComponent<AircraftComponent>();
        ac.m_HasLandingGear = true;
        // A wing that flies at a low speed, so the takeoff roll fits a test window.
        ac.m_WingArea = 24.0f;
        ac.m_ZeroLiftCoefficient = 0.6f;
        ac.m_MaxThrust = 6000.0f;
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    static f32 PitchDeg(Entity e)
    {
        const glm::vec3 fwd = e.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
    }
};

// -----------------------------------------------------------------------------
// The gear carries the aircraft, holding the fuselage clear of the deck. The
// discriminator is the belly: with the gear off the box just lies on the runway
// with its centre at its half-height (0.5); on its gear it must sit distinctly
// higher, and must not sink through or bounce off.
// -----------------------------------------------------------------------------
TEST_F(AircraftGearTest, GearHoldsTheFuselageClearOfTheRunway)
{
    MakeRunway();
    Entity plane = MakeGearedAircraft();
    EnablePhysics3D();

    TickFor(3.0f); // damped spring — settles well inside this

    const glm::vec3 p = Pos(plane);
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    EXPECT_GT(p.y, 0.8f)
        << "the fuselage settled onto the deck instead of riding on its gear; y=" << p.y;
    EXPECT_LT(p.y, 1.4f)
        << "the gear springs pushed the aircraft up off the runway; y=" << p.y;
    // Parked with no throttle, it must not creep.
    EXPECT_LT(glm::length(glm::vec2(p.x, p.z)), 1.0f) << "a parked aircraft rolled away on its own";
}

// -----------------------------------------------------------------------------
// Gear off = the old behaviour, unchanged. This is what makes the feature opt-in:
// every aircraft authored before the gear existed must still rest on its belly.
// -----------------------------------------------------------------------------
TEST_F(AircraftGearTest, WithoutGearTheFuselageRestsOnTheDeck)
{
    MakeRunway();
    Entity plane = MakeGearedAircraft();
    plane.GetComponent<AircraftComponent>().m_HasLandingGear = false;
    EnablePhysics3D();

    TickFor(3.0f);

    EXPECT_LT(Pos(plane).y, 0.8f)
        << "a gear-less aircraft was still held up off the deck; y=" << Pos(plane).y;
}

// -----------------------------------------------------------------------------
// The gear rolls but does not slide. A sideways shove must die far faster than
// the same shove along the runway — that asymmetry is the tyre model, and it is
// what keeps an aircraft tracking the centreline during its takeoff roll.
// -----------------------------------------------------------------------------
TEST_F(AircraftGearTest, GearRollsForwardFreelyButResistsSideways)
{
    MakeRunway();
    Entity plane = MakeGearedAircraft();
    EnablePhysics3D();
    TickFor(3.0f); // settle on the gear first

    Ref<JoltBody> body = GetScene().GetPhysicsScene()->GetBody(plane);
    ASSERT_TRUE(body);

    body->SetLinearVelocity({ 8.0f, 0.0f, 0.0f }); // across the runway
    TickFor(1.5f);
    const f32 lateralRemaining = std::abs(body->GetLinearVelocity().x);

    body->SetLinearVelocity({ 0.0f, 0.0f, 8.0f }); // along it
    TickFor(1.5f);
    const f32 rollingRemaining = std::abs(body->GetLinearVelocity().z);

    EXPECT_TRUE(std::isfinite(lateralRemaining) && std::isfinite(rollingRemaining));
    EXPECT_LT(lateralRemaining, rollingRemaining * 0.5f)
        << "the tyres did not resist sideways motion more than rolling; lateral="
        << lateralRemaining << " rolling=" << rollingRemaining;
}

// -----------------------------------------------------------------------------
// THE point of the whole feature: on its gear the elevator can rotate the
// aircraft nose-up during the takeoff roll. Without gear the same input against
// the same airframe cannot — the box is pinned flat on its rear edge — so this
// pair is the direct before/after of the limitation being fixed.
// -----------------------------------------------------------------------------
TEST_F(AircraftGearTest, ElevatorCanRotateTheAircraftOnItsGear)
{
    // --- with gear ---
    MakeRunway();
    Entity plane = MakeGearedAircraft();
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_ThrottleInput = 1.0f;
    EnablePhysics3D();
    TickFor(2.0f); // settle + start rolling
    ac.m_PitchInput = 1.0f;
    TickFor(2.5f);
    const f32 gearedPitch = PitchDeg(plane);

    EXPECT_GT(gearedPitch, 3.0f)
        << "full back-stick did not rotate the aircraft on its gear; pitch=" << gearedPitch;
    EXPECT_TRUE(std::isfinite(Pos(plane).y));
}

TEST_F(AircraftGearTest, BellyLandedAircraftCannotRotate)
{
    MakeRunway();
    Entity plane = MakeGearedAircraft(0.6f);
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_HasLandingGear = false; // the pre-gear behaviour
    ac.m_ThrottleInput = 1.0f;
    EnablePhysics3D();
    TickFor(2.0f);
    ac.m_PitchInput = 1.0f;
    TickFor(2.5f);

    // The discriminator for the geared test above: same airframe, same input,
    // but pinned on its belly the elevator's ~2 kN.m cannot beat the ~29 kN.m
    // weight moment about the box's rear edge.
    EXPECT_LT(PitchDeg(plane), 3.0f)
        << "a belly-down aircraft rotated, so the geared test proves nothing; pitch=" << PitchDeg(plane);
}

// -----------------------------------------------------------------------------
// End to end: full throttle from a standing start on the runway must produce an
// actual takeoff — rotate, unstick, and climb away. This is the capability the
// limitation used to deny outright.
// -----------------------------------------------------------------------------
TEST_F(AircraftGearTest, AircraftTakesOffFromTheRunwayUnderItsOwnPower)
{
    MakeRunway();
    Entity plane = MakeGearedAircraft();
    auto& ac = plane.GetComponent<AircraftComponent>();
    ac.m_ThrottleInput = 1.0f;
    EnablePhysics3D();

    TickFor(2.0f);
    const f32 restingY = Pos(plane).y;
    ac.m_PitchInput = 0.35f; // steady back-pressure through the roll

    ASSERT_TRUE(TickUntil([&]
                          { return Pos(plane).y > restingY + 12.0f; }, 25.0f))
        << "the aircraft never left the runway; y=" << Pos(plane).y
        << " after rolling to z=" << Pos(plane).z;

    const glm::vec3 p = Pos(plane);
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    EXPECT_GT(p.z, 20.0f) << "it rose without rolling forward — that is a jump, not a takeoff";
    // Wings level: nothing in this scenario applies a roll input.
    const glm::vec3 up = plane.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 1.0f, 0.0f);
    EXPECT_GT(up.y, 0.8f) << "the aircraft rolled during the takeoff run; up.y=" << up.y;
}

// =============================================================================
// Save-game round-trip — every authored AircraftComponent field must survive
// capture + restore. No physics here: this exercises the serializer's symmetry.
// =============================================================================
TEST_F(AircraftTest, AircraftComponentSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("AircraftSaveGame");
    auto& a = e.AddComponent<AircraftComponent>();
    a.m_Enabled = false;
    a.m_MaxThrust = 8200.0f;
    a.m_WingArea = 22.5f;
    a.m_AirDensity = 0.9f;
    a.m_LiftSlope = 6.1f;
    a.m_ZeroLiftCoefficient = 0.15f;
    a.m_StallAngleDeg = 18.0f;
    a.m_DragCoefficient = 0.045f;
    a.m_InducedDragFactor = 0.07f;
    a.m_PitchTorque = 26000.0f;
    a.m_RollTorque = 31000.0f;
    a.m_YawTorque = 13000.0f;
    a.m_ControlAuthoritySpeed = 55.0f;
    a.m_PitchDamping = 5.5f;
    a.m_RollDamping = 3.5f;
    a.m_YawDamping = 4.5f;
    a.m_WeathervaneStrength = 2.25f;
    a.m_ThrottleInput = 0.75f;
    a.m_PitchInput = -0.4f;
    a.m_RollInput = 0.6f;
    a.m_YawInput = -0.2f;
    a.m_HasLandingGear = true;
    a.m_MainGearOffsetZ = -0.85f;
    a.m_MainGearHalfTrack = 2.4f;
    a.m_NoseGearOffsetZ = 3.1f;
    a.m_GearLength = 1.45f;
    a.m_GearStiffness = 15.5f;
    a.m_GearDamping = 0.65f;
    a.m_GearRollingResistance = 0.08f;
    a.m_GearLateralGrip = 5.5f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("AircraftSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<AircraftComponent>())
        << "AircraftComponent dropped by the save-game round-trip";

    const auto& ra = re.GetComponent<AircraftComponent>();
    EXPECT_FALSE(ra.m_Enabled);
    EXPECT_NEAR(ra.m_MaxThrust, 8200.0f, kEps);
    EXPECT_NEAR(ra.m_WingArea, 22.5f, kEps);
    EXPECT_NEAR(ra.m_AirDensity, 0.9f, kEps);
    EXPECT_NEAR(ra.m_LiftSlope, 6.1f, kEps);
    EXPECT_NEAR(ra.m_ZeroLiftCoefficient, 0.15f, kEps);
    EXPECT_NEAR(ra.m_StallAngleDeg, 18.0f, kEps);
    EXPECT_NEAR(ra.m_DragCoefficient, 0.045f, kEps);
    EXPECT_NEAR(ra.m_InducedDragFactor, 0.07f, kEps);
    EXPECT_NEAR(ra.m_PitchTorque, 26000.0f, kEps);
    EXPECT_NEAR(ra.m_RollTorque, 31000.0f, kEps);
    EXPECT_NEAR(ra.m_YawTorque, 13000.0f, kEps);
    EXPECT_NEAR(ra.m_ControlAuthoritySpeed, 55.0f, kEps);
    EXPECT_NEAR(ra.m_PitchDamping, 5.5f, kEps);
    EXPECT_NEAR(ra.m_RollDamping, 3.5f, kEps);
    EXPECT_NEAR(ra.m_YawDamping, 4.5f, kEps);
    EXPECT_NEAR(ra.m_WeathervaneStrength, 2.25f, kEps);
    EXPECT_NEAR(ra.m_ThrottleInput, 0.75f, kEps);
    EXPECT_NEAR(ra.m_PitchInput, -0.4f, kEps);
    EXPECT_NEAR(ra.m_RollInput, 0.6f, kEps);
    EXPECT_NEAR(ra.m_YawInput, -0.2f, kEps);
    EXPECT_TRUE(ra.m_HasLandingGear);
    EXPECT_NEAR(ra.m_MainGearOffsetZ, -0.85f, kEps);
    EXPECT_NEAR(ra.m_MainGearHalfTrack, 2.4f, kEps);
    EXPECT_NEAR(ra.m_NoseGearOffsetZ, 3.1f, kEps);
    EXPECT_NEAR(ra.m_GearLength, 1.45f, kEps);
    EXPECT_NEAR(ra.m_GearStiffness, 15.5f, kEps);
    EXPECT_NEAR(ra.m_GearDamping, 0.65f, kEps);
    EXPECT_NEAR(ra.m_GearRollingResistance, 0.08f, kEps);
    EXPECT_NEAR(ra.m_GearLateralGrip, 5.5f, kEps);
}
