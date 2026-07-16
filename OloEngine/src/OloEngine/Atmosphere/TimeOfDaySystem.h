#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"

namespace OloEngine
{
    class Scene;

    // Astronomical time-of-day driver (issue #633, Pillar B).
    //
    // Split responsibilities:
    //  - AdvanceClock: advances TimeOfDayComponent's clock. Registered in the
    //    gameplay scheduler as "TimeOfDay" (play/simulate only). The editor
    //    tick calls AdvanceClockInEditMode instead, which only moves when the
    //    component opted in via m_AdvanceInEditMode.
    //  - Apply: runs on the RENDER path in BOTH editor and runtime (right
    //    before Scene::LoadAndRenderSkybox, which is followed by the light
    //    gather in ProcessScene3DSharedLogic) — computes the ephemeris and
    //    drives the scene's FIRST DirectionalLightComponent (direction,
    //    colour, intensity; swapped sun↔moon across the horizon with the
    //    weather director's sun-dimming folded in) plus the component's
    //    derived script-readable outputs. This makes the TimeOfDayComponent
    //    the single authoritative sun source: the fog/contact-shadow/caustics
    //    channels and the AtmosphereSky bake all follow the directional light.
    //
    // Apply deliberately mutates the DirectionalLightComponent every frame
    // while a TimeOfDayComponent is enabled (same precedent as the retired
    // m_LinkSunToDirectionalLight pull, in the opposite — correct — direction).
    class TimeOfDaySystem
    {
      public:
        static void AdvanceClock(Scene& scene, Timestep ts);
        static void AdvanceClockInEditMode(Scene& scene, Timestep ts);
        static void Apply(Scene& scene);
    };
} // namespace OloEngine
