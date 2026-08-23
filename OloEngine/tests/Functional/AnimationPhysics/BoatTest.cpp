#include "OloEnginePCH.h"

// =============================================================================
// BoatTest — Functional Test.
//
// OLO_TEST_LAYER: Functional
//
// Cross-subsystem seam under test:
//   BoatComponent (authored ECS data) × BoatSystem's force model × the shared
//   water surface (Physics3D/WaterProbe, the CPU mirror of Water.glsl) ×
//   BuoyancySystem × Jolt, all driven through a real Scene::OnUpdateRuntime.
//
// Issue #438: buoyancy already made a boat FLOAT; this slice makes it DRIVE.
// The boat is deliberately layered on top of BuoyancySystem rather than
// replacing it, so these tests assert the emergent behaviour of the two
// together — with a hull that still floats at the waterline throughout.
//
// Each assertion is chosen so a broken/absent BoatSystem fails it by a wide
// margin, never on a float `==` (see CLAUDE.md / docs/testing.md):
//   * throttle drives the hull forward along its local +Z by metres, where an
//     unpowered boat drifts ~0;
//   * the rudder only bites when the boat is MOVING (a real rudder needs flow
//     past it), and reverses when backing up;
//   * the hull TRACKS: a sideways shove is killed far faster than the same
//     shove along the hull, which is the whole point of the lateral-drag term;
//   * a boat over dry land — no water tile — gets no thrust at all;
//   * everything stays finite, and the hull keeps floating rather than being
//     driven under or launched out of the water.
//
// The wave clock is frozen where a deterministic surface matters, mirroring
// WaterBuoyancyTest.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // The hull is a 2 x 1 x 4 m box (the half-extents below) massing 2000 kg.
    // With the buoyancy probe box matching it, the water displaced at full
    // immersion is 2 * 1 * 4 * 1000 = 8000 kg — four times the hull's mass, so
    // it settles with its origin near the waterline (roughly quarter-immersed)
    // rather than sinking or riding clear. That is the normal operating state a
    // boat controller has to work in.
    constexpr glm::vec3 kHullHalfExtents{ 1.0f, 0.5f, 2.0f };
    constexpr f32 kHullMass = 2000.0f;
    constexpr f32 kWaterPlaneY = 0.0f;
} // namespace

class BoatTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}

    void TearDown() override
    {
        // Always release the frozen wave clock, even if an ASSERT aborted the
        // body, so a leaked mock time can't desync later tests in the process.
        Time::ClearMockTime();
        FunctionalTest::TearDown();
    }

    /// A flat, still water tile (zero wave amplitude) so rest height and
    /// immersion are deterministic and the assertions are about propulsion
    /// rather than about which wave crest the hull happened to be on.
    Entity SpawnFlatWater()
    {
        Entity water = GetScene().CreateEntity("Water");
        water.GetComponent<TransformComponent>().Translation = { 0.0f, kWaterPlaneY, 0.0f };
        auto& wc = water.AddComponent<WaterComponent>();
        wc.m_Enabled = true;
        wc.m_WorldSizeX = 400.0f;
        wc.m_WorldSizeZ = 400.0f;
        wc.m_WaveAmplitude = 0.0f;
        return water;
    }

    /// A floating hull with a default-tuned BoatComponent. `yawDegrees` rotates
    /// it about world up so a test can prove the thrust follows the HULL's own
    /// forward axis rather than a hard-coded world axis.
    Entity SpawnBoat(const glm::vec3& pos, f32 yawDegrees = 0.0f)
    {
        Entity boat = GetScene().CreateEntity("Boat");
        auto& tc = boat.GetComponent<TransformComponent>();
        tc.Translation = pos;
        tc.SetRotationEuler(glm::vec3(0.0f, glm::radians(yawDegrees), 0.0f));

        auto& rb = boat.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = kHullMass;
        rb.m_LinearDrag = 0.0f; // the boat + buoyancy supply their own drag
        rb.m_AngularDrag = 0.0f;

        boat.AddComponent<BoxCollider3DComponent>().m_HalfExtents = kHullHalfExtents;

        auto& buoyancy = boat.AddComponent<BuoyancyComponent>();
        buoyancy.m_ProbeExtents = kHullHalfExtents;
        buoyancy.m_FluidDensity = 1000.0f;
        buoyancy.m_SubmergenceRamp = 1.0f;
        buoyancy.m_LinearDrag = 1.0f; // settle quickly inside the test window
        buoyancy.m_AngularDrag = 2.0f;

        boat.AddComponent<BoatComponent>();
        return boat;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    static glm::quat Rot(Entity e)
    {
        return e.GetComponent<TransformComponent>().GetRotation();
    }

    /// Heading in degrees about world up, derived from where the hull's local
    /// +Z now points. Signed so that turning to STARBOARD increases it —
    /// matching the sign of BoatComponent::m_SteerInput. Starboard is local -X
    /// for a +Z-forward hull (forward x up), hence the negated x: reading it
    /// the other way round is exactly the bug issue #897 fixed.
    static f32 HeadingDeg(Entity e)
    {
        const glm::vec3 fwd = e.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::degrees(std::atan2(-fwd.x, fwd.z));
    }

    /// World-space starboard beam of the hull: forward x up, in the horizontal
    /// plane. This is the axis a starboard turn must displace the boat along.
    static glm::vec3 StarboardDir(Entity e)
    {
        const glm::vec3 fwd = e.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 flat(fwd.x, 0.0f, fwd.z);
        const f32 len = glm::length(flat);
        if (!(len > 1.0e-4f))
            return { 0.0f, 0.0f, 0.0f };
        return { -flat.z / len, 0.0f, flat.x / len };
    }

    /// Let buoyancy bring the hull to its floating equilibrium before the test
    /// applies any input, so a measured displacement is propulsion and not the
    /// tail of the initial drop.
    void Settle()
    {
        Time::SetMockTime(0.0f); // frozen wave clock ⇒ a deterministic surface
        TickFor(3.0f);
    }
};

// -----------------------------------------------------------------------------
// The premise: without any throttle the boat just floats. This is the baseline
// every propulsion assertion below is measured against — if a parked boat crept
// on its own, "throttle moved it" would prove nothing.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, UnpoweredBoatFloatsWithoutDrifting)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(3.0f);
    const glm::vec3 end = Pos(boat);

    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_LT(glm::length(glm::vec2(end.x - start.x, end.z - start.z)), 0.25f)
        << "an unpowered boat drifted across the water";
    EXPECT_NEAR(end.y, kWaterPlaneY, 0.75f)
        << "the hull did not stay at the waterline; y=" << end.y;
}

// -----------------------------------------------------------------------------
// Throttle drives it forward. The discriminator is the margin: an unpowered
// boat moves < 0.25 m over the same window (test above), so metres of travel
// can only come from BoatSystem's thrust. A sign error would send it astern.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, ThrottleDrivesTheBoatForward)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);

    boat.GetComponent<BoatComponent>().m_ThrottleInput = 1.0f;
    TickFor(4.0f);

    const glm::vec3 end = Pos(boat);
    const f32 forward = end.z - start.z;
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_GT(forward, 2.0f) << "throttle did not drive the boat forward (+Z); dz=" << forward;
    EXPECT_GT(forward, std::abs(end.x - start.x) * 3.0f)
        << "the boat slid sideways instead of tracking forward";
    EXPECT_NEAR(end.y, kWaterPlaneY, 1.0f)
        << "the boat left the water under power (planed into orbit / driven under); y=" << end.y;
}

// -----------------------------------------------------------------------------
// Reverse throttle backs it up. Cheap to assert, and it catches an
// abs()/clamp-to-positive slip in the throttle path that forward-only tests
// would happily pass.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, ReverseThrottleDrivesTheBoatAstern)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);

    boat.GetComponent<BoatComponent>().m_ThrottleInput = -1.0f;
    TickFor(4.0f);

    const f32 forward = Pos(boat).z - start.z;
    EXPECT_LT(forward, -1.0f) << "reverse throttle did not back the boat up; dz=" << forward;
}

// -----------------------------------------------------------------------------
// Thrust follows the HULL, not the world. Yawed +90° about world up the boat's
// local +Z points along world +X, so full throttle must move it in X and barely
// in Z. A model that pushed along a hard-coded world axis passes every
// unrotated test and fails this one. (That +90° is a turn to PORT — starboard
// is -X for a +Z-forward hull — but which way it points is beside the point
// here; only that thrust follows it.)
// -----------------------------------------------------------------------------
TEST_F(BoatTest, ThrustFollowsTheHullHeading)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f }, 90.0f);
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);

    boat.GetComponent<BoatComponent>().m_ThrottleInput = 1.0f;
    TickFor(4.0f);

    const glm::vec3 delta = Pos(boat) - start;
    EXPECT_GT(delta.x, 2.0f) << "a boat yawed 90 deg did not drive along world +X; dx=" << delta.x;
    EXPECT_GT(delta.x, std::abs(delta.z) * 3.0f)
        << "thrust was applied along a world axis rather than the hull's forward";
}

// -----------------------------------------------------------------------------
// The rudder needs flow past it. Steering hard with NO throttle must leave the
// heading essentially unchanged, while the same steering under power swings the
// bow by tens of degrees. This pair is the real test of the speed-scaled rudder
// authority — either half alone would pass a model that ignored speed entirely.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, RudderDoesNothingAtRestButTurnsTheBoatUnderPower)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();

    Settle();

    // Hard starboard, dead in the water.
    const f32 headingBeforeStationary = HeadingDeg(boat);
    boat.GetComponent<BoatComponent>().m_SteerInput = 1.0f;
    TickFor(3.0f);
    const f32 stationaryTurn = std::abs(HeadingDeg(boat) - headingBeforeStationary);
    EXPECT_LT(stationaryTurn, 5.0f)
        << "the rudder pivoted a stationary boat on the spot; turned " << stationaryTurn << " deg";

    // Same rudder, now with way on. The heading is sampled over a SHORT window:
    // at full rudder the hull can swing through more than 180 deg in a few
    // seconds, and atan2 wraps — a long window could read a big turn as a small
    // one and pass/fail for the wrong reason.
    boat.GetComponent<BoatComponent>().m_ThrottleInput = 1.0f;
    TickFor(2.5f); // build up speed first
    const f32 headingBeforeMoving = HeadingDeg(boat);
    TickFor(1.0f);
    const f32 movingTurn = std::abs(HeadingDeg(boat) - headingBeforeMoving);
    EXPECT_GT(movingTurn, 8.0f)
        << "the rudder did not turn a boat that was making way; turned " << movingTurn << " deg in 1 s";
    EXPECT_LT(movingTurn, 180.0f) << "heading sample wrapped — shorten the window";
    EXPECT_TRUE(std::isfinite(Pos(boat).y));
}

// -----------------------------------------------------------------------------
// The rudder REVERSES astern, exactly like a real boat backing up: the same
// helm order swings the bow the other way. This is the sign half of the
// speed-scaled authority, and it is invisible to any forward-only test.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, RudderReversesWhenMakingSternway)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    Ref<JoltBody> body = GetScene().GetPhysicsScene()->GetBody(boat);
    ASSERT_TRUE(body);

    auto& bc = boat.GetComponent<BoatComponent>();
    bc.m_SteerInput = 1.0f; // hard starboard, held throughout

    // The observable is the YAW RATE, not a heading delta: under full rudder the
    // hull sweeps through more than a full circle over this test, and an
    // atan2-derived heading wraps. The rate carries the same sign information
    // and cannot wrap.
    bc.m_ThrottleInput = 1.0f;
    TickFor(3.0f);
    const f32 yawRateAhead = body->GetAngularVelocity().y;

    bc.m_ThrottleInput = -1.0f;
    TickFor(5.0f); // stop, then gather sternway
    const f32 yawRateAstern = body->GetAngularVelocity().y;

    ASSERT_LT(glm::dot(body->GetLinearVelocity(), Rot(boat) * glm::vec3(0.0f, 0.0f, 1.0f)), -0.5f)
        << "the boat never actually gathered sternway, so the reversal is untested";
    // Starboard is local -X for a +Z-forward hull, and a rotation about +Y takes
    // +Z toward +X — so swinging the bow to starboard is a NEGATIVE yaw rate.
    EXPECT_LT(yawRateAhead, -0.1f)
        << "hard starboard did not swing the bow to starboard when going ahead; yaw rate=" << yawRateAhead;
    EXPECT_GT(yawRateAstern, 0.1f)
        << "the rudder did not reverse when making sternway; yaw rate=" << yawRateAstern;
}

// -----------------------------------------------------------------------------
// A starboard helm order moves the boat to STARBOARD. Issue #897: it moved it to
// port, and no test noticed, because every rudder test above measures a rate or
// an absolute turn magnitude — all of which a mirrored model reproduces exactly.
//
// The observable here is deliberately the sign of the WORLD-SPACE DISPLACEMENT
// against the hull's starting beam, not the yaw rate: that is what a player
// actually sees from a chase camera, and it is the thing the Drift scene had to
// negate its steer input to get right. Measured over a window short enough that
// the hull has not swung more than 90 degrees, so "starboard of where I started"
// is still unambiguous.
//
// The magnitude bar is set against the same-window drift of a boat under
// throttle with NO rudder, asserted first — otherwise the threshold is arbitrary.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, StarboardHelmMovesTheBoatToStarboardNotToPort)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    const glm::vec3 startingStarboard = StarboardDir(boat);
    ASSERT_GT(glm::length(startingStarboard), 0.5f) << "the hull has no usable heading to measure against";

    // Baseline: same throttle, rudder amidships. Whatever this drifts sideways is
    // noise, and the steered run has to beat it by a wide margin.
    auto& bc = boat.GetComponent<BoatComponent>();
    bc.m_ThrottleInput = 1.0f;
    bc.m_SteerInput = 0.0f;
    glm::vec3 start = Pos(boat);
    TickFor(3.0f);
    const f32 straightBeamOffset = std::abs(glm::dot(Pos(boat) - start, startingStarboard));
    EXPECT_LT(straightBeamOffset, 0.5f)
        << "an unsteered boat wandered off its own beam by " << straightBeamOffset << " m";

    // Now hard starboard, measured against the beam as it is at THIS moment.
    // The window is short on purpose: once the hull has swung past 90 degrees,
    // "starboard of where I started" stops being a well-defined direction.
    const glm::vec3 beam = StarboardDir(boat);
    const f32 headingBefore = HeadingDeg(boat);
    start = Pos(boat);
    bc.m_SteerInput = 1.0f;
    TickFor(2.0f);

    const f32 beamOffset = glm::dot(Pos(boat) - start, beam);
    const f32 turned = HeadingDeg(boat) - headingBefore;

    ASSERT_TRUE(std::isfinite(beamOffset) && std::isfinite(turned));
    ASSERT_LT(std::abs(turned), 90.0f) << "the hull swung too far for the beam to stay meaningful — shorten the window";
    EXPECT_GT(beamOffset, 0.75f)
        << "steer +1 (documented as full starboard) displaced the boat " << beamOffset
        << " m along its starboard beam — negative means it went to PORT, which is issue #897";
    EXPECT_GT(beamOffset, straightBeamOffset * 4.0f)
        << "the sideways displacement was within noise of an unsteered run ("
        << straightBeamOffset << " m)";

    // And the heading agrees with the displacement, so the two cannot drift apart.
    EXPECT_GT(turned, 5.0f) << "the bow did not swing to starboard; turned " << turned << " deg";
}

// -----------------------------------------------------------------------------
// The hull TRACKS. Give the boat identical shoves along and across the hull and
// the sideways one must die far faster — that asymmetry is exactly what
// m_LateralDrag exists to produce, and it is what stops a boat sliding
// broadside through a turn like a crate on ice.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, HullResistsSidewaysMotionFarMoreThanForwardMotion)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    EnablePhysics3D();
    Settle();

    // Sideways (world +X == the hull's starboard beam, unrotated).
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    Ref<JoltBody> body = GetScene().GetPhysicsScene()->GetBody(boat);
    ASSERT_TRUE(body);

    body->SetLinearVelocity({ 4.0f, 0.0f, 0.0f });
    TickFor(1.5f);
    const f32 lateralRemaining = std::abs(body->GetLinearVelocity().x);

    // Forward (world +Z == the hull's bow), same magnitude, same window.
    body->SetLinearVelocity({ 0.0f, 0.0f, 4.0f });
    TickFor(1.5f);
    const f32 forwardRemaining = std::abs(body->GetLinearVelocity().z);

    EXPECT_TRUE(std::isfinite(lateralRemaining) && std::isfinite(forwardRemaining));
    EXPECT_LT(lateralRemaining, forwardRemaining * 0.5f)
        << "the hull did not resist sideways motion more than forward motion; lateral="
        << lateralRemaining << " forward=" << forwardRemaining;
}

// -----------------------------------------------------------------------------
// No water, no propulsion. A boat spawned over dry land (no WaterComponent at
// all) must not be pushed by its own throttle — the propeller has nothing to
// bite. This is what keeps the force model honest about the medium it needs,
// and it is the case a naive "always AddForce" implementation gets wrong.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, BoatWithNoWaterGetsNoThrust)
{
    // Ground to land on, and deliberately NO WaterComponent.
    Entity ground = GetScene().CreateEntity("Ground");
    ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
    ground.AddComponent<Rigidbody3DComponent>().m_Type = BodyType3D::Static;
    ground.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 50.0f, 0.5f, 50.0f };

    Entity boat = SpawnBoat({ 0.0f, 2.0f, 0.0f });
    EnablePhysics3D();

    TickFor(2.0f); // fall and settle on the ground
    const glm::vec3 start = Pos(boat);

    boat.GetComponent<BoatComponent>().m_ThrottleInput = 1.0f;
    TickFor(3.0f);

    const glm::vec3 end = Pos(boat);
    EXPECT_LT(glm::length(glm::vec2(end.x - start.x, end.z - start.z)), 0.5f)
        << "a beached boat drove itself across dry land";
    EXPECT_TRUE(std::isfinite(end.y));
}

// -----------------------------------------------------------------------------
// A disabled boat is inert. Cheap, and it pins the m_Enabled gate that scenes
// and scripts use to park a boat without removing its component.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, DisabledBoatIgnoresThrottle)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    auto& bc = boat.GetComponent<BoatComponent>();
    bc.m_Enabled = false;
    bc.m_ThrottleInput = 1.0f;
    EnablePhysics3D();

    Settle();
    const glm::vec3 start = Pos(boat);
    TickFor(3.0f);
    const glm::vec3 end = Pos(boat);

    EXPECT_LT(glm::length(glm::vec2(end.x - start.x, end.z - start.z)), 0.25f)
        << "a disabled BoatComponent still drove the hull";
}

// -----------------------------------------------------------------------------
// Garbage tunables must not NaN the solver. Scripts write these fields raw, so
// BoatSystem sanitizes them; the pass condition is that the hull is still at a
// finite pose, floating, after several seconds of full throttle and rudder.
// -----------------------------------------------------------------------------
TEST_F(BoatTest, NonFiniteTunablesDoNotCorruptTheSimulation)
{
    SpawnFlatWater();
    Entity boat = SpawnBoat({ 0.0f, 0.5f, 0.0f });
    auto& bc = boat.GetComponent<BoatComponent>();
    bc.m_MaxThrust = std::numeric_limits<f32>::quiet_NaN();
    bc.m_MaxRudderTorque = std::numeric_limits<f32>::infinity();
    bc.m_RudderAuthoritySpeed = 0.0f;
    bc.m_LateralDrag = -3.0f;
    bc.m_ImmersionDepth = std::numeric_limits<f32>::quiet_NaN();
    bc.m_ThrottleInput = std::numeric_limits<f32>::quiet_NaN();
    bc.m_SteerInput = 1.0f;
    EnablePhysics3D();

    Settle();
    TickFor(3.0f);

    const glm::vec3 end = Pos(boat);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z))
        << "garbage boat tunables produced a non-finite pose";
    EXPECT_NEAR(end.y, kWaterPlaneY, 2.0f) << "the hull left the water; y=" << end.y;
}

// =============================================================================
// Save-game round-trip — every authored BoatComponent field must survive
// capture + restore (mirrors VehicleTest's equivalent). No physics here: this
// exercises the serializer's symmetry, not runtime behaviour.
// =============================================================================
TEST_F(BoatTest, BoatComponentSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("BoatSaveGame");
    auto& b = e.AddComponent<BoatComponent>();
    b.m_Enabled = false;
    b.m_MaxThrust = 7500.0f;
    b.m_ThrustOffsetZ = -3.25f;
    b.m_ThrustOffsetY = -0.65f;
    b.m_MaxRudderTorque = 12000.0f;
    b.m_RudderAuthoritySpeed = 6.5f;
    b.m_LateralDrag = 4.25f;
    b.m_ForwardDrag = 0.45f;
    b.m_YawDrag = 2.75f;
    b.m_ImmersionDepth = 0.85f;
    b.m_ThrottleInput = 0.5f;
    b.m_SteerInput = -0.25f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("BoatSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<BoatComponent>())
        << "BoatComponent dropped by the save-game round-trip";

    const auto& rb = re.GetComponent<BoatComponent>();
    EXPECT_FALSE(rb.m_Enabled);
    EXPECT_NEAR(rb.m_MaxThrust, 7500.0f, kEps);
    EXPECT_NEAR(rb.m_ThrustOffsetZ, -3.25f, kEps);
    EXPECT_NEAR(rb.m_ThrustOffsetY, -0.65f, kEps);
    EXPECT_NEAR(rb.m_MaxRudderTorque, 12000.0f, kEps);
    EXPECT_NEAR(rb.m_RudderAuthoritySpeed, 6.5f, kEps);
    EXPECT_NEAR(rb.m_LateralDrag, 4.25f, kEps);
    EXPECT_NEAR(rb.m_ForwardDrag, 0.45f, kEps);
    EXPECT_NEAR(rb.m_YawDrag, 2.75f, kEps);
    EXPECT_NEAR(rb.m_ImmersionDepth, 0.85f, kEps);
    EXPECT_NEAR(rb.m_ThrottleInput, 0.5f, kEps);
    EXPECT_NEAR(rb.m_SteerInput, -0.25f, kEps);
}
