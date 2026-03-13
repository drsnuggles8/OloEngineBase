#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"

#include <string>

struct SteamNetConnectionStatusChangedCallback_t;

namespace OloEngine
{
    class NetworkServer;
    class NetworkClient;

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
        [[nodiscard]] static const NetworkStats* GetStats();

        // Access to server/client (for debug panel, etc.)
        [[nodiscard]] static NetworkServer* GetServer();
        [[nodiscard]] static NetworkClient* GetClient();

        // Connection status callback (called by GNS)
        static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        static bool s_Initialized;
        static Scope<NetworkServer> s_Server;
        static Scope<NetworkClient> s_Client;
    };
} // namespace OloEngine
