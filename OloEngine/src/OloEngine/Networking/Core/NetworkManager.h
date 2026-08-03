#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/ClientReplicationDriver.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Networking/Core/ServerReplicationDriver.h"
#include "OloEngine/Networking/P2P/NetworkPeerMesh.h"
#include "OloEngine/Networking/Prediction/ClientPrediction.h"
#include "OloEngine/Networking/Prediction/LagCompensator.h"
#include "OloEngine/Networking/Prediction/ServerInputHandler.h"
#include "OloEngine/Networking/RPC/RpcTypes.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"
#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"
#include "OloEngine/Threading/Mutex.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SteamNetConnectionStatusChangedCallback_t;

namespace OloEngine
{
    class NetworkServer;
    class NetworkClient;
    class Scene;

    // Static singleton facade for the networking subsystem.
    //
    // ── Threading contract ───────────────────────────────────────────────────
    //
    // There are exactly two threads in play and the split is by DATA, not by
    // subject matter:
    //
    //   NETWORK THREAD (NetworkThread) — runs SteamNetworkingSockets::RunCallbacks()
    //     and drains the named-thread task queue. GNS delivers connection-status
    //     callbacks here, so OnConnectionStatusChanged runs here. It touches
    //     transport state only. It does NOT touch the Scene, and it does NOT drive
    //     replication.
    //
    //   GAME THREAD — everything that reads or writes the ECS: Tick() (message
    //     polling, the replication tick, entity spawn/despawn, RPC handlers,
    //     interpolation), plus the public control API (Init, Connect, StartServer,
    //     SendInput, SetActiveScene…).
    //
    // Replication CANNOT run on the network thread, however convenient the tick
    // would be there: capturing components, applying snapshots and creating or
    // destroying entities all mutate the registry the game thread is iterating.
    // That is why Tick() is a game-thread call the host drives, and why a client
    // connecting is only RECORDED by the transport and acted on (spawning that
    // client's pawn) later, from Tick(). An earlier revision of this header claimed
    // the network thread invoked the snapshot tick; it did not — the tick had no
    // call site at all, in either thread.
    //
    // s_Mutex guards the members shared across that boundary: s_Server, s_Client,
    // s_ActiveScene and s_Initialized. It is released before any scene work, so a
    // replication tick never blocks a GNS callback for the length of a capture.
    // The drivers themselves are game-thread-only and are not mutex-protected.
    class NetworkManager
    {
      public:
        static bool Init();
        static void Shutdown();

        [[nodiscard]] static bool IsInitialized();

        // Server API
        static bool StartServer(u16 port);
        static void StopServer();
        [[nodiscard]] static bool IsServer();

        // Client API
        static bool Connect(const std::string& address, u16 port);
        static void Disconnect();
        [[nodiscard]] static bool IsClient();
        [[nodiscard]] static bool IsConnected();

        // Message sending (high-level)
        // Client: sends to server. Server: broadcasts to all clients.
        static bool SendNetworkMessage(ENetworkMessageType type, const u8* payload, u32 payloadSize, i32 sendFlags);

        // Server-only: broadcast a snapshot to all connected clients. Bypasses the
        // per-connection scoping in the replication tick — for bespoke/manual use;
        // the loop itself does not go through here.
        static void BroadcastSnapshot(const u8* snapshotData, u32 snapshotSize);

        // Message dispatching
        static NetworkMessageDispatcher& GetServerDispatcher();
        static NetworkMessageDispatcher& GetClientDispatcher();

        // Statistics
        [[nodiscard]] static std::optional<NetworkStats> GetStats();

        // Access to server/client (for debug panel, etc.)
        [[nodiscard]] static NetworkServer* GetServer();
        [[nodiscard]] static NetworkClient* GetClient();

        // Connection status callback (called by GNS on the network thread)
        static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

        // Snapshot broadcast configuration
        static void SetSnapshotRate(u32 hz);
        [[nodiscard]] static u32 GetSnapshotRate();

        // Set the active scene for replication. Both the dedicated server
        // (OloServerApp) and the editor / runtime hosts must call this when their
        // runtime scene starts, and pass nullptr when it stops — the drivers hold
        // no ownership and a dangling scene pointer would be dereferenced by the
        // very next Tick().
        static void SetActiveScene(Scene* scene);
        [[nodiscard]] static Scene* GetActiveScene();

        // ── The loop ─────────────────────────────────────────────────────────

        // Drive one frame of networking. GAME THREAD ONLY. Call once per frame
        // from the host's update, after the scene has simulated:
        //   * polls the transport (running every queued message handler),
        //   * drains connect/disconnect events and runs the player lifecycle,
        //   * fires the fixed-rate replication tick when due,
        //   * advances client-side interpolation.
        // Safe to call with no scene, no server and no client — it early-outs.
        static void Tick(f32 dt);

        [[nodiscard]] static ServerReplicationDriver& GetServerDriver();
        [[nodiscard]] static ClientReplicationDriver& GetClientDriver();

        // Access server snapshot history (delta baselines, lag compensation)
        [[nodiscard]] static SnapshotBuffer& GetSnapshotBuffer();

        // Access client interpolator
        [[nodiscard]] static SnapshotInterpolator& GetClientInterpolator();

        // Current server replication tick (incremented once per replication tick).
        [[nodiscard]] static u32 GetCurrentTick();

        // ── Prediction & Input ───────────────────────────────────────────────

        // Client: record an input, apply it locally (prediction), and send it to
        // the server. The tick carried is the CLIENT's own input counter — see
        // ClientReplicationDriver::SendInput for why it cannot be the server tick.
        static void SendInput(u64 entityUUID, std::vector<u8> inputData);

        // Set the callback defining how inputs are applied to the simulation.
        // Registered on both the client prediction path and the server's
        // authoritative path, so the two cannot diverge.
        static void SetInputApplyCallback(InputApplyCallback callback);

        [[nodiscard]] static ClientPrediction& GetClientPrediction();
        [[nodiscard]] static ServerInputHandler& GetServerInputHandler();

        // ── Entity lifecycle ─────────────────────────────────────────────────

        // Server-side: create a replicated entity. Clients pick it up through the
        // normal relevance pass.
        //
        // DEFERRED, and returns the entity's UUID rather than the entity: the
        // structural registry change is applied at the top of the next Tick, not
        // here. This is the script-facing surface, and Scene::UpdateScripts
        // dispatches OnUpdate WHILE iterating the script pools — spawning inline
        // from a script would invalidate that iterator. Same "always defer" rule
        // and pre-allocated-UUID shape as Scene::ScriptCreateEntity.
        //
        // The returned UUID is valid immediately as an identifier (it is what the
        // entity will have); the entity itself does not exist until the next Tick.
        // Returns 0 if there is no active scene.
        static u64 SpawnReplicated(std::string_view archetype, const std::string& name, u32 ownerClientID,
                                   ENetworkAuthority authority);

        // Server-side: destroy a replicated entity and tell every client that knows
        // it. Deferred for the same reason as SpawnReplicated.
        static void DespawnReplicated(u64 entityUUID);

        // ── RPC ──────────────────────────────────────────────────────────────

        static void RegisterRPC(RpcDescriptor descriptor);

        // Invoke a registered RPC by name. Routing follows the descriptor's target:
        // a Server RPC goes to the server (or runs locally if we ARE the server), a
        // Client RPC goes to `targetClientID`, a Multicast goes to everyone and runs
        // locally. Returns false when the call is refused (unknown name, an
        // authority violation, or no transport in the required direction).
        static bool InvokeRPC(std::string_view name, u64 entityUUID, const RpcArgList& args, u32 targetClientID = 0);

        // ── Lag compensation ─────────────────────────────────────────────────

        // Get the RTT in milliseconds for a specific client (server-side only).
        // Returns -1 if the connection is not found.
        [[nodiscard]] static i32 GetClientPingMs(u32 clientID);

        [[nodiscard]] static LagCompensator& GetLagCompensator();

        // Server-side: rewind the world to where `clientID` saw it, run `callback`,
        // then restore. This is the hook an authoritative hit check goes through.
        static bool PerformLagCompensatedCheck(u32 clientID, const LagCompensator::RewindCallback& callback);

        // ── Session & Lobby ──────────────────────────────────────────────────

        [[nodiscard]] static NetworkSession* GetSession();
        [[nodiscard]] static NetworkLobby* GetLobby();
        [[nodiscard]] static NetworkPeerMesh& GetPeerMesh();

      private:
        // Install the server-side message handlers that route into the driver.
        static void RegisterServerHandlers();

        static FMutex s_Mutex; // Protects the members shared with the network thread

        static bool s_Initialized;
        static Scope<NetworkServer> s_Server;
        static Scope<NetworkClient> s_Client;
        static Scene* s_ActiveScene;

        // Game-thread-only. Deliberately not mutex-protected: every path that
        // touches them is already confined to the game thread by the ECS access
        // they perform, and taking a lock here would only hide that requirement.
        static ServerReplicationDriver s_ServerDriver;
        static ClientReplicationDriver s_ClientDriver;

        static NetworkSession s_Session;
        static NetworkLobby s_Lobby;
        static NetworkPeerMesh s_PeerMesh;
    };
} // namespace OloEngine
