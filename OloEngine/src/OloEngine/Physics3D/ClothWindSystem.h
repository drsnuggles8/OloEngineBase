#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // ClothWindSystem — couples the scene's WindSystem to cloth soft bodies
    // (issue #460, wind-coupling slice).
    //
    // For every entity carrying an enabled ClothComponent with a live cloth
    // body, samples WindSystem::GetWindAtPoint at the entity's world position
    // and queues it as a uniform whole-body force (JoltScene::ApplyClothWindForce),
    // scaled by the cloth's mass and its own m_WindInfluence. This is a
    // deliberately simple model (no per-vertex sampling, no relative-velocity /
    // surface-area drag) — Jolt's soft-body AddForce only accepts a single
    // whole-body force per step anyway (it is divided evenly across every
    // particle), so a per-cloth uniform sample is the natural fit.
    //
    // Bridges three subsystems — Scene (components), Physics3D (Jolt forces)
    // and Wind (the CPU-side analytical query) — so it lives behind a small
    // static entry point mirroring BuoyancySystem / NavigationSystem / AISystem.
    // =========================================================================
    class ClothWindSystem
    {
      public:
        /// Apply wind forces to every cloth for this physics step. Call once per
        /// tick, BEFORE JoltScene::Simulate / BeginSteps, so the queued forces
        /// are integrated this frame (same timing contract as BuoyancySystem).
        /// `time` should be the deterministic Scene::m_SimulationTime, not
        /// wall-clock time, so cloth billow is reproducible across frame
        /// pacings and rollback re-sim (issue #452).
        static void OnUpdate(Scene* scene, f32 time);
    };
} // namespace OloEngine
