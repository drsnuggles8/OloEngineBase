#pragma once

#include "OloEngine/Core/Base.h"
#include "NetworkConnection.h"

#include <steam/steamnetworkingsockets.h>
#include <string>

namespace OloEngine
{
    // @class NetworkClient
    // @brief Manages a single outgoing GNS connection to a server.
    //
    // Call Connect() to initiate a connection, then call PollMessages() every
    // network tick (from NetworkThread) to drain incoming data.
    class NetworkClient
    {
      public:
        NetworkClient() = default;
        ~NetworkClient() { Disconnect(); }

        // Non-copyable / non-movable (owns GNS handle)
        NetworkClient(const NetworkClient&)            = delete;
        NetworkClient& operator=(const NetworkClient&) = delete;

        bool Connect(const std::string& address, u16 port);
        void Disconnect();
        void PollMessages();

        // @brief Send raw bytes to the server.
        bool Send(const void* data, u32 size, i32 sendFlags) const;

        [[nodiscard]] bool             IsConnected() const { return m_State == EConnectionState::Connected; }
        [[nodiscard]] EConnectionState GetState()    const { return m_State; }

        // Called from the GNS status-changed callback
        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        HSteamNetConnection  m_Connection = k_HSteamNetConnection_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        EConnectionState     m_State      = EConnectionState::None;
    };

} // namespace OloEngine
