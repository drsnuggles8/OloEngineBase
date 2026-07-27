# Force-model vehicles (boats, aircraft) on top of Jolt

Lessons from issue #438 (`BoatComponent`/`BoatSystem`, `AircraftComponent`/
`AircraftSystem`, and the FWD/AWD differential slice on `VehicleComponent`).
Read this before adding any new **force-model** vehicle — a submarine, a
hovercraft, a helicopter, a hot-air balloon — i.e. anything driven by
`AddForce`/`AddTorque` on a plain dynamic rigidbody rather than by a Jolt
constraint.

The failure mode this whole document exists to prevent: **every one of these
bugs leaves the test suite green.** A boat that never gets thrust still floats;
an aircraft that oscillates still has finite positions; a differential wired to
the wrong axle still drives forward. They are only visible in a running frame or
in a test written specifically to discriminate them.

---

## 1. Constraint or forces? Pick by what the vehicle rides on

`VehicleComponent` uses Jolt's `VehicleConstraint` + `WheeledVehicleController`
because a wheeled vehicle needs per-wheel raycast suspension and a tyre friction
model, which Jolt already implements well. A boat or an aircraft has none of
that — it rides on a *field* (water height, air density), not on discrete
contact points.

Use forces when:

* the medium is continuous (buoyancy, lift, drag),
* the vehicle should stay an ordinary rigid body for collision purposes,
* you want it to compose with other force systems (`BuoyancySystem`,
  `ClothWindSystem`, `FluidSystem`) by simple summation.

The payoff is real: a force-model vehicle needs **no** `OnComponentAdded` /
`OnComponentRemoved` body, no runtime token field, and no teardown path — all
of which `VehicleComponent` does need, and all of which are places to leak a
Jolt handle. The generated no-op handlers cover it automatically.

## 2. Both physics call sites, or the feature only works in one editor mode

Anything that queues Jolt forces must run **before** the world step (Jolt clears
accumulated forces in `PhysicsSystem::Update`). There are **two** places that
step physics and they are not the same code path:

| Path | Entry point | How to hook in |
| --- | --- | --- |
| Runtime / Play | `Scene::SimulateRuntimeStep` → the gameplay scheduler | register a system node with `.Writes(kBodyForces).Before("PhysicsKick")` |
| Editor Simulate | `Scene::StepPhysics` | a direct call at the top, next to `FluidSystem::OnUpdate` |

Miss the second and the vehicle works in Play but is inert in Simulate — or the
reverse. `FluidSystem` and now `BoatSystem`/`AircraftSystem` all carry a
"keep the two call sites in sync" comment for exactly this reason.
(`BuoyancySystem` and `ClothWindSystem` instead live *inside*
`Scene::KickPhysicsStep`, which both paths reach — also fine, but then they are
invisible in the scheduler's ordered list.)

Use the deterministic `Scene::GetSimulationTime()` — never `Time::GetTime()` —
as the wave/field clock, so the vehicle is reproducible across frame pacings and
rollback re-sim (issue #452).

## 3. Sample the medium where the vehicle actually touches it

**The single most costly bug to find in this area.** A floating body in
equilibrium sits with its **origin exactly at the waterline** — that is what
floating *means*. So sampling "how immersed am I?" at the body origin returns a
value that hovers around zero and flickers with every passing wave: the hull's
drag switches on and off at wave frequency, precisely when the boat is behaving
most normally. The boat then slides broadside through turns, intermittently.

Sample at the geometry that is genuinely submerged:

* **propeller thrust** → at the authored drive point (`m_ThrustOffsetY/Z`), so a
  stern lifted clear by a wave really does lose bite;
* **hull drag** → at the **keel**, taken from `BuoyancyComponent::m_ProbeExtents.y`
  when the entity has one. Reusing the buoyancy box is not a shortcut, it is the
  correct coupling: that box already *is* the submerged-hull model.

Do the sampling through `Physics3D/WaterProbe` rather than re-resolving the
water tiles locally. `BuoyancySystem` and `BoatSystem` must agree about where
the surface is; two copies of the tile-resolution logic will eventually drift,
and the symptom ("the boat sometimes doesn't accelerate") points nowhere near
the cause.

## 4. Rate damping is not stability

An aerodynamic model with only per-axis angular damping is **neutrally stable**:
damping slows a divergence but never returns the airframe to trim. Left there,
the aircraft wallows, and every "is it flyable?" test you write is really
testing how long your window was.

Two terms, doing different jobs:

* **rate damping** — `torque -= axis * (dot(angVel, axis) * coefficient * mass)`
  per body axis. Absorbs control inputs and gusts. Applies at any airspeed, so
  a stalled or stationary airframe still stops tumbling.
* **weathervane / static stability** — `torque += cross(forward, velDir) * (k * q * wingArea)`.
  This is the restoring term. `cross(forward, velDir)` has magnitude
  `sin(angle-off-the-wind)`, a natural small-angle proportional response, and it
  is perpendicular to `forward` **by construction** — so it produces no roll
  component and can never fight the ailerons. Scaling by dynamic pressure `q`
  makes stability fall away with airspeed, which is also the real behaviour.

Related invariants worth keeping:

* Scale control-surface torques by `clamp(airspeed / authoritySpeed, 0, 1)`.
  Without it, a parked aircraft spins itself in place on full deflection.
* Give the lift curve a **post-stall falloff**. A bare `Cl = Cl0 + slope·alpha`
  produces *more* lift the harder you pull, forever — the toy-flight-model
  failure where an aircraft can loop indefinitely and never departs.
* Include induced drag (`Cd = Cd0 + k·Cl²`). Without it, hard turns are free.
* Derive the lift direction as `normalize(cross(velDir, rightWing))` and skip
  lift when that cross product is degenerate — at extreme sideslip the wing is
  edge-on to the airflow and genuinely produces none.

## 5. Project thrust into the plane the vehicle actually travels in

A propeller really does push along its shaft, but letting a pitching hull aim
its thrust vertically is how a boat "planes into orbit". `BoatSystem` projects
the hull's forward axis onto the horizontal plane before applying thrust, and
bails out when that projection degenerates (hull pointing straight up or down).
Applying the force at an **off-centre** point still gives the bow-up trim under
power, so nothing expressive is lost.

An aircraft is the opposite case — thrust along the true nose vector is the
whole point — so this is a per-vehicle decision, not a blanket rule. Ask: *can
this vehicle legitimately climb under its own thrust?*

## 6. Jolt `VehicleDifferentialSettings` gotchas

* `mEngineTorqueRatio` across all differentials **must sum to 1** — it is Jolt's
  stated contract, not a suggestion. Derive the second from the first
  (`1 - frontSplit`) rather than authoring both.
* `mLimitedSlipRatio` and the controller-level `mDifferentialLimitedSlipRatio`
  must be **strictly greater than 1** — `JPH_ASSERT(mLimitedSlipRatio > 1.0f)`
  in both `VehicleDifferential.cpp` and `WheeledVehicleController.cpp`. Clamping
  a garbage value to exactly `1.0f` looks like the obvious sanitization and
  **crashes Debug builds**; the floor has to be a hair above (`1.001f`, which is
  effectively "locked"). This was a real bug in the #438 slice, found only
  because the sanitization test fed the field a below-range value — a test that
  asserted `>= 1.0f` would have accepted the broken clamp too, so pin it with
  `EXPECT_GT(..., 1.0f)`.
* A wheel may **steer and be driven at the same time**, so front-wheel drive
  needs no special handling beyond re-indexing the differential to wheels 0/1.
* Guard the drive-mode enum itself. An out-of-range value (corrupt save, raw
  script write) must fall back to a valid mode rather than indexing off the end
  of the wheel array.

## 7. Testing: contract *and* behaviour, because neither alone is enough

For the differential slice the two layers catch genuinely different bugs:

* **Contract** — read the built drivetrain back off the live controller
  (`JoltScene::GetVehicleDrivetrain`) and assert which wheel indices receive
  torque, in what ratio. Only this can catch a drive mode that silently drove
  the *wrong axle*: a car with front-wheel drive wired to the rear wheels
  accelerates just as convincingly.
* **Behaviour** — assert each mode actually moves the chassis by metres. Only
  this can catch a structurally plausible configuration that produces no motion
  at all.

For the force models, the discriminator has to be built into the scenario:

* pair every "X does something" test with the **baseline** it is measured
  against (an unpowered boat drifts < 0.25 m; a disabled aircraft free-falls
  ~78 m in 4 s) — otherwise the threshold is arbitrary;
* test the **negative** case explicitly (a boat over dry land gets no thrust; a
  stationary aircraft has no lift; a stationary rudder does not pivot the boat).
  These are what pin the model to its medium, and they are what a naive
  "always AddForce" implementation gets wrong;
* for stability, assert the angular **rate has collapsed** after the stick is
  released, and run one long (60 s) hands-off flight. An under-damped model
  reaches the same peak attitude as a good one — the difference only shows in
  what happens next;
* rotate the vehicle and re-test. A model that pushes along a hard-coded world
  axis passes every unrotated test.

## 8. Still verify it in a running frame

None of the above removes the CLAUDE.md requirement to run the editor. The
`VehiclesTest.olo` sandbox scene authors every driver input directly in the
scene YAML (throttle/steer/pitch are ordinary serialized fields), so the whole
scene animates on Play or Simulate with no scripting — which is the cheapest
possible way to look at three drive modes, two boats and an aircraft at once.

Practical notes from doing exactly that over MCP (issue #438):

* **`OLO_MCP_ALLOW_WRITES=1`** unlocks `olo_scene_open` / `olo_scene_play`, which
  is what makes a headless drive-the-editor loop possible at all. The
  `run-oloengine` skill's `attach` action inherits the environment, so exporting
  it before `attach` is enough.
* **Play mode renders from the runtime `CameraComponent`**, so `olo_screenshot`
  refuses a one-shot `camera` pose there. Either author the scene camera where
  you need it, or read entity poses numerically (`olo_scene_get_entity`) and
  treat the pixels as the confirmation rather than the measurement.
* **Camera aiming**: forward is `rot * (0,0,-1)` and
  `glm::quat(vec3(pitch,yaw,0)) == Ry(yaw)·Rx(pitch)`, so for a desired look
  direction `d`: `pitch = asin(d.y)`, `yaw = atan2(-d.x, -d.z)`. Guessing eulers
  wastes captures on empty sky.
* **Chasing a fast vehicle by predicting where it will be does not work** — frame
  pacing makes the position at a given wall-clock unrepeatable. Put the camera
  *on* the flight/track line so the subject stays framed, and take a burst.
* **Box colliders are multiplied by the transform scale**
  (`JoltShapes::ApplyScaleToBoxExtents`), and the mesh primitives are unit-sized.
  So a collider matching a scaled cube is always `HalfExtents: [0.5, 0.5, 0.5]`.
  Authoring the *world* half-extents makes every collider scale-times too large;
  in #438 that put the quay's deck at y = 6 with the cars spawned inside it, and
  they fell straight through. The symptom ("the cars ignore the ground") points
  nowhere near the cause.

## 9. Ground contact: put the pivot where the gear is

A force-model aircraft resting on its fuselage collider **cannot rotate for
takeoff**, and the reason is worth internalising because it generalises to any
force-driven vehicle that has to push off the ground.

A box lying on the ground pivots about its **rear edge**, so the weight moment
the elevator must beat is `m·g·(half the length)` — about 29 kN·m for a 1 t, 6 m
airframe. A believable elevator is worth maybe 2–3 kN·m. It is not a tuning
problem: no realistic control authority closes a 10× gap, and cranking
`m_PitchTorque` until it does buys you an aircraft that backflips in the air.

Real aircraft rotate easily because the **main gear sits just behind the centre
of mass**, cutting that arm from metres to tens of centimetres. So the fix is to
model the contact points, not to add authority:

* `AircraftComponent::m_HasLandingGear` turns on three sprung, ray-cast legs
  (`AircraftSystem`): mains slightly aft of the CoM at `m_MainGearOffsetZ`, nose
  wheel forward. Each leg raycasts down, and its spring/damper force is applied
  **at its own contact point** — that off-centre application is what creates the
  pivot.
* Keep `m_GearLength` greater than the fuselage collider's half-height, or the
  belly grounds out and the legs never take the load.
* Express the spring rate as a multiple of the aircraft's own weight
  (`m_GearStiffness`), not raw N/m, so it doesn't need re-deriving per airframe.
* Split tyre friction into rolling (small) and lateral (large). The asymmetry is
  what holds the aircraft on the centreline during the roll — the same
  forward/lateral split the boat's hull uses, for the same reason.
* Keep it **opt-in**. An air-only airframe is a legitimate configuration, and
  every aircraft authored before the gear existed must behave identically.

The test that proves this is a *pair*: the same airframe with gear rotates under
back-stick, and with gear off does not. Either half alone proves nothing.

## 10. A scene must be able to author a moving body

`Rigidbody3DComponent::m_InitialLinearVelocity` / `m_InitialAngularVelocity` are
applied once at body creation (`JoltBody::SetupCreatedBody`) and are now
scene-serialized. Before that they were runtime-only, which meant a scene could
only ever author bodies **at rest** — so "an aircraft already in cruise" was
unexpressible and every flight demo had to start from a standstill and dive,
injecting energy the phugoid then had to shed over a full loop.

Two things made adding them safe, and both are the general pattern for widening
a hand-written serializer:

* they default to zero, so a scene written before the keys existed loads exactly
  as it always did (pinned by
  `ComponentRoundTrip.Rigidbody3DWithoutInitialVelocityKeysLoadsAtRest`);
* they are clamped on read, so a corrupt file cannot launch a body at 1e30 m/s.

Watch for the failure mode when reviewing a write-once field like this: if it
silently reverted to zero on load, **everything would still simulate perfectly**
— the vehicle would just start from a standstill, which reads as a design choice
rather than a dropped field. That is precisely why it needs a round-trip test
rather than a behavioural one.
