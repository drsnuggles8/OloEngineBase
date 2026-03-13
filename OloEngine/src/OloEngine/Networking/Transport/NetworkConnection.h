#pragma once

#include "OloEngine/Core/Base.h"

#include <steam/steamnetworkingsockets.h>

namespace OloEngine
{
    enum class EConnectionState : u8
    {
        None,
        Connecting,
        Connected,
        ClosedByPeer,
        ProblemDetectedLocally,
        FindingRoute
    };

    class NetworkConnection
    {
      public:
        explicit NetworkConnection(HSteamNetConnection handle);
        ~NetworkConnection();

        // Non-copyable, movable
        NetworkConnection(const NetworkConnection&) = delete;
        NetworkConnection& operator=(const NetworkConnection&) = delete;
        NetworkConnection(NetworkConnection&& other) noexcept;
        NetworkConnection& operator=(NetworkConnection&& other) noexcept;

        [[nodiscard]] HSteamNetConnection GetHandle() const;
        [[nodiscard]] EConnectionState GetState() const;
        [[nodiscard]] u32 GetClientID() const;

        void SetState(EConnectionState state);
        void SetClientID(u32 clientID);

        bool Send(const void* data, u32 size, i32 sendFlags);
        void Close(i32 reason = 0, const char* debug = "Closing");

      private:
        HSteamNetConnection m_Handle;
        EConnectionState m_State = EConnectionState::None;
        u32 m_ClientID = 0;
    };
} // namespace OloEngine
