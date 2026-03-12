#include "OloEnginePCH.h"
#include "NetworkConnection.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    NetworkConnection::NetworkConnection(HSteamNetConnection handle, u32 clientID)
        : m_Handle(handle), m_State(EConnectionState::None), m_ClientID(clientID)
    {
    }

    bool NetworkConnection::Send(const void* data, u32 size, i32 sendFlags) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Handle == k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_WARN("[NetworkConnection] Send called on invalid handle.");
            return false;
        }

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (!pInterface)
        {
            return false;
        }

        EResult result = pInterface->SendMessageToConnection(
            m_Handle, data, size, sendFlags, nullptr);

        if (result != k_EResultOK)
        {
            OLO_CORE_WARN("[NetworkConnection] SendMessageToConnection failed with result {}.",
                          static_cast<i32>(result));
            return false;
        }

        return true;
    }

    void NetworkConnection::Close(i32 reason, const char* debug) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Handle == k_HSteamNetConnection_Invalid)
        {
            return;
        }

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (pInterface)
        {
            pInterface->CloseConnection(m_Handle, reason, debug, false);
        }
    }

} // namespace OloEngine
