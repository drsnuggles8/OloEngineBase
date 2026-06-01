#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // BuoyancySystem — Gerstner-wave buoyancy for dynamic rigid bodies.
    //
    // For every entity carrying a BuoyancyComponent + a *dynamic*
    // Rigidbody3DComponent that floats over an enabled WaterComponent, samples
    // the water surface (Renderer/WaterSurface, the CPU mirror of Water.glsl) at
    // eight corner probes and applies:
    //   * an upward Archimedes force per submerged probe (acting at the corner,
    //     so the net result includes a self-righting torque), and
    //   * submerged linear + angular drag to damp bobbing / rocking.
    //
    // Bridges three subsystems — Scene (components), Physics3D (Jolt forces) and
    // the renderer's wave field — so it lives behind a small static entry point
    // mirroring NavigationSystem / AISystem. See
    // docs/WATER_FUTURE_IMPROVEMENTS.md §5.1.
    // =========================================================================
    class BuoyancySystem
    {
    public:
        /// Apply buoyancy forces for this physics step. Call once per tick, BEFORE
        /// JoltScene::Simulate, so the queued forces are integrated this frame.
        /// `rawTime` must be Time::GetTime() — the same clock the water shader is
        /// fed — so a floating body tracks the wave crest it is rendered on.
        static void OnUpdate(Scene* scene, f32 rawTime, f32 deltaTime);
    };
}
