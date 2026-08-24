#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // Entity-aware orchestration for the discovery loop (issue #881): landing
    // on a DiscoverableComponent-tagged, trigger-volume entity registers it
    // in the toucher's DiscoveredSetComponent, and the tick also drives the
    // opt-in UI (DiscoveryObjectiveMarkerComponent / DiscoveryReadoutComponent
    // — see Components.h for the full contract).
    class DiscoverySystem
    {
      public:
        // Per-frame: scan active physics contacts for new landings, then
        // refresh the objective marker + "Discovered N of M" readout.
        static void OnUpdate(Scene* scene, f32 dt);
    };

} // namespace OloEngine
