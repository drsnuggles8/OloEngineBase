#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    class Scene;

    class EntitySnapshot
    {
      public:
        // Capture a full snapshot of all replicated entities.
        static std::vector<u8> Capture(Scene& scene);

        // Capture a delta snapshot containing only entities whose transform
        // differs from the baseline. Returns empty if nothing changed.
        static std::vector<u8> CaptureDelta(Scene& scene, const std::vector<u8>& baseline);

        // Apply a full or delta snapshot to the scene.
        static void Apply(Scene& scene, const std::vector<u8>& data);
    };
} // namespace OloEngine
