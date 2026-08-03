#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <steam/steamnetworkingsockets.h>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

namespace OloEngine
{
    // Invoked with the client's ID after its connection is torn down (peer close
    // or locally-detected problem), so owners of per-client state (e.g.
    // ServerInputHandler's last-processed-tick map) can prune it.
    using ClientDisconnectedCallback = std::function<void(u32 clientID)>;

    // A connect/disconnect transition, queued for the game thread.
    //
    // GNS delivers connection-status callbacks on the NETWORK thread. Anything that
    // reacts to a client arriving or leaving by touching the ECS — spawning that
    // client's player entity, destroying it again — must not run there: the game
    // thread is simultaneously iterating the same registry. So the transport only
    // RECORDS the transition, and the replication tick drains it on the game thread
    // at a point where mutating the scene is safe. See the threading contract on
    // NetworkManager.
    struct ClientConnectionEvent
    {
        u32 ClientID = 0;
        bool Connected = false; // false => disconnected
    };

    class NetworkServer
    {
      public:
        bool Start(u16 port);
        void Stop();
        void PollMessages();

        // Send data to a specific client connection
        bool SendTo(HSteamNetConnection connection, const void* data, u32 size, i32 sendFlags);

        // Broadcast data to all connected clients
        void Broadcast(const void* data, u32 size, i32 sendFlags);

        // Send a typed message to a specific client
        bool SendMessage(HSteamNetConnection connection, ENetworkMessageType type, const u8* payload, u32 payloadSize,
                         i32 sendFlags);

        // Broadcast a typed message to all clients
        void BroadcastMessage(ENetworkMessageType type, const u8* payload, u32 payloadSize, i32 sendFlags);

        // Send a typed message to one client by its logical client ID (the id the
        // message dispatcher reports as the sender, and the id every per-connection
        // replication/interest/ownership structure is keyed by). Returns false when
        // no connection currently carries that ID.
        bool SendMessageToClient(u32 clientID, ENetworkMessageType type, const u8* payload, u32 payloadSize,
                                 i32 sendFlags);

        // Broadcast to every connected client EXCEPT one — the shape a server-origin
        // event that the originating client already applied locally needs.
        void BroadcastMessageExcept(u32 exceptClientID, ENetworkMessageType type, const u8* payload, u32 payloadSize,
                                    i32 sendFlags);

        // Take the connect/disconnect transitions recorded since the last call.
        // Drained from the game thread; see ClientConnectionEvent.
        [[nodiscard]] std::vector<ClientConnectionEvent> DrainClientEvents();

        // The logical client IDs of every currently-connected client.
        [[nodiscard]] std::vector<u32> GetConnectedClientIDs() const;

        [[nodiscard]] bool IsRunning() const;
        [[nodiscard]] u32 GetConnectionCount() const;

        // Thread-safe iteration over connections — holds the mutex for the duration.
        template<typename Fn>
        void ForEachConnection(Fn&& fn) const;

        [[nodiscard]] NetworkMessageDispatcher& GetDispatcher();
        [[nodiscard]] NetworkStats GetStats() const;

        // Get the round-trip time in milliseconds for a specific client connection.
        // Returns -1 if the connection is not found or RTT is unavailable.
        //
        // DEADLOCK WARNING: this takes m_Mutex, and ForEachConnection holds m_Mutex
        // for the whole callback. Calling it from inside a ForEachConnection lambda
        // self-deadlocks on the non-recursive mutex. Use GetClientPingMsById below,
        // or collect the handles first and query after the iteration.
        [[nodiscard]] i32 GetClientPingMs(HSteamNetConnection connection) const;

        // Round-trip time for a client by its logical ID. Resolves the handle under
        // the lock and queries the transport after releasing it, so this is safe to
        // call from anywhere — including from code that has just iterated the
        // connections.
        [[nodiscard]] i32 GetClientPingMsById(u32 clientID) const;

        // One connection's identity + liveness, captured for use outside the lock.
        struct ConnectionInfo
        {
            HSteamNetConnection Handle = k_HSteamNetConnection_Invalid;
            u32 ClientID = 0;
            EConnectionState State = EConnectionState::None;
        };

        // Snapshot every connection. Prefer this over ForEachConnection whenever the
        // caller wants to do anything that re-enters the server (ping queries,
        // sends, closes) — it returns data rather than running the caller's code
        // under the lock.
        [[nodiscard]] std::vector<ConnectionInfo> GetConnectionSnapshot() const;

        // Set maximum number of simultaneous connections (0 = unlimited).
        void SetMaxConnections(u32 maxConnections);
        [[nodiscard]] u32 GetMaxConnections() const;

        // Set idle timeout in seconds. Connections with no messages for this duration are closed (0 = disabled).
        void SetIdleTimeout(f32 timeoutSeconds);
        [[nodiscard]] f32 GetIdleTimeout() const;

        // Close a specific client connection by handle (e.g., for kick).
        void CloseConnection(HSteamNetConnection connection, i32 reason = 0, const char* debug = "Kicked");

        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

        // Set the callback invoked when a client's connection is torn down.
        void SetClientDisconnectedCallback(ClientDisconnectedCallback callback);

      private:
        void HandlePing(HSteamNetConnection senderConn, const u8* data, u32 size);

        HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        std::unordered_map<HSteamNetConnection, NetworkConnection> m_Connections;
        u32 m_NextClientID = 1;

        NetworkMessageDispatcher m_Dispatcher;
        NetworkStats m_Stats;
        mutable FMutex m_Mutex;
        u32 m_MaxConnections = 0; // 0 = unlimited
        f32 m_IdleTimeout = 0.0f; // 0 = disabled
        ClientDisconnectedCallback m_ClientDisconnectedCallback;
        std::vector<ClientConnectionEvent> m_PendingClientEvents;
    };

    template<typename Fn>
    void NetworkServer::ForEachConnection(Fn&& fn) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        for (auto const& [handle, connection] : m_Connections)
        {
            fn(handle, connection);
        }
    }
} // namespace OloEngine
