#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // BoatSystem — propulsion, rudder and hull hydrodynamics for BoatComponent
    // (issue #438).
    //
    // Deliberately layered ON TOP of BuoyancySystem rather than replacing it:
    // buoyancy owns the vertical axis (Archimedes + the corner-probe righting
    // torque), this owns the horizontal one. Both queue plain Jolt forces before
    // the physics step, so they simply sum — a boat is a buoyant hull that also
    // gets pushed.
    //
    // Per boat, per tick:
    //   * propeller thrust along the hull's horizontal forward axis, applied at
    //     the authored stern point (so it also trims the bow),
    //   * a rudder yaw torque whose authority scales with forward speed and
    //     REVERSES astern,
    //   * hull drag split into forward (skin friction) and lateral (keel)
    //     components, plus yaw drag — the lateral term is what makes the hull
    //     track through a turn instead of sliding broadside.
    //
    // Everything is scaled by how deeply the relevant point is immersed, sampled
    // through Physics3D/WaterProbe — the same surface BuoyancySystem floats the
    // hull on, so the two can't disagree about where the water is.
    // =========================================================================
    class BoatSystem
    {
      public:
        /// Queue this step's boat forces. Call once per tick, BEFORE
        /// JoltScene::Simulate / the world step, so Jolt integrates them this
        /// frame (the same queue-before-step contract as BuoyancySystem).
        /// `rawTime` must be the wave clock the water surface is driven from
        /// (Scene::m_SimulationTime on the physics path).
        static void OnUpdate(Scene* scene, f32 rawTime, f32 deltaTime);
    };
} // namespace OloEngine
