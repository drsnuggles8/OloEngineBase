#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Networking/P2P/NetworkPeerMesh.h"
#include "OloEngine/Networking/Prediction/ClientPrediction.h"
#include "OloEngine/Networking/Prediction/LagCompensator.h"
#include "OloEngine/Networking/Prediction/ServerInputHandler.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"
#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"
#include "OloEngine/Threading/Mutex.h"

#include <optional>
#include <string>

struct SteamNetConnectionStatusChangedCallback_t;

namespace OloEngine
{
    class NetworkServer;
    class NetworkClient;
    class Scene;

    // Static singleton facade for the networking subsystem.
    //
    // Threading contract:
    //   - The network thread (NetworkThread) calls SteamNetworkingSockets::RunCallbacks(),
    //     dispatches queued tasks, and invokes TickSnapshots(). All calls from this thread
    //     acquire s_Mutex before accessing shared state.
    //   - The game thread calls the public API (Init, Shutdown, Connect, StartServer,
    //     SendInput, SetActiveScene, etc.). These also acquire s_Mutex.
    //   - s_Server, s_Client, s_ActiveScene, s_SnapshotRateHz, s_TickCounter,
    //     s_SnapshotAccumulator, s_SnapshotBuffer, and s_ClientInterpolator are shared
    //     between both threads and are always protected by s_Mutex.
    //   - s_ClientPrediction, s_ServerInputHandler, and s_LagCompensator are only
    //     mutated from the game thread (via handler callbacks registered at connection
    //     time). Their accessors intentionally do NOT acquire s_Mutex to avoid
    //     locking overhead on game-thread-only state.
    //   - s_Session and s_Lobby are game-thread-only.
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

        // Server-only: broadcast a snapshot to all connected clients
        static void BroadcastSnapshot(const u8* snapshotData, u32 snapshotSize);

        // Message dispatching
        static NetworkMessageDispatcher& GetServerDispatcher();
        static NetworkMessageDispatcher& GetClientDispatcher();

        // Statistics
        [[nodiscard]] static std::optional<NetworkStats> GetStats();

        // Access to server/client (for debug panel, etc.)
        [[nodiscard]] static NetworkServer* GetServer();
        [[nodiscard]] static NetworkClient* GetClient();

        // Connection status callback (called by GNS)
        static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

        // Snapshot broadcast configuration
        static void SetSnapshotRate(u32 hz);
        [[nodiscard]] static u32 GetSnapshotRate();

        // Set the active scene for snapshot capture/apply.
        static void SetActiveScene(Scene* scene);

        // Called by the network thread each tick. Captures + broadcasts server snapshots
        // at the configured rate, and on client side applies received snapshots to the interpolator.
        static void TickSnapshots();

        // Access server snapshot buffer (for delta baselines, lag compensation)
        [[nodiscard]] static SnapshotBuffer& GetSnapshotBuffer();

        // Access client interpolator
        [[nodiscard]] static SnapshotInterpolator& GetClientInterpolator();

        // Current server tick counter (incremented each TickSnapshots call on server)
        [[nodiscard]] static u32 GetCurrentTick();

        // --- Prediction & Input (Phase 2) ---

        // Client: record an input, apply locally (prediction), and send to server.
        // Serialized payload format: tick(u32) + entityUUID(u64) + inputData.
        static void SendInput(u64 entityUUID, std::vector<u8> inputData);

        // Set the callback defining how inputs are applied to the simulation.
        // Used by both client prediction and server input processing.
        static void SetInputApplyCallback(InputApplyCallback callback);

        // Access client prediction state
        [[nodiscard]] static ClientPrediction& GetClientPrediction();

        // Access server input handler
        [[nodiscard]] static ServerInputHandler& GetServerInputHandler();

        // --- Lag Compensation (Phase 3) ---

        // Get the RTT in milliseconds for a specific client (server-side only).
        // Returns -1 if the connection is not found.
        [[nodiscard]] static i32 GetClientPingMs(u32 clientID);

        // Access the lag compensator for server-side rewind checks.
        [[nodiscard]] static LagCompensator& GetLagCompensator();

        // --- Session & Lobby (Phase 7) ---

        // Access the session manager.
        [[nodiscard]] static NetworkSession* GetSession();

        // Access the lobby manager.
        [[nodiscard]] static NetworkLobby* GetLobby();

        // Access the P2P peer mesh.
        [[nodiscard]] static NetworkPeerMesh& GetPeerMesh();

      private:
        static FMutex s_Mutex; // Protects all static members accessed from network thread callbacks

        static bool s_Initialized;
        static Scope<NetworkServer> s_Server;
        static Scope<NetworkClient> s_Client;

        static u32 s_SnapshotRateHz;
        static u32 s_TickCounter;
        static u32 s_ClientReceivedTick; // Separate counter for client-received snapshots
        static f32 s_SnapshotAccumulator;
        static Scene* s_ActiveScene;
        static SnapshotBuffer s_SnapshotBuffer;
        static SnapshotInterpolator s_ClientInterpolator;
        static ClientPrediction s_ClientPrediction;
        static ServerInputHandler s_ServerInputHandler;
        static LagCompensator s_LagCompensator;
        static NetworkSession s_Session;
        static NetworkLobby s_Lobby;
        static NetworkPeerMesh s_PeerMesh;
    };
} // namespace OloEngine
