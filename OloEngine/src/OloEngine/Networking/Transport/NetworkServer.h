#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"

#include <steam/steamnetworkingsockets.h>
#include <unordered_map>
#include <string>

namespace OloEngine
{
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

        [[nodiscard]] bool IsRunning() const;
        [[nodiscard]] const std::unordered_map<HSteamNetConnection, NetworkConnection>& GetConnections() const;
        [[nodiscard]] NetworkMessageDispatcher& GetDispatcher();
        [[nodiscard]] const NetworkStats& GetStats() const;

        // Get the round-trip time in milliseconds for a specific client connection.
        // Returns -1 if the connection is not found or RTT is unavailable.
        [[nodiscard]] i32 GetClientPingMs(HSteamNetConnection connection) const;

        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        void HandlePing(HSteamNetConnection senderConn, const u8* data, u32 size);

        HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        std::unordered_map<HSteamNetConnection, NetworkConnection> m_Connections;
        u32 m_NextClientID = 1;

        NetworkMessageDispatcher m_Dispatcher;
        NetworkStats m_Stats;
    };
} // namespace OloEngine
