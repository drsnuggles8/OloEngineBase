#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Prediction/ClientPrediction.h"
#include "OloEngine/Networking/RPC/RpcTypes.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class NetworkClient;
    class Scene;

    // The client half of the server-authoritative loop.
    //
    // Like ServerReplicationDriver this is an object rather than more statics, so a
    // single-process test can run two independent clients against one server — the
    // only way to actually observe cross-client convergence.
    //
    // WHERE THIS RUNS. Tick() is game-thread only, for the same reason the server
    // driver is: it polls the transport (which dispatches handlers that create,
    // destroy and write ECS entities) and then writes interpolated component values
    // into the scene.
    //
    // DELTA REASSEMBLY. The server sends deltas — only the entities whose state
    // changed. The driver folds each delta into a running authoritative map and
    // pushes the REASSEMBLED full state into the interpolator. Pushing raw deltas
    // instead would give the interpolator two brackets with different entity sets,
    // so an entity absent from the newer one would either hold a stale value or
    // snap, depending on which side of the bracket it fell — motion that looks like
    // packet loss but is really a client-side reassembly bug.
    class ClientReplicationDriver
    {
      public:
        ClientReplicationDriver();

        // Install the handlers this driver needs on a client's dispatcher. Safe to
        // call once per connection; re-registering replaces the previous handler.
        //
        // `scene` must outlive the connection: the dispatcher invokes the handlers
        // from Tick(), on the game thread, with this scene.
        void AttachTo(NetworkClient& client, Scene& scene);

        // Bumped by every AttachTo. The handlers installed above capture the scene
        // POINTER by value, so a host that swaps the runtime scene and fails to
        // re-attach leaves them dereferencing freed memory — a use-after-free with
        // no error of its own. A caller cannot check that by comparing scene
        // pointers, because a new Scene can be allocated at the address the old one
        // just vacated and compare equal; this counter is what makes "did the
        // rebinding actually happen" observable.
        [[nodiscard]] u32 GetAttachGeneration() const
        {
            return m_AttachGeneration;
        }

        // Poll the transport (running every queued handler) and advance
        // interpolation by `dt` seconds. Game thread only.
        void Tick(Scene& scene, NetworkClient& client, f32 dt);

        // Record an input for the entity this client owns, apply it locally
        // (prediction), and send it to the server.
        //
        // The tick is the driver's OWN monotonically increasing input counter. It
        // must not be the server's replication tick: on a client that counter never
        // advances, so every input would carry tick 0 and the server — which rejects
        // `tick <= last processed` — would drop everything after the first.
        void SendInput(Scene& scene, NetworkClient& client, u64 entityUUID, std::vector<u8> inputData);

        // Send a Server-target RPC. Refuses Client/Multicast descriptors: only the
        // server may originate those.
        bool InvokeRpc(NetworkClient& client, const RpcDescriptor& descriptor, u64 entityUUID, const RpcArgList& args);

        // Listen-server mode: this client shares its Scene with a server running in
        // the same process.
        //
        // The scene is then ALREADY authoritative, so every client-side write is not
        // just redundant but actively wrong — interpolation would drag entities back
        // to a ~100 ms-old position the server had already moved past, and
        // reconciliation would replay inputs the server has already applied. The
        // client half still runs its *identity* and *RPC* handlers (it needs its
        // assigned id, and multicast RPCs must reach it), and still sends input; it
        // just stops writing replicated state.
        void SetSharedSceneWithServer(bool shared);
        [[nodiscard]] bool IsSharedSceneWithServer() const;

        // The input-apply callback drives BOTH local prediction and the server's
        // authoritative application, so a game registers exactly one and the two
        // sides cannot diverge by construction.
        void SetInputApplyCallback(InputApplyCallback callback);

        // ── Identity ─────────────────────────────────────────────────────────

        // Assigned by the server's Connect message. 0 until it arrives.
        [[nodiscard]] u32 GetLocalClientID() const;

        // The replicated entity this client owns and predicts (its pawn), or 0.
        // Resolved by ownership rather than by name so it survives renames and
        // multiple pawns per player being added later.
        [[nodiscard]] u64 FindLocalPlayerEntity(Scene& scene) const;

        // ── Introspection ────────────────────────────────────────────────────

        [[nodiscard]] SnapshotInterpolator& GetInterpolator();
        [[nodiscard]] ClientPrediction& GetPrediction();
        [[nodiscard]] u32 GetLastReceivedServerTick() const;
        [[nodiscard]] u32 GetCurrentInputTick() const;
        [[nodiscard]] const std::unordered_set<u64>& GetLocallySpawnedEntities() const;

        // Drop all per-session state (authoritative map, spawned set, prediction
        // buffers, client id). Call on disconnect so a reconnect never mixes
        // session-A state into session-B.
        void Reset();

        // ── Message entry points (public so tests can drive them directly) ────

        void HandleConnectAssignment(const u8* data, u32 size);
        void HandleSpawn(Scene& scene, const u8* data, u32 size);
        void HandleDespawn(Scene& scene, const u8* data, u32 size);
        // `client` may be null when a test drives the driver without a transport;
        // the snapshot is still applied, only the ack is skipped.
        void HandleSnapshot(const u8* data, u32 size, NetworkClient* client);
        void HandleInputAck(Scene& scene, const u8* data, u32 size);
        void HandleRpc(Scene& scene, const u8* data, u32 size);

      private:
        // Fold m_Authoritative back into a snapshot buffer and hand it to the
        // interpolator.
        void PushReassembledSnapshot(u32 serverTick);

        // Snap every entity this client owns to the newest authoritative state,
        // before prediction replays the unacknowledged inputs on top. Without this
        // the replay stacks on the client's own drifting prediction and never
        // converges — reconciliation that never actually reconciles.
        void SnapOwnedEntitiesToAuthoritative(Scene& scene);

        u32 m_LocalClientID = 0;
        u32 m_InputTick = 0;
        u32 m_LastServerTick = 0;
        u32 m_AttachGeneration = 0;
        bool m_SharedSceneWithServer = false;

        // Running authoritative state, keyed by UUID. Deltas are folded in;
        // despawns are erased.
        ParsedSnapshot m_Authoritative;

        // Entities THIS driver created from a spawn message. A despawn may destroy
        // only these — an entity that came from the client's own scene file must
        // survive leaving relevance, or walking out of range would permanently
        // delete the level.
        std::unordered_set<u64> m_LocallySpawned;

        SnapshotInterpolator m_Interpolator;
        ClientPrediction m_Prediction;
        InputApplyCallback m_ApplyCallback;
    };
} // namespace OloEngine
