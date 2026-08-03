#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Prediction/LagCompensator.h"
#include "OloEngine/Networking/Prediction/ServerInputHandler.h"
#include "OloEngine/Networking/RPC/RpcTypes.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Networking/Replication/NetworkInterestManager.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"
#include "OloEngine/Scene/Components.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine
{
    class Entity;
    class NetworkServer;
    class Scene;

    // The authoritative server half of the multiplayer loop.
    //
    // WHERE THIS RUNS. Tick() must be called from the GAME thread, after the
    // scene's simulation step. Everything it does — capturing components, spawning
    // and destroying entities, running RPC handlers — touches the ECS registry the
    // game thread is already iterating, so running it on the network thread is a
    // data race, not a latency optimisation. The network thread's job stays what it
    // always was: GNS callbacks and named-thread tasks. (The header comment that
    // used to claim NetworkThread invoked the snapshot tick was simply false; the
    // tick had no call site at all.)
    //
    // WHY IT IS AN OBJECT, not more statics on NetworkManager. A singleton facade
    // can hold exactly one server and one client, which makes a two-client test
    // impossible to write in one process — and "no test could observe the loop" is
    // a large part of how this stack stayed dead. The driver takes its Scene and
    // NetworkServer as parameters so a test can stand up one server driver and two
    // independent client drivers side by side.
    //
    // PER-CONNECTION, NOT BROADCAST. Each connection carries its own relevance set,
    // its own delta baseline, and its own known-entity set. One shared baseline is
    // wrong the moment two clients see different subsets: a delta computed against
    // "the last thing anyone was sent" silently omits entities the receiving client
    // has never seen.
    class ServerReplicationDriver
    {
      public:
        // Returns the UUID of the entity that now represents `clientID`, or 0 to
        // leave that client without a pawn. Runs on the game thread inside Tick().
        using PlayerSpawnCallback = std::function<u64(Scene&, u32 clientID)>;
        // Invoked before the driver forgets the client. `playerEntity` is whatever
        // the spawn callback returned (0 if none).
        using PlayerDespawnCallback = std::function<void(Scene&, u32 clientID, u64 playerEntity)>;

        ServerReplicationDriver();

        // ── Configuration ────────────────────────────────────────────────────

        // Replication ticks per second. Independent of both the render frame rate
        // and the simulation's fixed timestep: Tick() accumulates real time and
        // fires on a fixed interval, so the wire rate does not drift with frame
        // rate. 0 is rejected.
        void SetSnapshotRate(u32 hz);
        [[nodiscard]] u32 GetSnapshotRate() const;

        // Scope each connection's snapshot through NetworkInterestManager. Turning
        // it off sends every replicated entity to every client — useful for a small
        // scene, and the switch a test flips to isolate an interest bug from a
        // replication bug.
        void SetInterestScopingEnabled(bool enabled);
        [[nodiscard]] bool IsInterestScopingEnabled() const;

        // Archetype the default player-per-connection lifecycle spawns. Empty
        // disables the built-in spawn (a game that supplies its own
        // PlayerSpawnCallback does not need it).
        void SetPlayerArchetype(std::string archetype);
        [[nodiscard]] const std::string& GetPlayerArchetype() const;

        void SetPlayerSpawnCallback(PlayerSpawnCallback callback);
        void SetPlayerDespawnCallback(PlayerDespawnCallback callback);

        // ── The loop ─────────────────────────────────────────────────────────

        // Drain connection events, then (at the configured rate) capture history,
        // and send each connection its spawns, despawns, scoped delta and input ack.
        // `dt` is real frame time in seconds.
        void Tick(Scene& scene, NetworkServer& server, f32 dt);

        // ── Inbound message handling (register these on the server dispatcher) ─

        // Validate + apply a client input command, and remember that we owe that
        // client an ack for the tick it carried.
        void HandleInputCommand(Scene& scene, u32 senderClientID, const u8* data, u32 size);

        // Decode + authority-check + run an RPC a client pushed to us.
        void HandleRpc(Scene& scene, u32 senderClientID, const u8* data, u32 size);

        // Advance a client's delta baseline to the newest snapshot it confirms
        // having applied.
        void HandleSnapshotAck(u32 senderClientID, const u8* data, u32 size);

        // ── Server-side gameplay API ─────────────────────────────────────────

        // Create a replicated entity IMMEDIATELY. Clients learn about it through
        // the normal relevance pass, so there is no separate "broadcast the spawn"
        // step.
        //
        // This is a structural registry mutation, so it is only safe from a caller
        // that is NOT inside Scene::UpdateScripts. The driver's own player lifecycle
        // qualifies (it runs from Tick); anything script-reachable must use the
        // deferred queue below.
        [[nodiscard]] Entity SpawnReplicated(Scene& scene, std::string_view archetype, const std::string& name,
                                             u32 ownerClientID, ENetworkAuthority authority);

        // As above, but with a caller-chosen UUID — how a deferred spawn keeps the
        // id it already handed back to its caller.
        [[nodiscard]] Entity SpawnReplicatedWithUUID(Scene& scene, UUID uuid, std::string_view archetype,
                                                     const std::string& name, u32 ownerClientID,
                                                     ENetworkAuthority authority);

        // Destroy a replicated entity and tell every client that already knows it.
        void DespawnReplicated(Scene& scene, NetworkServer& server, u64 entityUUID);

        // Queue a spawn/despawn to be applied at the top of the next Tick.
        //
        // The script-safe path. Scene::UpdateScripts dispatches OnUpdate WHILE
        // iterating the script component pools, so a script that spawned or
        // destroyed an entity inline would invalidate that iterator — a delayed,
        // unrelated-looking failure rather than a crash at the call site. Same
        // "always defer" rule (and pre-allocated-UUID shape) as
        // Scene::ScriptCreateEntity; see
        // docs/agent-rules/script-structural-command-safe-point.md.
        void QueueSpawn(UUID uuid, std::string archetype, std::string name, u32 ownerClientID,
                        ENetworkAuthority authority);
        void QueueDespawn(u64 entityUUID);

        // Send a Client- or Multicast-target RPC. `targetClientID` is required for
        // ERpcTarget::Client and ignored for Multicast (which also runs locally).
        bool InvokeRpc(Scene& scene, NetworkServer& server, const RpcDescriptor& descriptor, u64 entityUUID,
                       u32 targetClientID, const RpcArgList& args);

        // Rewind the world to where `clientID` saw it, run `callback`, restore.
        //
        // The rewind target is the client's own view time: current tick minus the
        // half-RTT it took their command to reach us, minus the interpolation delay
        // their client renders behind the newest snapshot. Rewinding by RTT alone
        // over-shoots by the render delay and under-registers hits on moving targets.
        bool PerformLagCompensatedCheck(Scene& scene, NetworkServer& server, u32 clientID,
                                        const LagCompensator::RewindCallback& callback);

        // The render delay clients are assumed to run (seconds). Must match
        // SnapshotInterpolator::SetRenderDelay on the client or lag compensation
        // rewinds to the wrong moment.
        void SetClientRenderDelay(f32 seconds);
        [[nodiscard]] f32 GetClientRenderDelay() const;

        // ── Introspection ────────────────────────────────────────────────────

        [[nodiscard]] u32 GetCurrentTick() const;
        [[nodiscard]] const SnapshotBuffer& GetHistory() const;
        [[nodiscard]] SnapshotBuffer& GetHistory();
        [[nodiscard]] NetworkInterestManager& GetInterestManager();
        [[nodiscard]] ServerInputHandler& GetInputHandler();
        [[nodiscard]] LagCompensator& GetLagCompensator();
        [[nodiscard]] u64 GetPlayerEntity(u32 clientID) const;
        [[nodiscard]] std::vector<u32> GetTrackedClients() const;

        // Forget every connection and reset the tick clock. Called when the server
        // stops so a restart never replays the previous session's baselines.
        void Reset();

      private:
        // How many un-acked snapshots we keep per client before giving up on the
        // oldest. At 20 Hz this is ~1.6 s of loss tolerance; past that the client
        // gets a full resend, which is the correct degradation.
        static constexpr sizet kMaxPendingSnapshotsPerClient = 32;

        struct ClientState
        {
            // The newest state this connection has CONFIRMED applying. Deltas are
            // computed against this, never against "whatever we sent last": a
            // snapshot lost in flight must not silently become the baseline for
            // every delta that follows it.
            std::vector<u8> AckedBaseline;
            u32 AckedTick = 0;
            // tick → the full scoped state sent at that tick, kept until acked (or
            // aged out).
            std::vector<std::pair<u32, std::vector<u8>>> PendingSnapshots;
            // Entities this connection has been told exist. Drives spawn on entry
            // and despawn on exit, and is what stops a despawn from destroying an
            // entity the client loaded from its own scene file.
            std::unordered_set<u64> Known;
            u64 PlayerEntity = 0;
            // Last tick we told this client we had processed. Only re-sent when it
            // changes, so an idle client costs no acks.
            u32 LastAckSent = 0;
            bool HasSentAck = false;
        };

        void HandleClientConnected(Scene& scene, NetworkServer& server, u32 clientID);
        void HandleClientDisconnected(Scene& scene, NetworkServer& server, u32 clientID);

        // The replicated entities `clientID` may currently see, in a stable order.
        [[nodiscard]] std::vector<u64> ComputeRelevantSet(Scene& scene, u32 clientID);

        // The archetype an entity was spawned with, or "" for a scene-authored one.
        // Recorded at spawn rather than guessed from the entity's shape: two
        // archetypes can produce the same components, and guessing would send a
        // client the wrong construction recipe.
        [[nodiscard]] std::string_view LookupArchetype(u64 entityUUID) const;

        void ReplicateToClient(Scene& scene, NetworkServer& server, u32 clientID, ClientState& state);

        u32 m_SnapshotRateHz = 20;
        f32 m_Accumulator = 0.0f;
        u32 m_Tick = 0;
        bool m_InterestScoping = true;
        f32 m_ClientRenderDelay = 0.1f;
        std::string m_PlayerArchetype = NetworkSpawnRegistry::kNetworkPlayerArchetype;

        PlayerSpawnCallback m_PlayerSpawnCallback;
        PlayerDespawnCallback m_PlayerDespawnCallback;

        // A spawn requested from script-reachable code, waiting for the safe point
        // at the top of Tick.
        struct PendingSpawn
        {
            UUID EntityUUID{ 0 };
            std::string Archetype;
            std::string Name;
            u32 OwnerClientID = 0;
            ENetworkAuthority Authority = ENetworkAuthority::Server;
        };

        std::vector<PendingSpawn> m_PendingSpawns;
        std::vector<u64> m_PendingDespawns;

        std::unordered_map<u32, ClientState> m_Clients;
        std::unordered_map<u64, std::string> m_SpawnedArchetypes;
        SnapshotBuffer m_History;
        NetworkInterestManager m_Interest;
        ServerInputHandler m_InputHandler;
        LagCompensator m_LagCompensator;
    };
} // namespace OloEngine
