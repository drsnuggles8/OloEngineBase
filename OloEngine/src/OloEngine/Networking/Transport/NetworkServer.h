#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"

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

        [[nodiscard]] bool IsRunning() const;
        [[nodiscard]] const std::unordered_map<HSteamNetConnection, NetworkConnection>& GetConnections() const;

        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

    private:
        HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        std::unordered_map<HSteamNetConnection, NetworkConnection> m_Connections;
        u32 m_NextClientID = 1;
    };
}
