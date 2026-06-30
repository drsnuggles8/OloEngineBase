#pragma once

#include "OloEngine/Core/Base.h"

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;

    // One serialized component within an entity's snapshot record: its stable
    // wire id (ComponentInterpolationRegistry::HashName) and the opaque bytes
    // produced by that component's ComponentReplicator serializer.
    struct SnapshotComponentData
    {
        u32 Id = 0;
        std::vector<u8> Bytes;
    };

    // The components serialized for a single entity in a snapshot, in registry order.
    using SnapshotEntity = std::vector<SnapshotComponentData>;

    // A whole snapshot parsed into UUID → per-component byte-blobs.
    using ParsedSnapshot = std::unordered_map<u64, SnapshotEntity>;

    // Server-side capture and client/rollback apply of replicated entity state.
    //
    // Wire format (repeated per entity, only for entities with ≥1 replicated
    // component):
    //   [uuid: u64][componentCount: u16]
    //   componentCount × { [componentId: u32][byteLen: u32][bytes…] }
    //
    // componentId is the FNV-1a-32 of the component name; an unknown id is skipped
    // by its byteLen, so the format tolerates registry drift and component
    // additions on either side of the wire. The set of components captured per
    // entity is driven entirely by ComponentInterpolationRegistry — adding a
    // replicated-and-interpolated component is a registration, not an edit here.
    class EntitySnapshot
    {
      public:
        // Capture a full snapshot of all replicated entities.
        static std::vector<u8> Capture(Scene& scene);

        // Capture a delta snapshot containing only entities whose serialized
        // component set differs from the baseline. Returns empty if nothing changed.
        static std::vector<u8> CaptureDelta(Scene& scene, const std::vector<u8>& baseline);

        // Apply a full or delta snapshot to the scene (snaps each component to its
        // serialized value — used for direct apply and rollback restore).
        static void Apply(Scene& scene, const std::vector<u8>& data);

        // Parse a snapshot buffer into UUID → per-component byte-blobs without
        // touching a Scene. Used by the interpolator (which caches and blends the
        // parsed result) and internally by Apply / CaptureDelta.
        [[nodiscard]] static ParsedSnapshot Parse(const std::vector<u8>& data);
    };
} // namespace OloEngine
