#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    // Forward declarations
    class Scene;

    // @class EntitySnapshot
    // @brief Captures and applies full-state snapshots of all replicated entities.
    //
    // Capture iterates all entities that carry NetworkIdentityComponent +
    // TransformComponent and serializes them into a compact binary buffer using
    // FMemoryWriter (ArIsNetArchive = true).
    //
    // Apply reads the buffer with FMemoryReader and writes component values back
    // via Scene::GetEntityByUUID().
    //
    // Initial implementation uses full-state serialization.
    // Delta/dirty-flag tracking is deferred to a future optimization pass.
    class EntitySnapshot
    {
      public:
        // @brief Capture a snapshot of all replicated entities in the scene.
        // @param scene The scene to iterate.
        // @return Binary buffer containing the serialized snapshot.
        static std::vector<u8> Capture(Scene& scene);

        // @brief Apply a previously captured snapshot to the scene.
        // @param scene The scene to update.
        // @param data  The binary buffer produced by Capture().
        static void Apply(Scene& scene, const std::vector<u8>& data);
    };

} // namespace OloEngine
