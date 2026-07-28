#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // AircraftSystem — a force-based fixed-wing flight model for
    // AircraftComponent (issue #438).
    //
    // Like BoatSystem, and unlike VehicleComponent's wheeled Jolt constraint,
    // this is plain AddForce/AddTorque on the entity's dynamic rigidbody, queued
    // before the physics step. That keeps an aircraft an ordinary Jolt body: it
    // collides with terrain, can be hit, and needs no special-case teardown.
    //
    // Per aircraft, per tick, all derived from the body's own velocity:
    //   * thrust along local +Z,
    //   * lift perpendicular to the relative wind — 0.5·rho·V²·A·Cl(alpha) with a
    //     post-stall falloff,
    //   * drag along the relative wind — Cd = Cd0 + k·Cl²,
    //   * pitch/roll/yaw control torques scaled by airspeed authority,
    //   * per-axis rate damping plus a weathervane term pulling the nose onto
    //     the relative wind. Those last two are the stability budget: rate
    //     damping alone only slows a divergence, the weathervane is what
    //     actually returns the airframe to trim.
    // =========================================================================
    class AircraftSystem
    {
      public:
        /// Queue this step's aerodynamic forces. Call once per tick, BEFORE
        /// JoltScene::Simulate / the world step, so Jolt integrates them this
        /// frame (the same queue-before-step contract as BuoyancySystem).
        static void OnUpdate(Scene* scene, f32 deltaTime);
    };
} // namespace OloEngine
