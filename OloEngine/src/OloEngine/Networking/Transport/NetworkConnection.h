#pragma once

#include "OloEngine/Core/Base.h"

#include <steam/steamnetworkingsockets.h>

namespace OloEngine
{
    // @enum EConnectionState
    // @brief Connection state mirroring GNS ESteamNetworkingConnectionState.
    enum class EConnectionState : u8
    {
        None = 0,
        Connecting,
        Connected,
        ClosedByPeer,
        ProblemDetectedLocally,
        FindingRoute
    };

    // @class NetworkConnection
    // @brief RAII wrapper around an HSteamNetConnection handle.
    //
    // Tracks per-connection state and exposes type-safe send/close helpers.
    // Instances are owned by NetworkServer's connection map or NetworkClient.
    class NetworkConnection
    {
      public:
        explicit NetworkConnection(HSteamNetConnection handle, u32 clientID = 0);

        // Move-only (handle cannot be duplicated)
        NetworkConnection(const NetworkConnection&)            = delete;
        NetworkConnection& operator=(const NetworkConnection&) = delete;
        NetworkConnection(NetworkConnection&&)                 = default;
        NetworkConnection& operator=(NetworkConnection&&)      = default;

        [[nodiscard]] HSteamNetConnection GetHandle() const { return m_Handle; }
        [[nodiscard]] EConnectionState    GetState()  const { return m_State;  }
        [[nodiscard]] u32                 GetClientID() const { return m_ClientID; }

        void SetState(EConnectionState state) { m_State = state; }

        // @brief Send raw bytes over this connection.
        // @param data   Pointer to the data buffer.
        // @param size   Number of bytes to send.
        // @param sendFlags GNS send flags (e.g. k_nSteamNetworkingSend_Reliable).
        // @return true on success.
        bool Send(const void* data, u32 size, i32 sendFlags) const;

        // @brief Close the connection gracefully.
        void Close(i32 reason = 0, const char* debug = "Closing") const;

      private:
        HSteamNetConnection m_Handle   = k_HSteamNetConnection_Invalid;
        EConnectionState    m_State    = EConnectionState::None;
        u32                 m_ClientID = 0;
    };

} // namespace OloEngine
