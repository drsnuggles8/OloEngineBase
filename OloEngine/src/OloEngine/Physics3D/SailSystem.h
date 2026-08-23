#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // SailSystem — wind propulsion for SailComponent (issue #899).
    //
    // The air-side sibling of BoatSystem. BoatSystem owns the water (propeller,
    // rudder, keel); this owns the air (apparent wind, yard trim, sail force).
    // Both queue plain Jolt forces before the physics step, so a boat carrying
    // both simply gets the sum — a sailing boat with an auxiliary engine.
    //
    // Per sail, per tick:
    //   * sample the true wind at the centre of effort (WindSystem's analytical
    //     CPU query against the scene's WindSettings) and subtract the hull's
    //     own horizontal velocity to get the APPARENT wind — the only wind a
    //     sail ever feels, and the reason a boat cannot outrun the breeze on a
    //     dead run;
    //   * brace the yard, either to the closed-form angle that maximises
    //     forward drive (auto-trim) or to the driver's manual command, slewed
    //     rather than snapped;
    //   * apply the flat-plate normal force at the centre of effort. Applying
    //     it ABOVE the centre of mass is what heels the boat; applying it aft
    //     of the centre of mass is what gives it weather helm.
    //
    // The model, the sign conventions and the emergent no-go zone are all
    // documented on SailComponent in Scene/Components.h — this file implements
    // what is described there.
    // =========================================================================
    class SailSystem
    {
      public:
        /// Queue this step's sail forces. Call once per tick, BEFORE
        /// JoltScene::Simulate / the world step, so Jolt integrates them this
        /// frame (the same queue-before-step contract as BoatSystem).
        /// `rawTime` must be the deterministic simulation clock the wind's gust
        /// phase is driven from (Scene::m_SimulationTime on the physics path).
        static void OnUpdate(Scene* scene, f32 rawTime, f32 deltaTime);
    };
} // namespace OloEngine
