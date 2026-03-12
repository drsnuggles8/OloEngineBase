#pragma once

#include "OloEngine/Core/Base.h"
#include "NetworkConnection.h"

#include <steam/steamnetworkingsockets.h>
#include <unordered_map>

namespace OloEngine
{
    // @class NetworkServer
    // @brief Listens for incoming GNS connections and manages connected peers.
    //
    // Call Start() to open a listen socket, then call PollMessages() every network
    // tick (from NetworkThread) to drain incoming data.
    class NetworkServer
    {
      public:
        NetworkServer() = default;
        ~NetworkServer() { Stop(); }

        // Non-copyable / non-movable (owns GNS handles)
        NetworkServer(const NetworkServer&)            = delete;
        NetworkServer& operator=(const NetworkServer&) = delete;

        bool Start(u16 port);
        void Stop();
        void PollMessages();

        [[nodiscard]] bool IsRunning() const { return m_ListenSocket != k_HSteamListenSocket_Invalid; }
        [[nodiscard]] const std::unordered_map<HSteamNetConnection, NetworkConnection>& GetConnections() const
        {
            return m_Connections;
        }

        // Called from the GNS status-changed callback
        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup m_PollGroup    = k_HSteamNetPollGroup_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        std::unordered_map<HSteamNetConnection, NetworkConnection> m_Connections;
        u32 m_NextClientID = 1;
    };

} // namespace OloEngine
