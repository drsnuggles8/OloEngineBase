#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Components.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    class Entity;
    class Scene;

    // Identity + construction recipe for a replicated entity, shared by the
    // server-side spawn and the client-side apply so the two sides cannot drift.
    struct NetworkSpawnParams
    {
        u64 EntityUUID = 0;
        std::string Name;
        // Names a NetworkSpawnRegistry factory. Empty means "no archetype" — the
        // client creates a bare entity and lets the replicated component set fill
        // it in, which is what a scene-static entity wants.
        std::string Archetype;
        u32 OwnerClientID = 0;
        ENetworkAuthority Authority = ENetworkAuthority::Server;
    };

    // Applied to an entity that has ALREADY been created with the replicated UUID.
    // A factory adds the components the archetype implies; the replicated component
    // values are snapped on top afterwards, so a factory should not try to
    // reconstruct state — only structure.
    using NetworkSpawnFactory = std::function<void(Entity&, const NetworkSpawnParams&)>;

    // Archetype name → factory. The wire carries the name, so both ends must have
    // registered it; an unknown archetype degrades to a bare entity (the replicated
    // components still arrive) rather than dropping the spawn.
    class NetworkSpawnRegistry
    {
      public:
        // The engine-provided archetype used by the dedicated server's default
        // player-per-connection lifecycle and by the 2-client demo: a transform, a
        // client-authoritative NetworkIdentityComponent, and a per-client-coloured
        // sprite so the entity is visible with no asset dependencies.
        static constexpr const char* kNetworkPlayerArchetype = "NetworkPlayer";

        static void RegisterDefaults();
        static void Register(std::string archetype, NetworkSpawnFactory factory);
        [[nodiscard]] static NetworkSpawnFactory Find(std::string_view archetype);
        static void Clear();

        // Stable, readable per-client colour so two clients are visually
        // distinguishable in the demo without any per-scene authoring.
        [[nodiscard]] static glm::vec4 PlayerColorForClient(u32 clientID);
    };

    // Wire codec for ENetworkMessageType::EntitySpawn / EntityDespawn.
    //
    // Spawn payload:
    //   [uuid: u64][name: string][archetype: string][ownerClientID: u32][authority: u8]
    //   followed by ONE standard EntitySnapshot entity record
    //   ([uuid: u64][componentCount: u16] then [id][len][bytes] per component)
    //
    // The uuid appears in BOTH the header and the record, and the two must agree.
    // It has to be in the header because an entity whose replicated component set is
    // empty produces an EMPTY record — a spawn that could not name its own entity
    // would be undecodable.
    //
    // Reusing the snapshot record keeps a single hardened parser for the
    // attacker-controlled part of the payload and means a spawn ships the entity's
    // full initial state in the same message that creates it — the client never
    // renders a frame of default-constructed placeholder.
    //
    // Despawn payload: [uuid: u64]
    class EntityLifecycle
    {
      public:
        [[nodiscard]] static std::vector<u8> EncodeSpawn(const NetworkSpawnParams& params, const SnapshotEntity& comps);
        [[nodiscard]] static bool DecodeSpawn(const u8* data, u32 size, NetworkSpawnParams& outParams,
                                              SnapshotEntity& outComps);

        [[nodiscard]] static std::vector<u8> EncodeDespawn(u64 entityUUID);
        [[nodiscard]] static bool DecodeDespawn(const u8* data, u32 size, u64& outEntityUUID);

        // Build the spawn parameters for a live server-side entity.
        [[nodiscard]] static NetworkSpawnParams DescribeEntity(Entity& entity, std::string_view archetype);

        // Client side: find-or-create the entity by UUID, run the archetype factory
        // when we created it, then snap every replicated component in the record
        // (adding the ones the entity lacks). Returns the resolved entity, and sets
        // `outCreated` when this call is what brought it into existence — the
        // caller uses that to decide whether a later despawn may destroy it. A
        // client must never destroy an entity that came from its own scene file.
        static Entity ApplySpawn(Scene& scene, const NetworkSpawnParams& params, const SnapshotEntity& comps,
                                 bool& outCreated);
    };
} // namespace OloEngine
