#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"

#include <steam/steamnetworkingsockets.h>
#include <string>

namespace OloEngine
{
    class NetworkClient
    {
      public:
        bool Connect(const std::string& address, u16 port);
        void Disconnect();
        void PollMessages();

        [[nodiscard]] bool IsConnected() const;
        [[nodiscard]] EConnectionState GetState() const;
        [[nodiscard]] HSteamNetConnection GetConnectionHandle() const;

        bool Send(const void* data, u32 size, i32 sendFlags);

        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        HSteamNetConnection m_Connection = k_HSteamNetConnection_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        EConnectionState m_State = EConnectionState::None;
    };
} // namespace OloEngine
