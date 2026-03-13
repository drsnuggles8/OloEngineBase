#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Threading/Mutex.h"

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

        // Send raw data to the server
        bool Send(const void* data, u32 size, i32 sendFlags);

        // Send a typed message to the server
        bool SendMessage(ENetworkMessageType type, const u8* payload, u32 payloadSize, i32 sendFlags);

        [[nodiscard]] bool IsConnected() const;
        [[nodiscard]] EConnectionState GetState() const;
        [[nodiscard]] HSteamNetConnection GetConnectionHandle() const;
        [[nodiscard]] NetworkMessageDispatcher& GetDispatcher();
        [[nodiscard]] const NetworkStats& GetStats() const;

        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        HSteamNetConnection m_Connection = k_HSteamNetConnection_Invalid;
        ISteamNetworkingSockets* m_Interface = nullptr;
        EConnectionState m_State = EConnectionState::None;

        NetworkMessageDispatcher m_Dispatcher;
        NetworkStats m_Stats;
        mutable FMutex m_Mutex;
    };
} // namespace OloEngine
