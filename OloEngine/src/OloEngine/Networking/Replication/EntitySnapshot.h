#pragma once

#include "OloEngine/Core/Base.h"

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;
    class Entity;

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

        // ── Per-connection (interest-scoped) capture ──────────────────────────
        //
        // The plain Capture/CaptureDelta above serialize EVERY replicated entity
        // and are what a single shared broadcast needs. A server-authoritative loop
        // instead sends each connection only what that connection is allowed to see
        // (NetworkInterestManager) and deltas it against THAT connection's own
        // baseline — one client moving out of relevance must not change what
        // another client receives. These take the already-computed relevant set.

        // Capture only `uuids`, in the given order, skipping any that are missing,
        // not replicated, or carry no replicated component.
        [[nodiscard]] static std::vector<u8> CaptureScoped(Scene& scene, const std::vector<u64>& uuids);

        // As CaptureScoped, but emits only the entities whose serialized component
        // set differs from `baseline` (which must be a snapshot buffer produced for
        // the SAME connection). Returns empty when nothing in scope changed.
        [[nodiscard]] static std::vector<u8> CaptureScopedDelta(Scene& scene, const std::vector<u64>& uuids,
                                                                const std::vector<u8>& baseline);

        // Serialize one entity's replicated components (registry order). Empty when
        // the entity carries none. Used by entity-spawn replication, which ships an
        // entity's full initial state alongside its identity.
        [[nodiscard]] static SnapshotEntity CaptureEntity(Entity& entity);

        // Append one entity record to an existing snapshot buffer writer's bytes.
        // Exposed so spawn payloads and snapshots share one encoder.
        static void AppendEntityRecord(std::vector<u8>& buffer, u64 uuid, const SnapshotEntity& comps);

        // Write one parsed entity record onto a live entity. When `ensureComponents`
        // is true, a component the entity does not yet carry is ADDED first (via the
        // registry's Ensure hook) instead of being skipped — which is exactly what a
        // freshly spawned client-side entity needs, and exactly what a routine
        // snapshot apply must NOT do.
        static void ApplyEntityRecord(Entity& entity, const SnapshotEntity& comps, bool ensureComponents);
    };
} // namespace OloEngine
